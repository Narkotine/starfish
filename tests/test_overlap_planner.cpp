#include <QtTest>

#include "core/overlap_planner.hpp"

using namespace starfish::core;

// Session 60: desktop icons never stay under a fence when overlap is off.
class TestOverlapPlanner : public QObject
{
    Q_OBJECT

private slots:
    void anIconUnderAFenceMovesToTheNearestFreeSlot()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QRect area(0, 0, 1000, 600);
        const QRect fence(200, 0, 300, 300); // covers columns 2..4 of rows 0..2
        // A at column 0, B under the fence at column 2 row 0, C at column 1 row 0.
        const QList<QPair<QString, QPoint>> items{
            {"A", QPoint(26, 0)}, {"B", QPoint(226, 0)}, {"C", QPoint(126, 0)}};
        const QList<OverlapMove> moves = OverlapPlanner::plan(items, {fence}, {area}, slot);
        QCOMPARE(moves.size(), 1);
        QCOMPARE(moves.first().key, QStringLiteral("B"));
        const QRect box = slot.boxAt(moves.first().to);
        QVERIFY(!box.intersects(fence));
        QVERIFY(area.contains(box));
        QVERIFY(!box.intersects(slot.boxAt(QPoint(26, 0))));
        QVERIFY(!box.intersects(slot.boxAt(QPoint(126, 0))));
        // Same grid as before: a whole number of steps away.
        QCOMPARE((moves.first().to.x() - 226) % 100, 0);
        QCOMPARE(moves.first().to.y() % 100, 0);
    }

    void twoIconsUnderAFenceNeverShareASlot()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QList<QPair<QString, QPoint>> items{{"A", QPoint(226, 0)}, {"B", QPoint(326, 0)}};
        const QList<OverlapMove> moves =
            OverlapPlanner::plan(items, {QRect(200, 0, 300, 300)}, {QRect(0, 0, 1000, 600)}, slot);
        QCOMPARE(moves.size(), 2);
        QVERIFY(moves.at(0).to != moves.at(1).to);
    }

    void nothingMovesWithoutAFenceOrWithNowhereToGo()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QList<QPair<QString, QPoint>> items{{"A", QPoint(26, 0)}};
        QVERIFY(OverlapPlanner::plan(items, {}, {QRect(0, 0, 1000, 600)}, slot).isEmpty());
        // A fence covering the whole area: the icon stays.
        QVERIFY(OverlapPlanner::plan(items, {QRect(0, 0, 1000, 600)}, {QRect(0, 0, 1000, 600)}, slot)
                    .isEmpty());
    }

    // Session 61: an icon dragged on the desktop.
    void aDragSnapsToTheGridOfTheIcons()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QRect area(0, 0, 1000, 600);
        const QList<QPair<QString, QPoint>> items{{"A", QPoint(26, 0)}};
        // 140,60 is 1.4 and 0.6 steps: one step right, one down.
        const QList<OverlapMove> moves =
            OverlapPlanner::planMove(items, {"A"}, QPoint(140, 60), true, {}, {area}, slot);
        QCOMPARE(moves.size(), 1);
        QCOMPARE(moves.first().to, QPoint(126, 100));
        // Less than half a step: nothing moves.
        QVERIFY(OverlapPlanner::planMove(items, {"A"}, QPoint(30, -20), true, {}, {area}, slot).isEmpty());
    }

    void aDragOntoATakenSlotGoesToTheNearestFreeOne()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QRect area(0, 0, 1000, 600);
        const QList<QPair<QString, QPoint>> items{{"A", QPoint(26, 0)}, {"B", QPoint(226, 200)}};
        const QList<OverlapMove> moves =
            OverlapPlanner::planMove(items, {"A"}, QPoint(200, 200), true, {}, {area}, slot);
        QCOMPARE(moves.size(), 1);
        const QPoint to = moves.first().to;
        QVERIFY(to != QPoint(226, 200));
        // A neighbour of the wanted slot, on the same grid.
        QCOMPARE(qAbs(to.x() - 226) + qAbs(to.y() - 200), 100);
    }

    void aDragNeverEndsUnderAFenceAndASelectionKeepsItsShape()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QRect area(0, 0, 1000, 600);
        const QRect fence(400, 0, 300, 300);
        const QList<QPair<QString, QPoint>> items{
            {"A", QPoint(26, 0)}, {"B", QPoint(26, 100)}, {"C", QPoint(126, 0)}};
        // A and B, two steps right: free, the column keeps its shape.
        QList<OverlapMove> moves =
            OverlapPlanner::planMove(items, {"A", "B"}, QPoint(200, 0), true, {fence}, {area}, slot);
        QCOMPARE(moves.size(), 2);
        QCOMPARE(moves.at(0).to, QPoint(226, 0));
        QCOMPARE(moves.at(1).to, QPoint(226, 100));
        // Into the fence: it lands next to it instead.
        moves = OverlapPlanner::planMove(items, {"A"}, QPoint(400, 0), true, {fence}, {area}, slot);
        QCOMPARE(moves.size(), 1);
        QVERIFY(!slot.boxAt(moves.first().to).intersects(fence));
        // Overlap allowed (no obstacles): it goes there.
        moves = OverlapPlanner::planMove(items, {"A"}, QPoint(400, 0), true, {}, {area}, slot);
        QCOMPARE(moves.first().to, QPoint(426, 0));
    }

    void aSelectionMayTakeTheSlotsItLeaves()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QList<QPair<QString, QPoint>> items{{"A", QPoint(26, 0)}, {"B", QPoint(126, 0)}};
        // A and B one step right: A takes B's old slot, B the next one.
        const QList<OverlapMove> moves = OverlapPlanner::planMove(
            items, {"A", "B"}, QPoint(100, 0), true, {}, {QRect(0, 0, 1000, 600)}, slot);
        QCOMPARE(moves.size(), 2);
        QCOMPARE(moves.at(0).to, QPoint(126, 0));
        QCOMPARE(moves.at(1).to, QPoint(226, 0));
    }

    void withoutTheGridTheDropIsExact()
    {
        const OverlapSlot slot{QSize(100, 100), 48, 5};
        const QList<QPair<QString, QPoint>> items{{"A", QPoint(26, 0)}, {"B", QPoint(126, 0)}};
        const QList<OverlapMove> moves = OverlapPlanner::planMove(
            items, {"A"}, QPoint(113, 7), false, {}, {QRect(0, 0, 1000, 600)}, slot);
        QCOMPARE(moves.size(), 1);
        QCOMPARE(moves.first().to, QPoint(139, 7)); // on B: Explorer allows it without the grid
    }
};

QTEST_MAIN(TestOverlapPlanner)
#include "test_overlap_planner.moc"
