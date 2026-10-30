#include <QtTest>

#include <QImage>
#include <QPaintDevice>
#include <QPainter>
#include <QPixmap>

#include "core/desktop_item.hpp"
#include "ui/icon_grid.hpp"
#include "ui/icon_layout.hpp"

using namespace starfish::core;
using namespace starfish::ui;

namespace {

DesktopItem makeItem(const QString& name)
{
    DesktopItem item;
    item.name = name;
    item.fileName = name;
    item.path = QStringLiteral("C:/Desktop/") + name;
    return item;
}

} // namespace

class TestIconGrid : public QObject
{
    Q_OBJECT

private slots:
    void aDropGoesWhereItFellAndTheOrderStaysManual();
    void aDropIsSortedOnlyWhenTheUserAskedForIt();
    void columnsFitTheWidth();
    void computePositionsFirstTwoCells();
    // Session 64: the same margin on the left and on the right.
    void theColumnsAreCentred();
    void aCellIsWiderThanItsIcon();
    void hitTestFindsCellAndGap();
    void aLongCaptionWrapsInsteadOfBeingCut();
    void aCaptionTooLongForBothLinesIsElidedOnTheRight();
    void aShortCaptionStaysOnOneLine();
    void aSingleWordWiderThanTheLineIsCutNotSplit();
    void aCellKeepsItsFixedWidthWhenTheNamesFit();
    void aCellTakesExactlyTheWidthTheLongestNameNeeds();
    void aCellStopsAtTwiceTheIcon();
    void aGridWithoutAFontKeepsTheFixedCell();
    void aGridWithAFontWidensTheCellForALongName();
    void selectionSelectOnlyToggleRange();
    void launchPathResolvesShortcut();
    void sortReordersItems();
    void paintDoesNotCrash();
    void moveItemReordersAndFollowsSelection();
    void moveItemNoOpOnEqualIndexes();
    void moveItemRejectsOutOfRange();
    void moveItemClampsShiftedTarget();
    void appendItemsAddsDedupesAndSorts();
    void removeItemsByPathTakesTilesOut();
    void cellRectsAreTheTilesAndNothingElse();
};

void TestIconGrid::columnsFitTheWidth()
{
    // viewport 480, padding 12 each side -> usable 456; (456+8)/(72+8) = 5.8 -> 5.
    // The column is the *cell* width (72 for a 48 px icon, T-049), not the icon.
    QCOMPARE(IconLayout::columnsForWidth(480, IconLayout::cellWidthFor(48), 8, 12), 5);
    // Narrow fence -> at least one column.
    QCOMPARE(IconLayout::columnsForWidth(40, IconLayout::cellWidthFor(48), 8, 12), 1);
    // Degenerate inputs never divide by zero.
    QCOMPARE(IconLayout::columnsForWidth(0, IconLayout::cellWidthFor(48), 8, 12), 1);
}

void TestIconGrid::computePositionsFirstTwoCells()
{
    const int cellW = IconLayout::cellWidthFor(48); // 72
    const QVector<IconCell> cells =
        IconLayout::compute(QSize(416, 320), 48, cellW, 34, 8, 12, 10); // 5 columns, nothing left over
    QCOMPARE(cells.size(), 10);

    // First cell: the icon is centred in a 72-wide cell, the caption below it.
    QCOMPARE(cells.at(0).icon, QRect(12 + (72 - 48) / 2, 12, 48, 48));
    QCOMPARE(cells.at(0).label, QRect(12, 60, 72, 34));

    // Second cell: one column-step to the right (72 + 8 gap).
    QCOMPARE(cells.at(1).icon.x(), 12 + 72 + 8 + (72 - 48) / 2);
    QCOMPARE(cells.at(1).icon.y(), 12);

    // ... and the fifth item of a 5-column grid starts the next row.
    QCOMPARE(cells.at(4).icon.y(), 12);
    QCOMPARE(cells.at(5).icon.y(), 12 + 48 + 34 + 8);
}

