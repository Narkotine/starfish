#include <QtTest>

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include "ui/fence_geometry.hpp"
#include "ui/screen_layout.hpp"

using namespace starfish::ui;

namespace {

// The canonical two-monitor arrangement these tests reason about (T-014):
//
//   +-----------------------------+-----------------+
//   |  A  \\.\DISPLAY1            |  B  DISPLAY2    |
//   |  2560x1440, 100 %           |  1920x1080,150% |
//   +-----------------------------+-----------------+
//                                 |   dead zone     |
//                                 +-----------------+
//
// B is shorter than A, so the area under B belongs to no monitor at all. That
// dead zone is the whole reason a fence is clamped to *its* screen rather than
// to the bounding box of the virtual desktop: a fence landing there would be
// drawn by nothing and impossible to grab with the mouse.
ScreenInfo screenA()
{
    ScreenInfo screen;
    screen.geometry = QRect(0, 0, 2560, 1440);
    screen.devicePixelRatio = 1.0;
    screen.name = QStringLiteral("\\\\.\\DISPLAY1");
    screen.primary = true;
    return screen;
}

ScreenInfo screenB()
{
    ScreenInfo screen;
    screen.geometry = QRect(2560, 0, 1920, 1080);
    screen.devicePixelRatio = 1.5;
    screen.name = QStringLiteral("\\\\.\\DISPLAY2");
    screen.primary = false;
    return screen;
}

ScreenLayout twoScreens()
{
    ScreenLayout layout;
    layout.addScreen(screenA());
    layout.addScreen(screenB());
    return layout;
}

} // namespace

// Pure multi-monitor model (T-014 / F-032). No QScreen and no Win32: the
// platform layer hands over a physical, virtual-screen description of the
// monitors, and every rule below is decided here -- which is what makes a
// mixed-DPI two-monitor setup testable on a machine that owns one monitor.
class TestScreenLayout : public QObject
{
    Q_OBJECT

private slots:
    void emptyLayoutKnowsNothing();
    void singleScreenIsTheOriginAnchoredCase();
    void boundsIsTheUnionOfEveryScreen();
    void screensAreFoundByNameAndByPoint();
    void screenForRectUsesTheCentre();
    void deadZonePointFallsBackToTheNearestScreen();
    void deadZoneRectFallsBackToTheNearestScreen();
    void primaryFallsBackToTheFirstScreen();
    void clampIntoRectHonoursNegativeOrigins();
    void clampIntoRectShrinksAnOversizedRect();
    void devicePixelRatioFollowsTheScreen();
    void clampRectForIsTheOwningScreen();
    void restoreKeepsARectThatIsStillOnAScreen();
    void restoreUsesTheRecordedMonitorWhenTheRectIsOffScreen();
    void restoreFallsBackToTheNearestScreenWhenTheMonitorIsGone();
    void restoreShrinksAFenceThatNoLongerFits();
    void snapDocksToTheInnerEdgeOfTheNeighbouringMonitor();
    void snapToTheOuterEdgeOfTheSecondScreen();
    void snapWithNoFramesIsATranslationOnly();
    void applyRequestClampsInsideTheOwningScreen();
    void applyRequestSnapsARolledTitleBarToTheInnerEdge();
};

void TestScreenLayout::emptyLayoutKnowsNothing()
{
    const ScreenLayout layout;
    QVERIFY(layout.isEmpty());
    QCOMPARE(layout.count(), 0);
    QVERIFY(layout.frames().isEmpty());
    QVERIFY(layout.bounds().isNull());
    QVERIFY(layout.primary() == nullptr);
    QVERIFY(layout.screenNamed(QStringLiteral("\\\\.\\DISPLAY1")) == nullptr);
    QVERIFY(layout.screenForRect(QRect(10, 10, 100, 100)) == nullptr);
    // No scaling to invent, and no name to persist.
    QCOMPARE(layout.devicePixelRatioFor(QRect(0, 0, 100, 100)), 1.0);
    QVERIFY(layout.nameFor(QRect(0, 0, 100, 100)).isEmpty());
    QVERIFY(layout.clampRectFor(QRect(0, 0, 100, 100)).isNull());
    // A restore with no information must not move a fence.
    const QRect stored(1234, 567, 320, 240);
    QCOMPARE(layout.restoreRect(stored, QStringLiteral("\\\\.\\DISPLAY9")), stored);
}

