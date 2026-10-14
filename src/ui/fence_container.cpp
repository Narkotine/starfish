#include "ui/fence_container.hpp"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>

#include "platform/win/desktop_surface.hpp"
#include "ui/fence_view.hpp"

#if defined(_WIN32)
#include "platform/win/desktop_scanner.hpp"
#endif

namespace starfish::ui {

namespace {

QLoggingCategory lc("starfish.container");

// The paths the portals of a fence contribute, lowercased (Windows paths are
// case-insensitive). This is the one place that decides what "a tile that
// belongs to a portal" means: the document skips exactly these, and a refresh
// withdraws exactly the previous ones.
QSet<QString> portalProvided(const QList<core::Portal>& portals)
{
    QSet<QString> provided;
    for (const core::Portal& portal : portals) {
        for (const core::DesktopItem& item : portal.items) {
            if (!item.path.isEmpty())
                provided.insert(item.path.toLower());
        }
    }
    return provided;
}

QStringList pathsOf(const QList<core::DesktopItem>& items)
{
    QStringList paths;
    paths.reserve(items.size());
    for (const core::DesktopItem& item : items) {
        if (!item.path.isEmpty())
            paths.append(item.path);
    }
    return paths;
}

} // namespace

FenceContainer::FenceContainer(QObject* parent)
    : QObject(parent)
{
}

FenceContainer::~FenceContainer()
{
    for (FenceWindow& fence : m_fences) {
        delete fence.surface; // destroys the hosted window before the view
        delete fence.view;
    }
    m_fences.clear();
}

void FenceContainer::setIconProvider(IconGrid::IconProvider provider)
{
    m_iconProvider = std::move(provider);
    for (FenceWindow& fence : m_fences) {
        if (fence.view)
            fence.view->setIconProvider(m_iconProvider);
    }
}

void FenceContainer::setItemResolver(ItemResolver resolver)
{
    m_itemResolver = std::move(resolver);
}

core::DesktopItem FenceContainer::resolveItem(const QString& path) const
{
    if (path.isEmpty())
        return {};
    // Without a Shell resolver (tests, headless probes) the path alone is
    // enough: the layout is the authority on what a fence shows, so the entry
    // is kept even when the file has moved away.
    return m_itemResolver ? m_itemResolver(path) : core::itemFromPathOnly(path);
}

core::DesktopItem FenceContainer::itemForPath(const QString& path) const
{
    return resolveItem(path);
}

void FenceContainer::setRules(QList<core::Rule> rules)
{
    m_rules = std::move(rules);
    qCInfo(lc).noquote() << QStringLiteral("rules set: %1").arg(m_rules.size());
    emit rulesChanged();
}

std::optional<QString> FenceContainer::classify(const core::DesktopItem& item) const
{
    if (m_rules.isEmpty() || item.path.isEmpty())
        return std::nullopt;
    return core::RuleEngine::classify(core::fileMetaFromItem(item), m_rules);
}

int FenceContainer::applyRulesTo(const QList<core::DesktopItem>& items)
{
    int placed = 0;
    for (const core::DesktopItem& item : items) {
        const std::optional<QString> target = classify(item);
        if (!target)
            continue; // no rule wanted it: it stays where it is

        FenceWindow* fence = find(*target);
        if (!fence || !fence->view) {
            // A rule naming a fence that no longer exists is stale, not fatal.
            // Say so rather than dropping the item in silence.
            qCWarning(lc).noquote()
                << QStringLiteral("rule target %1 does not exist, \"%2\" left alone")
                       .arg(*target, item.path);
            continue;
        }

        // Windows paths are case-insensitive, and an item already filed must
        // not be appended again: that would re-render the fence and rewrite the
        // document for nothing.
        bool alreadyThere = false;
        for (const core::DesktopItem& existing : fence->view->items()) {
            if (existing.path.compare(item.path, Qt::CaseInsensitive) == 0) {
                alreadyThere = true;
                break;
            }
        }
        if (alreadyThere)
            continue;

        // addDroppedItems() emits contentChanged(), which the container already
        // relays as fenceContentChanged() -- no need to emit it here.
        fence->view->addDroppedItems({item});
        emit ruleApplied(item.path, fence->id);
        ++placed;
    }
    if (placed > 0)
        qCInfo(lc).noquote() << QStringLiteral("rules filed %1 item(s)").arg(placed);
    return placed;
}

void FenceContainer::setDesktopBounds(const QSize& bounds)
{
    if (bounds.isEmpty())
        return;
    m_bounds = bounds;
}

QString FenceContainer::nextId()
{
    return QStringLiteral("fence-%1").arg(m_nextId++, 3, 10, QLatin1Char('0'));
}

FenceWindow* FenceContainer::find(const QString& id)
{
    for (FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return &fence;
    }
    return nullptr;
}

const FenceWindow* FenceContainer::find(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return &fence;
    }
    return nullptr;
}