void TestIconGrid::aCellIsWiderThanItsIcon()
{
    // T-049: the caption band is the *cell*, not the icon square -- that is the
    // room the names were missing. The icon stays centred, so the cell is not
    // simply the icon shifted.
    const QVector<IconCell> cells = IconLayout::compute(QSize(480, 320), 48,
                                                        IconLayout::cellWidthFor(48), 34, 8, 12, 1);
    QCOMPARE(cells.size(), 1);
    QVERIFY(cells.at(0).label.width() > cells.at(0).icon.width());
    QCOMPARE(cells.at(0).label.width(), IconLayout::cellWidthFor(48));
    QCOMPARE(cells.at(0).label.center().x(), cells.at(0).icon.center().x());
    // A cellWidth narrower than the icon cannot happen (the icon would be cut):
    // the cell is clamped to the icon.
    const QVector<IconCell> narrow = IconLayout::compute(QSize(480, 320), 48, 16, 34, 8, 12, 1);
    QCOMPARE(narrow.at(0).icon.width(), 48);
    QCOMPARE(narrow.at(0).label.width(), 48);
}

void TestIconGrid::hitTestFindsCellAndGap()
{
    const QVector<IconCell> cells =
        IconLayout::compute(QSize(416, 320), 48, IconLayout::cellWidthFor(48), 34, 8, 12, 10);

    // Inside the first icon -> index 0.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(30, 30)), 0);
    // Inside the first caption band, to the right of the icon -> still index 0:
    // the band belongs to the cell (T-049).
    QCOMPARE(IconLayout::hitTest(cells, QPoint(30, 70)), 0);
    QCOMPARE(IconLayout::hitTest(cells, QPoint(12 + 72 - 2, 70)), 0);
    // Inside the second icon -> index 1.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(12 + 72 + 8 + 12 + 10, 30)), 1);
    // In the gap between two cells -> nothing, in the icon rows as well as in the
    // caption rows.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(12 + 72 + 2, 30)), -1);
    QCOMPARE(IconLayout::hitTest(cells, QPoint(12 + 72 + 2, 70)), -1);
    // Far outside -> nothing.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(9999, 9999)), -1);
}

void TestIconGrid::aLongCaptionWrapsInsteadOfBeingCut()
{
    // T-049, the user's own example: "Epic Games Launcher" used to come out as
    // "Epi...her" -- elided in the middle of a 48 px band. The stub metric is 7 px
    // per character, so the arithmetic is exact and no font is involved: 72 px is
    // ten characters.
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int cellW = IconLayout::cellWidthFor(48);

    const QStringList lines =
        IconLayout::wrapToLines(QStringLiteral("Epic Games Launcher"), 2, cellW, widthOf);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QStringLiteral("Epic Games"));
    QCOMPARE(lines.at(1), QStringLiteral("Launcher"));
    // Nothing is lost when it fits on the two lines, and it is *not* cut in the
    // middle: the name is readable from one end to the other.
    QVERIFY(!lines.join(QLatin1Char(' ')).contains(QChar(0x2026)));
}

void TestIconGrid::aCaptionTooLongForBothLinesIsElidedOnTheRight()
{
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int cellW = IconLayout::cellWidthFor(48);

    const QStringList lines = IconLayout::wrapToLines(
        QStringLiteral("Epic Games Launcher Deluxe Extra"), 2, cellW, widthOf);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QStringLiteral("Epic Games"));
    // The *last* line carries the ellipsis, and it is a right-hand one: the text
    // that survives starts at the beginning, it is the tail that goes.
    QVERIFY(lines.at(1).endsWith(QChar(0x2026)));
    QVERIFY(lines.at(1).startsWith(QStringLiteral("Launcher")));
    // And the elided line really fits.
    QVERIFY(widthOf(lines.at(1)) <= cellW);
}

void TestIconGrid::aShortCaptionStaysOnOneLine()
{
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int cellW = IconLayout::cellWidthFor(48);

    const QStringList lines =
        IconLayout::wrapToLines(QStringLiteral("notes.txt"), 2, cellW, widthOf);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.at(0), QStringLiteral("notes.txt"));

    // Degenerate input never produces a line (the painter then draws nothing).
    QVERIFY(IconLayout::wrapToLines(QStringLiteral("   "), 2, cellW, widthOf).isEmpty());
    QVERIFY(IconLayout::wrapToLines(QString(), 2, cellW, widthOf).isEmpty());

    // A width of zero (a fence squeezed to nothing) does not loop or crash.
    const QStringList squeezed = IconLayout::wrapToLines(QStringLiteral("abc"), 2, 0, widthOf);
    QCOMPARE(squeezed.size(), 1);
}

