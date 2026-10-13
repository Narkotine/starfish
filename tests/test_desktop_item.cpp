#include <QtTest>

#include "core/desktop_item.hpp"

using namespace starfish::core;

namespace {

DesktopItem makeItem(const QString& name)
{
    DesktopItem item;
    item.name = name;
    item.fileName = name;
    item.path = QStringLiteral("C:/Desktop/") + name;
    return item;
}

QStringList namesOf(const QList<DesktopItem>& items)
{
    QStringList names;
    names.reserve(items.size());
    for (const DesktopItem& item : items)
        names.append(item.name);
    return names;
}

} // namespace

class TestDesktopItem : public QObject
{
    Q_OBJECT

private slots:
    void dotHiddenNames();
    void naturalOrderPadsDigitRuns();
    void naturalOrderIsCaseInsensitive();
    void leadingZerosCompareEqual();
    void sortsByNameBothDirections();
    void sortsByTypeFoldersFirst();
    void sortsBySize();
    void sortsByDate();
    void mergePrefersTheUserDesktop();
    void mergeKeepsOrderAndDropsDuplicates();
};

void TestDesktopItem::dotHiddenNames()
{
    QVERIFY(isDotHiddenName(QStringLiteral(".gitignore")));
    QVERIFY(!isDotHiddenName(QStringLiteral("notes.txt")));
    QVERIFY(!isDotHiddenName(QStringLiteral("."))); // never a real entry
}

void TestDesktopItem::naturalOrderPadsDigitRuns()
{
    // The reason the helper exists: plain string compare puts "shot10" first.
    QVERIFY(naturalSortKey(QStringLiteral("shot9.png"))
            < naturalSortKey(QStringLiteral("shot10.png")));
    QVERIFY(QStringLiteral("shot10.png") < QStringLiteral("shot9.png")); // the naive order
}

void TestDesktopItem::naturalOrderIsCaseInsensitive()
{
    QCOMPARE(naturalSortKey(QStringLiteral("Alpha")), naturalSortKey(QStringLiteral("alpha")));
    QVERIFY(naturalSortKey(QStringLiteral("apple")) < naturalSortKey(QStringLiteral("Banana")));
}

void TestDesktopItem::leadingZerosCompareEqual()
{
    QCOMPARE(naturalSortKey(QStringLiteral("file01.txt")),
             naturalSortKey(QStringLiteral("file1.txt")));
}

void TestDesktopItem::sortsByNameBothDirections()
{
    QList<DesktopItem> items{makeItem(QStringLiteral("shot10.png")),
                             makeItem(QStringLiteral("shot9.png")),
                             makeItem(QStringLiteral("shot1.png"))};

    sortForDisplay(items, QStringLiteral("name"), true);
    QCOMPARE(namesOf(items),
             QStringList({QStringLiteral("shot1.png"), QStringLiteral("shot9.png"),
                          QStringLiteral("shot10.png")}));

    sortForDisplay(items, QStringLiteral("name"), false);
    QCOMPARE(namesOf(items),
             QStringList({QStringLiteral("shot10.png"), QStringLiteral("shot9.png"),
                          QStringLiteral("shot1.png")}));
}

void TestDesktopItem::sortsByTypeFoldersFirst()
{
    DesktopItem folder = makeItem(QStringLiteral("zzz"));
    folder.isDir = true;
    DesktopItem txt = makeItem(QStringLiteral("a.txt"));
    DesktopItem png = makeItem(QStringLiteral("b.png"));

    QList<DesktopItem> items{txt, png, folder};
    sortForDisplay(items, QStringLiteral("type"), true);
    QCOMPARE(namesOf(items),
             QStringList({QStringLiteral("zzz"), QStringLiteral("b.png"),
                          QStringLiteral("a.txt")}));
}

void TestDesktopItem::sortsBySize()
{
    DesktopItem small = makeItem(QStringLiteral("small.bin"));
    small.sizeBytes = 10;
    DesktopItem big = makeItem(QStringLiteral("big.bin"));
    big.sizeBytes = 4096;
    DesktopItem unknown = makeItem(QStringLiteral("folder"));
    unknown.sizeBytes = -1; // folders and unreadable entries

    QList<DesktopItem> items{big, unknown, small};
    sortForDisplay(items, QStringLiteral("size"), true);
    QCOMPARE(namesOf(items),
             QStringList({QStringLiteral("folder"), QStringLiteral("small.bin"),
                          QStringLiteral("big.bin")}));

    sortForDisplay(items, QStringLiteral("size"), false);
    QCOMPARE(namesOf(items).first(), QStringLiteral("big.bin"));
}

void TestDesktopItem::sortsByDate()
{
    DesktopItem old = makeItem(QStringLiteral("old.txt"));
    old.modified = QDateTime(QDate(2026, 1, 1), QTime(0, 0));
    DesktopItem recent = makeItem(QStringLiteral("recent.txt"));
    recent.modified = QDateTime(QDate(2026, 10, 13), QTime(12, 0));

    QList<DesktopItem> items{recent, old};
    sortForDisplay(items, QStringLiteral("date"), true);
    QCOMPARE(namesOf(items).first(), QStringLiteral("old.txt"));
}

void TestDesktopItem::mergePrefersTheUserDesktop()
{
    DesktopItem user = makeItem(QStringLiteral("report.docx"));
    DesktopItem shared = makeItem(QStringLiteral("report.docx"));
    shared.fromPublicDesktop = true;
    shared.path = QStringLiteral("C:/Users/Public/Desktop/report.docx");

    // Public first in the input: the per-user entry must still win.
    const QList<DesktopItem> merged = mergeDesktopItems({shared, user});
    QCOMPARE(merged.size(), 1);
    QVERIFY(!merged.first().fromPublicDesktop);
}

void TestDesktopItem::mergeKeepsOrderAndDropsDuplicates()
{
    DesktopItem a = makeItem(QStringLiteral("a.txt"));
    DesktopItem b = makeItem(QStringLiteral("b.txt"));
    DesktopItem shared = makeItem(QStringLiteral("b.txt"));
    shared.fromPublicDesktop = true;

    const QList<DesktopItem> merged = mergeDesktopItems({a, b, shared});
    QCOMPARE(namesOf(merged), QStringList({QStringLiteral("a.txt"), QStringLiteral("b.txt")}));
}

QTEST_MAIN(TestDesktopItem)
#include "test_desktop_item.moc"
