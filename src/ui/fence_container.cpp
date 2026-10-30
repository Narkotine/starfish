#include "ui/fence_container.hpp"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>

#include "platform/win/desktop_surface.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_material.hpp"
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

// The two halves of the physical/logical conversion (T-014). A fence's rect is
// stored in physical pixels -- Progman's client space -- while a Qt widget is
// sized and painted in logical pixels, i.e. physical / devicePixelRatio. Both
// live here so the frame, the widget and the mouse routing cannot disagree.
QSize logicalSize(const QSize& physical, qreal dpr)
{
    if (dpr <= 0.0 || qFuzzyCompare(dpr, 1.0))
        return physical;
    return QSize(qMax(1, qRound(physical.width() / dpr)), qMax(1, qRound(physical.height() / dpr)));
}

} // namespace

FenceContainer::FenceContainer(QObject* parent)
    : QObject(parent)
{
    m_rehostTimer.setInterval(1000);
    connect(&m_rehostTimer, &QTimer::timeout, this, &FenceContainer::rehostLostFences);
    // T-039: the backdrop exists from the start but is inert until enabled, so
    // every caller -- headless probes included -- can ask it what the desktop
    // would show without owning a window.
    m_backdrop = new DesktopBackdrop(this);
    // The backdrop is a *view* of the fences, so it follows the same signals the
    // document does: whatever changes which icons a fence holds changes what the
    // desktop shows. A self-connection keeps the call sites free of bookkeeping
    // -- and there is exactly one place to look when an icon does not move.
    connect(this, &FenceContainer::fenceCreated, this,
            [this](const QString&) { refreshBackdrop(); });
    connect(this, &FenceContainer::fenceRemoved, this,
            [this](const QString&) { refreshBackdrop(); });
    connect(this, &FenceContainer::fenceRectChanged, this,
            [this](const QString&, const QRect&) { refreshBackdrop(); });
    connect(this, &FenceContainer::fenceContentChanged, this,
            [this](const QString&) { refreshBackdrop(); });
    connect(this, &FenceContainer::fenceRolledUpChanged, this,
            [this](const QString&, bool) { refreshBackdrop(); });
}

FenceContainer::~FenceContainer()
{
    for (FenceWindow& fence : m_fences) {
        // T-082: the overlay goes *before* the panel it is stacked above -- it
        // holds a pointer to that window (setStackAbove), so the order is not a
        // detail.
        delete fence.iconOverlay;
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
    if (m_backdrop)
        m_backdrop->setIconProvider(m_backdropIconProvider ? m_backdropIconProvider
                                                           : m_iconProvider);
}

void FenceContainer::setBackdropIconProvider(IconGrid::IconProvider provider)
{
    m_backdropIconProvider = std::move(provider);
    if (m_backdrop)
        m_backdrop->setIconProvider(m_backdropIconProvider ? m_backdropIconProvider
                                                           : m_iconProvider);
}

void FenceContainer::refreshFenceIcons()
{
    for (FenceWindow& fence : m_fences) {
        if (fence.view) {
            fence.view->setIconProvider(m_iconProvider);
            renderFence(fence.id);
        }
    }
}

void FenceContainer::refreshBackdropIcons()
{
    if (!m_backdrop)
        return;
    m_backdrop->setIconProvider(m_backdropIconProvider ? m_backdropIconProvider : m_iconProvider);
    m_backdrop->refresh();
}

void FenceContainer::setSortOnAdd(bool sort)
{
    m_sortOnAdd = sort;
    for (FenceWindow& fence : m_fences) {
        if (fence.view)
            fence.view->setSortOnAdd(sort);
    }
}

QRect FenceContainer::fenceTitleScreenRect(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id != id || !fence.view)
            continue;
        const QRect visible = FenceGeometry::visibleRect(fence.rect, fence.rolledUp);
        const qreal dpr = m_screens.devicePixelRatioFor(fence.rect);
        const QRect local = fence.view->titleTextRect();
        const QPoint origin(qRound(visible.x() / dpr), qRound(visible.y() / dpr));
        return local.translated(origin);
    }
    return {};
}