void TestIconGrid::aSingleWordWiderThanTheLineIsCutNotSplit()
{
    // A single long word has no break inside it: it takes the line and is elided,
    // rather than being split in half across the two lines.
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int cellW = IconLayout::cellWidthFor(48);

    const QStringList lines = IconLayout::wrapToLines(
        QStringLiteral("Supercalifragilisticexpialidocious"), 2, cellW, widthOf);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.at(0).endsWith(QChar(0x2026)));
    QVERIFY(widthOf(lines.at(0)) <= cellW);
}

void TestIconGrid::aCellKeepsItsFixedWidthWhenTheNamesFit()
{
    // T-078 keeps T-049's cell for every name that already fits it: a fence of
    // ordinary names is *exactly* what it was before, so the change costs nothing
    // where nothing was wrong. The stub metric is 7 px per character, so 72 px is
    // ten characters -- and "Epic Games Launcher" is the very name T-049 measured,
    // which fits on its two lines and therefore does not move a pixel.
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int floorWidth = IconLayout::cellWidthFor(48);
    const int ceiling = IconLayout::maxCellWidthFor(48);

    QCOMPARE(IconLayout::cellWidthForCaptions(floorWidth, ceiling, {}, 2, widthOf), floorWidth);
    QCOMPARE(IconLayout::cellWidthForCaptions(floorWidth, ceiling,
                                              {QStringLiteral("notes.txt")}, 2, widthOf),
             floorWidth);
    QCOMPARE(IconLayout::cellWidthForCaptions(floorWidth, ceiling,
                                              {QStringLiteral("Epic Games Launcher")}, 2,
                                              widthOf),
             floorWidth);
    // ...and that is the same wrapping the painter does, not a parallel rule:
    // both lines together are the whole name.
    const QStringList lines =
        IconLayout::wrapToLines(QStringLiteral("Epic Games Launcher"), 2, floorWidth, widthOf);
    QCOMPARE(lines.join(QLatin1Char(' ')), QStringLiteral("Epic Games Launcher"));
}

void TestIconGrid::aCellTakesExactlyTheWidthTheLongestNameNeeds()
{
    // The smallest width at which the caption survives its two lines -- not a
    // blanket "make the cell twice as wide". With 7 px characters, "Epic Games
    // Launcher Xx" needs eleven characters on its second line ("Launcher Xx"),
    // i.e. 77 px; one pixel less and that line is elided again. This is the
    // boundary, checked from both sides.
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int floorWidth = IconLayout::cellWidthFor(48);
    const int ceiling = IconLayout::maxCellWidthFor(48);
    const QString caption = QStringLiteral("Epic Games Launcher Xx");

    const int needed =
        IconLayout::cellWidthForCaptions(floorWidth, ceiling, {caption}, 2, widthOf);
    QCOMPARE(needed, 77);
    QVERIFY(needed > floorWidth && needed < ceiling);

    // The caption is whole at the width the cell takes: nothing lost, no stub.
    const QStringList lines = IconLayout::wrapToLines(caption, 2, needed, widthOf);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.join(QLatin1Char(' ')), caption);

    // ...and one pixel narrower it is still cut, so the width is what the name
    // *needed* rather than merely a width that happens to work.
    const QStringList tight = IconLayout::wrapToLines(caption, 2, needed - 1, widthOf);
    QVERIFY(tight.join(QLatin1Char(' ')).contains(IconLayout::kEllipsis));
}

void TestIconGrid::aCellStopsAtTwiceTheIcon()
{
    // There is a ceiling, because this is a grid: one very long name must not
    // thin out every column of the fence, and past twice the icon the caption is
    // elided whatever the cell does. A name starting with a word wider than the
    // whole ceiling is what proves the ceiling is *applied* -- no split ever fits
    // it, so without a stop the search would keep widening for nothing.
    const auto widthOf = [](const QString& text) { return text.size() * 7; };
    const int floorWidth = IconLayout::cellWidthFor(48);
    const int ceiling = IconLayout::maxCellWidthFor(48);

    QCOMPARE(IconLayout::cellWidthForCaptions(
                 floorWidth, ceiling,
                 {QStringLiteral("Supercalifragilisticexpialidocious Extra Words")}, 2, widthOf),
             ceiling);
    // A single long word is sized like any other caption: the cell grows to hold
    // it (13 characters = 91 px here)...
    QCOMPARE(IconLayout::cellWidthForCaptions(floorWidth, ceiling,
                                              {QStringLiteral("abcdefghijklm")}, 2, widthOf),
             91);
    // ...up to the ceiling, where "even longer" stops changing anything.
    QCOMPARE(IconLayout::cellWidthForCaptions(floorWidth, ceiling,
                                              {QStringLiteral("abcdefghijklmnop")}, 2, widthOf),
             ceiling);
    // And within its bounds whatever it is handed (an empty caption included).
    const QStringList motley{QStringLiteral("a"), QString(), QStringLiteral("x  y zzzz")};
    const int width = IconLayout::cellWidthForCaptions(floorWidth, ceiling, motley, 2, widthOf);
    QCOMPARE(width, floorWidth);
}

