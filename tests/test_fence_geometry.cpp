#include <QtTest>

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>

#include "ui/fence_geometry.hpp"

using namespace starfish::ui;

// Pure geometry of the fence lifecycle (T-007): hit-testing a handle, resizing
// with a minimum size, edge/centre snapping and clamping to the desktop. No
// widget and no Win32: the interactive code only feeds these functions.
class TestFenceGeometry : public QObject
{
    Q_OBJECT

private slots:
    void theDiagonalGripFollowsARoundedCorner()
    {
        // Session 65: with corners of 40 px the diagonal grip runs along the arc.
        const QRect r(0, 0, 300, 200);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(10, 3), 6, 30, 40), FenceHandle::TopLeft);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(3, 10), 6, 30, 40), FenceHandle::TopLeft);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(14, 14), 6, 30, 40), FenceHandle::TopLeft);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(20, 20), 6, 30, 40), FenceHandle::TitleBar);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(286, 190), 6, 30, 40), FenceHandle::BottomRight);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(150, 3), 6, 30, 40), FenceHandle::Top);
        QCOMPARE(FenceGeometry::hitTest(r, QPoint(10, 3), 6, 30, 0), FenceHandle::Top); // square: as before
    }

    // Session 64: overlaps off.
    void aRolledUpFenceKeepsItsEdge();
    void aMoveStopsAgainstAnotherFence();
    void aResizeStopsAgainstAnotherFence();
    void anOldOverlapNeitherBlocksNorMoves();
    void aDrawnFenceGoesToTheNearestFreePlace();
    void hitTestClassifiesRegions();
    void hitTestCornersWinOverEdges();
    void resizeGrowsFromEastAndSouth();
    void resizeFromWestKeepsOppositeEdge();
    void resizeClampsToMinimumFromWest();
    void resizeClampsToMinimumFromNorth();
    void resizeOnNonResizeHandleIsNoOp();
    void snapToScreenEdges();
    void snapToOtherFenceEdge();
    void snapToScreenCentre();
    void snapLeavesFarAwayRectsAlone();
    void snapReportsWhichAxisMoved();
    void clampToBoundsKeepsRectInside();
    void clampToBoundsShrinksOversizedRect();
    void visibleRectReducesRolledFenceToItsTitleBar();
    void resizeFromTheRightNeverMovesTheTopLeft();
    void resizeFromTheLeftMovesOnlyTheLeftEdge();
    void applyRequestKeepsTheStoredHeightWhenRolled();
    void applyRequestSnapsARolledFenceByItsTitleBar();
    void applyRequestKeepsARolledTitleBarOnScreen();
    void applyRequestStillClampsAnUnrolledFenceToTheMinimum();
    // T-066: where a brand-new fence goes.
    void freeSpotTakesTheFirstCandidateOnAnEmptyDesk();
    void freeSpotAvoidsAnExistingFence();
    void freeSpotMovesToTheNextRow();
    void freeSpotSkipsTheColumnOfATallFence();
    void freeSpotPicksTheLeastCoveredSpotWhenThereIsNoRoom();
    void freeSpotNeverLeavesTheArea();
    void freeSpotStillAnswersWhenNothingFits();
};

void TestFenceGeometry::hitTestClassifiesRegions()
{
    const QRect rect(0, 0, 300, 200);
    const int border = 6;
    const int titleBar = 30;

    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(2, 2), border, titleBar), FenceHandle::TopLeft);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(297, 2), border, titleBar), FenceHandle::TopRight);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(2, 197), border, titleBar),
             FenceHandle::BottomLeft);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(297, 197), border, titleBar),
             FenceHandle::BottomRight);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(2, 100), border, titleBar), FenceHandle::Left);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(297, 100), border, titleBar), FenceHandle::Right);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(150, 2), border, titleBar), FenceHandle::Top);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(150, 197), border, titleBar), FenceHandle::Bottom);
    // Below the top border, above the content: the title bar.
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(150, 15), border, titleBar),
             FenceHandle::TitleBar);
    // The content area is None: that is what lets the icon grid keep its clicks.
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(150, 100), border, titleBar), FenceHandle::None);
    // Outside the fence nothing is grabbed.
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(400, 400), border, titleBar), FenceHandle::None);
}