int FenceContainer::fileIntoAt(const QString& fenceId, const QList<core::DesktopItem>& items,
                               const QPoint& screenPoint)
{
    FenceWindow* fence = find(fenceId);
    if (!fence || !fence->view)
        return 0;
    const QRect visible = FenceGeometry::visibleRect(fence->rect, fence->rolledUp);
    const qreal dpr = m_screens.devicePixelRatioFor(fence->rect);
    const QPoint local((screenPoint.x() - visible.x()) / dpr, (screenPoint.y() - visible.y()) / dpr);
    QList<core::DesktopItem> fresh;
    for (const core::DesktopItem& item : items) {
        bool alreadyThere = false;
        for (const core::DesktopItem& existing : fence->view->items())
            alreadyThere = alreadyThere || existing.path.compare(item.path, Qt::CaseInsensitive) == 0;
        if (!item.path.isEmpty() && !alreadyThere)
            fresh.append(item);
    }
    const int filed = fresh.isEmpty() ? 0 : fence->view->insertItems(fresh, fence->view->itemAt(local));
    if (filed > 0)
        qCInfo(lc).noquote() << QStringLiteral("filed %1 item(s) into %2 at %3,%4")
                                    .arg(filed).arg(fence->id).arg(local.x()).arg(local.y());
    return filed;
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

int FenceContainer::fileInto(const QString& fenceId, const QList<core::DesktopItem>& items)
{
    FenceWindow* fence = find(fenceId);
    if (!fence || !fence->view) {
        qCWarning(lc).noquote()
            << QStringLiteral("nothing to file into: fence %1 does not exist").arg(fenceId);
        return 0;
    }

    int filed = 0;
    for (const core::DesktopItem& item : items) {
        if (item.path.isEmpty())
            continue;

        // The same rule the rule engine obeys: an item the fence already holds is
        // not appended again. Re-adding it would re-render the fence and rewrite
        // the document for a change nobody made.
        bool alreadyThere = false;
        for (const core::DesktopItem& existing : fence->view->items()) {
            if (existing.path.compare(item.path, Qt::CaseInsensitive) == 0) {
                alreadyThere = true;
                break;
            }
        }
        if (alreadyThere)
            continue;

        // addDroppedItems() emits contentChanged(), which this container relays as
        // fenceContentChanged() -- the product writes the document from there.
        fence->view->addDroppedItems({item});
        ++filed;
    }

    if (filed > 0)
        qCInfo(lc).noquote()
            << QStringLiteral("filed %1 item(s) into %2").arg(filed).arg(fence->id);
    return filed;
}

QString FenceContainer::fenceAt(const QPoint& point) const
{
    // Quick-hidden fences are not on screen: nothing can be dropped on what the
    // user cannot see.
    if (!m_fencesVisible)
        return QString();

    // Reverse creation order: the last fence created is the one drawn on top of
    // any overlap, so it is the one the user is aiming at when they let go.
    for (int i = m_fences.size() - 1; i >= 0; --i) {
        const FenceWindow& fence = m_fences.at(i);
        if (FenceGeometry::visibleRect(fence.rect, fence.rolledUp).contains(point))
            return fence.id;
    }
    return QString();
}

void FenceContainer::setScreens(const ScreenLayout& layout)
{
    if (layout.isEmpty())
        return; // no information: keep what we know rather than losing the screen
    m_screens = layout;
    if (m_backdrop)
        m_backdrop->setScreens(m_screens);
    // A change of screens is exactly when fences end up off-screen (a monitor
    // unplugged, moved, or its resolution changed): put them back now.
    revalidateScreens();
}

void FenceContainer::setDesktopBounds(const QSize& bounds)
{
    if (bounds.isEmpty())
        return;
    m_screens = ScreenLayout::single(bounds);
    if (m_backdrop)
        m_backdrop->setScreens(m_screens);
}

int FenceContainer::revalidateScreens()
{
    int moved = 0;
    for (FenceWindow& fence : m_fences) {
        const QRect restored = m_screens.restoreRect(fence.rect, fence.monitor);
        const QString monitor = m_screens.nameFor(restored);
        if (restored == fence.rect && monitor == fence.monitor)
            continue; // untouched: no signal, so the product writes nothing

        qCInfo(lc).noquote() << QStringLiteral("fence %1 brought back on screen: %2,%3 %4x%5 -> "
                                               "%6,%7 %8x%9")
                                    .arg(fence.id)
                                    .arg(fence.rect.x())
                                    .arg(fence.rect.y())
                                    .arg(fence.rect.width())
                                    .arg(fence.rect.height())
                                    .arg(restored.x())
                                    .arg(restored.y())
                                    .arg(restored.width())
                                    .arg(restored.height());
        fence.rect = restored;
        fence.monitor = monitor;
        applyVisibleGeometry(fence);
        emit fenceRectChanged(fence.id, restored);
        ++moved;
    }
    return moved;
}

QString FenceContainer::nextId()
{
    return QStringLiteral("fence-%1").arg(m_nextId++, 3, 10, QLatin1Char('0'));
}

// --- the desktop backdrop (T-039) ------------------------------------------

bool FenceContainer::setBackdropEnabled(bool enabled)
{
    if (!m_backdrop)
        return false;
    m_backdrop->setIconProvider(m_backdropIconProvider ? m_backdropIconProvider : m_iconProvider);
    m_backdrop->setScreens(m_screens);
    refreshBackdrop(); // so the first frame is drawn from a current free set
    m_backdrop->setEnabled(enabled);
    return !enabled || m_backdrop->isAttached();
}

bool FenceContainer::backdropEnabled() const
{
    return m_backdrop && m_backdrop->enabled();
}

void FenceContainer::setDesktopItems(QList<core::DesktopItem> items)
{
    if (m_backdrop)
        m_backdrop->setDesktopItems(std::move(items));
}

int FenceContainer::releaseItems(const QString& fenceId, const QStringList& paths)
{
    FenceWindow* fence = find(fenceId);
    if (!fence || !fence->view || paths.isEmpty())
        return 0;

    // A tile a portal contributes is the folder's, not the fence's: releasing it
    // would take it off the desktop only until the next refresh put it straight
    // back (T-011). Refuse it out loud rather than let the icon flicker.
    const QSet<QString> provided = portalProvided(fence->portals);
    QStringList releasable;
    releasable.reserve(paths.size());
    for (const QString& path : paths) {
        if (path.isEmpty())
            continue;
        if (provided.contains(path.toLower())) {
            qCInfo(lc).noquote()
                << QStringLiteral("fence %1: %2 is a mirror of a folder, not the fence's")
                       .arg(fenceId, path);
            continue;
        }
        releasable.append(path);
    }
    if (releasable.isEmpty())
        return 0;

    // The view's contentChanged is what puts the rest in motion: the fence
    // re-renders and the backdrop recomputes its free set. Nothing is written
    // but the shorter item list, and no file is touched.
    const int released = fence->view->removeItemsByPath(releasable);
    if (released > 0) {
        qCInfo(lc).noquote() << QStringLiteral("fence %1: %2 icon(s) back on the desktop")
                                    .arg(fenceId)
                                    .arg(released);
    }
    return released;
}

void FenceContainer::refreshBackdrop()
{
    if (!m_backdrop)
        return;
    // Everything a fence holds. Computed here, on every change, and stored
    // nowhere: the document keeps describing *fences*, and the desktop layer is
    // whatever is left over (ADR-7, and the reason F-007/F-011 need no restore
    // code).
    m_backdrop->setOccupiedPaths(filedPathKeys());
}

QSet<QString> FenceContainer::filedPathKeys() const
{
    QSet<QString> occupied;
    for (const FenceWindow& fence : m_fences) {
        if (!fence.view)
            continue;
        for (const core::DesktopItem& item : fence.view->items()) {
            if (!item.path.isEmpty())
                occupied.insert(BackdropLayout::pathKey(item.path));
        }
    }
    return occupied;
}

QList<QRect> FenceContainer::fenceObstacleRects() const
{
    QList<QRect> rects;
    if (!m_fencesVisible)
        return rects;
    for (const FenceWindow& fence : m_fences)
        rects.append(FenceGeometry::visibleRect(fence.rect, fence.rolledUp));
    return rects;
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
        // T-080: an empty string is "no band of its own" -- QColor() is invalid,
        // which is exactly what FenceView means by it.
        fence->view->setTitleBarColor(QColor(style.titleBarColor));
    }
    // F-038 / T-057: the panel's alpha is a *mode* of the hosted window, not a
    // paint detail, so a style change can switch it -- and this is the only place
    // that has to know, because every appearance change comes through here.
    applyMaterialToSurface(*fence);
    // The render is driven by the view's contentChanged (see addFence): the
    // style reaches the desktop through exactly one path, like everything else.
    emit fenceContentChanged(id);
}

