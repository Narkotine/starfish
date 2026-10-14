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
    void hitTestFindsCellAndGap();
    void selectionSelectOnlyToggleRange();
    void launchPathResolvesShortcut();
    void sortReordersItems();
    void paintDoesNotCrash();
    void moveItemReordersAndFollowsSelection();
    void moveItemNoOpOnEqualIndexes();
    void moveItemRejectsOutOfRange();
    void moveItemClampsShiftedTarget();
    void appendItemsAddsDedupesAndSorts();
};

void TestIconGrid::columnsFitTheWidth()
{
    // viewport 480, padding 12 each side -> usable 456; (456+8)/(48+8) = 8.28 -> 8.
    QCOMPARE(IconLayout::columnsForWidth(480, 48, 8, 12), 8);
    // Narrow fence -> at least one column.
    QCOMPARE(IconLayout::columnsForWidth(40, 48, 8, 12), 1);
    // Degenerate inputs never divide by zero.
    QCOMPARE(IconLayout::columnsForWidth(0, 48, 8, 12), 1);
}

void TestIconGrid::computePositionsFirstTwoCells()
{
    const QVector<IconCell> cells =
        IconLayout::compute(QSize(480, 320), 48, 34, 8, 12, 10);
    QCOMPARE(cells.size(), 10);

    // First cell: icon centred in a 48-wide cell with no offset, label below.
    QCOMPARE(cells.at(0).icon, QRect(12, 12, 48, 48));
    QCOMPARE(cells.at(0).label, QRect(12, 60, 48, 34));

    // Second cell: one column-step to the right (48 + 8 gap).
    QCOMPARE(cells.at(1).icon.x(), 12 + 48 + 8);
    QCOMPARE(cells.at(1).icon.y(), 12);
}

void TestIconGrid::hitTestFindsCellAndGap()
{
    const QVector<IconCell> cells =
        IconLayout::compute(QSize(480, 320), 48, 34, 8, 12, 10);

    // Inside the first icon -> index 0.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(30, 30)), 0);
    // Inside the first label -> index 0.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(30, 70)), 0);
    // Inside the second icon -> index 1.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(12 + 48 + 8 + 10, 30)), 1);
    // In the gap between two columns -> nothing.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(12 + 48 + 2, 30)), -1);
    // Far outside -> nothing.
    QCOMPARE(IconLayout::hitTest(cells, QPoint(9999, 9999)), -1);
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

QTEST_MAIN(TestIconGrid)
#include "test_icon_grid.moc"
