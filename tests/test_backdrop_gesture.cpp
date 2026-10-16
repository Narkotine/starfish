#include <QtTest>

#include <QList>
#include <QRect>
#include <QVector>

#include "core/desktop_item.hpp"
#include "ui/backdrop_gesture.hpp"
#include "ui/backdrop_layout.hpp"

using namespace starfish::ui;
using starfish::core::DesktopItem;

namespace {

DesktopItem item(const QString& name)
{
    DesktopItem it;
    it.name = name;
    it.path = QStringLiteral("C:\\Users\\u\\Desktop\\") + name + QStringLiteral(".txt");
    it.fileName = name + QStringLiteral(".txt");
    return it;
}

QList<DesktopItem> manyItems(int count)
{
    QList<DesktopItem> items;
    for (int i = 0; i < count; ++i)
        items.append(item(QStringLiteral("item%1").arg(i)));
    return items;
}

// The cells the backdrop actually lays out, so the hit test is exercised against
// the geometry the desktop really uses rather than against rectangles invented
// here.
QVector<BackdropCell> cellsOf(int count, const QRect& area)
{
    return BackdropLayout::compute(area, BackdropLayout::Metrics{}, manyItems(count));
}

} // namespace

// T-041: the pure half of the empty desktop's gestures -- F-001 (drag on the
// bare desktop draws a fence) and F-022 (double-click hides or shows them all).
//
// The rules worth testing are the ones that decide *whether* a gesture happened
// at all: a click is not a draw, a wobble is not a draw, a press on one of the
// icons is not ours, and a rectangle too small to be a fence is refused rather
// than grown. Each of those is a way to create something the user did not ask
// for, which on a desktop is the expensive kind of bug.
class TestBackdropGesture : public QObject
{
    Q_OBJECT

private slots:
    // --- F-001: drawing a fence --------------------------------------------