void FenceContainer::setFenceMaterial(const QString& id, const QString& material)
{
    const int alpha = FenceMaterial::alphaFor(material);
    if (alpha < 0) {
        // A name nobody defined is not a material: refuse it out loud rather than
        // silently showing nothing (the same discipline as a failed hot key).
        qCWarning(lc).noquote()
            << QStringLiteral("unknown fence material %1 (known: %2)")
                   .arg(material, FenceMaterial::summary());
        return;
    }
    FenceWindow* fence = find(id);
    if (!fence)
        return;

    core::FenceStyle style = fence->style;
    const QString updated = FenceMaterial::withAlpha(style.background, alpha);
    if (updated == style.background)
        return; // already shown in that material -- no document write
    style.background = updated;
    qCInfo(lc).noquote() << QStringLiteral("fence %1: material %2 (background %3)")
                                .arg(id, material, updated);
    setFenceStyle(id, style);
}

QString FenceContainer::fenceMaterial(const QString& id) const
{
    if (!find(id))
        return QString();
    return FenceMaterial::materialOf(fenceStyle(id).background);
}

bool FenceContainer::applyMaterialToSurface(FenceWindow& fence)
{
    if (!fence.surface || !fence.view)
        return true; // headless: nothing to switch, the mode is the surface's
    // ⚠️ T-079 / ADR-15. This function used to choose the *window's mode* from the
    // material, and since T-075 every fence took the per-pixel push -- which is
    // exactly the path that does not compose on a child of Progman (measured on
    // the user's own desktop: the UpdateLayeredWindow window invisible, the
    // SetLayeredWindowAttributes one beside it visible, same rect, same process).
    // So every fence was invisible, whatever its material: that is the report
    // "je ne vois plus les fences ... même en matériel opaque", and "même en
    // opaque" was the clue -- the opaque material used to select the blit path,
    // and after T-075 it no longer selected anything.
    //
    // The material is now the window's **uniform alpha** (all a single-colour
    // panel can mean) and the rounded panel is the window's **region**, which is
    // what removes the pixels outside the panel -- so the blit path, the one that
    // has always reached the screen, serves every material. The gain beside the
    // fix: a material change is two calls instead of a window recreation.
    const int alpha = fence.view->panelColor().alpha();
    if (!fence.surface->setUniformAlpha(alpha)) {
        qCWarning(lc).noquote()
            << QStringLiteral("fence %1: the window refused the material's alpha (%2)")
                   .arg(fence.id)
                   .arg(alpha);
        return false;
    }
    fence.surface->setCornerRadius(fence.view->cornerRadius());
    // T-082: the icons are not part of that window any more. The panel is what
    // fades, so the tiles get the second window that keeps them opaque -- or lose
    // it, when the material is opaque or the fence asks for its icons to fade too.
    syncIconOverlay(fence);
    qCInfo(lc).noquote() << QStringLiteral("fence %1: window alpha %2, rounded panel %3 px, "
                                           "icons %4")
                                .arg(fence.id)
                                .arg(alpha)
                                .arg(fence.view->cornerRadius())
                                .arg(fence.iconOverlay ? QStringLiteral("opaque (icon overlay)")
                                                       : QStringLiteral("faded with the panel"));
    return true;
}

