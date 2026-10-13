#include "ui/fence_view.hpp"

#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRect>

namespace starfish::ui {

namespace {
constexpr int kTitleBarHeight = 30;
constexpr int kCornerRadius = 10;
} // namespace

FenceView::FenceView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("starfishFenceView"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
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

    painter.setFont(font());
    painter.setPen(hintText);
    painter.drawText(QRect(0, kTitleBarHeight, width(), qMax(0, height() - kTitleBarHeight)),
                     Qt::AlignCenter,
                     QStringLiteral("empty fence -- no icons yet (T-005)"));
}

} // namespace starfish::ui
