#include "ui/fence_view.hpp"

#include <QMetaMethod>
#include <QtMath>
#include <cmath>
#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QUrl>
#include <QWheelEvent>

#if defined(_WIN32)
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/shell_context_menu.hpp"
#include <windows.h>
#endif

namespace starfish::ui {

namespace {

// Pointer feedback for the fence geometry handles (T-007). Only visible in
// windowed mode: on the hosted desktop the real window is the DesktopSurface,
// which owns the cursor.
Qt::CursorShape cursorForHandle(FenceHandle handle)
{
    switch (handle) {
    case FenceHandle::TitleBar:
        return Qt::SizeAllCursor;
    case FenceHandle::Left:
    case FenceHandle::Right:
        return Qt::SizeHorCursor;
    case FenceHandle::Top:
    case FenceHandle::Bottom:
        return Qt::SizeVerCursor;
    case FenceHandle::TopLeft:
    case FenceHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case FenceHandle::TopRight:
    case FenceHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case FenceHandle::None:
    default:
        return Qt::ArrowCursor;
    }
}

} // namespace

FenceView::FenceView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("starfishFenceView"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    m_grid.setIconSize(48);
    m_grid.setGap(m_metrics.cellGap);
    m_grid.setPadding(m_metrics.gridPadding);
    m_grid.setLabelHeight(m_metrics.labelHeight);
    // The grid derives its cell width from the captions it holds (T-078), and it
    // must measure them with the font it paints them with -- so it is handed the
    // widget's font here rather than reading it off the painter later, which
    // hitTest (a mouse event, no painter in sight) could not do.
    m_grid.setBaseFont(font());

    // The fence accepts files dropped from the Explorer (T-006-3). This is the
    // path used when the fence is a real Qt window; the desktop-hosted surface
    // takes the OLE IDropTarget path in DesktopSurface instead.
    setAcceptDrops(true);
    // Session 66: the resize arrows follow the pointer with no button held --
    // without tracking, Qt drops those moves and the cursor only changed on a
    // fence that had been grabbed once.
    setMouseTracking(true);
}

void FenceView::setTitle(const QString& title)
{
    m_title = title;
    update();
}

QString FenceView::title() const
{
    return m_title;
}

void FenceView::setItems(QList<core::DesktopItem> items)
{
    m_grid.setItems(std::move(items));
    update();
    emit contentChanged();
}

void FenceView::setSort(const QString& by, bool ascending)
{
    m_grid.setSort(by, ascending);
    update();
    emit contentChanged();
}

void FenceView::setPanelColor(const QColor& color)
{
    // F-038 / T-057: the alpha is kept. It used to be dropped here, because the
    // opaque GDI blit of the hosted surface ignores it -- but that is the
    // surface's mode, not the widget's business, and while the alpha was thrown
    // away here the persisted one could never reach a single pixel. The widget
    // paints what the surface will push; a fence whose material is translucent
    // lets the wallpaper read through its panel. The layer below the panel is
    // filled by FenceContainer::renderFence at the input alpha floor, which is
    // what makes the corners *outside* the rounded panel transparent (T-075) --
    // and what keeps them reachable by the mouse (ADR-11).
    m_panelColor = QColor(color.red(), color.green(), color.blue(), color.alpha());
    update();
    emit contentChanged();
}

void FenceView::setCornerRadius(int px)
{
    // T-080: the widget keeps the value it was *asked* for; cornerRadius() bounds
    // it by the geometry, because a radius wider than half the fence describes no
    // shape at all. It used to be clamped to 64 here, which was a guess at the
    // same limit made without knowing the size.
    m_cornerRadius = qMax(0, px);
    update();
    emit contentChanged();
}

int FenceView::cornerRadius() const
{
    // T-080: the radius never exceeds half the shorter side (T-080's own warning:
    // past that, the rounded shape is empty and the window's region would erase the
    // panel). Recomputed on every ask rather than stored, so a fence that is
    // resized smaller -- by a gesture, or by coming back on a smaller screen --
    // cannot keep a rounding its own size no longer allows.
    return qBound(0, m_cornerRadius, qMin(width(), height()) / 2);
}

void FenceView::setTitleBarColor(const QColor& color)
{
    // T-080: an invalid colour is not an error, it is the *absence* of a band --
    // the fence as it looked before this existed, the title on the panel. Storing
    // the alpha as given keeps the same contract as the panel's colour.
    m_titleBarColor = color.isValid()
        ? QColor(color.red(), color.green(), color.blue(), color.alpha())
        : QColor();
    update();
    emit contentChanged();
}

void FenceView::setRolledUp(bool rolledUp)
{
    if (m_rolledUp == rolledUp)
        return;
    m_rolledUp = rolledUp;
    // No content area is left, so a cursor left over from a grip would be
    // misleading until the pointer moves again.
    setCursor(Qt::ArrowCursor);
    update();
}

QRect FenceView::contentRect() const
{
    // A rolled-up fence is nothing but its title bar: there is no content area
    // to hit-test or paint into.
    if (m_rolledUp)
        return QRect();
    const FenceMetrics& m = m_metrics;
    return rect().adjusted(m.inset, titleBarHeight() + (titleShown() ? m.titleGap : 0), -m.inset, -m.inset);
}

void FenceView::setMetrics(const FenceMetrics& metrics)
{
    m_metrics = metrics;
    m_grid.setPadding(metrics.gridPadding);
    m_grid.setGap(metrics.cellGap);
    m_grid.setLabelHeight(metrics.labelHeight);
    update();
}

QRect FenceView::titleTextRect() const
{
    QFont titleFont = font();
    titleFont.setBold(true);
    const QFontMetrics fm(titleFont);
    const QString text = m_title.isEmpty() ? QStringLiteral("Fence") : m_title;
    const int inset = titleInset();
    const int available = qMax(0, width() - 2 * inset);
    const int w = qMin(available, fm.horizontalAdvance(text) + 2);
    const int h = fm.height();
    int x = inset;
    if (m_titleAlign & Qt::AlignHCenter)
        x = inset + (available - w) / 2;
    else if (m_titleAlign & Qt::AlignRight)
        x = inset + available - w;
    return QRect(x, (m_metrics.titleBarHeight - h) / 2, w, h);
}

int FenceView::titleInset() const
{
    // Session 67 (the user: "décaler le titre du rayon de courbure"): the
    // title starts past the whole rounded corner and the border -- the s66
    // curve-at-the-capitals offset was not enough to the eye.
    const int border = qMax(0, m_borderWidth);
    const int r = cornerRadius();
    return qMax(m_metrics.titleInset, r > 0 ? r + border : m_metrics.titleInset + border);
}

void FenceView::setTitleAlignment(Qt::Alignment align)
{
    if (m_titleAlign == align)
        return;
    m_titleAlign = align;
    update();
    emit contentChanged();
}

void FenceView::setIconOpacity(qreal opacity)
{
    m_iconOpacity = qBound<qreal>(0.0, opacity, 1.0);
}

int FenceView::insertItems(const QList<core::DesktopItem>& items, int index)
{
    const int added = m_grid.insertItems(items, index, m_sortOnAdd);
    if (added > 0) {
        update();
        emit contentChanged();
    }
    return added;
}

int FenceView::itemAt(const QPoint& localPoint) const
{
    if (!contentRect().contains(localPoint))
        return -1;
    return m_grid.hitTest(localPoint - contentRect().topLeft(), contentRect().size());
}

QVector<QRect> FenceView::tileRects() const
{
    // The grid lays its cells out from the content origin (hitTest subtracts it
    // above), so the tile rectangles are the cells moved there.
    const QRect content = contentRect();
    QVector<QRect> rects = m_grid.cellRects(content.size());
    for (QRect& rect : rects)
        rect.translate(content.topLeft());
    return rects;
}

void FenceView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QColor titleText(0xE5, 0xE7, 0xEB);
    const QColor hintText(0x9C, 0xA3, 0xAF);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // T-094: the border's own width and colour (0: none); session 65 hover:
    // a touch lighter, the border brighter.
    QColor border = m_borderColor;
    QColor fill = m_panelColor;
    if (m_hovered && m_hoverHighlight) {
        border = border.lighter(140);
        fill = QColor::fromHsv(fill.hsvHue(), fill.hsvSaturation(), qMin(255, fill.value() + 14), fill.alpha());
    }
    const qreal half = qMax(0.5, m_borderWidth / 2.0);
    const QRectF panelRect = QRectF(rect()).adjusted(half, half, -half, -half);
    const int radius = cornerRadius();
    QPainterPath panel;
    panel.addRoundedRect(panelRect, radius, radius);
    // T-080, session 66: the title bar's own colour is *not* laid over the
    // panel (two translucent layers made it near opaque): the panel fills
    // below the band, the band fills the band, the border goes over both.
    if (m_titleBarColor.isValid()) {
        const int band = titleBarHeight();
        painter.save();
        painter.setClipRect(QRect(0, band, width(), height() - band));
        painter.fillPath(panel, fill);
        painter.setClipRect(QRect(0, 0, width(), band));
        QColor bandFill = m_titleBarColor;
        if (m_hovered && m_hoverHighlight)
            bandFill = QColor::fromHsv(bandFill.hsvHue(), bandFill.hsvSaturation(),
                                       qMin(255, bandFill.value() + 14), bandFill.alpha());
        painter.fillPath(panel, bandFill);
        painter.restore();
    } else {
        painter.fillPath(panel, fill);
    }
    if (m_borderWidth > 0) {
        painter.setPen(QPen(border, m_borderWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(panel);
    }

    if (titleShown()) {
        QFont titleFont = font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        const int inset = titleInset();
        const QRect titleRect(inset, 0, qMax(0, width() - 2 * inset), m_metrics.titleBarHeight);
        const QString titleText_ = painter.fontMetrics().elidedText(
            m_title.isEmpty() ? QStringLiteral("Fence") : m_title, Qt::ElideRight, titleRect.width());
        const Qt::Alignment align = Qt::AlignVCenter | (m_titleAlign ? m_titleAlign : Qt::AlignLeft);
        if (m_grid.textShadow()) {
            painter.setPen(QColor(0, 0, 0, 0xA0));
            painter.drawText(titleRect.translated(1, 1), align, titleText_);
        }
        painter.setPen(titleText);
        painter.drawText(titleRect, align, titleText_);
    } else if (m_hovered) {
        // T-076: no title bar -- a small grip says where the fence is moved from.
        painter.setPen(QPen(QColor(255, 255, 255, 0x70), 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(width() / 2.0 - 14, kHiddenTitleBand / 2.0 + 1),
                         QPointF(width() / 2.0 + 14, kHiddenTitleBand / 2.0 + 1));
    }

    const QRect content = contentRect();
    // Rolled up (F-020): the widget is the title bar, nothing else is drawn.
    if (m_rolledUp || content.isEmpty())
        return;

    if (m_grid.items().isEmpty()) {
        painter.setFont(font());
        painter.setPen(hintText);
        // F-036 / T-014b: a string drawn *in* a fence is part of the interface, so
        // it goes through tr() like a menu label. It was a QStringLiteral -- the
        // one kind of user-visible text a translation catalogue cannot see.
        painter.drawText(content, Qt::AlignCenter, tr("empty fence -- no icons yet"));
        return;
    }

    // Session 66: "the material on the icons too" fades the icons, and only them.
    painter.setOpacity(m_iconOpacity);
    m_grid.paint(painter, content, m_iconProvider);
    painter.setOpacity(1.0);
    if (m_dragging) {
        // Either the grid is about to receive the tile, or the desktop is:
        // never both, so the two hints are mutually exclusive by construction.
        if (m_releaseArmed) {
            if (!m_dragOverOtherFence)
                paintReleaseHint(painter);
        } else {
            paintDropIndicator(m_dropTargetIndex, painter);
        }
    }
    // Session 65: tiles from another fence would land here.
    if (m_externalDropIndex >= 0)
        paintDropIndicator(m_externalDropIndex, painter);
}

void FenceView::mousePressEvent(QMouseEvent* event)
{
    emit pressed();
    if (event->button() == Qt::RightButton) {
        // Right-click selects the item under the cursor and shows the Windows
        // shell context menu for it. The single path (mousePressEvent) covers
        // both a real Qt window and a desktop-hosted surface that posts
        // QMouseEvent(RightButton) from its own WndProc: a QContextMenuEvent
        // would only fire from the platform, not from a posted event.
        const int index = itemAt(event->pos());
        // T-076: when no tile is under the cursor -- the title bar, the empty
        // part of the panel, or the bar of a rolled-up fence -- the menu is
        // **ours** (FenceMenu). The selection is left alone in that case: a
        // right-click on empty space is not a statement about the icons, and the
        // tile path below is unchanged, so an icon keeps the Shell's own menu.
        if (index >= 0) {
            // As on the desktop: the clicked icon joins the selection it acts
            // on, or becomes it.
            if (!m_grid.isSelected(index))
                m_grid.selectOnly(index);
            update();
            emit contentChanged();
            emit selectionGesture();
            if (isSignalConnected(QMetaMethod::fromSignal(&FenceView::itemMenuRequested)))
                emit itemMenuRequested(selectedPaths(), event->pos());
            else
                showContextMenuForIndex(index, event->globalPosition().toPoint());
            return;
        }
        emit fenceMenuRequested(event->globalPosition().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int index = itemAt(event->pos());
    const Qt::KeyboardModifiers mods = event->modifiers();

    // T-007: the title bar moves the fence, a border grip resizes it. Checked
    // before the icon grid so a fence gesture never disturbs the icon selection
    // -- which is also what keeps the two drags mutually exclusive.
    const FenceHandle handle =
        FenceGeometry::hitTest(rect(), event->pos(), kResizeBorder, titleBarHeight(), cornerRadius());
    if (handle != FenceHandle::None) {
        m_geometryHandle = handle;
        m_geometryStartScreen = event->globalPosition();
        m_geometryDragging = true;
        m_dragOriginIndex = -1;
        m_dragging = false;
        updateDragGhost(false);
        m_dropTargetIndex = -1;
        m_releaseArmed = false;
        emit geometryDragStarted();
        return;
    }

    if (index < 0) {
        if (!(mods & Qt::ControlModifier))
            m_grid.clearSelection();
    } else if (mods & Qt::ControlModifier) {
        m_grid.toggle(index);
    } else if (mods & Qt::ShiftModifier) {
        m_grid.selectRangeTo(index);
    } else if (!m_grid.isSelected(index)) {
        // Session 65: a press on an icon already selected keeps the selection,
        // so a multi-selection can be dragged; the release narrows it.
        m_grid.selectOnly(index);
    }
    emit selectionGesture();

    // Arm a possible drag (T-006-2). The move enters drag mode only once the
    // cursor leaves the system threshold -- a click in place never reorders.
    m_dragOriginIndex = (index >= 0 && index < m_grid.items().size()) ? index : -1;
    m_dragStartPos = event->pos();
    m_dropTargetIndex = -1;
    m_dragging = false;
    updateDragGhost(false);
    m_releaseArmed = false;

    update();
    emit contentChanged();
}

void FenceView::mouseMoveEvent(QMouseEvent* event)
{
    // Fence gesture (T-007) takes priority: while the title bar or a border
    // grip is held, the pointer moves the fence, never an icon.
    if (m_geometryDragging) {
        // Measured against the *screen*, not against this widget: the widget
        // moves with the fence, so a widget-local delta (`event->pos()`) shrinks
        // as the fence catches up with the cursor and the drag ends up covering
        // only about half the distance the cursor actually travelled, in lagging
        // steps. Screen coordinates are the one frame that does not move.
        emit geometryDragDelta(m_geometryHandle,
                               (event->globalPosition() - m_geometryStartScreen).toPoint());
        return;
    }

    if (m_dragOriginIndex < 0 || !(event->buttons() & Qt::LeftButton)) {
        // Idle: reflect the handle under the pointer so the user can see where
        // the fence can be grabbed.
        if (!(event->buttons() & Qt::LeftButton))
            setCursor(cursorForHandle(
                FenceGeometry::hitTest(rect(), event->pos(), kResizeBorder, titleBarHeight(), cornerRadius())));
        return;
    }

    if (!m_dragging) {
        // QApplication::startDragDistance() is the platform-consistent
        // threshold (Qt::Drag is too sensitive for an icon grid where the
        // cells are small and the cursor jitters on them).
        if (QApplication::startDragDistance() >=
            (event->pos() - m_dragStartPos).manhattanLength())
            return;
        // Session 65: the selection is what moves (the dragged icon always
        // belongs to it -- see the press).
        if (!m_grid.isSelected(m_dragOriginIndex))
            m_grid.selectOnly(m_dragOriginIndex);
        m_dragPaths = selectedPaths();
        m_dragging = true;
    }
    updateDragGhost(true);

    m_dropTargetIndex = itemAt(event->pos());
    // F-011: outside the fence there is no cell to drop onto, and the release
    // hands the tile back to the desktop instead of reordering the grid. The
    // hint has to follow the cursor both ways -- dragging back inside re-arms
    // the ordinary drop.
    const bool outside = !rect().contains(event->pos());
    if (outside != m_releaseArmed) {
        m_releaseArmed = outside;
        update();
    }
    emit itemsDraggedOutside(m_dragPaths, event->pos(), false);
}

void FenceView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_geometryDragging) {
        // The container already applied every delta as it arrived; the release
        // only closes the gesture (and lets it settle, e.g. commit the final
        // snapped rect to the layout in T-008).
        m_geometryDragging = false;
        m_geometryHandle = FenceHandle::None;
        emit geometryDragFinished();
        return;
    }

    if (m_dragging) {
        const int from = m_dragOriginIndex;
        // F-011: letting go outside the fence gives the tile back to the
        // desktop. Decided *before* the reorder, because it is a different
        // gesture with a different owner: the fence stops holding the icon and
        // the desktop layer draws it again, with no restore step in between.
        if (!rect().contains(event->pos()) && from >= 0 && from < m_grid.items().size()
            && !m_grid.items().at(from).path.isEmpty()) {
            const QStringList paths = m_dragPaths.isEmpty() ? QStringList{m_grid.items().at(from).path} : m_dragPaths;
            m_dragging = false;
            updateDragGhost(false);
            m_dragOriginIndex = -1;
            m_dragStartPos = QPoint();
            m_dropTargetIndex = -1;
            m_releaseArmed = false;
            m_dragPaths.clear();
            m_dragOverOtherFence = false;
            update();
            // Session 65: the container decides -- another fence under the
            // cursor takes them, otherwise the desktop does (F-011).
            if (isSignalConnected(QMetaMethod::fromSignal(&FenceView::itemsDraggedOutside)))
                emit itemsDraggedOutside(paths, event->pos(), true);
            else
                emit itemReleaseRequested(paths);
            return;
        }
        if (m_dragPaths.size() > 1) {
            // Session 65: a multi-selection moves as a block, in its order, to
            // where the cursor is.
            int to = itemAt(event->pos());
            QString anchor = to >= 0 ? m_grid.items().at(to).path : QString();
            QList<core::DesktopItem> moving;
            for (const core::DesktopItem& item : m_grid.items()) {
                if (m_dragPaths.contains(item.path))
                    moving.append(item);
            }
            if (!m_dragPaths.contains(anchor)) {
                m_grid.removeItemsByPath(m_dragPaths);
                int at = -1;
                for (int i = 0; i < m_grid.items().size() && !anchor.isEmpty(); ++i) {
                    if (m_grid.items().at(i).path == anchor)
                        at = i;
                }
                m_grid.insertItems(moving, at, false);
                m_grid.setManualOrder();
            }
            m_dragging = false;
            updateDragGhost(false);
            m_dragOriginIndex = -1;
            m_dragStartPos = QPoint();
            m_dropTargetIndex = -1;
            m_releaseArmed = false;
            m_dragPaths.clear();
            update();
            emit contentChanged();
            return;
        }
        // The drop target is the cell under the cursor; if the cursor is in
        // empty space (no cell), drop at the end of the grid.
        int to = itemAt(event->pos());
        if (to < 0)
            to = m_grid.items().size() - 1;
        if (to >= 0 && from >= 0 && from != to)
            m_grid.moveItem(from, to);
        m_dragPaths.clear();
        m_dragging = false;
        updateDragGhost(false);
        m_dragOriginIndex = -1;
        m_dragStartPos = QPoint();
        m_dropTargetIndex = -1;
        m_releaseArmed = false;
        update();
        emit contentChanged();
        return;
    }

    // A press that never crossed the drag threshold is just a click -- the
    // selection already happened in mousePressEvent; nothing else to do.
    m_dragOriginIndex = -1;
    m_dragStartPos = QPoint();
}

void FenceView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // F-020: a double-click on the title bar rolls the fence up or down. Checked
    // before the grid so a double-click on an icon still launches it.
    if (FenceGeometry::hitTest(rect(), event->pos(), kResizeBorder, titleBarHeight(), cornerRadius())
        == FenceHandle::TitleBar) {
        // Session 60: on the title's text, a double-click renames in place (as a
        // file's name in Explorer); anywhere else on the band it still rolls up.
        if (titleShown() && titleTextRect().adjusted(-4, -4, 4, 4).contains(event->pos())) {
            emit titleEditRequested();
            return;
        }
        emit rollUpRequested(!m_rolledUp);
        return;
    }

    const int index = itemAt(event->pos());
    if (index < 0 || index >= m_grid.items().size()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const QString target = IconGrid::launchPath(m_grid.items().at(index));
    if (target.startsWith(QLatin1String("::")))
        starfish::platform::win::openShellItem(target); // This PC, the Recycle Bin (ADR-21)
    else if (!target.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    emit itemActivated(index);
}

void FenceView::wheelEvent(QWheelEvent* event)
{
    // F-020: the wheel over the title bar rolls the fence up (away from the
    // user) or down (towards them). Only the title bar reacts, so scrolling over
    // the icon grid is never hijacked.
    if (FenceGeometry::hitTest(rect(), event->position().toPoint(), kResizeBorder, titleBarHeight(), cornerRadius())
        != FenceHandle::TitleBar) {
        QWidget::wheelEvent(event);
        return;
    }

    const int dy = event->angleDelta().y();
    if (dy == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    const bool wantRolledUp = dy > 0;
    if (wantRolledUp != m_rolledUp)
        emit rollUpRequested(wantRolledUp);
    event->accept();
}

void FenceView::addDroppedItems(const QList<core::DesktopItem>& items)
{
    // Session 60: a drop is an addition -- at the end, sorted only when the user
    // asked for it (the switch); appendItems() stays the portals' listing.
    insertItems(items, -1);
}

int FenceView::appendItems(const QList<core::DesktopItem>& items)
{
    const int added = m_grid.appendItems(items);
    if (added > 0) {
        update();
        emit contentChanged();
    }
    return added;
}

int FenceView::replaceItems(const QStringList& previousPaths, const QList<core::DesktopItem>& items)
{
    const int removed = m_grid.removeItemsByPath(previousPaths);
    const int added = m_grid.appendItems(items);
    if (removed > 0 || added > 0) {
        update();
        emit contentChanged();
    }
    return added;
}

int FenceView::removeItemsByPath(const QStringList& paths)
{
    // F-011: the tile is not hidden, moved or deleted -- the fence simply stops
    // listing it, and the desktop backdrop draws it again because it is no
    // longer in the occupied set (T-039). One place holds the fact, and it is
    // the fence's item list.
    const int removed = m_grid.removeItemsByPath(paths);
    if (removed > 0) {
        update();
        emit contentChanged();
    }
    return removed;
}

void FenceView::dragEnterEvent(QDragEnterEvent* event)
{
    // Qt translates the Explorer's CF_HDROP into a list of file:// URLs.
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void FenceView::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void FenceView::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    QList<core::DesktopItem> items;
#if defined(_WIN32)
    // Resolve each dropped path the same way the desktop scan does, so the
    // tile (display name, shortcut target, hidden flag) matches the Shell.
    items.reserve(event->mimeData()->urls().size());
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        if (local.isEmpty())
            continue;
        const core::DesktopItem item =
            starfish::platform::win::DesktopScanner::itemForPath(local);
        if (!item.path.isEmpty())
            items.append(item);
    }
#else
    // Non-Windows: keep the path so the model still gains a tile; name falls
    // back to the file name through itemForPath's equivalent on that platform.
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        if (local.isEmpty())
            continue;
        core::DesktopItem item;
        item.path = local;
        item.fileName = QFileInfo(local).fileName();
        item.name = item.fileName;
        items.append(item);
    }
#endif

    if (!items.isEmpty())
        insertItems(items, itemAt(event->position().toPoint())); // where it was let go
    event->acceptProposedAction();
}

void FenceView::paintDropIndicator(int index, QPainter& painter)
{
    // `size()` is the free cell after the last one (an append).
    if (index < 0 || index > m_grid.items().size())
        return;

    // Recompute the layout in content-local coordinates (matches paint()).
    const QRect content = contentRect();
    const QVector<ui::IconCell> cells = ui::IconLayout::compute(
        content.size(), m_grid.iconSize(), m_grid.cellWidth(), m_grid.labelHeight(), m_grid.gap(),
        m_grid.padding(), m_grid.items().size() + 1);
    if (index >= cells.size())
        return;

    const QRect iconRect = cells.at(index).icon.translated(content.topLeft());
    const QRect labelRect = cells.at(index).label.translated(content.topLeft());
    const QRect combined = iconRect.united(labelRect).adjusted(-4, -4, 4, 4);

    const QColor accent(0x3B, 0x82, 0xF6);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(accent, 2.0));
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 60));
    painter.drawRoundedRect(combined, 6, 6);
    painter.restore();
}

