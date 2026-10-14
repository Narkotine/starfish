#include "ui/fence_container.hpp"

#include <QColor>
#include <QDebug>
#include <QLoggingCategory>
#include <QPainter>

#include "platform/win/desktop_surface.hpp"
#include "ui/fence_view.hpp"

#if defined(_WIN32)
#include "platform/win/desktop_scanner.hpp"
#endif

namespace starfish::ui {

namespace {

QLoggingCategory lc("starfish.container");

// Matches FenceView's panel colour: the opaque surface path ignores the alpha
// channel, so the pixels outside the rounded corners have to be filled with
// something instead of staying transparent (which GDI would render black).
const QColor kPanelColor(0x1F, 0x29, 0x37);

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
    FenceWindow fence;
    fence.id = nextId();
    fence.title = title;
    fence.rect = FenceGeometry::clampToBounds(rect, m_bounds);
    if (fence.rect.width() < m_minSize.width())
        fence.rect.setWidth(m_minSize.width());
    if (fence.rect.height() < m_minSize.height())
        fence.rect.setHeight(m_minSize.height());
    fence.rect = FenceGeometry::clampToBounds(fence.rect, m_bounds);

    fence.view = new FenceView;
    fence.view->setTitle(title);
    fence.view->setIconProvider(m_iconProvider);
    fence.view->resize(fence.rect.size());
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
                const core::DesktopItem item =
                    starfish::platform::win::DesktopScanner::itemForPath(path);
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
    frame.fill(perPixel ? QColor(Qt::transparent) : kPanelColor);
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
