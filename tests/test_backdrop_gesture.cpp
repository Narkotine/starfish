#include <QtTest>

#include <QList>
#include <QRect>
#include <QVector>

#include "core/desktop_item.hpp"
#include "ui/backdrop_gesture.hpp"
#include "ui/backdrop_layout.hpp"

using namespace starfish::ui;
using starfish::core::DesktopItem;

using Button = BackdropGesture::Button;
using Action = BackdropGesture::Action;

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

// T-041 / T-055: the pure half of the empty desktop's gestures -- F-001 (a
// right-drag draws a fence, and the gesture ends on the desktop's own menu) and
// F-022 (a left double-click hides or shows every fence).
//
// The rules worth testing are the ones that decide *whether* a gesture happened
// at all: a click is not a draw, a wobble is not a draw, a press on one of the
// icons is not ours, the left button draws nothing, and a rectangle too small to
// be a fence is never offered. Each of those is a way to create something the
// user did not ask for, which on a desktop is the expensive kind of bug.
class TestBackdropGesture : public QObject
{
    Q_OBJECT

private slots:
    // --- F-001: drawing a fence --------------------------------------------

    void aRightDragDrawsThatRectangle()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.press(QPoint(100, 100), Button::Right, false).action, Action::None);
        QCOMPARE(gesture.move(QPoint(300, 260)).action, Action::Drawing);
        const BackdropGesture::Result result = gesture.release(QPoint(300, 260), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QCOMPARE(result.rect, QRect(100, 100, 200, 160));
        QCOMPARE(result.point, QPoint(300, 260));
        QVERIFY(!gesture.drawing());
    }

    void aDragBackwardsDescribesTheSameRectangle()
    {
        // Right-to-left and bottom-to-top are the same gesture: the rectangle is
        // normalised, and only the rectangle is handed on.
        BackdropGesture gesture;
        gesture.press(QPoint(300, 260), Button::Right, false);
        gesture.move(QPoint(100, 100));
        const BackdropGesture::Result result = gesture.release(QPoint(100, 100), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QCOMPARE(result.rect, QRect(100, 100, 200, 160));
    }

    void aDragThatIsExactlyTheMinimumIsOffered()
    {
        // The boundary is inclusive: the smallest fence the container keeps is
        // the smallest draw that is offered, not one pixel more.
        BackdropGesture gesture;
        gesture.press(QPoint(40, 40), Button::Right, false);
        gesture.move(QPoint(200, 160));
        const BackdropGesture::Result result = gesture.release(QPoint(200, 160), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QCOMPARE(result.rect, QRect(40, 40, 160, 120));
    }

    void aRectangleSmallerThanAFenceIsNeverOffered()
    {
        // A draw that small is a slip of the hand, so no rectangle is proposed --
        // the menu still opens, because the user did right-click the desktop, but
        // it has no fence to offer. Growing it to the minimum would put a fence
        // somewhere the user never drew one.
        BackdropGesture gesture;
        gesture.press(QPoint(40, 40), Button::Right, false);
        gesture.move(QPoint(120, 120));
        const BackdropGesture::Result result = gesture.release(QPoint(120, 120), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QVERIFY(result.rect.isNull());
        QCOMPARE(result.point, QPoint(120, 120));
    }

    void theMetricsDecideWhatAFenceIs()
    {
        // The premise of the check above: the refusal comes from the metric and
        // not from a size written into the machine. With a smaller minimum the
        // same draw is offered, so the caller's minimum really is the rule -- and
        // the container's minimum is what the product passes in.
        BackdropGesture gesture;
        BackdropGesture::Metrics metrics;
        metrics.minimumFence = QSize(40, 40);
        gesture.setMetrics(metrics);

        gesture.press(QPoint(40, 40), Button::Right, false);
        gesture.move(QPoint(120, 120));
        const BackdropGesture::Result result = gesture.release(QPoint(120, 120), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QCOMPARE(result.rect, QRect(40, 40, 80, 80));
    }

    // --- the desktop's own menu (T-054 / T-055) ----------------------------

    void aPlainRightClickOpensTheMenuWithNoRectangle()
    {
        // This is the desktop menu T-039 had taken away (T-054), restored by the
        // same gesture that draws a fence: a right-click that draws nothing still
        // has to reach it.
        BackdropGesture gesture;
        gesture.press(QPoint(500, 500), Button::Right, false);
        const BackdropGesture::Result result = gesture.release(QPoint(500, 500), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QVERIFY(result.rect.isNull());
        QCOMPARE(result.point, QPoint(500, 500));
    }

    void aWobbleIsStillAPlainRightClick()
    {
        BackdropGesture gesture;
        gesture.press(QPoint(500, 500), Button::Right, false);
        QCOMPARE(gesture.move(QPoint(503, 502)).action, Action::None);
        const BackdropGesture::Result result = gesture.release(QPoint(503, 502), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QVERIFY(result.rect.isNull());
    }

    void theThresholdIsWhatTurnsAPressIntoADraw()
    {
        // One pixel either side of the threshold, so the number is measured
        // rather than assumed. Below it nothing is drawn, at it the draw starts.
        BackdropGesture gesture;
        gesture.press(QPoint(500, 500), Button::Right, false);
        QCOMPARE(gesture.move(QPoint(505, 500)).action, Action::None);
        QVERIFY(!gesture.drawing());
        QCOMPARE(gesture.move(QPoint(506, 500)).action, Action::Drawing);
        QVERIFY(gesture.drawing());
    }

    // --- what must NOT start a draw ----------------------------------------

    void theLeftButtonDrawsNothing()
    {
        // F-001 is the right button, and the split is deliberate: the left one is
        // the desktop icons' own gesture (marquee selection, T-042), and F-022's
        // double-click arrives as a left press too. If the left button drew, every
        // attempt to select an icon would leave a fence behind.
        BackdropGesture gesture;
        QCOMPARE(gesture.press(QPoint(500, 500), Button::Left, false).action, Action::None);
        QVERIFY(!gesture.armed());
        QCOMPARE(gesture.move(QPoint(900, 900)).action, Action::None);
        QCOMPARE(gesture.release(QPoint(900, 900), Button::Left).action, Action::None);
        QVERIFY(!gesture.drawing());
    }

    void aPressOnAnIconStartsNothing()
    {
        // The desktop's own icons belong to the desktop: their behaviour is
        // T-042's, and a fence drawn from on top of one is a fence the user did
        // not ask for. Their right-click menu is T-042's too, so no menu opens
        // here either.
        BackdropGesture gesture;
        QCOMPARE(gesture.press(QPoint(500, 500), Button::Right, true).action, Action::None);
        QVERIFY(!gesture.armed());
        QCOMPARE(gesture.move(QPoint(900, 900)).action, Action::None);
        QCOMPARE(gesture.release(QPoint(900, 900), Button::Right).action, Action::None);
        QVERIFY(!gesture.drawing());
    }

    void aMoveWithoutAPressDoesNothing()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.move(QPoint(700, 700)).action, Action::None);
        QVERIFY(!gesture.drawing());
    }

    void aReleaseOfTheOtherButtonDoesNotEndTheGesture()
    {
        // A stray left click in the middle of a right-drag must neither open the
        // menu nor cancel the draw: the gesture belongs to the button that
        // started it.
        BackdropGesture gesture;
        gesture.press(QPoint(100, 100), Button::Right, false);
        gesture.move(QPoint(400, 300));
        QCOMPARE(gesture.release(QPoint(400, 300), Button::Left).action, Action::None);
        QVERIFY(gesture.drawing());

        const BackdropGesture::Result result = gesture.release(QPoint(400, 300), Button::Right);
        QCOMPARE(result.action, Action::DesktopMenu);
        QCOMPARE(result.rect, QRect(100, 100, 300, 200));
    }

    // --- F-022: quick hide --------------------------------------------------

    void aDoubleClickOnBareDesktopAsksForQuickHide()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.doubleClick(QPoint(600, 600), false).action, Action::ToggleVisibility);
    }

    void aDoubleClickOnAnIconIsNotOurs()
    {
        BackdropGesture gesture;
        QCOMPARE(gesture.doubleClick(QPoint(600, 600), true).action, Action::None);
    }

    void aDoubleClickEndsAnArmedDraw()
    {
        // The real sequence is press, release, double-click, release: whatever
        // the first press armed has to be gone by the time the trailing release
        // arrives, or the desktop would sprout a fence out of a double-click.
        BackdropGesture gesture;
        gesture.press(QPoint(600, 600), Button::Left, false);
        gesture.release(QPoint(600, 600), Button::Left);
        QCOMPARE(gesture.doubleClick(QPoint(600, 600), false).action, Action::ToggleVisibility);
        QVERIFY(!gesture.armed());
        QCOMPARE(gesture.release(QPoint(600, 600), Button::Left).action, Action::None);
    }

    // --- the rubber band ----------------------------------------------------

    void theRubberBandIsOnlyThereWhileDrawing()
    {
        BackdropGesture gesture;
        QVERIFY(gesture.rubberBand().isNull());
        gesture.press(QPoint(100, 100), Button::Right, false);
        QVERIFY(gesture.rubberBand().isNull()); // armed, but nothing drawn yet
        gesture.move(QPoint(300, 260));
        QCOMPARE(gesture.rubberBand(), QRect(100, 100, 200, 160));
        gesture.release(QPoint(300, 260), Button::Right);
        QVERIFY(gesture.rubberBand().isNull());
    }

    void cancelEndsADrawWithoutOpeningAnything()
    {
        // The mouse capture was stolen, or the mode was turned off: the draw
        // stops where it was, and the release that may still arrive opens nothing.
        BackdropGesture gesture;
        gesture.press(QPoint(100, 100), Button::Right, false);
        gesture.move(QPoint(400, 400));
        QCOMPARE(gesture.cancel().action, Action::Cancelled);
        QVERIFY(!gesture.drawing());
        QVERIFY(gesture.rubberBand().isNull());
        QCOMPARE(gesture.release(QPoint(400, 400), Button::Right).action, Action::None);
    }

    void cancellingNothingIsNotWorthARepaint()
    {
        // The caller repaints on Cancelled, so an idle cancel has to be silent.
        BackdropGesture gesture;
        QCOMPARE(gesture.cancel().action, Action::None);
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