void FenceView::paintReleaseHint(QPainter& painter)
{
    // F-011: what letting go *now* would do. The cursor is outside the fence,
    // so the tile is about to go back to the desktop. Drawn inside the fence
    // because the fence is all we own -- and a dashed accent border is
    // deliberately *not* the drop indicator: one means "the grid takes it",
    // the other means "the desktop takes it".
    const QColor accent(0x3B, 0x82, 0xF6);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(accent, 2.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0), m_cornerRadius,
                            m_cornerRadius);
    const QRect content = contentRect();
    if (!content.isEmpty()) {
        painter.setPen(accent);
        painter.drawText(content, Qt::AlignCenter,
                         tr("release to put it back on the desktop"));
    }
    painter.restore();
}

void FenceView::showContextMenuForIndex(int index, const QPoint& globalPos)
{
#if defined(_WIN32)
    if (index < 0 || index >= m_grid.items().size())
        return;
    // Prefer the resolved target for the menu (so "Open with", "Run as
    // administrator" and friends act on what the user actually meant to
    // launch); fall back to the desktop path for plain files / folders.
    const core::DesktopItem& item = m_grid.items().at(index);
    const QString path = !item.targetPath.isEmpty() ? item.targetPath : item.path;
    if (path.isEmpty())
        return;
    starfish::platform::win::ShellContextMenu::showForPath(
        path, globalPos, reinterpret_cast<void*>(::GetForegroundWindow()));
#else
    Q_UNUSED(index);
    Q_UNUSED(globalPos);
#endif
}

} // namespace starfish::ui