void TestFenceGeometry::hitTestCornersWinOverEdges()
{
    // On a small fence the top and bottom bands overlap. The corner must still
    // win, otherwise a diagonal resize would be unreachable.
    const QRect rect(0, 0, 20, 20);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(2, 2), 6, 30), FenceHandle::TopLeft);
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(18, 18), 6, 30), FenceHandle::BottomRight);
    // Mid-top stays a plain edge.
    QCOMPARE(FenceGeometry::hitTest(rect, QPoint(10, 2), 6, 30), FenceHandle::Top);
}

void TestFenceGeometry::resizeGrowsFromEastAndSouth()
{
    const QRect start(100, 100, 300, 200);
    const QSize minSize(160, 120);

    QCOMPARE(FenceGeometry::resize(start, FenceHandle::Right, QPoint(50, 0), minSize),
             QRect(100, 100, 350, 200));
    QCOMPARE(FenceGeometry::resize(start, FenceHandle::Bottom, QPoint(0, 40), minSize),
             QRect(100, 100, 300, 240));
    QCOMPARE(FenceGeometry::resize(start, FenceHandle::BottomRight, QPoint(50, 40), minSize),
             QRect(100, 100, 350, 240));
}

void TestFenceGeometry::resizeFromWestKeepsOppositeEdge()
{
    // Dragging the left edge right shrinks the fence; the right edge (400) must
    // not move.
    const QRect start(100, 100, 300, 200);
    const QRect result =
        FenceGeometry::resize(start, FenceHandle::Left, QPoint(50, 0), QSize(160, 120));
    QCOMPARE(result, QRect(150, 100, 250, 200));
    QCOMPARE(result.left() + result.width(), start.left() + start.width());
}

void TestFenceGeometry::resizeClampsToMinimumFromWest()
{
    // Asking for a 100 px width must stop at the 160 px minimum, with the right
    // edge still pinned -- the fence shrinks, it does not flip inside out.
    const QRect start(100, 100, 300, 200);
    const QRect result =
        FenceGeometry::resize(start, FenceHandle::Left, QPoint(200, 0), QSize(160, 120));
    QCOMPARE(result, QRect(240, 100, 160, 200));
    QCOMPARE(result.left() + result.width(), 400);
}

void TestFenceGeometry::resizeClampsToMinimumFromNorth()
{
    const QRect start(100, 100, 300, 200);
    const QRect result =
        FenceGeometry::resize(start, FenceHandle::Top, QPoint(0, 150), QSize(160, 120));
    QCOMPARE(result, QRect(100, 180, 300, 120));
    QCOMPARE(result.top() + result.height(), 300);
}

void TestFenceGeometry::resizeOnNonResizeHandleIsNoOp()
{
    const QRect start(100, 100, 300, 200);
    // The title bar moves the fence; resize() must leave it alone.
    QCOMPARE(FenceGeometry::resize(start, FenceHandle::TitleBar, QPoint(50, 50), QSize(160, 120)),
             start);
    QCOMPARE(FenceGeometry::resize(start, FenceHandle::None, QPoint(50, 50), QSize(160, 120)),
             start);
}

void TestFenceGeometry::snapToScreenEdges()
{
    const QSize bounds(1920, 1080);
    bool snappedX = false;
    bool snappedY = false;
    const QRect result = FenceGeometry::snap(QRect(5, 5, 300, 200), {}, bounds, 8, &snappedX,
                                             &snappedY);
    QCOMPARE(result, QRect(0, 0, 300, 200));
    QVERIFY(snappedX);
    QVERIFY(snappedY);
}

