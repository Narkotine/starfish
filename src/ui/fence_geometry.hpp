#pragma once

#include <climits>

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

// Pure geometry for the fence lifecycle (T-007) and for the navigation
// behaviours that move or hide a fence (T-009). No widget, no Win32 -- so every
// rule below is unit-tested in tests/test_fence_geometry.cpp and the interactive
// code (FenceView, FenceContainer) only supplies the inputs.
class FenceGeometry
{
public:
    // Shared fence metrics. Defined once so the widget and the container cannot
    // disagree about where the title bar ends or how wide a grip is.
    static constexpr int kTitleBarHeight = 30;
    static constexpr int kResizeBorder = 6;

    // The rect a fence actually shows. A rolled-up fence (F-020) is reduced to
    // its title bar; the *stored* rect keeps the expanded size, so rolling back
    // down restores the fence exactly as it was. Pure.
    static QRect visibleRect(const QRect& stored, bool rolledUp)
    {
        if (!rolledUp)
            return stored;
        return QRect(stored.x(), stored.y(), stored.width(), kTitleBarHeight);
    }

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

    // Snaps `moving` against every rect in `frames` (the screens, T-014) and
    // every rect in `others` (the other fences): left/centre/right on X,
    // top/middle/bottom on Y, within `threshold` px. The closest candidate on
    // each axis wins, independently. `snappedX` and `snappedY` report whether
    // that axis moved, so the caller can draw a guide.
    //
    // Snapping against a *list* of screens rather than one bounding box is what
    // lets a fence dock to the inner edge of a neighbouring monitor: each screen
    // contributes its own left/centre/right and top/middle/bottom lines.
    static QRect snap(const QRect& moving, const QList<QRect>& others, const QList<QRect>& frames,
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
        QList<int> targetsX;
        QList<int> targetsY;
        for (const QRect& frame : frames) {
            targetsX << frame.left() << frame.left() + frame.width() / 2
                     << frame.left() + frame.width();
            targetsY << frame.top() << frame.top() + frame.height() / 2
                     << frame.top() + frame.height();
        }
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

    // The single-screen case of the above: one screen whose origin is (0,0), so
    // its frame is its size. The logic lives in the frames version only.
    static QRect snap(const QRect& moving, const QList<QRect>& others, const QSize& bounds,
                      int threshold, bool* snappedX = nullptr, bool* snappedY = nullptr)
    {
        const QList<QRect> frames{QRect(QPoint(0, 0), bounds)};
        return snap(moving, others, frames, threshold, snappedX, snappedY);
    }

    // Keeps a rect fully inside `bounds`, shrinking it first when it does not
    // fit. Used after a move so a fence can never be dragged out of reach.
    //
    // Offset aware (T-014): `bounds` is a screen, and a screen is not
    // necessarily anchored at (0,0) -- a monitor left of or above the primary
    // one has negative coordinates.
    static QRect clampIntoRect(const QRect& rect, const QRect& bounds)
    {
        if (bounds.width() <= 0 || bounds.height() <= 0)
            return rect;

        QRect result = rect;
        if (result.width() > bounds.width())
            result.setWidth(bounds.width());
        if (result.height() > bounds.height())
            result.setHeight(bounds.height());

        const int maxX = bounds.left() + bounds.width() - result.width();
        const int maxY = bounds.top() + bounds.height() - result.height();
        result.moveTopLeft(QPoint(qBound(bounds.left(), result.x(), maxX),
                                  qBound(bounds.top(), result.y(), maxY)));
        return result;
    }

    // The single-screen case: a screen anchored at (0,0), described by its size.
    static QRect clampToBounds(const QRect& rect, const QSize& bounds)
    {
        return clampIntoRect(rect, QRect(QPoint(0, 0), bounds));
    }

    // Resolves a move/resize request into the rect that gets stored, honouring
    // the roll-up state (F-020). Two rules differ from the plain case:
    //
    //  * a rolled-up fence has no visible body, so only x, y and width change;
    //    the stored (expanded) height is preserved and rolling back down
    //    restores it exactly;
    //  * snapping and clamping act on the *visible* rect, so a rolled-up fence
    //    snaps its title bar against the screen edge and may sit flush with the
    //    bottom edge with its hidden body hanging off-screen.
    //
    // `others` are the visible rects of the other fences, `frames` the screens
    // the request snaps against, and `clampTo` the single screen the fence must
    // end up inside (T-014: `ScreenLayout::clampRectFor()` picks it -- the
    // screen under the requested rect's centre). Pure; this is the whole of
    // FenceContainer::setFenceRect minus the bookkeeping.
    static QRect applyRequest(const QRect& stored, bool rolledUp, const QRect& requested,
                              const QList<QRect>& others, const QList<QRect>& frames,
                              const QRect& clampTo, const QSize& minSize, int snapThreshold,
                              bool snap)
    {
        QRect target = requested;

        if (rolledUp) {
            target.setHeight(stored.height()); // the hidden body keeps its size
            if (target.width() < minSize.width())
                target.setWidth(minSize.width());

            if (snap) {
                const QRect visible = visibleRect(target, true);
                const QRect snapped = FenceGeometry::snap(visible, others, frames, snapThreshold);
                target.translate(snapped.topLeft() - visible.topLeft());
            }

            const QRect visible = visibleRect(target, true);
            const QRect clamped = clampIntoRect(visible, clampTo);
            target.setWidth(clamped.width());
            target.moveTopLeft(target.topLeft() + clamped.topLeft() - visible.topLeft());
            target.setHeight(stored.height());
            return target;
        }

        if (target.width() < minSize.width())
            target.setWidth(minSize.width());
        if (target.height() < minSize.height())
            target.setHeight(minSize.height());
        if (snap)
            target = FenceGeometry::snap(target, others, frames, snapThreshold);
        return clampIntoRect(target, clampTo);
    }

    // T-086 (session 60) -- a *resize* request. applyRequest() treats any rect as
    // a move: snap() translates the whole rect (its left, centre *or* right line
    // may catch a target -- the centre depends on the width, hence "it depends on
    // the length"), and clampIntoRect() slides it back on screen. Both moved the
    // edges the user was not holding. Here only the grabbed edges snap (each to
    // the nearest line, within the threshold) and only they are bounded by the
    // screen and the minimum size; the opposite edges are copied from `stored`
    // and never change. A rolled-up fence resizes its width only.
    static QRect applyResize(const QRect& stored, bool rolledUp, FenceHandle handle,
                             const QRect& requested, const QList<QRect>& others,
                             const QList<QRect>& frames, const QRect& clampTo,
                             const QSize& minSize, int snapThreshold, bool snap)
    {
        const bool west = handle == FenceHandle::Left || handle == FenceHandle::TopLeft
                          || handle == FenceHandle::BottomLeft;
        const bool east = handle == FenceHandle::Right || handle == FenceHandle::TopRight
                          || handle == FenceHandle::BottomRight;
        const bool north = !rolledUp
                           && (handle == FenceHandle::Top || handle == FenceHandle::TopLeft
                               || handle == FenceHandle::TopRight);
        const bool south = !rolledUp
                           && (handle == FenceHandle::Bottom || handle == FenceHandle::BottomLeft
                               || handle == FenceHandle::BottomRight);
        // Edges as exclusive coordinates (right = x + width).
        int left = stored.left();
        int top = stored.top();
        int right = stored.left() + stored.width();
        int bottom = stored.top() + stored.height();
        if (west)
            left = requested.left();
        if (east)
            right = requested.left() + requested.width();
        if (north)
            top = requested.top();
        if (south)
            bottom = requested.top() + requested.height();

        if (snap && snapThreshold > 0) {
            QList<int> xs;
            QList<int> ys;
            for (const QRect& r : frames + others) {
                xs << r.left() << r.left() + r.width();
                ys << r.top() << r.top() + r.height();
            }
            bool hit = false;
            if (west)
                left += bestOffset({left}, xs, snapThreshold, &hit);
            if (east)
                right += bestOffset({right}, xs, snapThreshold, &hit);
            if (north)
                top += bestOffset({top}, ys, snapThreshold, &hit);
            if (south)
                bottom += bestOffset({bottom}, ys, snapThreshold, &hit);
        }

        if (clampTo.width() > 0 && clampTo.height() > 0) {
            if (west)
                left = qMax(left, clampTo.left());
            if (east)
                right = qMin(right, clampTo.left() + clampTo.width());
            if (north)
                top = qMax(top, clampTo.top());
            if (south)
                bottom = qMin(bottom, clampTo.top() + clampTo.height());
        }
        const int minW = qMax(1, minSize.width());
        const int minH = qMax(1, minSize.height());
        if (right - left < minW) {
            if (west)
                left = right - minW;
            else
                right = left + minW;
        }
        if (!rolledUp && bottom - top < minH) {
            if (north)
                top = bottom - minH;
            else
                bottom = top + minH;
        }
        return QRect(left, top, right - left, rolledUp ? stored.height() : bottom - top);
    }

    // The single-screen case: one screen at (0,0), which is both the snapping
    // frame and the area to stay inside.
    static QRect applyRequest(const QRect& stored, bool rolledUp, const QRect& requested,
                              const QList<QRect>& others, const QSize& bounds,
                              const QSize& minSize, int snapThreshold, bool snap)
    {
        const QRect screen(QPoint(0, 0), bounds);
        return applyRequest(stored, rolledUp, requested, others, QList<QRect>{screen}, screen,
                            minSize, snapThreshold, snap);
    }

    // Where a brand-new fence goes: the first spot that fits inside `bounds`
    // without covering a fence that is already there (T-066). Candidates are
    // visited in reading order -- left to right, top to bottom -- on a grid with
    // `gap` between neighbours, which is the order a person looks for room. The
    // caller decides the margin by shrinking `bounds` before the call.
    //
    // Why this exists: "to the right of everything" is enough while the first row
    // has room, but the moment it wraps, every later fence lands on the same
    // point -- and the clamp to the screen (which the container applies anyway)
    // makes that point the same corner every time. Measured on a real document:
    // five fences, three of them at (80, 1200), i.e. three fences stacked on each
    // other at the bottom of the screen and looking like one. A fence created
    // where the user cannot see it is worse than no fence at all.
    //
    // When there is genuinely no free spot (the area is full), the candidate that
    // covers the least wins: an overlap is visible and can be dragged apart, while
    // a fence hidden underneath another one is not. Pure.
    static QRect freeSpot(const QList<QRect>& others, const QSize& size, const QRect& bounds,
                          int gap = 24)
    {
        const int stepX = qMax(1, size.width() + gap);
        const int stepY = qMax(1, size.height() + gap);
        QRect best;
        qint64 bestOverlap = -1;

        for (int y = bounds.top(); y + size.height() <= bounds.bottom() + 1; y += stepY) {
            for (int x = bounds.left(); x + size.width() <= bounds.right() + 1; x += stepX) {
                const QRect candidate(x, y, size.width(), size.height());
                qint64 overlap = 0;
                for (const QRect& other : others) {
                    const QRect hit = candidate.intersected(other);
                    if (!hit.isEmpty())
                        overlap += static_cast<qint64>(hit.width()) * hit.height();
                }
                if (overlap == 0)
                    return candidate;
                if (bestOverlap < 0 || overlap < bestOverlap) {
                    bestOverlap = overlap;
                    best = candidate;
                }
            }
        }

        if (bestOverlap >= 0)
            return best;
        // Nothing even fits (the fence is larger than the area): the caller clamps
        // it, exactly as it does for a rect that came from the document.
        return QRect(bounds.topLeft(), size);
    }

    // Session 64 ("Autoriser la superposition des zones" off): where a fence
    // going from `from` to `to` may go without covering another one. A fence it
    // did not cover before is a wall: the rect is pushed back, along the side
    // where `from` was clear of it, by the least amount -- the gesture stops
    // against the edge. A fence it already covered is left alone (an old
    // overlap neither blocks nor is resolved behind the user's back). When no
    // such place exists, `from`. `resizing` moves the dragged edge instead of
    // the whole rect.
    static QRect avoidOverlap(const QRect& from, const QRect& to, const QList<QRect>& others, bool resizing)
    {
        QRect r = to;
        for (int pass = 0; pass < 8; ++pass) {
            bool changed = false;
            for (const QRect& o : others) {
                if (!r.intersects(o) || from.intersects(o))
                    continue;
                QRect best;
                int bestCost = INT_MAX;
                const auto consider = [&](const QRect& candidate, int cost) {
                    if (candidate.isValid() && cost < bestCost) {
                        best = candidate;
                        bestCost = cost;
                    }
                };
                if (from.right() < o.left()) {
                    QRect c = r;
                    if (resizing) c.setRight(o.left() - 1); else c.moveRight(o.left() - 1);
                    consider(c, qAbs(r.right() - c.right()));
                }
                if (from.left() > o.right()) {
                    QRect c = r;
                    if (resizing) c.setLeft(o.right() + 1); else c.moveLeft(o.right() + 1);
                    consider(c, qAbs(r.left() - c.left()));
                }
                if (from.bottom() < o.top()) {
                    QRect c = r;
                    if (resizing) c.setBottom(o.top() - 1); else c.moveBottom(o.top() - 1);
                    consider(c, qAbs(r.bottom() - c.bottom()));
                }
                if (from.top() > o.bottom()) {
                    QRect c = r;
                    if (resizing) c.setTop(o.bottom() + 1); else c.moveTop(o.bottom() + 1);
                    consider(c, qAbs(r.top() - c.top()));
                }
                if (bestCost == INT_MAX)
                    return from;
                r = best;
                changed = true;
            }
            if (!changed)
                break;
        }
        for (const QRect& o : others) {
            if (r.intersects(o) && !from.intersects(o))
                return from;
        }
        return r;
    }

    // Session 64: the free place nearest to `rect` (same size) inside `bounds`
    // -- a fence drawn over another one when overlaps are off. Rings of 16 px
    // around the request; `rect` itself when nothing is free.
    static QRect nearestFree(const QRect& rect, const QList<QRect>& others, const QRect& bounds)
    {
        const auto free = [&](const QRect& c) {
            if (!bounds.contains(c))
                return false;
            for (const QRect& o : others) {
                if (c.intersects(o))
                    return false;
            }
            return true;
        };
        if (free(rect))
            return rect;
        const int step = 16;
        const int reach = qMax(bounds.width(), bounds.height());
        for (int ring = step; ring <= reach; ring += step) {
            QRect best;
            qint64 bestDistance = -1;
            for (int d = -ring; d <= ring; d += step) {
                for (const QPoint& offset : {QPoint(d, -ring), QPoint(d, ring), QPoint(-ring, d), QPoint(ring, d)}) {
                    const QRect c = rect.translated(offset);
                    if (!free(c))
                        continue;
                    const qint64 distance = qint64(offset.x()) * offset.x() + qint64(offset.y()) * offset.y();
                    if (bestDistance < 0 || distance < bestDistance) {
                        best = c;
                        bestDistance = distance;
                    }
                }
            }
            if (bestDistance >= 0)
                return best;
        }
        return rect;
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
