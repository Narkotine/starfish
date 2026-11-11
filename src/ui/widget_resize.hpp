#pragma once

// Session 98 (the user's ask): a widget is resized by hand only in a short
// "resize mode" opened from its menu -- an accent outline shows its exact
// bounds and the edges it can be pulled by; the mode ends 10 s after the last
// touch, on a click elsewhere or on Esc. Outside the mode nothing resizes.
// The pure parts shared by a fence's widgets and the desktop's.

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include "core/desktop_item.hpp"

namespace starfish::ui::widget_resize {

inline constexpr int kIdleMs = 10000;   // the mode ends this long after the last touch
inline constexpr int kMaxColumns = 8;   // the settings window's bounds (session 96)
inline constexpr int kMaxRows = 6;

enum Edge { None = 0, Right = 1, Bottom = 2, Corner = Right | Bottom };

// Which edges a point grabs: the right edge, the bottom edge, or the corner
// (within `margin` of them, on the outline's inside or outside).
inline int edgesAt(const QRect& r, const QPoint& p, int margin)
{
    if (!r.adjusted(-margin, -margin, margin, margin).contains(p))
        return None;
    int edges = None;
    if (qAbs(p.x() - r.right()) <= margin)
        edges |= Right;
    if (qAbs(p.y() - r.bottom()) <= margin)
        edges |= Bottom;
    return edges;
}

// The span after pulling `edges` by `delta`: whole cells of `pitch` (the
// distance from one cell to the next), at least 1 x 1, at most 8 x 6.
inline QSize spanAfter(const QSize& start, const QSize& pitch, const QPoint& delta, int edges)
{
    QSize span = start;
    if ((edges & Right) && pitch.width() > 0)
        span.setWidth(start.width() + qRound(double(delta.x()) / pitch.width()));
    if ((edges & Bottom) && pitch.height() > 0)
        span.setHeight(start.height() + qRound(double(delta.y()) / pitch.height()));
    return QSize(qBound(1, span.width(), kMaxColumns), qBound(1, span.height(), kMaxRows));
}

// Session 100: a widget's lock (its size and its place), in its settings so
// the layout keeps it, in a fence as on the desktop.
inline bool isLocked(const core::DesktopItem& widget)
{
    return widget.widgetSettings.value(QStringLiteral("locked")).toBool();
}

// Session 100: an Image widget on the desktop is sized to the pixel, freely
// (its own width and height, physical pixels); the others keep whole cells.
constexpr int kMinFreePx = 32;
inline bool resizesFreely(const core::DesktopItem& widget)
{
    return widget.widgetType == QLatin1String("image");
}
inline QSize freeSize(const core::DesktopItem& widget)
{
    const QSize size(widget.widgetSettings.value(QStringLiteral("pixelWidth")).toInt(),
                     widget.widgetSettings.value(QStringLiteral("pixelHeight")).toInt());
    return size.width() > 0 && size.height() > 0 ? size : QSize();
}
// The size after pulling `edges` by `delta`, to the pixel: at least
// kMinFreePx, at most `max` (what is left of the desktop from its corner).
inline QSize freeSizeAfter(const QSize& start, const QPoint& delta, int edges, const QSize& max)
{
    int w = start.width() + ((edges & Right) ? delta.x() : 0);
    int h = start.height() + ((edges & Bottom) ? delta.y() : 0);
    w = qBound(kMinFreePx, w, qMax(kMinFreePx, max.width()));
    h = qBound(kMinFreePx, h, qMax(kMinFreePx, max.height()));
    return QSize(w, h);
}

// The outline in the accent colour, with a grip on the right edge, the bottom
// edge and the corner.
inline void paintOutline(QPainter& painter, const QRect& r, const QColor& accent, qreal scale)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = qMax(2.0, 2.0 * scale);
    painter.setPen(QPen(accent, w));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(r).adjusted(w / 2, w / 2, -w / 2, -w / 2));
    const int g = qMax(6, qRound(8 * scale));
    painter.setPen(QPen(QColor(255, 255, 255), qMax(1.0, scale)));
    painter.setBrush(accent);
    const QPoint grips[] = {QPoint(r.right(), r.center().y()), QPoint(r.center().x(), r.bottom()),
                            QPoint(r.right(), r.bottom())};
    for (const QPoint& c : grips)
        painter.drawRect(QRect(c.x() - g / 2, c.y() - g / 2, g, g));
    painter.restore();
}

// Session 107: the same accent ring, on a circle (a Bouton's move mode).
inline void paintCircleOutline(QPainter& painter, const QRect& r, const QColor& accent, qreal scale)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = qMax(2.0, 2.0 * scale);
    painter.setPen(QPen(accent, w));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(r).adjusted(w / 2, w / 2, -w / 2, -w / 2));
    painter.restore();
}

} // namespace starfish::ui::widget_resize