QStringList FenceContainer::fenceIds() const
{
    QStringList ids;
    ids.reserve(m_fences.size());
    for (const FenceWindow& fence : m_fences)
        ids.append(fence.id);
    return ids;
}

QString FenceContainer::fenceTitle(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.title;
    }
    return {};
}

QRect FenceContainer::fenceRect(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.rect;
    }
    return {};
}

QList<core::DesktopItem> FenceContainer::fenceItems(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.view ? fence.view->items() : QList<core::DesktopItem>{};
    }
    return {};
}

core::FenceStyle FenceContainer::fenceStyle(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.style;
    }
    return {};
}

bool FenceContainer::fenceRolledUp(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.rolledUp;
    }
    return false;
}

void FenceContainer::setFenceTitle(const QString& id, const QString& title)
{
    FenceWindow* fence = find(id);
    if (!fence || fence->title == title)
        return;
    fence->title = title;
    if (fence->view)
        fence->view->setTitle(title);
    renderFence(id);
    emit fenceContentChanged(id);
}

void FenceContainer::setFenceStyle(const QString& id, const core::FenceStyle& style)
{
    FenceWindow* fence = find(id);
    if (!fence)
        return;
    fence->style = style;
    if (fence->view) {
        fence->view->setPanelColor(QColor(style.background));
        fence->view->setCornerRadius(style.radius);
    }
    renderFence(id);
    emit fenceContentChanged(id);
}

void FenceContainer::setFenceSort(const QString& id, const QString& by, bool ascending)
{
    FenceWindow* fence = find(id);
    if (!fence || !fence->view)
        return;
    fence->view->setSort(by, ascending);
    renderFence(id);
    emit fenceContentChanged(id);
}

FenceView* FenceContainer::view(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.view;
    }
    return nullptr;
}

starfish::platform::win::DesktopSurface* FenceContainer::surface(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.surface;
    }
    return nullptr;
}

QImage FenceContainer::frame(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.frame;
    }
    return {};
}

QList<QRect> FenceContainer::otherRects(const QString& excludeId) const
{
    // The *visible* rects: a rolled-up fence offers only its title bar as a
    // snapping target, which is what the user sees and aims at.
    QList<QRect> rects;
    rects.reserve(m_fences.size());
    for (const FenceWindow& fence : m_fences) {
        if (fence.id != excludeId)
            rects.append(FenceGeometry::visibleRect(fence.rect, fence.rolledUp));
    }
    return rects;
}

QString FenceContainer::createFence(const QString& title, const QRect& rect,
                                    const QList<core::DesktopItem>& items)
{
    const core::FenceStyle style; // schema v1 defaults (docs/technique.md §5)
    return addFence(QString(), title, rect, style, QStringLiteral("name"), true, false, items);
}

// --- folder portals (T-011, F-024) -----------------------------------------

bool FenceContainer::addPortalTo(FenceWindow& fence, const QString& folderPath, bool announce)
{
    if (!fence.view || folderPath.isEmpty())
        return false;

    // The same folder twice is the same portal: a second entry would mirror
    // every tile of it a second time.
    for (const core::Portal& existing : fence.portals) {
        if (core::sameFolder(existing.path, folderPath))
            return false;
    }

    const core::Portal portal = core::listPortal(folderPath);
    if (!portal.error.isEmpty()) {
        // Kept anyway: an unplugged drive or an offline share must not quietly
        // drop the portal from the document. It simply shows nothing until the
        // folder is reachable again -- and says so.
        qCWarning(lc).noquote()
            << QStringLiteral("portal %1: %2 (kept, showing nothing)").arg(folderPath, portal.error);
    }

    // The portal is recorded *before* its tiles are appended, and the order is
    // load bearing: appendItems() emits contentChanged(), which the product
    // relays as fenceContentChanged() and answers by reading toLayout(). With
    // the portal not yet in the list, that read would see the folder's files as
    // plain items and persist a frozen listing -- exactly what a portal must
    // never become. The document is read *during* the mutation, so the state
    // has to be consistent before the signal, not after it.
    fence.portals.append(portal);
    const int added = fence.view->appendItems(portal.items);

    qCInfo(lc).noquote() << QStringLiteral("portal %1 -> %2 item(s) in %3")
                                .arg(folderPath)
                                .arg(portal.items.size())
                                .arg(fence.id);

    // appendItems() already announced the change when it added tiles. When the
    // folder is empty or unreadable it added none, but the *document* gained a
    // portal entry all the same -- so say so, or the product would not save it.
    if (announce && added == 0)
        emit fenceContentChanged(fence.id);
    return true;
}

