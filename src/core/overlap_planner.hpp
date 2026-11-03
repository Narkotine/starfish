#pragma once

#include <QHash>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include <QSize>
#include <QStringList>
#include <QString>

#include <limits>

namespace starfish::core {

// Where desktop icons go when they must not sit under a fence (session 60:
// "Autoriser la superposition" off). Pure -- positions in, moves out -- so the
// rule is unit-tested (tests/test_overlap_planner.cpp) and ProductApp only
// applies the moves through the shell view, which Explorer persists.
//
// Positions are the shell view's: an item's top-left, in screen pixels. A slot
// is the box the desktop draws for it (icon and label).
struct OverlapSlot {
    QSize spacing;     // the view's grid step (IFolderView2 spacing)
    int iconSize = 48; // physical px
    int top = 5;       // the icon sits this far under its slot's top

    QRect boxAt(const QPoint& position) const
    {
        return QRect(position.x() - (spacing.width() - iconSize) / 2, position.y() + top,
                     spacing.width(), spacing.height() - top);
    }
};

struct OverlapMove {
    QString key;
    QPoint from;
    QPoint to;
};

class OverlapPlanner
{
public:
    // items: every free icon on the desktop (filed ones left out by the
    // caller). obstacles: the fences on screen. areas: the work areas. Each
    // item under a fence goes to the nearest free grid slot -- the grid being
    // the one its own position sits on -- that touches no fence and no other
    // icon. An item with nowhere to go stays put.
    static QList<OverlapMove> plan(const QList<QPair<QString, QPoint>>& items,
                                   const QList<QRect>& obstacles, const QList<QRect>& areas,
                                   const OverlapSlot& slot)
    {
        QList<OverlapMove> moves;
        if (!slot.spacing.isValid() || slot.spacing.isEmpty() || obstacles.isEmpty() || areas.isEmpty())
            return moves;
        // What stays is taken first; what moves takes its new slot in turn.
        QList<QRect> taken;
        QList<int> movers;
        for (int i = 0; i < items.size(); ++i) {
            const QRect box = slot.boxAt(items.at(i).second);
            if (underAny(box, obstacles))
                movers.append(i);
            else
                taken.append(box);
        }
        for (int i : std::as_const(movers)) {
            const QPoint from = items.at(i).second;
            QPoint to;
            if (!nearestFree(from, taken, obstacles, areas, slot, &to))
                continue; // a desktop full to the brim: left where it is
            taken.append(slot.boxAt(to));
            moves.append({items.at(i).first, from, to});
        }
        return moves;
    }

    // Session 61: icons the user dragged on the desktop. `moving` (keys of
    // `items`) all move by `delta`, the selection keeping its shape. With
    // `snap` (Explorer's "Align icons to grid") the delta is rounded to whole
    // grid steps -- the grid the icons already sit on -- and a slot already
    // taken by an icon that stays sends the dragged one to the nearest free
    // slot (Explorer's rule). Never into a fence (obstacles; empty when the
    // user allows overlap) nor off the work areas. Icons that end where they
    // started are not listed.
    static QList<OverlapMove> planMove(const QList<QPair<QString, QPoint>>& items,
                                       const QStringList& moving, const QPoint& delta, bool snap,
                                       const QList<QRect>& obstacles, const QList<QRect>& areas,
                                       const OverlapSlot& slot)
    {
        QList<OverlapMove> moves;
        const int sw = slot.spacing.width();
        const int sh = slot.spacing.height();
        if (sw <= 0 || sh <= 0 || areas.isEmpty() || moving.isEmpty())
            return moves;
        const QPoint step = snap ? QPoint(qRound(double(delta.x()) / sw) * sw,
                                          qRound(double(delta.y()) / sh) * sh)
                                 : delta;
        if (step.isNull())
            return moves;
        // Without the grid, Explorer lets icons sit on each other: only fences
        // and the screen bound the drop then.
        QList<QRect> taken;
        if (snap) {
            for (const auto& item : items) {
                if (!moving.contains(item.first))
                    taken.append(slot.boxAt(item.second));
            }
        }
        for (const auto& item : items) {
            if (!moving.contains(item.first))
                continue;
            const QPoint want = item.second + step;
            QPoint to = want;
            if (!fits(slot.boxAt(want), taken, obstacles, areas)
                && !nearestFree(want, taken, obstacles, areas, slot, &to))
                continue; // nowhere to go: it stays
            if (snap)
                taken.append(slot.boxAt(to));
            if (to != item.second)
                moves.append({item.first, item.second, to});
        }
        return moves;
    }

