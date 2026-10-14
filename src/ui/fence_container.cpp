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
    QList<QRect> rects;
    rects.reserve(m_fences.size());
    for (const FenceWindow& fence : m_fences) {
        if (fence.id != excludeId)
            rects.append(fence.rect);
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
    fence.view->resize(fence.rect.size());
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
        if (!surface->attach(m_fences.last().rect)) {
            qCWarning(lc).noquote()
                << QStringLiteral("fence %1: could not host the surface (no desktop?)").arg(id);
            delete surface;
        } else {
            find(id)->surface = surface;
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
    qCInfo(lc).noquote()
        << QStringLiteral("applied layout: %1 fence(s)").arg(m_fences.size());
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

    QRect target = rect;
    if (target.width() < m_minSize.width())
        target.setWidth(m_minSize.width());
    if (target.height() < m_minSize.height())
        target.setHeight(m_minSize.height());

    if (snap)
        target = FenceGeometry::snap(target, otherRects(id), m_bounds, m_snapThreshold);
    target = FenceGeometry::clampToBounds(target, m_bounds);

    fence->rect = target;
    if (fence->view)
        fence->view->resize(target.size());
    if (fence->surface)
        fence->surface->setGeometry(target);
    renderFence(id);

    emit fenceRectChanged(id, target);
    return target;
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