void TestIconGrid::aGridWithoutAFontKeepsTheFixedCell()
{
    // The derivation needs a font, and a grid that was never given one (a unit
    // test, a grid with no view) must not invent one: it falls back to T-049's
    // fixed cell. The long name is deliberate -- it shows the fallback is a
    // decision rather than short names hiding the question.
    IconGrid grid;
    grid.setItems({makeItem(QStringLiteral("Epic Games Launcher Deluxe Extra"))});
    QVERIFY(!grid.hasBaseFont());
    QCOMPARE(grid.cellWidth(), IconLayout::cellWidthFor(48));
}

void TestIconGrid::aGridWithAFontWidensTheCellForALongName()
{
    // Where the two meet. The view hands the grid its font (FenceView does it in
    // its constructor, so hitTest -- a mouse event, no painter -- measures the
    // same text the painter draws), and a fence holding one long name gets wider
    // cells while a fence of short names is left exactly as it was. The numbers
    // are relative: the real metrics of a real font are not a constant a test may
    // write down.
    IconGrid grid;
    QFont font;
    font.setPointSize(10);
    grid.setBaseFont(font);

    grid.setItems({makeItem(QStringLiteral("note.txt"))});
    QCOMPARE(grid.cellWidth(), IconLayout::cellWidthFor(48));

    grid.setItems({makeItem(QStringLiteral("Epic Games Launcher Deluxe Extra"))});
    QVERIFY(grid.cellWidth() > IconLayout::cellWidthFor(48));
    QVERIFY(grid.cellWidth() <= IconLayout::maxCellWidthFor(48));

    // The *longest* name decides for the whole grid, and taking it out gives the
    // room back: the width follows the items, it is not a one-way ratchet.
    grid.setItems({makeItem(QStringLiteral("note.txt")),
                   makeItem(QStringLiteral("Epic Games Launcher Deluxe Extra"))});
    QVERIFY(grid.cellWidth() > IconLayout::cellWidthFor(48));
    grid.setItems({makeItem(QStringLiteral("note.txt"))});
    QCOMPARE(grid.cellWidth(), IconLayout::cellWidthFor(48));
}

void TestIconGrid::selectionSelectOnlyToggleRange()
{
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("a")), makeItem(QStringLiteral("b")),
                             makeItem(QStringLiteral("c")), makeItem(QStringLiteral("d")),
                             makeItem(QStringLiteral("e"))};
    grid.setItems(items);

    grid.selectOnly(2);
    QCOMPARE(grid.selectedIndices(), QSet<int>({2}));

    grid.toggle(2); // deselect
    QVERIFY(grid.selectedIndices().isEmpty());
    grid.toggle(2); // re-select
    QCOMPARE(grid.selectedIndices(), QSet<int>({2}));

    // Shift from anchor (2) to 0 selects the contiguous range 0..2.
    grid.selectRangeTo(0);
    QCOMPARE(grid.selectedIndices(), QSet<int>({0, 1, 2}));

    grid.clearSelection();
    QVERIFY(grid.selectedIndices().isEmpty());
}

void TestIconGrid::launchPathResolvesShortcut()
{
    DesktopItem file = makeItem(QStringLiteral("notes.txt"));
    QCOMPARE(IconGrid::launchPath(file), QStringLiteral("C:/Desktop/notes.txt"));

    DesktopItem link = makeItem(QStringLiteral("app.lnk"));
    link.targetPath = QStringLiteral("C:/Program Files/App/app.exe");
    QCOMPARE(IconGrid::launchPath(link), QStringLiteral("C:/Program Files/App/app.exe"));
}

