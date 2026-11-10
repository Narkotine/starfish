#pragma once

#include <QtMath>
#include <cmath>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <algorithm>
#include <vector>

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

// Session 104: icon + label cell. Labels are reserved even if hidden, so
// boxes never overlap once names are shown.
inline constexpr int kButtonLabelHeight = 34;
inline constexpr int kButtonItemMargin = 8;
inline constexpr int kButtonCirclePad = 10;

inline int buttonCellWidth(int iconSize)
{
    return iconSize + iconSize / 2;
}

inline int buttonCellHeight(int iconSize, int labelHeight)
{
    return iconSize + qMax(0, labelHeight);
}

inline double buttonMinDistance(int iconSize, int labelHeight)
{
    return std::hypot(double(buttonCellWidth(iconSize)),
                      double(buttonCellHeight(iconSize, labelHeight)))
        + kButtonItemMargin;
}

struct ButtonItemGeom {
    QRect icon;
    QRect label;
    QRect bounds() const { return icon.united(label); }
};

struct ButtonLayout {
    QVector<ButtonItemGeom> items;
    int diameter = 0;
};

// Radius so `m` evenly spaced points have chord >= d. m<=1 sits at the centre.
inline double buttonRingRadius(int m, double minDist)
{
    if (m <= 1)
        return 0.0;
    return minDist / (2.0 * std::sin(3.14159265358979323846 / m));
}

inline std::vector<int> buttonSplitCounts(int n, int layers)
{
    std::vector<int> c(layers, n / layers);
    int rem = n % layers;
    for (int i = layers - 1; rem > 0; --i, --rem)
        ++c[i];
    return c;
}

// Angle 0 = top, then clockwise. Cell centres sit on the ring (icon above, label below).
inline ButtonLayout buttonLayout(int count, int iconSize, int labelHeight = kButtonLabelHeight)
{
    ButtonLayout out;
    const int n = qMax(0, count);
    const int cellW = buttonCellWidth(iconSize);
    const int cellH = buttonCellHeight(iconSize, labelHeight);
    const double d = buttonMinDistance(iconSize, labelHeight);

    auto placeAt = [&](double cx, double cy, double x, double y, ButtonItemGeom& g) {
        const int left = int(qRound(cx + x - cellW / 2.0));
        const int top = int(qRound(cy + y - cellH / 2.0));
        g.label = QRect(left, top + iconSize, cellW, qMax(0, labelHeight));
        g.icon = QRect(left + (cellW - iconSize) / 2, top, iconSize, iconSize);
    };

    auto enclosing = [&](const QVector<ButtonItemGeom>& items, double cx, double cy) {
        double r = 0.0;
        for (const ButtonItemGeom& g : items) {
            const QRect b = g.bounds();
            for (const QPoint& p : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()})
                r = qMax(r, std::hypot(p.x() - cx, p.y() - cy));
        }
        return r + kButtonCirclePad;
    };

    auto boxesOverlap = [](const QRect& a, const QRect& b) {
        return a.intersects(b);
    };

    if (n <= 0) {
        out.diameter = qMax(buttonCollapsedDiameter(iconSize) + 48, cellW + 2 * kButtonCirclePad);
        return out;
    }

    if (n == 1) {
        ButtonItemGeom g;
        // Temporary origin; recentred below.
        placeAt(0, 0, 0, 0, g);
        out.items = {g};
    } else {
        // Prefer the fewest layers that keep a compact circle; grow the outer
        // radius as needed so neighbour distance >= icon-cell + margin.
        const int maxLayers = qMax(1, n / 2);
        int bestK = 1;
        std::vector<int> bestCounts = {n};
        std::vector<double> bestRadii = {buttonRingRadius(n, d)};
        double bestOuter = bestRadii[0];
        for (int k = 1; k <= maxLayers; ++k) {
            const std::vector<int> counts = buttonSplitCounts(n, k);
            bool ok = true;
            for (int i = 0; i < k; ++i) {
                // A 1-item "ring" is only the centre (inner layer). Other rings need >= 2.
                if (counts[i] <= 0 || (counts[i] == 1 && i != 0) || (i > 0 && counts[i] < 2)) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
            std::vector<double> radii(k);
            radii[0] = buttonRingRadius(counts[0], d);
            for (int i = 1; i < k; ++i)
                radii[i] = qMax(buttonRingRadius(counts[i], d), radii[i - 1] + d);
            const double outer = radii.back();
            if (k == 1 || outer + 0.5 < bestOuter || (qAbs(outer - bestOuter) < 0.5 && k < bestK)) {
                bestK = k;
                bestCounts = counts;
                bestRadii = radii;
                bestOuter = outer;
            }
        }

        out.items.reserve(n);
        for (int layer = 0; layer < bestK; ++layer) {
            const int m = bestCounts[layer];
            const double r = bestRadii[layer];
            for (int i = 0; i < m; ++i) {
                const double theta = (m == 0) ? 0.0 : (2.0 * 3.14159265358979323846 * i / m);
                // 0 = top, clockwise.
                const double x = r * std::sin(theta);
                const double y = -r * std::cos(theta);
                ButtonItemGeom g;
                placeAt(0, 0, x, y, g);
                out.items.append(g);
            }
        }
    }

    // Grow uniformly if any axis-aligned boxes still overlap (should be rare).
    for (int pass = 0; pass < 8; ++pass) {
        bool hit = false;
        for (int i = 0; i < out.items.size() && !hit; ++i) {
            for (int j = i + 1; j < out.items.size(); ++j) {
                if (boxesOverlap(out.items[i].bounds(), out.items[j].bounds())) {
                    hit = true;
                    break;
                }
            }
        }
        if (!hit)
            break;
        for (ButtonItemGeom& g : out.items) {
            g.icon.translate(int(qRound(g.icon.center().x() * 0.08)), int(qRound(g.icon.center().y() * 0.08)));
            g.label.translate(int(qRound((g.label.center().x()) * 0.08)), int(qRound(g.label.center().y() * 0.08)));
        }
    }

    const double R = enclosing(out.items, 0.0, 0.0);
    const int diameter = qMax(buttonCollapsedDiameter(iconSize) + 48,
                              int(qCeil(R * 2.0)));
    // Recentre into a square (0,0,diameter,diameter).
    const double cx = diameter / 2.0;
    const double cy = diameter / 2.0;
    for (ButtonItemGeom& g : out.items) {
        g.icon.translate(int(qRound(cx)), int(qRound(cy)));
        g.label.translate(int(qRound(cx)), int(qRound(cy)));
    }
    out.diameter = diameter;
    return out;
}