bool FenceContainer::addPortal(const QString& fenceId, const QString& folderPath)
{
    FenceWindow* fence = find(fenceId);
    if (!fence)
        return false;
    return addPortalTo(*fence, folderPath, /*announce=*/true);
}

bool FenceContainer::removePortal(const QString& fenceId, const QString& folderPath)
{
    FenceWindow* fence = find(fenceId);
    if (!fence || !fence->view)
        return false;

    for (int i = 0; i < fence->portals.size(); ++i) {
        if (!core::sameFolder(fence->portals.at(i).path, folderPath))
            continue;

        // The tiles a portal contributed are exactly the ones its listing
        // names -- toLayout() never persists them as plain items, so nothing
        // else in the grid is at stake. The files and the folder are untouched.
        const QStringList contributed = pathsOf(fence->portals.at(i).items);
        const QString path = fence->portals.at(i).path;
        fence->portals.removeAt(i);
        fence->view->replaceItems(contributed, {});

        qCInfo(lc).noquote()
            << QStringLiteral("portal %1 removed from %2").arg(path, fenceId);
        if (contributed.isEmpty())
            emit fenceContentChanged(fenceId); // the document lost an entry, the grid did not change
        return true;
    }
    return false;
}

QList<core::Portal> FenceContainer::portals(const QString& fenceId) const
{
    const FenceWindow* fence = find(fenceId);
    return fence ? fence->portals : QList<core::Portal>{};
}

int FenceContainer::applyPortalListing(FenceWindow& fence, core::Portal& portal)
{
    const core::Portal fresh = core::listPortal(portal.path);
    if (!fresh.error.isEmpty()) {
        qCWarning(lc).noquote()
            << QStringLiteral("portal %1: %2 (kept, showing nothing)").arg(portal.path, fresh.error);
    }

    // Only the listing is swapped: `path` stays exactly as the document
    // declared it, because that string is what gets written back.
    const QStringList previous = pathsOf(portal.items);
    portal.items = fresh.items;
    portal.error = fresh.error;

    fence.view->replaceItems(previous, portal.items);
    return portal.items.size();
}

int FenceContainer::refreshPortalsFor(const QString& folder)
{
    int refreshed = 0;
    for (FenceWindow& fence : m_fences) {
        if (!fence.view)
            continue;
        for (core::Portal& portal : fence.portals) {
            if (!folder.isEmpty() && !core::sameFolder(portal.path, folder))
                continue;
            applyPortalListing(fence, portal);
            ++refreshed;
        }
    }
    if (refreshed > 0) {
        qCInfo(lc).noquote() << QStringLiteral("re-listed %1 portal(s)%2")
                                    .arg(refreshed)
                                    .arg(folder.isEmpty() ? QString()
                                                          : QStringLiteral(" for ") + folder);
    }
    return refreshed;
}

QStringList FenceContainer::portalFolders() const
{
    QStringList folders;
    for (const FenceWindow& fence : m_fences) {
        for (const core::Portal& portal : fence.portals) {
            if (!folders.contains(portal.path))
                folders.append(portal.path);
        }
    }
    return folders;
}

