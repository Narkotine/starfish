#include "ui/fence_view.hpp"

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
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
    m_grid.setGap(8);
    m_grid.setPadding(12);
    m_grid.setLabelHeight(34);

    // The fence accepts files dropped from the Explorer (T-006-3). This is the
    // path used when the fence is a real Qt window; the desktop-hosted surface
    // takes the OLE IDropTarget path in DesktopSurface instead.
    setAcceptDrops(true);
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
    // Alpha is dropped: this widget paints the opaque panel; translucency is a
    // property of the hosted surface (T-017), not of the QImage that feeds it.
    m_panelColor = QColor(color.red(), color.green(), color.blue());
    update();
    emit contentChanged();
}

void FenceView::setCornerRadius(int px)
{
    m_cornerRadius = qBound(0, px, 64);
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
    return rect().adjusted(kPadding, kTitleBarHeight + kPadding, -kPadding, -kPadding);
}

int FenceView::itemAt(const QPoint& localPoint) const
{
    if (!contentRect().contains(localPoint))
        return -1;
    return m_grid.hitTest(localPoint - contentRect().topLeft(), contentRect().size());
}

void FenceView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QColor panelBorder(0x3B, 0x82, 0xF6);
    const QColor titleText(0xE5, 0xE7, 0xEB);
    const QColor hintText(0x9C, 0xA3, 0xAF);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setPen(QPen(panelBorder, 1.0));
    painter.setBrush(m_panelColor);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            m_cornerRadius, m_cornerRadius);

    QFont titleFont = font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(titleText);
    painter.drawText(QRect(12, 0, qMax(0, width() - 24), kTitleBarHeight),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     m_title.isEmpty() ? QStringLiteral("Fence") : m_title);

    const QRect content = contentRect();
    // Rolled up (F-020): the widget is the title bar, nothing else is drawn.
    if (m_rolledUp || content.isEmpty())
        return;

    if (m_grid.items().isEmpty()) {
        painter.setFont(font());
        painter.setPen(hintText);
        painter.drawText(content, Qt::AlignCenter,
                         QStringLiteral("empty fence -- no icons yet"));
        return;
    }

    m_grid.paint(painter, content, m_iconProvider);
    if (m_dragging) {
        // Either the grid is about to receive the tile, or the desktop is:
        // never both, so the two hints are mutually exclusive by construction.
        if (m_releaseArmed)
            paintReleaseHint(painter);
        else
            paintDropIndicator(m_dropTargetIndex, painter);
    }
}

void FenceView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        // Right-click selects the item under the cursor and shows the Windows
        // shell context menu for it. The single path (mousePressEvent) covers
        // both a real Qt window and a desktop-hosted surface that posts
        // QMouseEvent(RightButton) from its own WndProc: a QContextMenuEvent
        // would only fire from the platform, not from a posted event.
        const int index = itemAt(event->pos());
        if (index >= 0)
            m_grid.selectOnly(index);
        update();
        emit contentChanged();
        showContextMenuForIndex(index, event->globalPosition().toPoint());
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
        FenceGeometry::hitTest(rect(), event->pos(), kResizeBorder, kTitleBarHeight);
    if (handle != FenceHandle::None) {
        m_geometryHandle = handle;
        m_geometryStartScreen = event->globalPosition();
        m_geometryDragging = true;
        m_dragOriginIndex = -1;
        m_dragging = false;
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
    } else {
        m_grid.selectOnly(index);
    }

    // Arm a possible drag (T-006-2). The move enters drag mode only once the
    // cursor leaves the system threshold -- a click in place never reorders.
    m_dragOriginIndex = (index >= 0 && index < m_grid.items().size()) ? index : -1;
    m_dragStartPos = event->pos();
    m_dropTargetIndex = -1;
    m_dragging = false;
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
                FenceGeometry::hitTest(rect(), event->pos(), kResizeBorder, kTitleBarHeight)));
        return;
    }

    if (!m_dragging) {
        // QApplication::startDragDistance() is the platform-consistent
        // threshold (Qt::Drag is too sensitive for an icon grid where the
        // cells are small and the cursor jitters on them).
        if (QApplication::startDragDistance() >=
            (event->pos() - m_dragStartPos).manhattanLength())
            return;
        // Lock the selection on the dragged item so the visual highlight
        // matches what's being moved, not whatever the user had selected
        // before (e.g. a Ctrl-click selection on another cell).
        m_grid.selectOnly(m_dragOriginIndex);
        m_dragging = true;
    }

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
            const QString path = m_grid.items().at(from).path;
            m_dragging = false;
            m_dragOriginIndex = -1;
            m_dragStartPos = QPoint();
            m_dropTargetIndex = -1;
            m_releaseArmed = false;
            update();
            emit itemReleaseRequested({path});
            return;
        }
        // The drop target is the cell under the cursor; if the cursor is in
        // empty space (no cell), drop at the end of the grid.
        int to = itemAt(event->pos());
        if (to < 0)
            to = m_grid.items().size() - 1;
        if (to >= 0 && from >= 0 && from != to)
            m_grid.moveItem(from, to);
        m_dragging = false;
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
    if (FenceGeometry::hitTest(rect(), event->pos(), kResizeBorder, kTitleBarHeight)
        == FenceHandle::TitleBar) {
        emit rollUpRequested(!m_rolledUp);
        return;
    }

    const int index = itemAt(event->pos());
    if (index < 0 || index >= m_grid.items().size()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const QString target = IconGrid::launchPath(m_grid.items().at(index));
    if (!target.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    emit itemActivated(index);
}

void FenceView::wheelEvent(QWheelEvent* event)
{
    // F-020: the wheel over the title bar rolls the fence up (away from the
    // user) or down (towards them). Only the title bar reacts, so scrolling over
    // the icon grid is never hijacked.
    if (FenceGeometry::hitTest(rect(), event->position().toPoint(), kResizeBorder, kTitleBarHeight)
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
    appendItems(items);
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
        addDroppedItems(items);
    event->acceptProposedAction();
}

void FenceView::paintDropIndicator(int index, QPainter& painter)
{
    if (index < 0 || index >= m_grid.items().size())
        return;

    // Recompute the layout in content-local coordinates (matches paint()).
    const QRect content = contentRect();
    const QVector<ui::IconCell> cells = ui::IconLayout::compute(
        content.size(), m_grid.iconSize(), m_grid.labelHeight(), m_grid.gap(),
        m_grid.padding(), m_grid.items().size());
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
                         QStringLiteral("release to put it back on the desktop"));
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