inline int buttonExpandedDiameter(int iconSize, int count, int labelHeight = kButtonLabelHeight)
{
    return buttonLayout(count, iconSize, labelHeight).diameter;
}

// Visible square while rolled up: a circle inscribed, centred in `stored`.
inline QRect buttonCollapsedRect(const QRect& stored, int iconSize)
{
    const int d = qMin(buttonCollapsedDiameter(iconSize), qMin(stored.width(), stored.height()));
    return QRect(stored.center().x() - d / 2, stored.center().y() - d / 2, d, d);
}

// A square of `diameter` centred on `center` (the Bouton's stored anchor).
inline QRect buttonSquareAt(QPoint center, int diameter)
{
    const int d = qMax(1, diameter);
    return QRect(center.x() - d / 2, center.y() - d / 2, d, d);
}

// Translate a square so it lies inside `clamp`, keeping width == height.
// Only moves as far as needed; if the square is larger than the clamp, it is
// pinned to the clamp's top-left. The stored anchor is not this rect -- this
// is only the circle shown while expanded near a screen edge.
inline QRect buttonFitSquareOnScreen(const QRect& square, const QRect& clamp)
{
    if (!square.isValid() || !clamp.isValid())
        return square;
    const int d = qMax(1, qMin(square.width(), square.height()));
    QRect r(square.center().x() - d / 2, square.center().y() - d / 2, d, d);
    if (r.left() < clamp.left())
        r.moveLeft(clamp.left());
    if (r.top() < clamp.top())
        r.moveTop(clamp.top());
    if (r.right() > clamp.right())
        r.moveRight(clamp.right());
    if (r.bottom() > clamp.bottom())
        r.moveBottom(clamp.bottom());
    return r;
}

inline bool buttonContainsPoint(const QRect& square, const QPoint& point)
{
    const QPoint c = square.center();
    const qint64 radius = qMin(square.width(), square.height()) / 2;
    if (radius <= 0)
        return false;
    const qint64 dx = point.x() - c.x();
    const qint64 dy = point.y() - c.y();
    return dx * dx + dy * dy <= radius * radius;
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
    const QPoint c = stored.center();
    const int side = qMax(1, int(qRound(small.width() + (stored.width() - small.width()) * u)));
    return QRect(c.x() - side / 2, c.y() - side / 2, side, side);
}

inline QVector<ButtonItemGeom> buttonItemGeometry(const QRect& content, int count, int iconSize,
                                                 int labelHeight = kButtonLabelHeight)
{
    ButtonLayout lay = buttonLayout(count, iconSize, labelHeight);
    if (lay.items.isEmpty() || content.isEmpty())
        return lay.items;
    const QPoint c = content.center();
    const QPoint o(lay.diameter / 2, lay.diameter / 2);
    const int room = qMin(content.width(), content.height());
    double scale = 1.0;
    if (lay.diameter > 0 && room > lay.diameter)
        scale = double(room) / lay.diameter;
    QVector<ButtonItemGeom> out;
    out.reserve(lay.items.size());
    for (const ButtonItemGeom& g : lay.items) {
        ButtonItemGeom h;
        auto mapRect = [&](QRect r) {
            const double x = c.x() + (r.x() - o.x()) * scale;
            const double y = c.y() + (r.y() - o.y()) * scale;
            return QRect(int(qRound(x)), int(qRound(y)),
                         qMax(1, int(qRound(r.width() * scale))),
                         qMax(1, int(qRound(r.height() * scale))));
        };
        h.icon = mapRect(g.icon);
        h.label = mapRect(g.label);
        out.append(h);
    }
    return out;
}

// Icon top-lefts inside an expanded circle (session 104: concentric rings, 0 = top).
inline QVector<QRect> buttonItemRects(const QRect& content, int count, int iconSize,
                                     int labelHeight = kButtonLabelHeight)
{
    QVector<QRect> rects;
    const auto items = buttonItemGeometry(content, count, iconSize, labelHeight);
    rects.reserve(items.size());
    for (const ButtonItemGeom& g : items)
        rects.append(g.icon);
    return rects;
}

} // namespace starfish::ui::zone_shape