QString FenceContainer::addFence(const QString& requestedId, const QString& title, const QRect& rect,
                                 const core::FenceStyle& style, const QString& sortBy,
                                 bool sortAscending, bool rolledUp,
                                 const QList<core::DesktopItem>& items,
                                 const QStringList& portalPaths)
{
    FenceWindow fence;
    fence.id = requestedId.isEmpty() ? nextId() : requestedId;
    fence.title = title;
    fence.style = style;
    fence.rolledUp = rolledUp;
    fence.rect = FenceGeometry::clampToBounds(rect, m_bounds);
    if (fence.rect.width() < m_minSize.width())
        fence.rect.setWidth(m_minSize.width());
    if (fence.rect.height() < m_minSize.height())
        fence.rect.setHeight(m_minSize.height());
    fence.rect = FenceGeometry::clampToBounds(fence.rect, m_bounds);

    // A restored id must not be handed out again by nextId().
    static const QRegularExpression numbered(QStringLiteral("^fence-(\\d+)$"));
    const auto match = numbered.match(fence.id);
    if (match.hasMatch())
        m_nextId = qMax(m_nextId, match.captured(1).toInt() + 1);

    fence.view = new FenceView;
    fence.view->setTitle(title);
    fence.view->setIconProvider(m_iconProvider);
    fence.view->setPanelColor(QColor(style.background));
    fence.view->setCornerRadius(style.radius);
    fence.view->setRolledUp(rolledUp);
    // The widget is sized to what is visible: a restored rolled-up fence comes
    // back as a title bar, not as a full panel.
    fence.view->resize(FenceGeometry::visibleRect(fence.rect, fence.rolledUp).size());
    fence.view->setSort(sortBy, sortAscending);
    fence.view->setItems(items);

    // T-011: the mirrored folders are listed here, before the view is connected
    // and before fenceCreated() is emitted, so a fence is never announced half
    // built. A portal whose folder is unreachable contributes nothing and is
    // still restored into the document.
    for (const QString& portalPath : portalPaths)
        addPortalTo(fence, portalPath, /*announce=*/false);

    const QString id = fence.id;
    m_fences.append(fence);

    FenceView* view = m_fences.last().view;
    connect(view, &FenceView::geometryDragStarted, this,
            [this, id] { beginGeometryDrag(id); });
    connect(view, &FenceView::geometryDragDelta, this,
            [this, id](FenceHandle handle, const QPoint& delta) {
                applyGeometryDrag(id, handle, delta);
            });
    connect(view, &FenceView::geometryDragFinished, this,
            [this, id] { endGeometryDrag(id); });
    connect(view, &FenceView::contentChanged, this,
            [this, id] { emit fenceContentChanged(id); });
    connect(view, &FenceView::rollUpRequested, this,
            [this, id](bool rolledUp) { setFenceRolledUp(id, rolledUp); });

    if (m_host) {
        auto* surface = new starfish::platform::win::DesktopSurface(this);
        surface->setInputTarget(view);
#if defined(_WIN32)
        // The hosted surface is an image on screen, so Qt's own drag-and-drop
        // never runs there: files dropped by the Explorer arrive through the
        // OLE IDropTarget of the surface (T-006-3) and are appended here.
        surface->setDropHandler([this, id](const QPoint&, const QStringList& paths) {
            FenceWindow* target = find(id);
            if (!target || !target->view)
                return;
            QList<core::DesktopItem> dropped;
            dropped.reserve(paths.size());
            for (const QString& path : paths) {
                const core::DesktopItem item = resolveItem(path);
                if (!item.path.isEmpty())
                    dropped.append(item);
            }
            if (!dropped.isEmpty())
                target->view->addDroppedItems(dropped);
        });
#endif
        if (!surface->attach(FenceGeometry::visibleRect(m_fences.last().rect, rolledUp))) {
            qCWarning(lc).noquote()
                << QStringLiteral("fence %1: could not host the surface (no desktop?)").arg(id);
            delete surface;
        } else {
            find(id)->surface = surface;
            // A fence created while the desktop is hidden or peeking joins the
            // current navigation state instead of appearing on its own.
            if (!m_fencesVisible)
                surface->setVisible(false);
            if (m_peekActive)
                surface->setRaisedAboveIcons(true);
            connect(surface, &starfish::platform::win::DesktopSurface::desktopLost, this,
                    [this, id] { renderFence(id); });
        }
    }

    renderFence(id);
    qCInfo(lc).noquote() << QStringLiteral("created fence %1 \"%2\" at %3,%4 %5x%6")
                                .arg(id)
                                .arg(title)
                                .arg(m_fences.last().rect.x())
                                .arg(m_fences.last().rect.y())
                                .arg(m_fences.last().rect.width())
                                .arg(m_fences.last().rect.height());
    emit fenceCreated(id);
    return id;
}

