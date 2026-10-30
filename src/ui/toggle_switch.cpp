#include "ui/toggle_switch.hpp"

#include <QPainter>

#include "ui/modern_menu.hpp"

namespace starfish::ui {

namespace {
constexpr int kTrackW = 40;
constexpr int kTrackH = 20;
constexpr int kGap = 10;
} // namespace

ToggleSwitch::ToggleSwitch(const QString& text, QWidget* parent)
    : QCheckBox(text, parent)
{
    setCursor(Qt::PointingHandCursor);
}

QSize ToggleSwitch::sizeHint() const
{
    const int textW = text().isEmpty() ? 0 : kGap + fontMetrics().horizontalAdvance(text());
    return QSize(kTrackW + textW + 2, qMax(kTrackH + 4, fontMetrics().height() + 4));
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    const ModernMenuStyle style = ModernMenuStyle::current();
    const QColor accent = accentColor();
    const QColor dim = style.dark ? QColor(0xC5, 0xC5, 0xC5) : QColor(0x5C, 0x5C, 0x5C);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track(1, (height() - kTrackH) / 2.0, kTrackW, kTrackH);
    const qreal knob = underMouse() ? 14 : 12;
    if (isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(isEnabled() ? accent : dim);
        p.drawRoundedRect(track, kTrackH / 2.0, kTrackH / 2.0);
        p.setBrush(accent.lightness() > 150 ? QColor(Qt::black) : QColor(Qt::white));
        p.drawEllipse(QPointF(track.right() - kTrackH / 2.0, track.center().y()), knob / 2, knob / 2);
    } else {
        p.setPen(QPen(dim, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), kTrackH / 2.0 - 0.5, kTrackH / 2.0 - 0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(dim);
        p.drawEllipse(QPointF(track.left() + kTrackH / 2.0, track.center().y()), knob / 2, knob / 2);
    }
    if (!text().isEmpty()) {
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRectF(track.right() + kGap, 0, width() - track.right() - kGap, height()),
                   Qt::AlignVCenter | Qt::AlignLeft, text());
    }
}

} // namespace starfish::ui