// --- the icon overlay (T-082) ----------------------------------------------
//
// F-038 asks for a *panel* that is partly transparent, and the user, after
// trying T-057: « je ne pensais pas que changer material s'appliquerait aussi
// aux icônes. Est-il possible de ne l'appliquer qu'à la fence ? » ADR-15's
// material is one uniform alpha for the whole window, so a translucent panel
// faded the tiles with it. A window carries one alpha and no more; the tiles
// therefore need a window of their own, and this is it.
//
// The pair: the **panel** (the fence's surface, translucent, rounded region,
// input target, drop target) below, the **overlay** (opaque, region = the union
// of the tiles, click-through) above. One render feeds both -- the overlay's
// region cuts the same image down to the tiles -- so a tile is the very pixel
// the panel would have drawn at that place, and nothing about the grid, the
// selection or the drag changes: every click still arrives through the panel.
bool FenceContainer::wantsIconOverlay(const FenceWindow& fence) const
{
    // `fadeIcons` is the whole option: the one case where the user asked for the
    // material to apply to the icons as well -- and therefore the one case with
    // exactly the behaviour of before T-082, which is one window and nothing else.
    if (fence.style.fadeIcons)
        return false;
    if (!m_host || !fence.surface)
        return false;
    return FenceMaterial::isTranslucent(QColor(fence.style.background).alpha());
}

QVector<QRect> FenceContainer::iconOverlayTiles(const FenceWindow& fence, qreal dpr) const
{
    QVector<QRect> tiles;
    if (!fence.view)
        return tiles;
    for (const QRect& tile : fence.view->tileRects()) {
        // The ratio only converts the widget's logical pixels into the window's
        // physical ones; at 100 % (the only shape v1 has been measured on) it is
        // the identity.
        tiles.append(QRect(QPoint(qRound(tile.x() * dpr), qRound(tile.y() * dpr)),
                           QSize(qRound(tile.width() * dpr), qRound(tile.height() * dpr))));
    }
    return tiles;
}

void FenceContainer::onFenceSurfaceLost()
{
    if (m_rehostTimer.isActive())
        return;
    qCWarning(lc) << "a fence's window went away with the desktop: hosting it again once the "
                     "desktop is back";
    m_rehostTries = 0;
    m_rehostTimer.start();
}