void TestFenceGeometry::snapToOtherFenceEdge()
{
    const QSize bounds(1920, 1080);
    const QList<QRect> others{QRect(400, 100, 300, 200)};
    bool snappedX = false;
    bool snappedY = false;
    // Left edge 4 px away from the other fence's left edge -> snapped onto it.
    const QRect result =
        FenceGeometry::snap(QRect(396, 600, 300, 200), others, bounds, 8, &snappedX, &snappedY);
    QCOMPARE(result.x(), 400);
    QCOMPARE(result.y(), 600); // far from any Y target: untouched
    QVERIFY(snappedX);
    QVERIFY(!snappedY);
}

void TestFenceGeometry::snapToScreenCentre()
{
    const QSize bounds(1000, 800);
    bool snappedX = false;
    // Centre line 3 px off the screen centre -> pulled to 500.
    const QRect result =
        FenceGeometry::snap(QRect(347, 100, 300, 200), {}, bounds, 8, &snappedX, nullptr);
    QCOMPARE(result.x(), 350);
    QCOMPARE(result.x() + result.width() / 2, 500);
    QVERIFY(snappedX);
}

void TestFenceGeometry::snapLeavesFarAwayRectsAlone()
{
    const QSize bounds(1000, 800);
    bool snappedX = true;
    bool snappedY = true;
    const QRect moving(20, 20, 300, 200);
    const QRect result = FenceGeometry::snap(moving, {}, bounds, 8, &snappedX, &snappedY);
    QCOMPARE(result, moving);
    QVERIFY(!snappedX);
    QVERIFY(!snappedY);

    // A threshold of 0 disables snapping entirely.
    QCOMPARE(FenceGeometry::snap(QRect(1, 1, 300, 200), {}, bounds, 0), QRect(1, 1, 300, 200));
}

void TestFenceGeometry::snapReportsWhichAxisMoved()
{
    const QSize bounds(1920, 1080);
    bool snappedX = false;
    bool snappedY = false;
    // Already flush with the top-left corner: no movement, but the axes are
    // reported as aligned (that is what draws a snap guide).
    FenceGeometry::snap(QRect(0, 0, 300, 200), {}, bounds, 8, &snappedX, &snappedY);
    QVERIFY(snappedX);
    QVERIFY(snappedY);
}

void TestFenceGeometry::clampToBoundsKeepsRectInside()
{
    const QSize bounds(1000, 800);
    QCOMPARE(FenceGeometry::clampToBounds(QRect(100, 100, 300, 200), bounds),
             QRect(100, 100, 300, 200));
    // Pushed past the bottom-right corner: brought back fully on screen.
    QCOMPARE(FenceGeometry::clampToBounds(QRect(900, 700, 300, 200), bounds),
             QRect(700, 600, 300, 200));
    // Negative origin: clamped to 0.
    QCOMPARE(FenceGeometry::clampToBounds(QRect(-50, -50, 300, 200), bounds),
             QRect(0, 0, 300, 200));
}

void TestFenceGeometry::clampToBoundsShrinksOversizedRect()
{
    const QSize bounds(1000, 800);
    // A fence larger than the desktop is shrunk to fit before being positioned.
    QCOMPARE(FenceGeometry::clampToBounds(QRect(0, 0, 1200, 900), bounds),
             QRect(0, 0, 1000, 800));
}

// --- T-009: roll-up (F-020) ------------------------------------------------

void TestFenceGeometry::visibleRectReducesRolledFenceToItsTitleBar()
{
    const QRect stored(120, 90, 320, 240);
    // Unrolled: the visible rect is the stored rect, unchanged.
    QCOMPARE(FenceGeometry::visibleRect(stored, false), stored);
    // Rolled up: only the title bar is visible; x, y and width are untouched.
    const QRect rolled = FenceGeometry::visibleRect(stored, true);
    QCOMPARE(rolled, QRect(120, 90, 320, FenceGeometry::kTitleBarHeight));
    QCOMPARE(rolled.x(), stored.x());
    QCOMPARE(rolled.y(), stored.y());
    QCOMPARE(rolled.width(), stored.width());
}

