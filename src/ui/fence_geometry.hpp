#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtGlobal>

namespace starfish::ui {

// Which part of a fence a point falls on (T-007). The title bar moves the
// fence; the eight border zones resize it; everything else is the content area
// (icon selection and drag), reported as None so FenceView can tell a fence
// gesture from an icon gesture.
enum class FenceHandle {
    None = 0,
    TitleBar,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

inline bool isResizeHandle(FenceHandle handle)
{
    return handle != FenceHandle::None && handle != FenceHandle::TitleBar;
}

// Pure geometry for the fence lifecycle (T-007): handle hit-testing, resize
// math, minimum-size clamping and edge/centre snapping. No widget, no Win32 --
// so every rule below is unit-tested in tests/test_fence_geometry.cpp and the
// interactive code (FenceView, FenceContainer) only supplies the inputs.
class FenceGeometry
{
public:
    // `border` is the resize grip width measured inwards from each edge,
    // `titleBarHeight` the height of the drag band at the top. Corners win over
    // edges: a point in the top-left corner square is TopLeft, never Left or
    // Top, so a diagonal resize stays reachable on a small fence.
    static FenceHandle hitTest(const QRect& rect, const QPoint& point, int border,
                               int titleBarHeight)
    {
        if (!rect.contains(point))
            return FenceHandle::None;

        const bool left = point.x() - rect.left() < border;
        const bool right = rect.right() - point.x() < border;
        const bool top = point.y() - rect.top() < border;
        const bool bottom = rect.bottom() - point.y() < border;

        if (top && left)
            return FenceHandle::TopLeft;
        if (top && right)
            return FenceHandle::TopRight;
        if (bottom && left)
            return FenceHandle::BottomLeft;
        if (bottom && right)
            return FenceHandle::BottomRight;
        if (left)
            return FenceHandle::Left;
        if (right)
            return FenceHandle::Right;
        if (top)
            return FenceHandle::Top;
        if (bottom)
            return FenceHandle::Bottom;
        if (point.y() - rect.top() < titleBarHeight)
            return FenceHandle::TitleBar;
        return FenceHandle::None;
    }

    // Applies a pointer delta to `start` for the grabbed handle. The result is
    // never smaller than `minSize`: when a west/north drag would cross the
    // opposite edge, the moving edge stops and the origin follows, so the fence
    // shrinks instead of flipping inside out.
    static QRect resize(const QRect& start, FenceHandle handle, const QPoint& delta,
                        const QSize& minSize)
    {
        if (!isResizeHandle(handle))
            return start;

        const bool west = handle == FenceHandle::Left || handle == FenceHandle::TopLeft
                          || handle == FenceHandle::BottomLeft;
        const bool east = handle == FenceHandle::Right || handle == FenceHandle::TopRight
                          || handle == FenceHandle::BottomRight;
        const bool north = handle == FenceHandle::Top || handle == FenceHandle::TopLeft
                           || handle == FenceHandle::TopRight;
        const bool south = handle == FenceHandle::Bottom || handle == FenceHandle::BottomLeft
                           || handle == FenceHandle::BottomRight;

        const int minW = qMax(1, minSize.width());
        const int minH = qMax(1, minSize.height());

        int x = start.x();
        int y = start.y();
        int w = start.width();
        int h = start.height();

        if (west) {
            int newX = x + delta.x();
            int newW = w - delta.x();
            if (newW < minW) {
                newW = minW;
                newX = x + w - minW; // pin the right edge, move the left one
            }
            x = newX;
            w = newW;
        } else if (east) {
            w = qMax(minW, w + delta.x());
        }

        if (north) {
            int newY = y + delta.y();
            int newH = h - delta.y();
            if (newH < minH) {
                newH = minH;
                newY = y + h - minH; // pin the bottom edge
            }
            y = newY;
            h = newH;
        } else if (south) {
            h = qMax(minH, h + delta.y());
        }

        return QRect(x, y, w, h);
    }

    // Snaps `moving` against the screen `bounds` and every rect in `others`:
    // left/centre/right on X, top/middle/bottom on Y, within `threshold` px.
    // The closest candidate on each axis wins, independently. `snappedX` and
    // `snappedY` report whether that axis moved, so the caller can draw a guide.
    static QRect snap(const QRect& moving, const QList<QRect>& others, const QSize& bounds,
                      int threshold, bool* snappedX = nullptr, bool* snappedY = nullptr)
    {
        bool sx = false;
        bool sy = false;
        if (threshold <= 0) {
            if (snappedX)
                *snappedX = false;
            if (snappedY)
                *snappedY = false;
            return moving;
        }

        QList<int> linesX{moving.left(), moving.left() + moving.width() / 2,
                          moving.left() + moving.width()};
        QList<int> linesY{moving.top(), moving.top() + moving.height() / 2,
                          moving.top() + moving.height()};
        QList<int> targetsX{0, bounds.width() / 2, bounds.width()};
        QList<int> targetsY{0, bounds.height() / 2, bounds.height()};
        for (const QRect& other : others) {
            targetsX << other.left() << other.left() + other.width() / 2
                     << other.left() + other.width();
            targetsY << other.top() << other.top() + other.height() / 2
                     << other.top() + other.height();
        }

        const int dx = bestOffset(linesX, targetsX, threshold, &sx);
        const int dy = bestOffset(linesY, targetsY, threshold, &sy);

        QRect result = moving;
        result.translate(dx, dy);
        if (snappedX)
            *snappedX = sx;
        if (snappedY)
            *snappedY = sy;
        return result;
    }

    // Keeps a rect fully inside `bounds`, shrinking it first when it does not
    // fit. Used after a move so a fence can never be dragged out of reach.
    static QRect clampToBounds(const QRect& rect, const QSize& bounds)
    {
        QRect result = rect;
        if (bounds.width() > 0 && result.width() > bounds.width())
            result.setWidth(bounds.width());
        if (bounds.height() > 0 && result.height() > bounds.height())
            result.setHeight(bounds.height());

        const int maxX = qMax(0, bounds.width() - result.width());
        const int maxY = qMax(0, bounds.height() - result.height());
        result.moveTopLeft(QPoint(qBound(0, result.x(), maxX), qBound(0, result.y(), maxY)));
        return result;
    }

private:
    // Closest (target - line) offset within the threshold, 0 when none applies.
    // `snapped` is set accordingly, so callers can distinguish "already aligned"
    // from "nothing close enough".
    static int bestOffset(const QList<int>& lines, const QList<int>& targets, int threshold,
                          bool* snapped)
    {
        int best = 0;
        int bestDistance = threshold + 1;
        for (const int line : lines) {
            for (const int target : targets) {
                const int delta = target - line;
                const int distance = qAbs(delta);
                if (distance <= threshold && distance < bestDistance) {
                    bestDistance = distance;
                    best = delta;
                }
            }
        }
        if (snapped)
            *snapped = bestDistance <= threshold;
        return best;
    }
};

} // namespace starfish::ui