void TestScreenLayout::singleScreenIsTheOriginAnchoredCase()
{
    const ScreenLayout layout = ScreenLayout::single(QSize(1920, 1080));
    QCOMPARE(layout.count(), 1);
    QCOMPARE(layout.bounds(), QRect(0, 0, 1920, 1080));
    QCOMPARE(layout.frames(), QList<QRect>{QRect(0, 0, 1920, 1080)});
    QVERIFY(layout.primary() != nullptr);
    QVERIFY(layout.primary()->primary);
    QCOMPARE(layout.primary()->devicePixelRatio, 1.0);
    QCOMPARE(layout.screenForRect(QRect(100, 100, 320, 240))->geometry,
             QRect(0, 0, 1920, 1080));
}

void TestScreenLayout::boundsIsTheUnionOfEveryScreen()
{
    const ScreenLayout layout = twoScreens();
    QCOMPARE(layout.count(), 2);
    // The bounding box covers both monitors -- including the dead zone under B.
    QCOMPARE(layout.bounds(), QRect(0, 0, 4480, 1440));
    QCOMPARE(layout.frames().size(), 2);
    QCOMPARE(layout.frames().at(0), QRect(0, 0, 2560, 1440));
    QCOMPARE(layout.frames().at(1), QRect(2560, 0, 1920, 1080));
}

void TestScreenLayout::screensAreFoundByNameAndByPoint()
{
    const ScreenLayout layout = twoScreens();
    QCOMPARE(layout.primary()->name, QStringLiteral("\\\\.\\DISPLAY1"));
    QVERIFY(layout.screenNamed(QStringLiteral("\\\\.\\DISPLAY2")) != nullptr);
    QCOMPARE(layout.screenNamed(QStringLiteral("\\\\.\\DISPLAY2"))->geometry,
             QRect(2560, 0, 1920, 1080));
    QVERIFY(layout.screenNamed(QStringLiteral("\\\\.\\DISPLAY7")) == nullptr);
    QVERIFY(layout.screenNamed(QString()) == nullptr);

    QCOMPARE(layout.screenAt(QPoint(2500, 1400))->name, QStringLiteral("\\\\.\\DISPLAY1"));
    QCOMPARE(layout.screenAt(QPoint(2560, 0))->name, QStringLiteral("\\\\.\\DISPLAY2"));
    // Below B, to the right of A: inside the bounding box, inside no screen.
    QVERIFY(layout.screenAt(QPoint(3000, 1300)) == nullptr);
}

void TestScreenLayout::screenForRectUsesTheCentre()
{
    const ScreenLayout layout = twoScreens();
    QCOMPARE(layout.screenForRect(QRect(500, 500, 320, 240))->name,
             QStringLiteral("\\\\.\\DISPLAY1"));
    QCOMPARE(layout.screenForRect(QRect(3000, 200, 320, 240))->name,
             QStringLiteral("\\\\.\\DISPLAY2"));
    // Straddling the boundary, but its centre (2660, 220) is on B: B owns it.
    QCOMPARE(layout.screenForRect(QRect(2500, 100, 320, 240))->name,
             QStringLiteral("\\\\.\\DISPLAY2"));
    // The same rect one pixel of centre further left belongs to A.
    QCOMPARE(layout.screenForRect(QRect(2400, 100, 320, 240))->name,
             QStringLiteral("\\\\.\\DISPLAY1"));
}

void TestScreenLayout::deadZonePointFallsBackToTheNearestScreen()
{
    const ScreenLayout layout = twoScreens();
    // (3000, 1300): just under B (220 px) versus 441 px to the right of A -> B.
    QCOMPARE(layout.nearestScreen(QPoint(3000, 1300))->name, QStringLiteral("\\\\.\\DISPLAY2"));
    // (2700, 1400): inside A's vertical span (0 px away) versus 321 px below B.
    QCOMPARE(layout.nearestScreen(QPoint(2700, 1400))->name, QStringLiteral("\\\\.\\DISPLAY1"));
    // A point inside a screen is its own nearest screen.
    QCOMPARE(layout.nearestScreen(QPoint(10, 10))->name, QStringLiteral("\\\\.\\DISPLAY1"));
}

void TestScreenLayout::deadZoneRectFallsBackToTheNearestScreen()
{
    const ScreenLayout layout = twoScreens();
    // Centre (2760, 1220) is under B, 141 px from B and 201 px from A.
    const ScreenInfo* owner = layout.screenForRect(QRect(2600, 1100, 320, 240));
    QCOMPARE(owner->name, QStringLiteral("\\\\.\\DISPLAY2"));
    // ... and clamping brings it back onto that screen, not into the dead zone.
    const QRect clamped = layout.clampRectFor(QRect(2600, 1100, 320, 240));
    QCOMPARE(clamped, QRect(2560, 0, 1920, 1080));
}

