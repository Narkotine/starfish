#pragma once

#include <QtMath>
#include <cmath>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

namespace starfish::ui::zone_shape {

// Which presentation a fence uses (FenceStyle::shape). Empty = classic.
inline bool isButton(const QString& shape)
{
    return shape == QLatin1String("button");
}

inline bool isClassic(const QString& shape)
{
    return shape.isEmpty() || (!isButton(shape) && shape != QLatin1String("petal"));
}

// --- Button (circular zone) -------------------------------------------------

// Collapsed diameter: about twice an icon (padding for the ring chrome).
inline int buttonCollapsedDiameter(int iconSize)
{
    return qMax(48, iconSize * 2 + 8);
}

// Expanded diameter for `count` icons on a ring (phyllotaxis-friendly).
inline int buttonExpandedDiameter(int iconSize, int count)
{
    const int n = qMax(1, count);
    // Chord between neighbours ≈ icon + gap; radius from regular polygon.
    const double chord = iconSize + 16.0;
    const double r = n <= 1 ? iconSize * 1.2 : (chord / (2.0 * qSin(3.14159265358979323846 / n))) + iconSize * 0.55;
    return qMax(buttonCollapsedDiameter(iconSize) + 48, int(qCeil(r * 2.0 + 24)));
}

// Visible square while rolled up: a circle inscribed, centred in `stored`.
inline QRect buttonCollapsedRect(const QRect& stored, int iconSize)
{
    const int d = qMin(buttonCollapsedDiameter(iconSize), qMin(stored.width(), stored.height()));
    return QRect(stored.center().x() - d / 2, stored.center().y() - d / 2, d, d);
}

// Progress 0 = collapsed, 1 = expanded. Ease-out back (slight overshoot).
inline double buttonExpandEase(double t)
{
    t = qBound(0.0, t, 1.0);
    constexpr double c1 = 1.70158;
    constexpr double c3 = c1 + 1.0;
    return 1.0 + c3 * qPow(t - 1.0, 3.0) + c1 * qPow(t - 1.0, 2.0);
}

inline QRect buttonVisibleRect(const QRect& stored, int iconSize, double progress)
{
    const QRect small = buttonCollapsedRect(stored, iconSize);
    const double u = buttonExpandEase(progress);
    // Overshoot may grow a few px past stored; clamp to stored afterwards for the window.
    const QPoint c = stored.center();
    const int w = int(qRound(small.width() + (stored.width() - small.width()) * u));
    const int h = int(qRound(small.height() + (stored.height() - small.height()) * u));
    QRect r(c.x() - w / 2, c.y() - h / 2, w, h);
    return r.intersected(stored).isEmpty() ? r : QRect(c.x() - w / 2, c.y() - h / 2, w, h);
}

// Icon top-lefts inside an expanded circle (ring; one item at centre if alone).
inline QVector<QRect> buttonItemRects(const QRect& content, int count, int iconSize)
{
    QVector<QRect> out;
    if (count <= 0 || content.isEmpty())
        return out;
    out.reserve(count);
    const QPoint c = content.center();
    if (count == 1) {
        out.append(QRect(c.x() - iconSize / 2, c.y() - iconSize / 2, iconSize, iconSize));
        return out;
    }
    // Phyllotaxis-ish ring: golden angle on a circle that fits inside content.
    const double radius = qMin(content.width(), content.height()) * 0.5 - iconSize * 0.65;
    constexpr double golden = 2.399963229728653; // π * (3 - √5)
    for (int i = 0; i < count; ++i) {
        const double a = i * golden;
        const int x = int(qRound(c.x() + radius * qCos(a) - iconSize / 2.0));
        const int y = int(qRound(c.y() + radius * qSin(a) - iconSize / 2.0));
        out.append(QRect(x, y, iconSize, iconSize));
    }
    return out;
}

} // namespace starfish::ui::zone_shape
