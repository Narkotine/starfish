#include "ui/fence_view.hpp"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QUrl>

#if defined(_WIN32)
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
    update();
    emit contentChanged();
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