void TestIconGrid::sortReordersItems()
{
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("c.txt")),
                             makeItem(QStringLiteral("a.txt")),
                             makeItem(QStringLiteral("b.txt"))};
    grid.setItems(items);
    QStringList asc = QStringList({QStringLiteral("a.txt"), QStringLiteral("b.txt"),
                                   QStringLiteral("c.txt")});
    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, asc);

    grid.setSort(QStringLiteral("name"), false);
    QStringList desc;
    for (const DesktopItem& it : grid.items())
        desc.append(it.name);
    QCOMPARE(desc, QStringList({QStringLiteral("c.txt"), QStringLiteral("b.txt"),
                                QStringLiteral("a.txt")}));
}

void TestIconGrid::paintDoesNotCrash()
{
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("a.txt")),
                             makeItem(QStringLiteral("b.txt"))};
    grid.setItems(items);
    grid.selectOnly(0);

    // A stub provider that never returns an icon: the grid must fall back to a
    // placeholder and not crash.
    QImage img(200, 120, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    grid.paint(painter, QRect(0, 0, 200, 120), [](const DesktopItem&, int) { return QPixmap(); });
    painter.end();

    QVERIFY(!img.isNull());
    // The selected cell highlight (accent blue) leaves a non-transparent pixel.
    bool painted = false;
    for (int y = 0; y < img.height() && !painted; ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) != 0)
                painted = true;
    QVERIFY(painted);
}

void TestIconGrid::moveItemReordersAndFollowsSelection()
{
    // 5 items in known order; move the first ("a") to slot 2 and verify the
    // order matches QList::move semantics (the moved item ends up at index `to`),
    // the selection, and the anchor (which Shift-select uses next).
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("a")), makeItem(QStringLiteral("b")),
                             makeItem(QStringLiteral("c")), makeItem(QStringLiteral("d")),
                             makeItem(QStringLiteral("e"))};
    grid.setItems(items);

    const int newIndex = grid.moveItem(/*from*/ 0, /*to*/ 2);
    QCOMPARE(newIndex, 2);

    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({QStringLiteral("b"), QStringLiteral("c"),
                                 QStringLiteral("a"), QStringLiteral("d"),
                                 QStringLiteral("e")}));
    // Only the moved item stays selected.
    QCOMPARE(grid.selectedIndices(), QSet<int>({2}));
    // Anchor follows the moved item.
    grid.selectRangeTo(4);
    QCOMPARE(grid.selectedIndices(), QSet<int>({2, 3, 4}));
}

void TestIconGrid::moveItemNoOpOnEqualIndexes()
{
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("a")),
                             makeItem(QStringLiteral("b"))};
    grid.setItems(items);

    QCOMPARE(grid.moveItem(1, 1), 1);
    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({QStringLiteral("a"), QStringLiteral("b")}));
}

void TestIconGrid::moveItemRejectsOutOfRange()
{
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("a")),
                             makeItem(QStringLiteral("b"))};
    grid.setItems(items);

    // Negative `from` / `to` and indices past the end are all rejected.
    QCOMPARE(grid.moveItem(-1, 0), -1);
    QCOMPARE(grid.moveItem(0, -1), -1);
    QCOMPARE(grid.moveItem(0, 2), -1);
    QCOMPARE(grid.moveItem(2, 0), -1);

    // Nothing should have moved.
    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({QStringLiteral("a"), QStringLiteral("b")}));
}

void TestIconGrid::moveItemClampsShiftedTarget()
{
    // moveItem(0, 4) on a 5-item list: "a" is taken out, then inserted at
    // index 4 -- which is the last slot. The other items shift left to fill
    // the gap. This catches an off-by-one in the take/insert pair (an earlier
    // version of moveItem applied a "shift left when from < to" adjustment
    // that landed the moved item at index 3 instead of 4).
    IconGrid grid;
    QList<DesktopItem> items{makeItem(QStringLiteral("a")), makeItem(QStringLiteral("b")),
                             makeItem(QStringLiteral("c")), makeItem(QStringLiteral("d")),
                             makeItem(QStringLiteral("e"))};
    grid.setItems(items);

    const int newIndex = grid.moveItem(/*from*/ 0, /*to*/ 4);
    QCOMPARE(newIndex, 4);

    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({QStringLiteral("b"), QStringLiteral("c"),
                                 QStringLiteral("d"), QStringLiteral("e"),
                                 QStringLiteral("a")}));
}