namespace starfish::ui {

QImage FenceView::dragImageFor(int index, const QPoint& press, qreal dpr, QPoint* hotspot) const
{
    if (index < 0 || index >= m_grid.items().size())
        return {};
    const QRect content = contentRect();
    const QVector<IconCell> cells = m_grid.cells(content.size());
    if (index >= cells.size())
        return {};
    DragTile tile;
    tile.icon = cells.at(index).icon.translated(content.topLeft());
    tile.label = cells.at(index).label.translated(content.topLeft());
    const core::DesktopItem& item = m_grid.items().at(index);
    tile.pixmap = m_iconProvider ? m_iconProvider(item, qRound(m_grid.iconSize() * dpr)) : QPixmap();
    tile.text = item.name;
    QPoint topLeft;
    const QImage image = DragImage::compose({tile}, m_grid.captionFont(), DragImage::kOpacity, dpr, &topLeft);
    if (hotspot)
        *hotspot = press - topLeft;
    return image;
}

bool FenceView::dragGhostVisible() const
{
    return m_ghost && m_ghost->isVisible();
}

void FenceView::updateDragGhost(bool dragging)
{
    if (!dragging) {
        if (m_ghost) {
            m_ghost->hide();
            m_ghost->deleteLater();
            m_ghost = nullptr;
        }
        return;
    }
    if (!m_ghost) {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
        QPoint hotspot;
        const QImage image = dragImageFor(m_dragOriginIndex, m_dragStartPos, dpr, &hotspot);
        if (image.isNull())
            return;
        m_ghost = new DragGhost;
        m_ghost->setImage(image, hotspot);
        connect(this, &QObject::destroyed, m_ghost, &QObject::deleteLater);
    }
    m_ghost->follow(QCursor::pos());
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setTextShadow(bool on)
{
    if (m_grid.textShadow() == on)
        return;
    m_grid.setTextShadow(on);
    update();
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::clearSelection()
{
    if (m_grid.selectedIndices().isEmpty())
        return;
    m_grid.clearSelection();
    update();
    emit contentChanged();
}

QStringList FenceView::selectedPaths() const
{
    QList<int> indices = m_grid.selectedIndices().values();
    std::sort(indices.begin(), indices.end());
    QStringList paths;
    for (int i : indices) {
        if (i >= 0 && i < m_grid.items().size() && !m_grid.items().at(i).path.isEmpty())
            paths.append(m_grid.items().at(i).path);
    }
    return paths;
}

void FenceView::setExternalDropIndex(int index)
{
    if (m_externalDropIndex == index)
        return;
    m_externalDropIndex = index;
    update();
    emit contentChanged();
}

void FenceView::setDragOverOtherFence(bool over)
{
    if (m_dragOverOtherFence == over)
        return;
    m_dragOverOtherFence = over;
    update();
    emit contentChanged();
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setBorder(int width, const QColor& color)
{
    if (m_borderWidth == width && m_borderColor == color)
        return;
    m_borderWidth = qMax(0, width);
    m_borderColor = color;
    update();
}

void FenceView::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
    m_hovered = true;
    if (m_hoverHighlight || !titleShown()) {
        update();
        emit contentChanged();
    }
}

void FenceView::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    m_hovered = false;
    if (m_hoverHighlight || !titleShown()) {
        update();
        emit contentChanged();
    }
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setFixedCellWidth(int px)
{
    if (m_grid.fixedCellWidth() == px)
        return;
    m_grid.setFixedCellWidth(px);
    update();
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setTitleBarShown(bool shown)
{
    if (m_titleBarShown == shown)
        return;
    m_titleBarShown = shown;
    update();
    emit contentChanged();
}

} // namespace starfish::ui