    void aDragOnBareDesktopDrawsThatRectangle()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.press(QPoint(100, 100), false).action, BackdropGesture::Action::None);
        QCOMPARE(gesture.move(QPoint(300, 260)).action, BackdropGesture::Action::Drawing);
        const BackdropGesture::Result result = gesture.release(QPoint(300, 260));
        QCOMPARE(result.action, BackdropGesture::Action::FenceDrawn);
        QCOMPARE(result.rect, QRect(100, 100, 200, 160));
        QVERIFY(!gesture.drawing());
    }

    void aDragBackwardsDescribesTheSameRectangle()
    {
        // Right-to-left and bottom-to-top are the same gesture: the rectangle is
        // normalised, and only the rectangle is handed on.
        BackdropGesture gesture;
        gesture.press(QPoint(300, 260), false);
        gesture.move(QPoint(100, 100));
        const BackdropGesture::Result result = gesture.release(QPoint(100, 100));
        QCOMPARE(result.action, BackdropGesture::Action::FenceDrawn);
        QCOMPARE(result.rect, QRect(100, 100, 200, 160));
    }

    void aDragThatIsExactlyTheMinimumIsAccepted()
    {
        // The boundary is inclusive: the smallest fence the container keeps is
        // the smallest draw that is accepted, not one pixel more.
        BackdropGesture gesture;
        gesture.press(QPoint(40, 40), false);
        gesture.move(QPoint(200, 160));
        const BackdropGesture::Result result = gesture.release(QPoint(200, 160));
        QCOMPARE(result.action, BackdropGesture::Action::FenceDrawn);
        QCOMPARE(result.rect, QRect(40, 40, 160, 120));
    }

    void aRectangleSmallerThanAFenceIsRefused()
    {
        // A draw that small is a slip of the hand. Growing it to the minimum
        // would put a fence somewhere the user never drew one.
        BackdropGesture gesture;
        gesture.press(QPoint(40, 40), false);
        gesture.move(QPoint(120, 120));
        const BackdropGesture::Result result = gesture.release(QPoint(120, 120));
        QCOMPARE(result.action, BackdropGesture::Action::Cancelled);
        QVERIFY(!gesture.drawing());
    }

    void theMetricsDecideWhatAFenceIs()
    {
        // The premise of the check above: the refusal comes from the metric and
        // not from a size written into the machine. With a smaller minimum the
        // same draw is accepted, so the caller's minimum really is the rule --
        // and the container's minimum is what the product passes in.
        BackdropGesture gesture;
        BackdropGesture::Metrics metrics;
        metrics.minimumFence = QSize(40, 40);
        gesture.setMetrics(metrics);

        gesture.press(QPoint(40, 40), false);
        gesture.move(QPoint(120, 120));
        QCOMPARE(gesture.release(QPoint(120, 120)).action, BackdropGesture::Action::FenceDrawn);
    }

    // --- what must NOT start a draw ----------------------------------------

    void aClickIsNotAGesture()
    {
        BackdropGesture gesture;
        gesture.press(QPoint(500, 500), false);
        const BackdropGesture::Result result = gesture.release(QPoint(500, 500));
        QCOMPARE(result.action, BackdropGesture::Action::None);
        QVERIFY(!gesture.armed());
    }

    void aWobbleIsStillAClick()
    {
        BackdropGesture gesture;
        gesture.press(QPoint(500, 500), false);
        QCOMPARE(gesture.move(QPoint(503, 502)).action, BackdropGesture::Action::None);
        QCOMPARE(gesture.release(QPoint(503, 502)).action, BackdropGesture::Action::None);
    }

    void theThresholdIsWhatTurnsAPressIntoADraw()
    {
        // One pixel either side of the threshold, so the number is measured
        // rather than assumed. Below it nothing is drawn, at it the draw starts.
        BackdropGesture gesture;
        gesture.press(QPoint(500, 500), false);
        QCOMPARE(gesture.move(QPoint(505, 500)).action, BackdropGesture::Action::None);
        QVERIFY(!gesture.drawing());
        QCOMPARE(gesture.move(QPoint(506, 500)).action, BackdropGesture::Action::Drawing);
        QVERIFY(gesture.drawing());
    }

    void aPressOnAnIconStartsNothing()
    {
        // The desktop's own icons belong to the desktop: their behaviour is
        // T-042's, and a fence drawn from on top of one is a fence the user did
        // not ask for.
        BackdropGesture gesture;
        QCOMPARE(gesture.press(QPoint(500, 500), true).action, BackdropGesture::Action::None);
        QVERIFY(!gesture.armed());
        QCOMPARE(gesture.move(QPoint(900, 900)).action, BackdropGesture::Action::None);
        QCOMPARE(gesture.release(QPoint(900, 900)).action, BackdropGesture::Action::None);
        QVERIFY(!gesture.drawing());
    }

    void aMoveWithoutAPressDoesNothing()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.move(QPoint(700, 700)).action, BackdropGesture::Action::None);
        QVERIFY(!gesture.drawing());
    }

    // --- F-022: quick hide --------------------------------------------------

    void aDoubleClickOnBareDesktopAsksForQuickHide()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.doubleClick(QPoint(600, 600), false).action,
                 BackdropGesture::Action::ToggleVisibility);
    }

    void aDoubleClickOnAnIconIsNotOurs()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.doubleClick(QPoint(600, 600), true).action,
                 BackdropGesture::Action::None);
    }

    void aDoubleClickEndsAnArmedDraw()
    {
        // The real sequence is press, release, double-click, release: whatever
        // the first press armed has to be gone by the time the trailing release
        // arrives, or the desktop would sprout a fence out of a double-click.
        BackdropGesture gesture;
        gesture.press(QPoint(600, 600), false);
        gesture.release(QPoint(600, 600));
        QCOMPARE(gesture.doubleClick(QPoint(600, 600), false).action,
                 BackdropGesture::Action::ToggleVisibility);
        QVERIFY(!gesture.armed());
        QCOMPARE(gesture.release(QPoint(600, 600)).action, BackdropGesture::Action::None);
    }

    // --- the rubber band ----------------------------------------------------

    void theRubberBandIsOnlyThereWhileDrawing()
    {
        BackdropGesture gesture;
        QVERIFY(gesture.rubberBand().isNull());
        gesture.press(QPoint(100, 100), false);
        QVERIFY(gesture.rubberBand().isNull()); // armed, but nothing drawn yet
        gesture.move(QPoint(300, 260));
        QCOMPARE(gesture.rubberBand(), QRect(100, 100, 200, 160));
        gesture.release(QPoint(300, 260));
        QVERIFY(gesture.rubberBand().isNull());
    }

    void cancelEndsADrawWithoutCreatingAnything()
    {
        // The mouse capture was stolen, or the mode was turned off: the draw
        // stops where it was, and the release that may still arrive creates
        // nothing.
        BackdropGesture gesture;
        gesture.press(QPoint(100, 100), false);
        gesture.move(QPoint(400, 400));
        QCOMPARE(gesture.cancel().action, BackdropGesture::Action::Cancelled);
        QVERIFY(!gesture.drawing());
        QVERIFY(gesture.rubberBand().isNull());
        QCOMPARE(gesture.release(QPoint(400, 400)).action, BackdropGesture::Action::None);
    }

    void cancellingNothingIsNotWorthARepaint()
    {
        // The caller repaints on Cancelled, so an idle cancel has to be silent.
        BackdropGesture gesture;
        QCOMPARE(gesture.cancel().action, BackdropGesture::Action::None);
    }

    // --- which icon is under the point --------------------------------------

    void anIconIsFoundByItsIconOrItsCaption()
    {
        const QRect area(0, 0, 1920, 1080);
        const QVector<BackdropCell> cells = cellsOf(16, area);
        QCOMPARE(cells.size(), 16);

        const QRect firstIcon = cells.at(0).icon;
        const QRect firstLabel = cells.at(0).label;
        QCOMPARE(BackdropGesture::iconAt(cells, firstIcon.center()), 0);
        // The caption band is part of the icon: clicking a name is clicking what
        // it names.
        QCOMPARE(BackdropGesture::iconAt(cells, firstLabel.center()), 0);

        // Bare wallpaper is bare wallpaper.
        const int gapX = cells.at(0).label.right() + 3;
        QCOMPARE(BackdropGesture::iconAt(cells, QPoint(gapX, firstIcon.center().y())), -1);
        QCOMPARE(BackdropGesture::iconAt(cells, QPoint(1800, 1000)), -1);
    }

    void theCellsCarryVirtualScreenPixels()
    {
        // A monitor left of the primary one has negative coordinates (T-014),
        // and the gesture is measured in the same space as the cells -- so the
        // hit test has to work there too, not only at the origin.
        const QRect area(-1920, 0, 1920, 1080);
        const QVector<BackdropCell> cells = cellsOf(4, area);
        QCOMPARE(cells.size(), 4);
        QCOMPARE(BackdropGesture::iconAt(cells, cells.at(0).icon.center()), 0);
        QVERIFY(cells.at(0).icon.x() < 0);
    }
};

QTEST_MAIN(TestBackdropGesture)
#include "test_backdrop_gesture.moc"