core::Layout FenceContainer::toLayout() const
{
    core::Layout layout;
    layout.rules = m_rules;
    layout.fences.reserve(m_fences.size());
    for (const FenceWindow& fence : m_fences) {
        core::Fence out;
        out.id = fence.id;
        out.title = fence.title;
        out.monitor = fence.monitor;
        out.rect = fence.rect;
        out.style = fence.style;
        out.rolledUp = fence.rolledUp;
        if (fence.view) {
            out.sortBy = fence.view->sortBy();
            out.sortAscending = fence.view->sortAscending();
            const QList<core::DesktopItem> items = fence.view->items();
            const QSet<QString> provided = portalProvided(fence.portals);
            out.items.reserve(items.size() + fence.portals.size());
            for (const core::DesktopItem& item : items) {
                if (item.path.isEmpty())
                    continue;
                // A tile a portal contributes is persisted *through* the portal
                // entry, never as a plain file reference: writing it twice would
                // freeze the listing into the document, and the mirror would
                // stop following its folder (T-011).
                if (provided.contains(item.path.toLower()))
                    continue;
                out.items.append({QStringLiteral("file"), item.path});
            }
            // The folder path is the whole entry: the listing is read back from
            // the folder, so a portal stays a mirror across restarts.
            for (const core::Portal& portal : fence.portals)
                out.items.append({QStringLiteral("portal"), portal.path});
        }
        layout.fences.append(out);
    }
    return layout;
}

bool FenceContainer::applyLayout(const core::Layout& layout, QString* error)
{
    if (layout.schema != core::Layout{}.schema) {
        if (error)
            *error = QStringLiteral("unsupported schema: %1").arg(layout.schema);
        return false;
    }

    // Replace: the current fences go first (the files they showed are never
    // touched), so a restore is idempotent whatever the container held.
    while (!m_fences.isEmpty())
        removeFence(m_fences.first().id);

    // The rules come back too, without announcing a change: applying the
    // document is not modifying it, and announcing it here would make a
    // startup restore look like an edit and trigger a pointless write.
    m_rules = layout.rules;

    for (const core::Fence& stored : layout.fences) {
        QList<core::DesktopItem> items;
        QStringList portalPaths;
        items.reserve(stored.items.size());
        for (const core::FenceItem& entry : stored.items) {
            // T-011: a portal is a folder to mirror, not a tile. Its listing is
            // deliberately not in the document -- it is read from the folder, so
            // the restored fence shows what the folder holds *now*.
            if (core::isPortalKind(entry.kind)) {
                if (!entry.path.isEmpty())
                    portalPaths.append(entry.path);
                continue;
            }
            const core::DesktopItem item = resolveItem(entry.path);
            if (!item.path.isEmpty())
                items.append(item);
        }
        const QString id = addFence(stored.id, stored.title, stored.rect, stored.style, stored.sortBy,
                                    stored.sortAscending, stored.rolledUp, items, portalPaths);
        find(id)->monitor = stored.monitor;
    }
    qCInfo(lc).noquote() << QStringLiteral("applied layout: %1 fence(s), %2 rule(s), %3 portal(s)")
                                .arg(m_fences.size())
                                .arg(m_rules.size())
                                .arg(portalFolders().size());
    return true;
}

bool FenceContainer::removeFence(const QString& id)
{
    for (int i = 0; i < m_fences.size(); ++i) {
        if (m_fences.at(i).id != id)
            continue;
        // Deleting a zone removes the zone, never the files it showed
        // (AGENTS.md golden rule).
        delete m_fences.at(i).surface;
        delete m_fences.at(i).view;
        m_fences.removeAt(i);
        qCInfo(lc).noquote() << QStringLiteral("removed fence %1").arg(id);
        emit fenceRemoved(id);
        return true;
    }
    return false;
}

QRect FenceContainer::setFenceRect(const QString& id, const QRect& rect, bool snap)
{
    FenceWindow* fence = find(id);
    if (!fence)
        return {};

    // All the roll-up aware math lives in the pure geometry (T-009); this is
    // just the bookkeeping.
    const QRect target = FenceGeometry::applyRequest(fence->rect, fence->rolledUp, rect,
                                                     otherRects(id), m_bounds, m_minSize,
                                                     m_snapThreshold, snap);
    fence->rect = target;
    applyVisibleGeometry(*fence);

    emit fenceRectChanged(id, target);
    return target;
}

void FenceContainer::applyVisibleGeometry(FenceWindow& fence)
{
    const QRect visible = FenceGeometry::visibleRect(fence.rect, fence.rolledUp);
    if (fence.view) {
        fence.view->setRolledUp(fence.rolledUp);
        fence.view->resize(visible.size());
    }
    if (fence.surface)
        fence.surface->setGeometry(visible);
    renderFence(fence.id);
}