    // Session 80: the desktop's grid, for showing it while an icon is held.
    // Its origin is where most icons sit modulo the spacing (Explorer's grid
    // runs from where it laid the first one out); with no icon, the first work
    // area's corner.
    static QPoint gridOrigin(const QList<QPair<QString, QPoint>>& items, const QList<QRect>& areas,
                             const OverlapSlot& slot)
    {
        const int sw = slot.spacing.width();
        const int sh = slot.spacing.height();
        if (sw <= 0 || sh <= 0)
            return {};
        QHash<qint64, int> votes;
        qint64 best = 0;
        int bestVotes = 0;
        for (const auto& item : items) {
            const int mx = item.second.x() - floorDiv(item.second.x(), sw) * sw;
            const int my = item.second.y() - floorDiv(item.second.y(), sh) * sh;
            const qint64 key = (qint64(mx) << 32) | quint32(my);
            const int v = ++votes[key];
            if (v > bestVotes) {
                bestVotes = v;
                best = key;
            }
        }
        if (bestVotes > 0)
            return QPoint(int(best >> 32), int(qint32(best & 0xFFFFFFFF)));
        if (areas.isEmpty())
            return {};
        return QPoint(areas.first().left() + (sw - slot.iconSize) / 2, areas.first().top());
    }

    // Every slot box of that grid that lies whole inside a work area.
    static QVector<QRect> gridSlots(const QList<QPair<QString, QPoint>>& items, const QList<QRect>& areas,
                                    const OverlapSlot& slot, int cap = 4000)
    {
        QVector<QRect> cells;
        const int sw = slot.spacing.width();
        const int sh = slot.spacing.height();
        if (sw <= 0 || sh <= 0)
            return cells;
        const QPoint origin = gridOrigin(items, areas, slot);
        for (const QRect& area : areas) {
            const int minX = area.left() + (sw - slot.iconSize) / 2;
            const int minY = area.top() - slot.top;
            const int x0 = minX + ((origin.x() - minX) % sw + sw) % sw;
            const int y0 = minY + ((origin.y() - minY) % sh + sh) % sh;
            for (int x = x0;; x += sw) {
                if (slot.boxAt(QPoint(x, y0)).right() > area.right())
                    break;
                for (int y = y0;; y += sh) {
                    const QRect box = slot.boxAt(QPoint(x, y));
                    if (box.bottom() > area.bottom())
                        break;
                    cells.append(box);
                    if (cells.size() >= cap)
                        return cells;
                }
            }
        }
        return cells;
    }

    // The grid position nearest `position` (an icon's top-left).
    static QPoint snapToGrid(const QPoint& position, const QPoint& origin, const OverlapSlot& slot)
    {
        const int sw = slot.spacing.width();
        const int sh = slot.spacing.height();
        if (sw <= 0 || sh <= 0)
            return position;
        return QPoint(origin.x() + qRound(double(position.x() - origin.x()) / sw) * sw,
                      origin.y() + qRound(double(position.y() - origin.y()) / sh) * sh);
    }

private:
    static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    static bool underAny(const QRect& box, const QList<QRect>& rects)
    {
        for (const QRect& r : rects)
            if (r.intersects(box))
                return true;
        return false;
    }

    static bool fits(const QRect& box, const QList<QRect>& taken, const QList<QRect>& obstacles,
                     const QList<QRect>& areas)
    {
        bool inArea = false;
        for (const QRect& a : areas)
            inArea = inArea || a.contains(box);
        return inArea && !underAny(box, obstacles) && !underAny(box, taken);
    }

    // The free slot of the grid through `from` nearest to it.
    static bool nearestFree(const QPoint& from, const QList<QRect>& taken,
                            const QList<QRect>& obstacles, const QList<QRect>& areas,
                            const OverlapSlot& slot, QPoint* out)
    {
        const int sw = slot.spacing.width();
        const int sh = slot.spacing.height();
        qint64 bestDistance = std::numeric_limits<qint64>::max();
        for (const QRect& a : areas) {
            const int i0 = floorDiv(a.left() - from.x() - sw, sw);
            const int i1 = floorDiv(a.right() - from.x() + sw, sw);
            const int j0 = floorDiv(a.top() - from.y() - sh, sh);
            const int j1 = floorDiv(a.bottom() - from.y() + sh, sh);
            for (int gx = i0; gx <= i1; ++gx) {
                for (int gy = j0; gy <= j1; ++gy) {
                    const QPoint p(from.x() + gx * sw, from.y() + gy * sh);
                    const qint64 dx = p.x() - from.x();
                    const qint64 dy = p.y() - from.y();
                    const qint64 d = dx * dx + dy * dy;
                    if (d >= bestDistance || !fits(slot.boxAt(p), taken, obstacles, areas))
                        continue;
                    *out = p;
                    bestDistance = d;
                }
            }
        }
        return bestDistance != std::numeric_limits<qint64>::max();
    }
};

} // namespace starfish::core