void TestFenceGeometry::resizeFromTheRightNeverMovesTheTopLeft()
{
    // T-086: the screen's centre line (960) is near the fence's *centre* at some
    // widths; applyRequest() snapped the whole rect onto it, moving the top-left.
    const QRect screen(0, 0, 1920, 1080);
    const QRect start(700, 300, 500, 400);
    const QList<QRect> others{QRect(1260, 100, 300, 300)};
    for (int dx = -150; dx <= 900; dx += 7) {
        for (const FenceHandle h : {FenceHandle::Right, FenceHandle::Bottom, FenceHandle::BottomRight}) {
            const QPoint delta(dx, dx / 3);
            const QRect requested = FenceGeometry::resize(start, h, delta, QSize(120, 80));
            const QRect r = FenceGeometry::applyResize(start, false, h, requested, others, {screen}, screen,
                                                       QSize(120, 80), 12, true);
            QCOMPARE(r.topLeft(), start.topLeft());
            QVERIFY(r.right() < screen.width()); // the screen trims the edge, it does not push the fence
        }
    }
    // ... and the moving edge still snaps (to the other fence's left, 1260).
    const QRect near = FenceGeometry::resize(start, FenceHandle::Right, QPoint(55, 0), QSize(120, 80));
    QCOMPARE(FenceGeometry::applyResize(start, false, FenceHandle::Right, near, others, {screen}, screen,
                                        QSize(120, 80), 12, true)
                 .left() + FenceGeometry::applyResize(start, false, FenceHandle::Right, near, others, {screen},
                                                      screen, QSize(120, 80), 12, true)
                               .width(),
             1260);
}

void TestFenceGeometry::resizeFromTheLeftMovesOnlyTheLeftEdge()
{
    const QRect screen(0, 0, 1920, 1080);
    const QRect start(700, 300, 500, 400);
    for (int dx = -800; dx <= 600; dx += 11) {
        const QRect requested = FenceGeometry::resize(start, FenceHandle::Left, QPoint(dx, 0), QSize(120, 80));
        const QRect r = FenceGeometry::applyResize(start, false, FenceHandle::Left, requested, {}, {screen},
                                                   screen, QSize(120, 80), 12, true);
        QCOMPARE(r.left() + r.width(), start.left() + start.width()); // the right edge stays
        QCOMPARE(r.top(), start.top());
        QCOMPARE(r.height(), start.height());
        QVERIFY(r.left() >= 0 && r.width() >= 120);
    }
}

void TestFenceGeometry::applyRequestKeepsTheStoredHeightWhenRolled()
{
    // A rolled-up fence has no visible body: a resize or a move can only change
    // x, y and width. The expanded height survives for the next roll-down.
    const QRect stored(100, 100, 300, 200);
    const QRect result = FenceGeometry::applyRequest(stored, /*rolledUp=*/true,
                                                     QRect(50, 50, 400, 900), {}, QSize(1920, 1080),
                                                     QSize(160, 120), 8, /*snap=*/false);
    QCOMPARE(result, QRect(50, 50, 400, 200));
    QCOMPARE(result.height(), stored.height());
}

void TestFenceGeometry::applyRequestSnapsARolledFenceByItsTitleBar()
{
    // 5 px above the bottom edge: the *title bar* is what snaps flush with the
    // screen, not the (hidden) expanded body.
    const QRect stored(100, 100, 300, 200);
    const QRect result = FenceGeometry::applyRequest(stored, /*rolledUp=*/true,
                                                     QRect(100, 1045, 300, 200), {}, QSize(1920, 1080),
                                                     QSize(160, 120), 8, /*snap=*/true);
    // The title bar's bottom edge (exclusive) is flush with the screen bottom.
    QCOMPARE(result.y() + FenceGeometry::kTitleBarHeight, 1080);
    QCOMPARE(result.height(), 200);
}

void TestFenceGeometry::applyRequestKeepsARolledTitleBarOnScreen()
{
    // Dragged far below the screen: the title bar is pulled back so the fence
    // stays reachable, while the hidden body is allowed to hang off-screen.
    const QRect stored(100, 100, 300, 200);
    const QRect result = FenceGeometry::applyRequest(stored, /*rolledUp=*/true,
                                                     QRect(100, 2000, 300, 200), {}, QSize(1920, 1080),
                                                     QSize(160, 120), 8, /*snap=*/true);
    QCOMPARE(result.y(), 1080 - FenceGeometry::kTitleBarHeight);
    QCOMPARE(result.height(), 200);
}

