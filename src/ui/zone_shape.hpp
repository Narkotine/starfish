#pragma once

#include <QtMath>
#include <cmath>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
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
// Session 111: `preferred` 0 = this default; otherwise clamped to the dialog range.
inline constexpr int kButtonDiameterMin = 48;
inline constexpr int kButtonDiameterMax = 256;
inline constexpr int kButtonDiameterStep = 8;
inline constexpr int kButtonRingMax = 8; // 0 in options = automatic

inline int buttonCollapsedDiameter(int iconSize, int preferred = 0)
{
    const int fallback = qMax(48, iconSize * 2 + 8);
    if (preferred <= 0)
        return fallback;
    return qBound(kButtonDiameterMin, preferred, kButtonDiameterMax);
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

struct ButtonItemGeomF {
    QRectF icon;
    QRectF label;
    QRectF bounds() const { return icon.united(label); }
};

struct ButtonLayout {
    QVector<ButtonItemGeom> items;
    int diameter = 0;
    int ringCount = 0;
};

// Session 108: the final ring layout in sub-pixel coordinates. Computed once
// from the item count (not from the animated radius), then scaled about the
// disc's centre so icons travel smoothly and never change slot.
struct ButtonLayoutF {
    QVector<ButtonItemGeomF> items;
    qreal diameter = 0;
    int ringCount = 0; // session 111: layers actually used (1 = a single ring or a centre)
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

// Maximum rings that can hold `n` items: a 1-item centre is allowed, every
// other ring needs at least 2 (same rule as the automatic layout).
inline int buttonMaxFeasibleRings(int n)
{
    if (n <= 1)
        return 1;
    return 1 + (n - 1) / 2;
}

inline bool buttonRingCountsOk(const std::vector<int>& c)
{
    if (c.empty())
        return false;
    for (int i = 0; i < int(c.size()); ++i) {
        if (c[i] <= 0 || (c[i] == 1 && i != 0) || (i > 0 && c[i] < 2))
            return false;
    }
    return true;
}

// Fixed ring count: outer rings take more, proportional to circumference
// (weights 1, 2, ..., k, then largest-remainder). Used only when the user
// picks a number of rings; automatic layout keeps buttonSplitCounts.
inline std::vector<int> buttonSplitCountsProportional(int n, int layers)
{
    std::vector<int> c(qMax(0, layers), 0);
    if (n <= 0 || layers <= 0)
        return c;
    if (layers == 1) {
        c[0] = n;
        return c;
    }
    const int sumW = layers * (layers + 1) / 2;
    std::vector<double> remainder(layers, 0.0);
    int assigned = 0;
    for (int i = 0; i < layers; ++i) {
        const double exact = double(n) * double(i + 1) / double(sumW);
        c[i] = int(std::floor(exact));
        remainder[i] = exact - double(c[i]);
        assigned += c[i];
    }
    const int leftover = n - assigned;
    std::vector<int> order(layers);
    for (int i = 0; i < layers; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (qAbs(remainder[a] - remainder[b]) > 1e-12)
            return remainder[a] > remainder[b];
        return a > b; // prefer the outer ring on a tie
    });
    for (int k = 0; k < leftover; ++k)
        ++c[order[k]];

    if (c[0] == 0) {
        for (int j = layers - 1; j >= 1; --j) {
            if (c[j] >= 3) {
                --c[j];
                ++c[0];
                break;
            }
        }
    }
    for (int i = 1; i < layers; ++i) {
        while (c[i] < 2) {
            int src = -1;
            for (int j = 0; j < layers; ++j) {
                if (j == i)
                    continue;
                if (j == 0 && c[j] >= 2) {
                    src = j;
                    break;
                }
                if (j > 0 && c[j] >= 3) {
                    src = j;
                    break;
                }
            }
            if (src < 0)
                break;
            --c[src];
            ++c[i];
        }
    }
    return c;
}

inline std::vector<int> buttonFixedRingCounts(int n, int requested)
{
    int k = qBound(1, requested, qMin(kButtonRingMax, buttonMaxFeasibleRings(n)));
    while (k >= 1) {
        const std::vector<int> c = buttonSplitCountsProportional(n, k);
        int sum = 0;
        for (int v : c)
            sum += v;
        if (sum == n && buttonRingCountsOk(c))
            return c;
        --k;
    }
    return {n};
}

// Angle 0 = top, then clockwise. Cell centres sit on the ring (icon above, label below).
inline ButtonLayoutF buttonLayoutF(int count, int iconSize, int labelHeight = kButtonLabelHeight,
                                   int ringCount = 0, int collapsedDiameter = 0)
{
    ButtonLayoutF out;
    const int n = qMax(0, count);
    const int cellW = buttonCellWidth(iconSize);
    const int cellH = buttonCellHeight(iconSize, labelHeight);
    const double d = buttonMinDistance(iconSize, labelHeight);
    const int collapsed = buttonCollapsedDiameter(iconSize, collapsedDiameter);

    auto placeAt = [&](double cx, double cy, double x, double y, ButtonItemGeomF& g) {
        const qreal left = cx + x - cellW / 2.0;
        const qreal top = cy + y - cellH / 2.0;
        g.label = QRectF(left, top + iconSize, cellW, qMax(0, labelHeight));
        g.icon = QRectF(left + (cellW - iconSize) / 2.0, top, iconSize, iconSize);
    };

    auto enclosing = [&](const QVector<ButtonItemGeomF>& items, double cx, double cy) {
        double r = 0.0;
        for (const ButtonItemGeomF& g : items) {
            const QRectF b = g.bounds();
            for (const QPointF& p : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()})
                r = qMax(r, std::hypot(p.x() - cx, p.y() - cy));
        }
        return r + kButtonCirclePad;
    };

    auto boxesOverlap = [](const QRectF& a, const QRectF& b) {
        return a.intersects(b);
    };

    if (n <= 0) {
        out.diameter = qMax(collapsed + 48, cellW + 2 * kButtonCirclePad);
        out.ringCount = 0;
        return out;
    }

    if (n == 1) {
        ButtonItemGeomF g;
        // Temporary origin; recentred below.
        placeAt(0, 0, 0, 0, g);
        out.items = {g};
        out.ringCount = 1;
    } else {
        // Prefer the fewest layers that keep a compact circle; grow the outer
        // radius as needed so neighbour distance >= icon-cell + margin.
        // Session 111: a fixed ring count skips the search and distributes
        // icons with outer rings taking more (proportional to circumference);
        // the expanded radius still grows so boxes never overlap.
        const int maxLayers = qMax(1, n / 2);
        int bestK = 1;
        std::vector<int> bestCounts = {n};
        std::vector<double> bestRadii = {buttonRingRadius(n, d)};
        double bestOuter = bestRadii[0];
        if (ringCount > 0) {
            bestCounts = buttonFixedRingCounts(n, ringCount);
            bestK = int(bestCounts.size());
            bestRadii.assign(bestK, 0.0);
            bestRadii[0] = buttonRingRadius(bestCounts[0], d);
            for (int i = 1; i < bestK; ++i)
                bestRadii[i] = qMax(buttonRingRadius(bestCounts[i], d), bestRadii[i - 1] + d);
            bestOuter = bestRadii.back();
        } else {
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
        }

        out.items.reserve(n);
        out.ringCount = bestK;
        for (int layer = 0; layer < bestK; ++layer) {
            const int m = bestCounts[layer];
            const double r = bestRadii[layer];
            for (int i = 0; i < m; ++i) {
                const double theta = (m == 0) ? 0.0 : (2.0 * 3.14159265358979323846 * i / m);
                // 0 = top, clockwise.
                const double x = r * std::sin(theta);
                const double y = -r * std::cos(theta);
                ButtonItemGeomF g;
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
        for (ButtonItemGeomF& g : out.items) {
            g.icon.translate(g.icon.center().x() * 0.08, g.icon.center().y() * 0.08);
            g.label.translate(g.label.center().x() * 0.08, g.label.center().y() * 0.08);
        }
    }

    const double R = enclosing(out.items, 0.0, 0.0);
    const qreal diameter = qMax(qreal(collapsed + 48),
                                qreal(qCeil(R * 2.0)));
    // Recentre into a square (0,0,diameter,diameter) without rounding the centre.
    const qreal cx = diameter / 2.0;
    const qreal cy = diameter / 2.0;
    for (ButtonItemGeomF& g : out.items) {
        g.icon.translate(cx, cy);
        g.label.translate(cx, cy);
    }
    out.diameter = diameter;
    return out;
}

inline ButtonLayout buttonLayout(int count, int iconSize, int labelHeight = kButtonLabelHeight,
                                 int ringCount = 0, int collapsedDiameter = 0)
{
    const ButtonLayoutF f = buttonLayoutF(count, iconSize, labelHeight, ringCount, collapsedDiameter);
    ButtonLayout out;
    out.diameter = qMax(1, int(qRound(f.diameter)));
    out.ringCount = f.ringCount;
    out.items.reserve(f.items.size());
    for (const ButtonItemGeomF& g : f.items) {
        ButtonItemGeom h;
        h.icon = g.icon.toRect();
        h.label = g.label.toRect();
        out.items.append(h);
    }
    return out;
}

inline int buttonExpandedDiameter(int iconSize, int count, int labelHeight = kButtonLabelHeight,
                                  int ringCount = 0, int collapsedDiameter = 0)
{
    return buttonLayout(count, iconSize, labelHeight, ringCount, collapsedDiameter).diameter;
}

// Visible square while rolled up: a circle inscribed, centred in `stored`.
inline QRect buttonCollapsedRect(const QRect& stored, int iconSize, int preferredDiameter = 0)
{
    const int d = qMin(buttonCollapsedDiameter(iconSize, preferredDiameter), qMin(stored.width(), stored.height()));
    QRect square(0, 0, d, d);
    square.moveCenter(stored.center());
    return square;
}

// A square of `diameter` centred on `center` (the Bouton's stored anchor).
// Must use QRect::moveCenter / QRect::center() -- not `x - d/2` then `.center()`.
// QRect stores x2 = x + width - 1, so centre() is (x1+x2)/2. For an even size
// that is one pixel up-left of x + width/2. Rebuilding with x - d/2 from that
// centre walked the disc one pixel toward the origin on every hover cycle
// (session 106).
inline QRect buttonSquareAt(QPoint center, int diameter)
{
    const int d = qMax(1, diameter);
    QRect square(0, 0, d, d);
    square.moveCenter(center);
    return square;
}

// A physical diameter that maps to an integer logical size at `dpr` and back
// without changing the centre. Using the unrounded size as the window, then
// painting a qRound(d/dpr) widget from the top-left, walked the circle toward
// the top-left at 125 % / 150 % (session 106).
inline int buttonPhysicalDiameter(int diameter, qreal dpr)
{
    const int raw = qMax(1, diameter);
    if (dpr <= 0.0 || qFuzzyCompare(dpr, 1.0))
        return raw;
    const int logical = qMax(1, qRound(raw / dpr));
    return qMax(1, qRound(logical * dpr));
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
    QRect r = buttonSquareAt(square.center(), d);
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

inline QRect buttonVisibleRect(const QRect& stored, int iconSize, double progress,
                               int preferredDiameter = 0)
{
    const QRect small = buttonCollapsedRect(stored, iconSize, preferredDiameter);
    const double u = buttonExpandEase(progress);
    const QPoint c = stored.center();
    const int side = qMax(1, int(qRound(small.width() + (stored.width() - small.width()) * u)));
    return QRect(c.x() - side / 2, c.y() - side / 2, side, side);
}

// Map the *final* ring layout (computed from the item count, never from the
// current radius) into `content`. `scale` is the current disc's size over the
// expanded one (already eased if the disc is); 0 piles every icon on the
// centre, 1 is the finished layout. Icons keep their slot and ring.
inline QVector<ButtonItemGeomF> buttonItemGeometryF(const QRectF& content, const ButtonLayoutF& lay)
{
    if (lay.items.isEmpty() || content.isEmpty() || lay.diameter <= 0)
        return {};
    const QPointF c = content.center();
    const QPointF o(lay.diameter / 2.0, lay.diameter / 2.0);
    const qreal room = qMin(content.width(), content.height());
    const qreal scale = room / lay.diameter;
    QVector<ButtonItemGeomF> out;
    out.reserve(lay.items.size());
    for (const ButtonItemGeomF& g : lay.items) {
        ButtonItemGeomF h;
        auto mapRect = [&](const QRectF& r) {
            const QPointF topLeft = c + QPointF(r.x() - o.x(), r.y() - o.y()) * scale;
            return QRectF(topLeft, QSizeF(r.width() * scale, r.height() * scale));
        };
        h.icon = mapRect(g.icon);
        h.label = mapRect(g.label);
        out.append(h);
    }
    return out;
}

inline QVector<ButtonItemGeomF> buttonItemGeometryF(const QRectF& content, int count, int iconSize,
                                                    int labelHeight = kButtonLabelHeight,
                                                    const ButtonLayoutF* cached = nullptr)
{
    const ButtonLayoutF lay = cached ? *cached : buttonLayoutF(count, iconSize, labelHeight);
    return buttonItemGeometryF(content, lay);
}

inline QVector<ButtonItemGeom> buttonItemGeometry(const QRect& content, int count, int iconSize,
                                                 int labelHeight = kButtonLabelHeight)
{
    const auto items = buttonItemGeometryF(QRectF(content), count, iconSize, labelHeight);
    QVector<ButtonItemGeom> out;
    out.reserve(items.size());
    for (const ButtonItemGeomF& g : items) {
        ButtonItemGeom h;
        h.icon = g.icon.toRect();
        h.label = g.label.toRect();
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