void TestScreenLayout::primaryFallsBackToTheFirstScreen()
{
    ScreenLayout layout;
    ScreenInfo first;
    first.geometry = QRect(0, 0, 800, 600);
    first.name = QStringLiteral("first");
    ScreenInfo second;
    second.geometry = QRect(800, 0, 800, 600);
    second.name = QStringLiteral("second");
    layout.addScreen(first);
    layout.addScreen(second);
    // No screen is flagged primary: the first known one answers.
    QCOMPARE(layout.primary()->name, QStringLiteral("first"));
}

void TestScreenLayout::clampIntoRectHonoursNegativeOrigins()
{
    // A monitor Windows has placed to the left of the primary one: negative x.
    const QRect leftScreen(-1920, 0, 1920, 1080);
    QCOMPARE(FenceGeometry::clampIntoRect(QRect(-1900, 50, 320, 240), leftScreen),
             QRect(-1900, 50, 320, 240));
    // Dragged past the top-left corner of that monitor.
    QCOMPARE(FenceGeometry::clampIntoRect(QRect(-2000, -50, 320, 240), leftScreen),
             QRect(-1920, 0, 320, 240));
    // ... and past its bottom-right corner.
    QCOMPARE(FenceGeometry::clampIntoRect(QRect(-100, 1000, 320, 240), leftScreen),
             QRect(-320, 840, 320, 240));
}

void TestScreenLayout::clampIntoRectShrinksAnOversizedRect()
{
    const QRect screen(2560, 0, 1920, 1080);
    QCOMPARE(FenceGeometry::clampIntoRect(QRect(2600, 100, 4000, 3000), screen),
             QRect(2560, 0, 1920, 1080));
    // An empty area means "no constraint" rather than "move it to the origin".
    const QRect untouched(123, 456, 100, 100);
    QCOMPARE(FenceGeometry::clampIntoRect(untouched, QRect()), untouched);
}

void TestScreenLayout::devicePixelRatioFollowsTheScreen()
{
    const ScreenLayout layout = twoScreens();
    QCOMPARE(layout.devicePixelRatioFor(QRect(500, 500, 320, 240)), 1.0);
    QCOMPARE(layout.devicePixelRatioFor(QRect(3000, 200, 320, 240)), 1.5);
    QCOMPARE(layout.nameFor(QRect(500, 500, 320, 240)), QStringLiteral("\\\\.\\DISPLAY1"));
    QCOMPARE(layout.nameFor(QRect(3000, 200, 320, 240)), QStringLiteral("\\\\.\\DISPLAY2"));
}

void TestScreenLayout::clampRectForIsTheOwningScreen()
{
    const ScreenLayout layout = twoScreens();
    QCOMPARE(layout.clampRectFor(QRect(500, 500, 320, 240)), QRect(0, 0, 2560, 1440));
    QCOMPARE(layout.clampRectFor(QRect(3000, 200, 320, 240)), QRect(2560, 0, 1920, 1080));
}

void TestScreenLayout::restoreKeepsARectThatIsStillOnAScreen()
{
    const ScreenLayout layout = twoScreens();
    // Nothing moved since the fence was saved: a restore is an identity.
    const QRect stored(3000, 400, 320, 240);
    QCOMPARE(layout.restoreRect(stored, QStringLiteral("\\\\.\\DISPLAY2")), stored);
    // Even a stale monitor name does not move a fence that is on a screen.
    QCOMPARE(layout.restoreRect(stored, QStringLiteral("\\\\.\\DISPLAY1")), stored);
}

void TestScreenLayout::restoreUsesTheRecordedMonitorWhenTheRectIsOffScreen()
{
    // Same fence, same document, but the second monitor now sits to the *left*
    // of the primary one: the stored coordinates land on no screen at all.
    ScreenLayout layout;
    layout.addScreen(screenA());
    ScreenInfo moved = screenB();
    moved.geometry = QRect(-1920, 0, 1920, 1080);
    layout.addScreen(moved);

    const QRect stored(3000, 400, 320, 240);
    QVERIFY(layout.screenAt(stored.center()) == nullptr);
    // Its recorded monitor still exists, so that is where it comes back.
    QCOMPARE(layout.restoreRect(stored, QStringLiteral("\\\\.\\DISPLAY2")),
             QRect(-320, 400, 320, 240));
}

void TestScreenLayout::restoreFallsBackToTheNearestScreenWhenTheMonitorIsGone()
{
    // The second monitor has been unplugged: only A is left.
    const ScreenLayout layout = ScreenLayout::single(QSize(2560, 1440));
    const QRect stored(3000, 400, 320, 240);
    // A fence must never come back off-screen and unreachable.
    const QRect restored = layout.restoreRect(stored, QStringLiteral("\\\\.\\DISPLAY2"));
    QCOMPARE(restored, QRect(2240, 400, 320, 240));
    QVERIFY(layout.screenForRect(restored) != nullptr);
}

