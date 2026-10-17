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
    void columnsFitTheWidth();
    void computePositionsFirstTwoCells();
    void aCellIsWiderThanItsIcon();
    void hitTestFindsCellAndGap();
    void aLongCaptionWrapsInsteadOfBeingCut();
    void aCaptionTooLongForBothLinesIsElidedOnTheRight();
    void aShortCaptionStaysOnOneLine();
    void aSingleWordWiderThanTheLineIsCutNotSplit();
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
        IconLayout::compute(QSize(480, 320), 48, cellW, 34, 8, 12, 10);
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
        IconLayout::compute(QSize(480, 320), 48, IconLayout::cellWidthFor(48), 34, 8, 12, 10);

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

QTEST_MAIN(TestIconGrid)
#include "test_icon_grid.moc"
