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
};

QTEST_MAIN(TestOverlapPlanner)
#include "test_overlap_planner.moc"