void TestScreenLayout::restoreShrinksAFenceThatNoLongerFits()
{
    // Saved on a 4K monitor, restored on the smaller one it now finds itself on.
    const ScreenLayout layout = twoScreens();
    const QRect stored(2560, 0, 3840, 2160);
    const QRect restored = layout.restoreRect(stored, QStringLiteral("\\\\.\\DISPLAY2"));
    QCOMPARE(restored, QRect(2560, 0, 1920, 1080));
}

void TestScreenLayout::snapDocksToTheInnerEdgeOfTheNeighbouringMonitor()
{
    const ScreenLayout layout = twoScreens();
    const QSize bounds = layout.bounds().size();
    // Left edge 5 px right of the boundary between the two monitors (x = 2560).
    const QRect moving(2555, 300, 320, 240);

    // The frames version docks the fence to the edge of the *other monitor*.
    const QRect snapped = FenceGeometry::snap(moving, {}, layout.frames(), 8);
    QCOMPARE(snapped.x(), 2560);

    // Premise of this test: the single-bounding-box rule cannot see that edge
    // at all. It only knows x = 0, x = 2240 (the centre of the 4480 px box) and
    // x = 4480 -- none of which is within the threshold -- so the fence would
    // stay where it was dropped. The test would go red if the frames were ever
    // replaced by the bounding box again.
    const QRect boundingOnly = FenceGeometry::snap(moving, {}, bounds, 8);
    QCOMPARE(boundingOnly, moving);
}

void TestScreenLayout::snapToTheOuterEdgeOfTheSecondScreen()
{
    const ScreenLayout layout = twoScreens();
    const QRect moving(4157, 300, 320, 240); // right edge 3 px short of 4480
    const QRect snapped = FenceGeometry::snap(moving, {}, layout.frames(), 8);
    QCOMPARE(snapped.x() + snapped.width(), 4480);

    // The centre of the second monitor is a target too: its centre line is
    // 3520, and this rect's centre sits 8 px short of it.
    const QRect centred(3352, 300, 320, 240);
    const QRect ontoCentre = FenceGeometry::snap(centred, {}, layout.frames(), 8);
    QCOMPARE(ontoCentre.x() + ontoCentre.width() / 2, 3520);
}

void TestScreenLayout::snapWithNoFramesIsATranslationOnly()
{
    const QRect moving(5, 5, 300, 200);
    bool snappedX = true;
    bool snappedY = true;
    QCOMPARE(FenceGeometry::snap(moving, {}, QList<QRect>{}, 8, &snappedX, &snappedY), moving);
    QVERIFY(!snappedX);
    QVERIFY(!snappedY);
}

void TestScreenLayout::applyRequestClampsInsideTheOwningScreen()
{
    const ScreenLayout layout = twoScreens();
    const QSize minSize(160, 120);

    // Requested below B's bottom edge: brought back up, inside B only. The
    // bounding box would have accepted y = 900 (A is 1440 tall).
    const QRect target = FenceGeometry::applyRequest(QRect(3000, 200, 320, 240), false,
                                                     QRect(3000, 900, 320, 240), {},
                                                     layout.frames(),
                                                     layout.clampRectFor(QRect(3000, 900, 320, 240)),
                                                     minSize, 8, /*snap=*/false);
    QCOMPARE(target, QRect(3000, 840, 320, 240));

    // Requested past B's right edge: pulled back inside B.
    const QRect far = QRect(5000, 200, 320, 240);
    const QRect clamped = FenceGeometry::applyRequest(QRect(3000, 200, 320, 240), false, far, {},
                                                      layout.frames(), layout.clampRectFor(far),
                                                      minSize, 8, /*snap=*/true);
    QCOMPARE(clamped, QRect(4160, 200, 320, 240));
}

void TestScreenLayout::applyRequestSnapsARolledTitleBarToTheInnerEdge()
{
    const ScreenLayout layout = twoScreens();
    // Rolled up and dragged next to B's left edge: the *title bar* docks, while
    // the stored (expanded) height survives.
    const QRect stored(2000, 100, 320, 240);
    const QRect requested(2237, 100, 320, 240);
    const QRect target = FenceGeometry::applyRequest(stored, /*rolledUp=*/true, requested, {},
                                                     layout.frames(), layout.clampRectFor(requested),
                                                     QSize(160, 120), 8, /*snap=*/true);
    QCOMPARE(target.x(), 2240);
    QCOMPARE(target.x() + target.width(), 2560);
    QCOMPARE(target.height(), stored.height());
}

QTEST_MAIN(TestScreenLayout)
#include "test_screen_layout.moc"