void TestIconGrid::appendItemsAddsDedupesAndSorts()
{
    // T-006-3: a drop from the Explorer appends to the grid. Duplicates (same
    // path) are ignored, an item with no path is skipped, the list is re-sorted
    // to the current order, and the selection is cleared (a drop is a fresh
    // batch, not a continuation of the current selection).
    IconGrid grid;
    grid.setItems(QList<DesktopItem>{makeItem(QStringLiteral("b.txt"))});
    grid.selectOnly(0);
    QCOMPARE(grid.selectedIndices(), QSet<int>({0}));

    const DesktopItem c = makeItem(QStringLiteral("c.txt"));
    const DesktopItem dupB = makeItem(QStringLiteral("b.txt")); // same path as the existing item
    const DesktopItem empty;                                    // no path -> skipped
    const int added = grid.appendItems(QList<DesktopItem>{c, dupB, empty});
    QCOMPARE(added, 1);

    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({QStringLiteral("b.txt"), QStringLiteral("c.txt")}));
    QVERIFY(grid.selectedIndices().isEmpty());

    // A second identical drop adds nothing.
    QCOMPARE(grid.appendItems(QList<DesktopItem>{c}), 0);
}

void TestIconGrid::removeItemsByPathTakesTilesOut()
{
    // T-040 / F-011: taking a tile out of a fence is a removal from the model
    // and nothing else -- no file is touched, nothing is hidden, and the icon
    // reappears on the desktop because no fence holds it any more. Paths are
    // compared case-insensitively (Windows paths are, and the released path can
    // come back from the Shell with another capitalisation), an unknown path is
    // ignored, and the selection *follows the tiles that stay*: releasing one
    // icon is not the user selecting something else.
    IconGrid grid;
    grid.setItems(QList<DesktopItem>{makeItem(QStringLiteral("a.txt")),
                                     makeItem(QStringLiteral("b.txt")),
                                     makeItem(QStringLiteral("c.txt"))});
    grid.selectOnly(0);
    grid.toggle(1);
    QCOMPARE(grid.selectedIndices(), QSet<int>({0, 1}));

    QCOMPARE(grid.removeItemsByPath({QStringLiteral("c:/desktop/B.TXT")}), 1);

    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({QStringLiteral("a.txt"), QStringLiteral("c.txt")}));
    QCOMPARE(grid.selectedIndices(), QSet<int>({0}));

    // Nothing matches: no change, and an empty list is not a failure.
    QCOMPARE(grid.removeItemsByPath({QStringLiteral("C:/Desktop/zzz.txt")}), 0);
    QCOMPARE(grid.removeItemsByPath({}), 0);
    QCOMPARE(grid.items().size(), 2);
}

void TestIconGrid::cellRectsAreTheTilesAndNothingElse()
{
    // T-082: the rectangles a fence's icon overlay is made of. They are the very
    // cells `paint` and `hitTest` use -- the icon square united with its caption
    // band -- clipped to the content area, so a row past the bottom contributes no
    // pixel. The overlay's window region *is* this list, so a discrepancy here is
    // a hole (or a rectangle of panel) on someone's desktop.
    IconGrid grid;
    QCOMPARE(grid.cellRects(QSize(480, 240)).size(), 0); // nothing to show: nothing to be

    grid.setItems(QList<DesktopItem>{
        makeItem(QStringLiteral("a.txt")), makeItem(QStringLiteral("b.txt")),
        makeItem(QStringLiteral("c.txt")), makeItem(QStringLiteral("d.txt")),
        makeItem(QStringLiteral("e.txt")), makeItem(QStringLiteral("f.txt"))});

    const QSize content(480, 240);
    const QVector<QRect> rects = grid.cellRects(content);
    QCOMPARE(rects.size(), 6);

    // Each rectangle is exactly one cell, and it is the cell its item is drawn in:
    // the rectangle's own centre has to hit-test back to that item.
    const int cellW = IconLayout::cellWidthFor(grid.iconSize());
    const int cellH = grid.iconSize() + grid.labelHeight();
    for (int i = 0; i < rects.size(); ++i) {
        const QRect& rect = rects.at(i);
        QCOMPARE(grid.hitTest(rect.center(), content), i);
        QCOMPARE(rect.width(), cellW);
        QCOMPARE(rect.height(), cellH);
    }
    // Five columns of 72 in 480 px (T-049/T-078), so the sixth tile is the first of
    // the second row -- and it starts a whole row lower, not beside the others.
    QCOMPARE(rects.at(1).top(), rects.at(0).top());
    QVERIFY(rects.at(1).left() >= rects.at(0).right());
    QVERIFY(rects.at(5).top() > rects.at(0).bottom());

    // A fence too short to show that second row contributes no rectangle for it:
    // the window must not exist where nothing is drawn, and "a region the size of
    // the whole window" is exactly what an empty list would mean.
    QCOMPARE(grid.cellRects(QSize(480, 100)).size(), 5);
    QCOMPARE(grid.cellRects(QSize(40, 40)).size(), 1); // narrower than a cell: one column
}