void FenceContainer::rehostLostFences()
{
    if (++m_rehostTries > 60) {
        m_rehostTimer.stop();
        qCWarning(lc) << "the desktop did not come back in a minute: lost fences stay unhosted";
        return;
    }
    if (!starfish::platform::win::DesktopSurface::isDesktopAvailable())
        return; // Explorer still on its way back
    int lost = 0;
    int back = 0;
    for (FenceWindow& fence : m_fences) {
        if (!fence.surface || fence.surface->isAttached())
            continue;
        ++lost;
        // The overlay died with the panel; it is rebuilt from scratch, above
        // the new panel, by renderFence() -> syncIconOverlay().
        destroyIconOverlay(fence);
        if (!fence.surface->attach(FenceGeometry::visibleRect(fence.rect, fence.rolledUp)))
            continue;
        ++back;
        ++m_rehosted;
        if (!m_fencesVisible)
            fence.surface->setVisible(false);
        renderFence(fence.id);
        qCInfo(lc).noquote() << QStringLiteral("fence %1 hosted again").arg(fence.id);
    }
    if (back == lost)
        m_rehostTimer.stop();
}

bool FenceContainer::attachIconOverlay(FenceWindow& fence)
{
    if (fence.iconOverlay || !fence.surface || !fence.view)
        return false;

    const QRect visible = FenceGeometry::visibleRect(fence.rect, fence.rolledUp);
    const qreal dpr = m_screens.devicePixelRatioFor(fence.rect);

    auto* overlay = new starfish::platform::win::DesktopSurface(this);
    // Pixels only. The mouse, the input target and the OLE drop target stay on
    // the panel underneath: HTTRANSPARENT hands the click straight down, in the
    // same thread, at the same coordinates -- so a click on a tile is the click
    // it was, and a file dropped on one still lands in the fence (T-042's rule).
    overlay->setClickThrough(true);
    overlay->setUniformAlpha(255);
    overlay->setDevicePixelRatio(dpr);
    // The pair's order, expressed the only way Win32 can: the overlay takes the
    // top of the host's children (its own placement), and the *panel* is told to
    // sit directly below it. See DesktopSurface::setStackedBelow.
    fence.surface->setStackedBelow(overlay);
    // The shape goes on *before* attach(): the window is then never a bare
    // rectangle, not even for the instant between being shown and being regioned.
    const QVector<QRect> tiles = iconOverlayTiles(fence, dpr);
    overlay->setRegionRects(tiles);

    if (!overlay->attach(visible)) {
        qCWarning(lc).noquote()
            << QStringLiteral("fence %1: could not host the icon overlay").arg(fence.id);
        delete overlay;
        return false;
    }
    fence.iconOverlay = overlay;
    fence.overlayShapeSize = visible.size();
    fence.overlayHasTiles = !tiles.isEmpty();
    if (!fence.frame.isNull())
        overlay->setFrame(fence.frame);
    overlay->setVisible(m_fencesVisible && fence.overlayHasTiles);
    qCInfo(lc).noquote() << QStringLiteral("fence %1: icon overlay hosted at %2,%3 %4x%5, "
                                           "%6 tile(s)")
                                .arg(fence.id)
                                .arg(visible.x())
                                .arg(visible.y())
                                .arg(visible.width())
                                .arg(visible.height())
                                .arg(tiles.size());
    return true;
}

void FenceContainer::destroyIconOverlay(FenceWindow& fence)
{
    if (!fence.iconOverlay)
        return;
    // The relation goes first: the panel must not be left holding a pointer to a
    // window that is about to be destroyed -- and dropping it puts the panel back
    // at the top of Progman's children, which is where a lone fence belongs.
    if (fence.surface)
        fence.surface->setStackedBelow(nullptr);
    delete fence.iconOverlay;
    fence.iconOverlay = nullptr;
    fence.overlayShapeSize = QSize();
    fence.overlayHasTiles = false;
    qCInfo(lc).noquote() << QStringLiteral("fence %1: icon overlay gone").arg(fence.id);
}

void FenceContainer::syncIconOverlay(FenceWindow& fence)
{
    if (!wantsIconOverlay(fence)) {
        // Opaque material, `fadeIcons` on, headless, or the panel is gone: there
        // is nothing to keep crisp, and a second window that shows nothing is a
        // window the desktop does not need.
        destroyIconOverlay(fence);
        return;
    }
    if (!fence.iconOverlay && !attachIconOverlay(fence))
        return;

    const QRect visible = FenceGeometry::visibleRect(fence.rect, fence.rolledUp);
    const qreal dpr = m_screens.devicePixelRatioFor(fence.rect);
    fence.iconOverlay->setDevicePixelRatio(dpr);
    // ⚠️ The tiles are measured once per *size*, never per move: the shape is a
    // function of the size (ADR-15), and measuring one caption costs a font
    // metric -- a title-bar drag would pay it sixty times a second. renderFence()
    // clears this record, which is what makes a content change re-shape the window.
    if (fence.overlayShapeSize != visible.size()) {
        const QVector<QRect> tiles = iconOverlayTiles(fence, dpr);
        fence.iconOverlay->setRegionRects(tiles);
        fence.overlayShapeSize = visible.size();
        fence.overlayHasTiles = !tiles.isEmpty();
    }
    fence.iconOverlay->setGeometry(visible);
    // A fence with no tile to show (empty, or rolled up to its bar) keeps its
    // overlay but shows none of it; quick hide hides both halves together.
    fence.iconOverlay->setVisible(m_fencesVisible && fence.overlayHasTiles);
}

