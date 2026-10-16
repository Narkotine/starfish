#include <QtTest>

#include <QList>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

#include "ui/backdrop_layout.hpp"

using namespace starfish::ui;
using starfish::core::DesktopItem;

namespace {

DesktopItem item(const QString& name, const QString& path)
{
    DesktopItem it;
    it.name = name;
    it.path = path;
    it.fileName = name;
    return it;
}

QList<DesktopItem> fourItems()
{
    return {
        item(QStringLiteral("alpha"), QStringLiteral("C:\\Users\\u\\Desktop\\alpha.txt")),
        item(QStringLiteral("bravo"), QStringLiteral("C:\\Users\\u\\Desktop\\bravo.txt")),
        item(QStringLiteral("charlie"), QStringLiteral("C:\\Users\\u\\Desktop\\charlie.txt")),
        item(QStringLiteral("delta"), QStringLiteral("C:\\Users\\u\\Desktop\\delta.txt")),
    };
}

} // namespace

// T-039: the pure half of the desktop backdrop. What it has to get right is not
// the arithmetic but the two rules -- an icon a fence holds is not on the
// backdrop, and the backdrop lays out column-major like the Windows desktop.
class TestBackdropLayout : public QObject
{
    Q_OBJECT

private slots:
    // --- "what is free" ----------------------------------------------------

    void anEmptyFenceSetLeavesEveryIconOnTheDesktop()
    {
        const QList<DesktopItem> desktop = fourItems();
        const QList<DesktopItem> free = BackdropLayout::freeItems(desktop, {});
        QCOMPARE(free.size(), 4);
        QCOMPARE(free.at(0).name, QStringLiteral("alpha"));
    }

    void anIconAFenceHoldsIsNotOnTheBackdrop()
    {
        const QList<DesktopItem> desktop = fourItems();
        const QSet<QString> occupied =
            BackdropLayout::keysOf({item(QStringLiteral("bravo"),
                                         QStringLiteral("C:\\Users\\u\\Desktop\\bravo.txt"))});
        const QList<DesktopItem> free = BackdropLayout::freeItems(desktop, occupied);
        QCOMPARE(free.size(), 3);
        for (const DesktopItem& it : free)
            QVERIFY(it.name != QStringLiteral("bravo"));
    }

    // Windows paths are case-insensitive, and a fence can hold the path a Shell
    // lookup returned while the scanner returned the other spelling. A
    // comparison that missed this would draw the icon twice.
    void theComparisonIgnoresCaseAndSeparators()
    {
        const QList<DesktopItem> desktop = fourItems();
        const QSet<QString> occupied = BackdropLayout::keysOf(
            {item(QStringLiteral("BRAVO"), QStringLiteral("c:/Users/u/desktop/BRAVO.TXT/"))});
        const QList<DesktopItem> free = BackdropLayout::freeItems(desktop, occupied);
        QCOMPARE(free.size(), 3);
        for (const DesktopItem& it : free)
            QVERIFY(it.name != QStringLiteral("bravo"));
    }

    // The whole point of F-007/F-011: an icon that no fence holds is simply a
    // free entry, so removing the fence puts it back with no restore code.
    void anEntryWithNoPathIsNeverDrawn()
    {
        DesktopItem broken;
        broken.name = QStringLiteral("no path");
        const QList<DesktopItem> free =
            BackdropLayout::freeItems({broken, item(QStringLiteral("ok"), QStringLiteral("C:\\a"))},
                                      {});
        QCOMPARE(free.size(), 1);
        QCOMPARE(free.at(0).name, QStringLiteral("ok"));
    }

    void everyIconComesBackWhenNothingIsOccupied()
    {
        const QList<DesktopItem> desktop = fourItems();
        const QList<DesktopItem> all = BackdropLayout::freeItems(desktop, {});
        const QList<DesktopItem> none =
            BackdropLayout::freeItems(desktop, BackdropLayout::keysOf(desktop));
        QCOMPARE(all.size(), 4);
        QCOMPARE(none.size(), 0);
    }

    // --- "where it lands" --------------------------------------------------

    void theFirstCellSitsAtTheMargin()
    {
        const QVector<BackdropCell> cells = BackdropLayout::compute(
            QRect(0, 0, 1000, 1000), BackdropLayout::Metrics{}, fourItems());
        QCOMPARE(cells.size(), 4);
        QCOMPARE(cells.at(0).icon.topLeft(), QPoint(8 + (72 - 48) / 2, 8));
        QCOMPARE(cells.at(0).label.top(), 8 + 48);
        QCOMPARE(cells.at(0).label.width(), 72);
    }

    // Column-major: the Windows desktop fills a column top to bottom, then
    // starts the next one. IconLayout fills rows, and reusing it here would put
    // the icons somewhere no desktop ever puts them.
    void cellsFillColumnsNotRows()
    {
        // 4 rows of 82 + gap 6 = 352 px of pitch, plus 2*8 margin: 368 is enough
        // for exactly 4 rows.
        const QVector<BackdropCell> cells = BackdropLayout::compute(
            QRect(0, 0, 1000, 368), BackdropLayout::Metrics{}, fourItems());
        QCOMPARE(cells.size(), 4);
        // The first four share a column...
        for (int i = 1; i < 4; ++i)
            QCOMPARE(cells.at(i).icon.x(), cells.at(0).icon.x());
        // ...and step down by one row pitch.
        QCOMPARE(cells.at(1).icon.y() - cells.at(0).icon.y(), 88);
        QCOMPARE(cells.at(2).icon.y() - cells.at(1).icon.y(), 88);
    }