void TestFenceGeometry::applyRequestStillClampsAnUnrolledFenceToTheMinimum()
{
    // The roll-up branch must not change the plain behaviour T-007 relies on.
    const QRect result = FenceGeometry::applyRequest(QRect(0, 0, 300, 200), /*rolledUp=*/false,
                                                     QRect(0, 0, 50, 40), {}, QSize(1920, 1080),
                                                     QSize(160, 120), 8, /*snap=*/true);
    QCOMPARE(result.size(), QSize(160, 120));
    QCOMPARE(result.topLeft(), QPoint(0, 0));
}

// --- T-066: where a brand-new fence goes ------------------------------------
//
// The area below is already margin-adjusted (what the caller passes): 900x600 at
// (80,80), fences 320x240, gap 24 -- so the candidates are (80,80), (424,80),
// (80,344) and (424,344), in reading order, and the third column would run off the
// right edge.
namespace {
const QRect kArea(80, 80, 900, 600);
const QSize kFence(320, 240);
const QRect kFirst(80, 80, 320, 240);
const QRect kSecond(424, 80, 320, 240);
const QRect kThird(80, 344, 320, 240);
const QRect kFourth(424, 344, 320, 240);
} // namespace

void TestFenceGeometry::freeSpotTakesTheFirstCandidateOnAnEmptyDesk()
{
    QCOMPARE(FenceGeometry::freeSpot({}, kFence, kArea), kFirst);
}

void TestFenceGeometry::freeSpotAvoidsAnExistingFence()
{
    // A fence where the first candidate is: the next one along, not on top of it.
    QCOMPARE(FenceGeometry::freeSpot({kFirst}, kFence, kArea), kSecond);
}

void TestFenceGeometry::freeSpotMovesToTheNextRow()
{
    // The whole first row taken: the search wraps, which is the part the old
    // "right of everything" policy got wrong (it wrapped to the same point every
    // time, and the container's clamp then made every later fence identical).
    QCOMPARE(FenceGeometry::freeSpot({kFirst, kSecond}, kFence, kArea), kThird);
}

void TestFenceGeometry::freeSpotSkipsTheColumnOfATallFence()
{
    // A fence taller than one row occupies the second row of its *own* column, so
    // the free spot is the other column -- not "the next row", blindly.
    const QRect tall(80, 80, 320, 528);
    QCOMPARE(FenceGeometry::freeSpot({tall, kSecond}, kFence, kArea), kFourth);
}

void TestFenceGeometry::freeSpotPicksTheLeastCoveredSpotWhenThereIsNoRoom()
{
    // Every candidate covered, and one of them only half: an overlap the user can
    // see and drag apart beats a fence hidden under another one.
    const QRect halfCovered(424, 344, 160, 240);
    const QRect answer = FenceGeometry::freeSpot({kFirst, kSecond, kThird, halfCovered},
                                                 kFence, kArea);
    QCOMPARE(answer, kFourth);
}

void TestFenceGeometry::freeSpotNeverLeavesTheArea()
{
    const QList<QList<QRect>> fixtures{
        {},
        {kFirst},
        {kFirst, kSecond},
        {kFirst, kSecond, kThird},
        {kFirst, kSecond, kThird, kFourth},
    };
    for (const QList<QRect>& others : fixtures) {
        const QRect spot = FenceGeometry::freeSpot(others, kFence, kArea);
        QVERIFY2(kArea.contains(spot), qPrintable(QStringLiteral("%1,%2 %3x%4 outside %5,%6 %7x%8")
                                                      .arg(spot.x()).arg(spot.y())
                                                      .arg(spot.width()).arg(spot.height())
                                                      .arg(kArea.x()).arg(kArea.y())
                                                      .arg(kArea.width()).arg(kArea.height())));
        QCOMPARE(spot.size(), kFence);
    }
}