bool FenceContainer::fenceIconsFollowMaterial(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.style.fadeIcons;
    }
    return false;
}

bool FenceContainer::setFenceIconsFollowMaterial(const QString& id, bool fadeIcons)
{
    FenceWindow* fence = find(id);
    if (!fence || fence->style.fadeIcons == fadeIcons)
        return false;
    // Through setFenceStyle, like every other appearance change: one path to the
    // document, one signal for the auto-save, and the material is re-applied in
    // the same breath (which is what creates or drops the overlay).
    core::FenceStyle style = fence->style;
    style.fadeIcons = fadeIcons;
    setFenceStyle(id, style);
    return true;
}

void FenceContainer::setFenceSort(const QString& id, const QString& by, bool ascending)
{
    FenceWindow* fence = find(id);
    if (!fence || !fence->view)
        return;
    fence->view->setSort(by, ascending);
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

starfish::platform::win::DesktopSurface* FenceContainer::iconOverlay(const QString& id) const
{
    for (const FenceWindow& fence : m_fences) {
        if (fence.id == id)
            return fence.iconOverlay;
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
    // Session 60: a new fence's order is the user's unless he asked for the alphabet.
    return addFence(QString(), title, rect, style,
                    m_sortOnAdd ? QStringLiteral("name") : QStringLiteral("manual"), true, false, items);
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
                                 const QStringList& portalPaths, const QString& monitorHint)
{
    FenceWindow fence;
    fence.id = requestedId.isEmpty() ? nextId() : requestedId;
    fence.title = title;
    fence.style = style;
    fence.rolledUp = rolledUp;
    // The rect comes from the document in virtual-screen coordinates (T-014),
    // so where it belongs is decided by ScreenLayout -- which keeps it as it is
    // when it still lands on a screen, and brings it back onto the recorded
    // monitor (or the nearest one) when it does not. Then the minimum size is
    // applied, and the result is clamped once more: growing to the minimum must
    // not push a fence off its screen.
    fence.rect = m_screens.restoreRect(rect, monitorHint);
    if (fence.rect.width() < m_minSize.width())
        fence.rect.setWidth(m_minSize.width());
    if (fence.rect.height() < m_minSize.height())
        fence.rect.setHeight(m_minSize.height());
    fence.rect = FenceGeometry::clampIntoRect(fence.rect, m_screens.clampRectFor(fence.rect));
    // Derived from where the fence ended up, never from where it claimed to be:
    // the document must not name a monitor the fence does not sit on.
    fence.monitor = m_screens.nameFor(fence.rect);

    // A restored id must not be handed out again by nextId().
    static const QRegularExpression numbered(QStringLiteral("^fence-(\\d+)$"));
    const auto match = numbered.match(fence.id);
    if (match.hasMatch())
        m_nextId = qMax(m_nextId, match.captured(1).toInt() + 1);

    fence.view = new FenceView;
    fence.view->setSortOnAdd(m_sortOnAdd);
    fence.view->setTitle(title);
    fence.view->setIconProvider(m_iconProvider);
    fence.view->setPanelColor(QColor(style.background));
    fence.view->setCornerRadius(style.radius);
    fence.view->setTitleBarColor(QColor(style.titleBarColor));
    fence.view->setRolledUp(rolledUp);
    // The widget is sized to what is visible: a restored rolled-up fence comes
    // back as a title bar, not as a full panel. Logical pixels -- the stored
    // rect is physical, so on a scaled monitor the widget is smaller than the
    // rect it fills (T-014).
    fence.view->resize(logicalSize(FenceGeometry::visibleRect(fence.rect, fence.rolledUp).size(),
                                   m_screens.devicePixelRatioFor(fence.rect)));
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
    // ... and the hosted surface shows an *image*, so a content change has to
    // rebuild it: the widget's own paintEvent never reaches the desktop. Without
    // this a drop, a portal refresh or a released icon would exist only in the
    // model (T-040).
    connect(view, &FenceView::contentChanged, this, [this, id] { renderFence(id); });
    connect(view, &FenceView::itemReleaseRequested, this,
            [this, id](const QStringList& paths) { releaseItems(id, paths); });
    connect(view, &FenceView::rollUpRequested, this,
            [this, id](bool rolledUp) { setFenceRolledUp(id, rolledUp); });
    connect(view, &FenceView::titleEditRequested, this, [this, id] { emit fenceTitleEditRequested(id); });
    // T-076: the fence's own menu, reported with the fence it belongs to. Nothing
    // is built here -- the container has no interface (it renders an image) and
    // the actions are the product's, exactly like the tray's entries.
    connect(view, &FenceView::fenceMenuRequested, this,
            [this, id](const QPoint& globalPos) { emit fenceMenuRequested(id, globalPos); });

    if (m_host) {
        auto* surface = new starfish::platform::win::DesktopSurface(this);
        surface->setInputTarget(view);
#if defined(_WIN32)
        // The hosted surface is an image on screen, so Qt's own drag-and-drop
        // never runs there: files dropped by the Explorer arrive through the
        // OLE IDropTarget of the surface (T-006-3) and are appended here.
        surface->setDropHandler([this, id](const QPoint& client, const QStringList& paths) {
            FenceWindow* target = find(id);
            if (!target || !target->view)
                return;
            // Session 60: inserted where it was let go (the client point is
            // physical, the grid logical).
            const qreal dpr = m_screens.devicePixelRatioFor(target->rect);
            const int at = target->view->itemAt(QPoint(qRound(client.x() / dpr), qRound(client.y() / dpr)));
            QList<core::DesktopItem> dropped;
            dropped.reserve(paths.size());
            for (const QString& path : paths) {
                const core::DesktopItem item = resolveItem(path);
                if (!item.path.isEmpty())
                    dropped.append(item);
            }
            if (!dropped.isEmpty())
                target->view->insertItems(dropped, at);
        });
#endif
        // ⚠️ T-079 / ADR-15: the window is born with the material as its uniform
        // alpha and the rounded panel as its region, *before* attach() so the
        // window is created right. It used to be born per-pixel (T-057/T-075) --
        // the path that does not reach the screen on a child of Progman.
        surface->setUniformAlpha(QColor(style.background).alpha());
        surface->setCornerRadius(style.radius);
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
                    &FenceContainer::onFenceSurfaceLost);
        }
    }

    // T-082: a fence born in a translucent material is born with its icon overlay
    // (the panel it stacks above has just been hosted). renderFence() below pushes
    // the first frame to both windows.
    syncIconOverlay(*find(id));

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
        addFence(stored.id, stored.title, stored.rect, stored.style, stored.sortBy,
                 stored.sortAscending, stored.rolledUp, items, portalPaths, stored.monitor);
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
        delete m_fences.at(i).iconOverlay; // T-082: above the panel, so first
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
    // just the bookkeeping. Since T-014 the screens are a list (snapping docks
    // to a neighbouring monitor's inner edge) and the fence is clamped to the
    // one screen its centre is on -- never into the dead zone between two
    // monitors of different sizes, where it would be invisible and unreachable.
    const QRect target = FenceGeometry::applyRequest(fence->rect, fence->rolledUp, rect,
                                                     otherRects(id), m_screens.frames(),
                                                     m_screens.clampRectFor(rect), m_minSize,
                                                     m_snapThreshold, snap);
    fence->rect = target;
    // Moving a fence to another monitor updates the monitor it is recorded on.
    fence->monitor = m_screens.nameFor(target);
    applyVisibleGeometry(*fence);

    emit fenceRectChanged(id, target);
    return target;
}