    void theFifthItemOpensTheSecondColumn()
    {
        QList<DesktopItem> five = fourItems();
        five.append(item(QStringLiteral("echo"), QStringLiteral("C:\\Users\\u\\Desktop\\echo.txt")));
        const QVector<BackdropCell> cells = BackdropLayout::compute(
            QRect(0, 0, 1000, 368), BackdropLayout::Metrics{}, five);
        QCOMPARE(cells.size(), 5);
        QCOMPARE(cells.at(4).icon.x(), cells.at(0).icon.x() + 72 + 6);
        QCOMPARE(cells.at(4).icon.y(), cells.at(0).icon.y());
    }

    void rowsForHeightNeverReturnsZero()
    {
        QCOMPARE(BackdropLayout::rowsForHeight(0, BackdropLayout::Metrics{}), 1);
        QCOMPARE(BackdropLayout::rowsForHeight(-500, BackdropLayout::Metrics{}), 1);
        QCOMPARE(BackdropLayout::rowsForHeight(4, BackdropLayout::Metrics{}), 1);
        // Four rows need 4*82 + 3*6 = 346 px of usable height, i.e. 362 with the
        // margins. One pixel less and the fourth row is gone -- the last row
        // does not pay for a trailing gap.
        QCOMPARE(BackdropLayout::rowsForHeight(362, BackdropLayout::Metrics{}), 4);
        QCOMPARE(BackdropLayout::rowsForHeight(361, BackdropLayout::Metrics{}), 3);
    }

    // A monitor left of or above the primary one has negative coordinates (T-014),
    // and the backdrop is anchored on the virtual screen's origin, which can be
    // negative. The layout must not assume (0,0).
    void theLayoutFollowsANegativeOrigin()
    {
        const QVector<BackdropCell> cells = BackdropLayout::compute(
            QRect(-1920, -200, 1000, 368), BackdropLayout::Metrics{}, fourItems());
        QCOMPARE(cells.at(0).icon.x(), -1920 + 8 + (72 - 48) / 2);
        QCOMPARE(cells.at(0).icon.y(), -200 + 8);
    }

    void theCellIsWiderThanTheIcon()
    {
        const QVector<BackdropCell> cells = BackdropLayout::compute(
            QRect(0, 0, 1000, 1000), BackdropLayout::Metrics{}, fourItems());
        QCOMPARE(cells.at(0).label.width(), 72);
        QCOMPARE(cells.at(0).icon.width(), 48);
        // The icon is centred in the cell, so the caption has the same width on
        // both sides -- the width a fence grid does not have today (T-049).
        QCOMPARE(cells.at(0).icon.x() - cells.at(0).label.x(), (72 - 48) / 2);
    }

    void theItemTravelsWithItsCell()
    {
        const QVector<BackdropCell> cells = BackdropLayout::compute(
            QRect(0, 0, 1000, 1000), BackdropLayout::Metrics{}, fourItems());
        QCOMPARE(cells.at(2).item.name, QStringLiteral("charlie"));
        QCOMPARE(cells.at(2).item.path, QStringLiteral("C:\\Users\\u\\Desktop\\charlie.txt"));
    }

    void anEmptyDesktopProducesNoCells()
    {
        QVERIFY(BackdropLayout::compute(QRect(0, 0, 1000, 1000), BackdropLayout::Metrics{}, {})
                    .isEmpty());
    }

    // The whole chain, on the case that matters: an icon a fence holds is not on
    // the backdrop, and taking it out of the fence puts it back at the same
    // place it would have had anyway.
    void claimingAnIconRemovesItsCellAndFreeingItBringsItBack()
    {
        const QList<DesktopItem> desktop = fourItems();
        const BackdropLayout::Metrics metrics;

        const QList<DesktopItem> all = BackdropLayout::freeItems(desktop, {});
        const QVector<BackdropCell> before = BackdropLayout::compute(QRect(0, 0, 1000, 1000),
                                                                     metrics, all);
        QCOMPARE(before.size(), 4);
        const QPoint charlieBefore = before.at(2).icon.topLeft();

        // A fence claims charlie.
        const QSet<QString> occupied =
            BackdropLayout::keysOf({item(QStringLiteral("charlie"),
                                         QStringLiteral("C:\\Users\\u\\Desktop\\charlie.txt"))});
        const QVector<BackdropCell> during = BackdropLayout::compute(
            QRect(0, 0, 1000, 1000), metrics, BackdropLayout::freeItems(desktop, occupied));
        QCOMPARE(during.size(), 3);
        for (const BackdropCell& cell : during)
            QVERIFY(cell.item.name != QStringLiteral("charlie"));

        // The fence is deleted: F-007 promises the icons come back.
        const QVector<BackdropCell> after = BackdropLayout::compute(
            QRect(0, 0, 1000, 1000), metrics, BackdropLayout::freeItems(desktop, {}));
        QCOMPARE(after.size(), 4);
        QCOMPARE(after.at(2).item.name, QStringLiteral("charlie"));
        QCOMPARE(after.at(2).icon.topLeft(), charlieBefore);
    }

    void pathKeyNormalisesBothSpellings()
    {
        QCOMPARE(BackdropLayout::pathKey(QStringLiteral("C:\\Users\\u\\Desktop\\a.txt")),
                 BackdropLayout::pathKey(QStringLiteral("c:/users/u/desktop/A.TXT")));
        QCOMPARE(BackdropLayout::pathKey(QStringLiteral("C:\\a\\")),
                 BackdropLayout::pathKey(QStringLiteral("c:/a")));
    }
};

QTEST_MAIN(TestBackdropLayout)
#include "test_backdrop_layout.moc"
