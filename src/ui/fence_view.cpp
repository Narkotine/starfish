#include "ui/fence_view.hpp"

#include <QApplication>
#include <QColor>
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

#if defined(_WIN32)
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/shell_context_menu.hpp"
#include <windows.h>
#endif

namespace starfish::ui {

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

QRect FenceView::contentRect() const
{
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

    const QColor panel(0x1F, 0x29, 0x37);
    const QColor panelBorder(0x3B, 0x82, 0xF6);
    const QColor titleText(0xE5, 0xE7, 0xEB);
    const QColor hintText(0x9C, 0xA3, 0xAF);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setPen(QPen(panelBorder, 1.0));
    painter.setBrush(panel);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            kCornerRadius, kCornerRadius);

    QFont titleFont = font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(titleText);
    painter.drawText(QRect(12, 0, qMax(0, width() - 24), kTitleBarHeight),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     m_title.isEmpty() ? QStringLiteral("Fence") : m_title);

    const QRect content = contentRect();
    if (m_grid.items().isEmpty()) {
        painter.setFont(font());
        painter.setPen(hintText);
        painter.drawText(content, Qt::AlignCenter,
                         QStringLiteral("empty fence -- no icons yet"));
        return;
    }

    m_grid.paint(painter, content, m_iconProvider);
    if (m_dragging)
        paintDropIndicator(m_dropTargetIndex, painter);
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

    update();
    emit contentChanged();
}

void FenceView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragOriginIndex < 0 || !(event->buttons() & Qt::LeftButton))
        return;

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
    update();
}

void FenceView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_dragging) {
        const int from = m_dragOriginIndex;
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

void FenceView::addDroppedItems(const QList<core::DesktopItem>& items)
{
    if (m_grid.appendItems(items) > 0) {
        update();
        emit contentChanged();
    }
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