void FenceContainer::applyVisibleGeometry(FenceWindow& fence)
{
    const QRect visible = FenceGeometry::visibleRect(fence.rect, fence.rolledUp);
    // The stored rect is physical; the widget is logical. On a 100 % monitor the
    // two are the same size, which is why this conversion can be added without
    // changing a single pixel of the v1 behaviour (T-014).
    const qreal dpr = m_screens.devicePixelRatioFor(fence.rect);
    if (fence.view) {
        fence.view->setRolledUp(fence.rolledUp);
        fence.view->resize(logicalSize(visible.size(), dpr));
    }
    // T-082: the overlay is placed *first*. It is the window that goes to the top
    // of Progman's children, and the panel then takes its place directly below it
    // (DesktopSurface::setStackedBelow) -- a pair, which is what keeps the tiles
    // over the panel they belong to when a fence is dragged past another.
    syncIconOverlay(fence);
    if (fence.surface) {
        // The surface owns a raw Win32 window, whose coordinates are physical
        // pixels: it gets the physical rect, plus the ratio it needs to turn the
        // mouse coordinates it receives back into widget coordinates.
        fence.surface->setDevicePixelRatio(dpr);
        // T-080: the region's rounding is the widget's, and the widget's is now
        // bounded by its own size -- so a fence resized *smaller* has to hand the
        // surface the smaller radius, or the window would be cut to a shape the
        // panel no longer draws. Idempotent (an unchanged radius returns at once).
        fence.surface->setCornerRadius(fence.view ? fence.view->cornerRadius() : 0);
        fence.surface->setGeometry(visible);
    }

    // ⚠️ T-077 -- a geometry change only needs a *redraw* when it changed the size
    // the fence is painted at, and that is not the same question as "did the
    // geometry change". Moving a fence (the title-bar drag) resizes nothing: the
    // frames are rendered at the fence's own rect, so the image is already the
    // right one, and the surface carries the new position in the same push (its
    // place() is what UpdateLayeredWindow is given the destination by). Repeating
    // the render there re-laid out the icon grid, and repainted every tile, ~60
    // times a second under the cursor, for an image whose bytes could not change.
    // What a fence's *contents* are is not this function's business anyway: a
    // content change reaches the desktop through the view's contentChanged (T-040).
    // The image itself is the record of what it was rendered for -- its size and
    // its device pixel ratio -- so the comparison needs no second source of truth.
    const bool needsRender = fence.frame.isNull() || fence.frame.size() != visible.size()
        || !qFuzzyCompare(fence.frame.devicePixelRatio(), dpr);
    if (needsRender)
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
            // T-082: ... and it is not alone any more when the material is
            // translucent. Hiding half a fence is the kind of bug the eye catches
            // and a counter does not, so the overlay is told in the same breath --
            // syncIconOverlay() keeps its own state (an empty fence shows nothing)
            // and is a no-op when there is no overlay.
            syncIconOverlay(fence);
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

    const qreal dpr = m_screens.devicePixelRatioFor(fence->rect);
    // The frame is always the size of the fence's *visible physical* rect: that
    // is exactly the hosted window's client area, so the surface blits it 1:1 on
    // any monitor. The device pixel ratio is what makes Qt paint the logical
    // widget into those physical pixels, so a fence keeps the same apparent size
    // (and the same icon density) on a 150 % monitor as on a 100 % one.
    const QRect visible = FenceGeometry::visibleRect(fence->rect, fence->rolledUp);
    QImage frame(visible.size(), QImage::Format_ARGB32_Premultiplied);
    frame.setDevicePixelRatio(dpr);

    // ⚠️ T-079 / ADR-15: the frame is **opaque**, and the shape is no longer its
    // business.
    //
    // T-075's reasoning was that the frame's alpha channel *is* the fence's shape,
    // so it filled the under-layer at the input alpha floor and pushed the whole
    // thing per-pixel. The first half was right and the second was fatal: the
    // per-pixel push does not compose on a child of Progman (measured), so the
    // fences this filled were never on screen. What draws the shape now is the
    // window's **region** (DesktopSurface::setCornerRadius): the pixels outside
    // the rounded panel are not the window's pixels at all, so the blit path --
    // which ignores alpha -- can be handed a plain rectangle and still show a
    // rounded panel.
    //
    // The fill is the panel's RGB at full alpha, through a QPainter rather than
    // QImage::fill: QPainter's output on a premultiplied image is premultiplied
    // by construction, and a "premultiplied" frame whose RGB exceeds its alpha
    // composites as a bright artefact instead of as an invisible pixel.
    const QColor panel = fence->view->panelColor();
    {
        QPainter painter(&frame);
        // The rect is the frame's *logical* extent -- the same conversion the
        // widget is sized with -- because the image carries the device pixel ratio.
        painter.fillRect(QRect(QPoint(0, 0), logicalSize(visible.size(), dpr)),
                         QColor(panel.red(), panel.green(), panel.blue()));
        fence->view->render(&painter);
    }
    ++m_frameRenders;
    fence->frame = frame;
    if (fence->surface)
        fence->surface->setFrame(frame);
    if (fence->iconOverlay) {
        // The very same image: the overlay's region already cuts it down to the
        // tiles, so a tile is the pixel the panel would have painted at that
        // place, and one render still serves both windows (T-082).
        fence->iconOverlay->setFrame(frame);
        // The tile layout changed with the frame -- an item added, removed,
        // wrapped or renamed -- and the overlay's shape *is* that layout: the
        // record is cleared so the next sync re-measures it (and a move, which
        // renders nothing, does not).
        fence->overlayShapeSize = QSize();
        syncIconOverlay(*fence);
    }
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
    // T-086: a resize moves the grabbed edges only (applyResize), never the
    // fence -- the top-left corner stays put unless the left/top edge is held.
    const QRect requested = FenceGeometry::resize(fence->dragStartRect, handle, delta, m_minSize);
    const QRect target = FenceGeometry::applyResize(fence->dragStartRect, fence->rolledUp, handle, requested,
                                                    otherRects(id), m_screens.frames(),
                                                    m_screens.clampRectFor(fence->dragStartRect), m_minSize,
                                                    m_snapThreshold, true);
    fence->rect = target;
    fence->monitor = m_screens.nameFor(target);
    applyVisibleGeometry(*fence);
    emit fenceRectChanged(id, target);
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
