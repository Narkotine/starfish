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

QTEST_MAIN(TestFenceGeometry)
#include "test_fence_geometry.moc"