// Session 60: no alphabetical re-sort unless the setting asks for it.
void TestIconGrid::aDropGoesWhereItFellAndTheOrderStaysManual()
{
    IconGrid grid;
    grid.setSort(QStringLiteral("manual"), true);
    grid.setItems({makeItem(QStringLiteral("c.txt")), makeItem(QStringLiteral("a.txt"))});
    QCOMPARE(grid.items().at(0).name, QStringLiteral("c.txt")); // manual: not re-sorted
    QCOMPARE(grid.insertItems({makeItem(QStringLiteral("b.txt"))}, 1, false), 1);
    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({"c.txt", "b.txt", "a.txt"}));
    QCOMPARE(grid.sortBy(), QStringLiteral("manual"));
    // -1: at the end. A duplicate is skipped.
    QCOMPARE(grid.insertItems({makeItem(QStringLiteral("d.txt")), makeItem(QStringLiteral("A.TXT"))}, -1, false), 1);
    QCOMPARE(grid.items().last().name, QStringLiteral("d.txt"));
    // What the document restores comes back in the same order.
    IconGrid restored;
    restored.setSort(grid.sortBy(), true);
    restored.setItems(grid.items());
    for (int i = 0; i < grid.items().size(); ++i)
        QCOMPARE(restored.items().at(i).name, grid.items().at(i).name);
}

void TestIconGrid::aDropIsSortedOnlyWhenTheUserAskedForIt()
{
    IconGrid grid;
    grid.setSort(QStringLiteral("manual"), true);
    grid.setItems({makeItem(QStringLiteral("c.txt")), makeItem(QStringLiteral("a.txt"))});
    QCOMPARE(grid.insertItems({makeItem(QStringLiteral("b.txt"))}, 0, true), 1);
    QStringList names;
    for (const DesktopItem& it : grid.items())
        names.append(it.name);
    QCOMPARE(names, QStringList({"a.txt", "b.txt", "c.txt"}));
    QCOMPARE(grid.sortBy(), QStringLiteral("name"));
}

void TestIconGrid::theColumnsAreCentred()
{
    const int cellW = IconLayout::cellWidthFor(48); // 72, a column step of 80
    for (int width = 120; width <= 900; width += 7) {
        const QVector<IconCell> cells = IconLayout::compute(QSize(width, 300), 48, cellW, 34, 8, 12, 40);
        const int columns = IconLayout::columnsForWidth(width, cellW, 8, 12);
        int right = 0;
        for (int i = 0; i < columns; ++i)
            right = qMax(right, cells.at(i).label.right());
        const int leftMargin = cells.at(0).label.left();
        const int rightMargin = width - 1 - right;
        QVERIFY2(qAbs(leftMargin - rightMargin) <= 1,
                 qPrintable(QStringLiteral("width %1: left %2, right %3").arg(width).arg(leftMargin).arg(rightMargin)));
        QVERIFY(leftMargin >= 12);
    }
    // A column more as soon as it fits with the same padding on both sides:
    // 2 x 12 + 5 x 72 + 4 x 8 = 416.
    QCOMPARE(IconLayout::columnsForWidth(416, cellW, 8, 12), 5);
    QCOMPARE(IconLayout::columnsForWidth(415, cellW, 8, 12), 4);
    // The narrowest fence (160 px, 12 px inset each side -> 136 of content) holds
    // one cell of the widest kind (2 x 48) with its padding.
    QVERIFY(2 * 12 + IconLayout::maxCellWidthFor(48) <= 160 - 2 * 12);
}

QTEST_MAIN(TestIconGrid)
#include "test_icon_grid.moc"
