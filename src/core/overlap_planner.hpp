#pragma once

#include <QHash>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
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
        const int sw = slot.spacing.width();
        const int sh = slot.spacing.height();
        if (sw <= 0 || sh <= 0 || obstacles.isEmpty() || areas.isEmpty())
            return moves;
        auto underFence = [&](const QRect& box) {
            for (const QRect& o : obstacles)
                if (o.intersects(box))
                    return true;
            return false;
        };
        auto inArea = [&](const QRect& box) {
            for (const QRect& a : areas)
                if (a.contains(box))
                    return true;
            return false;
        };
        // What stays is taken first; what moves takes its new slot in turn.
        QList<QRect> taken;
        QList<int> movers;
        for (int i = 0; i < items.size(); ++i) {
            const QRect box = slot.boxAt(items.at(i).second);
            if (underFence(box))
                movers.append(i);
            else
                taken.append(box);
        }
        for (int i : std::as_const(movers)) {
            const QPoint from = items.at(i).second;
            QPoint best;
            qint64 bestDistance = std::numeric_limits<qint64>::max();
            for (const QRect& a : areas) {
                // The grid through `from`, over the whole area.
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
                        if (d >= bestDistance)
                            continue;
                        const QRect box = slot.boxAt(p);
                        if (!inArea(box) || underFence(box))
                            continue;
                        bool free = true;
                        for (const QRect& t : std::as_const(taken))
                            free = free && !t.intersects(box);
                        if (!free)
                            continue;
                        best = p;
                        bestDistance = d;
                    }
                }
            }
            if (bestDistance == std::numeric_limits<qint64>::max())
                continue; // a desktop full to the brim: left where it is
            taken.append(slot.boxAt(best));
            moves.append({items.at(i).first, from, best});
        }
        return moves;
    }

private:
    static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
};

} // namespace starfish::core
