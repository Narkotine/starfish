#include "ui/fence_container.hpp"

#include <QColor>
#include <QDebug>
#include <QLoggingCategory>
#include <QPainter>
#include <QRegularExpression>

#include "platform/win/desktop_surface.hpp"
#include "ui/fence_view.hpp"

#if defined(_WIN32)
#include "platform/win/desktop_scanner.hpp"
#endif

namespace starfish::ui {

namespace {

QLoggingCategory lc("starfish.container");

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

QString FenceContainer::addFence(const QString& requestedId, const QString& title, const QRect& rect,
                                 const core::FenceStyle& style, const QString& sortBy,
                                 bool sortAscending, bool rolledUp,
                                 const QList<core::DesktopItem>& items)
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
            out.items.reserve(items.size());
            for (const core::DesktopItem& item : items) {
                if (item.path.isEmpty())
                    continue;
                // "portal" arrives with F-024 (T-011); until then every entry
                // is a plain file reference. A portal read back from a document
                // is resolved as the folder it points at.
                out.items.append({QStringLiteral("file"), item.path});
            }
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
        items.reserve(stored.items.size());
        for (const core::FenceItem& entry : stored.items) {
            const core::DesktopItem item = resolveItem(entry.path);
            if (!item.path.isEmpty())
                items.append(item);
        }
        const QString id = addFence(stored.id, stored.title, stored.rect, stored.style, stored.sortBy,
                                    stored.sortAscending, stored.rolledUp, items);
        find(id)->monitor = stored.monitor;
    }
    qCInfo(lc).noquote() << QStringLiteral("applied layout: %1 fence(s), %2 rule(s)")
                                .arg(m_fences.size())
                                .arg(m_rules.size());
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