void TestFenceGeometry::freeSpotStillAnswersWhenNothingFits()
{
    // A fence larger than the area: the first candidate, which the caller clamps.
    // (The area's edges are inclusive, so a fence exactly as large as the area does
    // *not* fit here -- the check is honest about which case it covers.)
    const QRect tightArea(0, 0, 100, 100);
    QCOMPARE(FenceGeometry::freeSpot({}, kFence, tightArea), QRect(0, 0, 320, 240));
}

void TestFenceGeometry::aMoveStopsAgainstAnotherFence()
{
    const QRect wall(400, 0, 200, 200);
    const QRect from(100, 50, 200, 100);
    // Dragged right into the wall: it stops with its right edge against it.
    const QRect stopped = FenceGeometry::avoidOverlap(from, from.translated(180, 10), {wall}, false);
    QCOMPARE(stopped.right(), wall.left() - 1);
    QCOMPARE(stopped.top(), 60); // the other axis follows the mouse
    QCOMPARE(stopped.size(), from.size());
    // Free movement is untouched.
    QCOMPARE(FenceGeometry::avoidOverlap(from, from.translated(50, 300), {wall}, false), from.translated(50, 300));
    // Past it entirely in one step: allowed (nothing covered).
    QCOMPARE(FenceGeometry::avoidOverlap(from, from.translated(500, 0), {wall}, false), from.translated(500, 0));
}

void TestFenceGeometry::aResizeStopsAgainstAnotherFence()
{
    const QRect wall(400, 0, 200, 200);
    const QRect from(100, 50, 200, 100);
    QRect wider = from;
    wider.setRight(500);
    const QRect stopped = FenceGeometry::avoidOverlap(from, wider, {wall}, true);
    QCOMPARE(stopped.left(), from.left());
    QCOMPARE(stopped.right(), wall.left() - 1);
}

void TestFenceGeometry::anOldOverlapNeitherBlocksNorMoves()
{
    const QRect other(150, 80, 200, 200);
    const QRect from(100, 50, 200, 100); // already over `other`
    QCOMPARE(FenceGeometry::avoidOverlap(from, from.translated(30, 30), {other}, false), from.translated(30, 30));
}

void TestFenceGeometry::aDrawnFenceGoesToTheNearestFreePlace()
{
    const QRect bounds(0, 0, 1920, 1080);
    const QRect other(500, 300, 300, 200);
    const QRect drawn(700, 350, 200, 150); // over the right part of `other`
    const QRect placed = FenceGeometry::nearestFree(drawn, {other}, bounds);
    QVERIFY(!placed.intersects(other));
    QVERIFY(bounds.contains(placed));
    QCOMPARE(placed.size(), drawn.size());
    QVERIFY((placed.center() - drawn.center()).manhattanLength() <= 160);
    QCOMPARE(FenceGeometry::nearestFree(QRect(0, 0, 100, 100), {other}, bounds), QRect(0, 0, 100, 100));
}

void TestFenceGeometry::aRolledUpFenceKeepsItsEdge()
{
    // Session 73: the strip sits on the kept edge; unrolled, the stored rect.
    using E = FenceGeometry::RollEdge;
    const QRect stored(100, 50, 300, 200);
    const int t = FenceGeometry::visibleRect(stored, true).height();
    QCOMPARE(FenceGeometry::visibleRect(stored, true, E::Top), QRect(100, 50, 300, t));
    QCOMPARE(FenceGeometry::visibleRect(stored, true, E::Bottom), QRect(100, 250 - t, 300, t));
    QCOMPARE(FenceGeometry::visibleRect(stored, true, E::Left), QRect(100, 50, t, 200));
    QCOMPARE(FenceGeometry::visibleRect(stored, true, E::Right), QRect(400 - t, 50, t, 200));
    QCOMPARE(FenceGeometry::visibleRect(stored, false, E::Right), stored);
}

QTEST_MAIN(TestFenceGeometry)
#include "test_fence_geometry.moc"