QRect FenceContainer::fenceVisibleRect(const QString& id) const
{
    const FenceWindow* fence = find(id);
    if (!fence)
        return {};
    return FenceGeometry::visibleRect(fence->rect, fence->rolledUp);
}

bool FenceContainer::setFenceRolledUp(const QString& id, bool rolledUp)
{
    FenceWindow* fence = find(id);
    if (!fence)
        return false;
    if (fence->rolledUp == rolledUp)
        return true;

    fence->rolledUp = rolledUp;
    applyVisibleGeometry(*fence);
    qCInfo(lc).noquote() << QStringLiteral("fence %1 %2")
                                .arg(id, rolledUp ? QStringLiteral("rolled up")
                                                  : QStringLiteral("rolled down"));
    emit fenceRolledUpChanged(id, rolledUp);
    return true;
}

bool FenceContainer::toggleFenceRolledUp(const QString& id)
{
    const FenceWindow* fence = find(id);
    if (!fence)
        return false;
    return setFenceRolledUp(id, !fence->rolledUp);
}

bool FenceContainer::setFencesVisible(bool visible)
{
    if (m_fencesVisible == visible)
        return true;
    m_fencesVisible = visible;

    for (FenceWindow& fence : m_fences) {
        if (fence.surface) {
            // The hosted surface is the only thing actually on screen.
            fence.surface->setVisible(visible);
        } else if (fence.view && !fence.view->isHidden()) {
            // Windowed mode (no surface): the widget is the window. A widget
            // that was never shown is left alone, so a headless container does
            // not pop windows into existence.
            fence.view->setVisible(visible);
        }
    }
    qCInfo(lc).noquote() << QStringLiteral("fences %1")
                                .arg(visible ? QStringLiteral("shown") : QStringLiteral("hidden"));
    emit fencesVisibilityChanged(visible);
    return true;
}

bool FenceContainer::setPeekActive(bool active)
{
    if (m_peekActive == active)
        return true;
    m_peekActive = active;

    for (FenceWindow& fence : m_fences) {
        if (fence.surface)
            fence.surface->setRaisedAboveIcons(active);
    }
    qCInfo(lc).noquote() << QStringLiteral("peek %1")
                                .arg(active ? QStringLiteral("on") : QStringLiteral("off"));
    emit peekChanged(active);
    return true;
}

void FenceContainer::renderFence(const QString& id)
{
    FenceWindow* fence = find(id);
    if (!fence || !fence->view)
        return;

    const bool perPixel = fence->surface && fence->surface->perPixelAlpha();
    QImage frame(fence->view->size(), QImage::Format_ARGB32_Premultiplied);
    // The opaque GDI blit ignores the alpha channel, so the pixels outside the
    // rounded corners are filled with the panel's own colour instead of staying
    // transparent (which GDI would render black). Derived from the view, so a
    // custom fence style cannot drift away from the fill behind it.
    frame.fill(perPixel ? QColor(Qt::transparent) : fence->view->panelColor());
    {
        QPainter painter(&frame);
        fence->view->render(&painter);
    }
    fence->frame = frame;
    if (fence->surface)
        fence->surface->setFrame(frame);
}

void FenceContainer::beginGeometryDrag(const QString& id)
{
    FenceWindow* fence = find(id);
    if (!fence)
        return;
    fence->dragStartRect = fence->rect;
    fence->dragging = true;
}

void FenceContainer::applyGeometryDrag(const QString& id, FenceHandle handle, const QPoint& delta)
{
    FenceWindow* fence = find(id);
    if (!fence)
        return;
    if (!fence->dragging) {
        // A delta without a start (e.g. the widget was driven directly, as in
        // the tests) still works: the current rect is the reference.
        fence->dragStartRect = fence->rect;
        fence->dragging = true;
    }

    if (handle == FenceHandle::TitleBar) {
        QRect moved = fence->dragStartRect;
        moved.moveTopLeft(moved.topLeft() + delta);
        setFenceRect(id, moved, /*snap=*/true);
        return;
    }
    setFenceRect(id, FenceGeometry::resize(fence->dragStartRect, handle, delta, m_minSize),
                 /*snap=*/true);
}

void FenceContainer::endGeometryDrag(const QString& id)
{
    FenceWindow* fence = find(id);
    if (!fence)
        return;
    fence->dragging = false;
    fence->dragStartRect = QRect();
}

} // namespace starfish::ui
