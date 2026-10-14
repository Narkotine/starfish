#include <QtTest>

#include "core/desktop_item.hpp"
#include "core/rules.hpp"

using namespace starfish::core;

class TestRules : public QObject
{
    Q_OBJECT

private slots:
    void noRulesReturnsNullopt();
    void extensionMatchIsCaseInsensitive();
    void extensionMismatchReturnsNullopt();
    void namePatternWildcard();
    void firstMatchingRuleWins();
    void emptyExtensionsMeansAny();
    void moveFilesDefaultsToFalse();

    // T-010: date criteria (F-017 "par type, nom, date, emplacement cible").
    void newerThanDaysMatchesRecentFiles();
    void olderThanDaysMatchesStaleFiles();
    void dateWindowKeepsBothEnds();
    void dateCriteriaRejectAnUnknownModificationTime();
    void dateCriteriaAreAndedWithTheOthers();
    void fileMetaFromItemUsesTheOnDiskName();
};

void TestRules::noRulesReturnsNullopt()
{
    const FileMeta f{QStringLiteral("a.txt"), QStringLiteral("C:/Desktop/a.txt")};
    QVERIFY(!RuleEngine::classify(f, {}).has_value());
}

void TestRules::extensionMatchIsCaseInsensitive()
{
    Rule r;
    r.extensions = {QStringLiteral("jpg")};
    r.targetFenceId = QStringLiteral("f-media");

    const FileMeta jpeg{QStringLiteral("PHOTO.JPG"), QStringLiteral("C:/Desktop/PHOTO.JPG")};
    QCOMPARE(RuleEngine::classify(jpeg, {r}).value(), QStringLiteral("f-media"));
    QVERIFY(RuleEngine::matches(r, jpeg));
}

void TestRules::extensionMismatchReturnsNullopt()
{
    Rule r;
    r.extensions = {QStringLiteral("jpg")};
    const FileMeta txt{QStringLiteral("notes.txt"), QStringLiteral("C:/Desktop/notes.txt")};
    QVERIFY(!RuleEngine::classify(txt, {r}).has_value());
}

void TestRules::namePatternWildcard()
{
    Rule r;
    r.namePattern = QStringLiteral("screenshot*");
    r.targetFenceId = QStringLiteral("f-shots");

    const FileMeta hit{QStringLiteral("Screenshot 2026-10-13.png"), {}};
    QCOMPARE(RuleEngine::classify(hit, {r}).value(), QStringLiteral("f-shots"));

    const FileMeta miss{QStringLiteral("photo.png"), {}};
    QVERIFY(!RuleEngine::classify(miss, {r}).has_value());
}

void TestRules::firstMatchingRuleWins()
{
    Rule first;
    first.extensions = {QStringLiteral("png")};
    first.targetFenceId = QStringLiteral("f-media");
    Rule second;
    second.namePattern = QStringLiteral("screenshot*");
    second.targetFenceId = QStringLiteral("f-shots");

    const FileMeta screenshot{QStringLiteral("screenshot-1.png"), {}};
    QCOMPARE(RuleEngine::classify(screenshot, {first, second}).value(), QStringLiteral("f-media"));
}

void TestRules::emptyExtensionsMeansAny()
{
    Rule r;
    r.targetFenceId = QStringLiteral("f-catchall");
    const FileMeta any{QStringLiteral("whatever.bin"), {}};
    QCOMPARE(RuleEngine::classify(any, {r}).value(), QStringLiteral("f-catchall"));
}

void TestRules::moveFilesDefaultsToFalse()
{
    const Rule r;
    QVERIFY(!r.moveFiles); // golden rule: never move user files without opt-in
}

void TestRules::newerThanDaysMatchesRecentFiles()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-14T12:00:00"),
                                                Qt::ISODate);
    Rule r;
    r.newerThanDays = 7;
    r.targetFenceId = QStringLiteral("f-recent");

    FileMeta yesterday{QStringLiteral("a.txt"), {}};
    yesterday.modified = now.addDays(-1);
    QCOMPARE(RuleEngine::classify(yesterday, {r}, now).value(), QStringLiteral("f-recent"));

    FileMeta lastMonth{QStringLiteral("b.txt"), {}};
    lastMonth.modified = now.addDays(-30);
    QVERIFY(!RuleEngine::classify(lastMonth, {r}, now).has_value());
}

void TestRules::olderThanDaysMatchesStaleFiles()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-14T12:00:00"),
                                                Qt::ISODate);
    Rule r;
    r.olderThanDays = 30;
    r.targetFenceId = QStringLiteral("f-archive");

    FileMeta lastMonth{QStringLiteral("a.txt"), {}};
    lastMonth.modified = now.addDays(-45);
    QCOMPARE(RuleEngine::classify(lastMonth, {r}, now).value(), QStringLiteral("f-archive"));

    FileMeta today{QStringLiteral("b.txt"), {}};
    today.modified = now;
    QVERIFY(!RuleEngine::classify(today, {r}, now).has_value());
}

void TestRules::dateWindowKeepsBothEnds()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-14T12:00:00"),
                                                Qt::ISODate);
    Rule r;
    r.newerThanDays = 30; // no older than 30 days
    r.olderThanDays = 7;  // and no younger than 7 days
    r.targetFenceId = QStringLiteral("f-window");

    FileMeta inside{QStringLiteral("a.txt"), {}};
    inside.modified = now.addDays(-15);
    QVERIFY(RuleEngine::matches(r, inside, now));

    FileMeta tooFresh{QStringLiteral("b.txt"), {}};
    tooFresh.modified = now.addDays(-2);
    QVERIFY(!RuleEngine::matches(r, tooFresh, now));

    FileMeta tooOld{QStringLiteral("c.txt"), {}};
    tooOld.modified = now.addDays(-60);
    QVERIFY(!RuleEngine::matches(r, tooOld, now));
}

void TestRules::dateCriteriaRejectAnUnknownModificationTime()
{
    Rule r;
    r.newerThanDays = 7;
    const FileMeta unknown{QStringLiteral("a.txt"), {}}; // no modified time at all
    QVERIFY(!RuleEngine::matches(r, unknown, QDateTime::currentDateTime()));
}

void TestRules::dateCriteriaAreAndedWithTheOthers()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-14T12:00:00"),
                                                Qt::ISODate);
    Rule r;
    r.extensions = {QStringLiteral("png")};
    r.newerThanDays = 7;
    r.targetFenceId = QStringLiteral("f-shots");

    FileMeta freshPng{QStringLiteral("shot.png"), {}};
    freshPng.modified = now.addDays(-1);
    QVERIFY(RuleEngine::matches(r, freshPng, now));

    // Same extension, but far too old: the date criterion still rejects it.
    FileMeta oldPng{QStringLiteral("shot.png"), {}};
    oldPng.modified = now.addDays(-90);
    QVERIFY(!RuleEngine::matches(r, oldPng, now));

    // Fresh, but the wrong type.
    FileMeta freshTxt{QStringLiteral("notes.txt"), {}};
    freshTxt.modified = now.addDays(-1);
    QVERIFY(!RuleEngine::matches(r, freshTxt, now));
}

void TestRules::fileMetaFromItemUsesTheOnDiskName()
{
    DesktopItem item;
    item.name = QStringLiteral("Rapport final");       // what the Shell draws
    item.fileName = QStringLiteral("Rapport final.pdf"); // what is on disk
    item.path = QStringLiteral("C:/Desktop/Rapport final.pdf");
    item.modified = QDateTime::fromString(QStringLiteral("2026-10-01T09:00:00"), Qt::ISODate);

    const FileMeta meta = fileMetaFromItem(item);
    QCOMPARE(meta.name, QStringLiteral("Rapport final.pdf"));
    QCOMPARE(meta.path, item.path);
    QCOMPARE(meta.modified, item.modified);

    // Which is the whole point: an extension rule must be able to see ".pdf".
    Rule r;
    r.extensions = {QStringLiteral("pdf")};
    QVERIFY(RuleEngine::matches(r, meta));

    // A path-only item (no Shell metadata) falls back to the display name.
    DesktopItem bare;
    bare.path = QStringLiteral("C:/Desktop/notes.txt");
    bare.name = QStringLiteral("notes.txt");
    QCOMPARE(fileMetaFromItem(bare).name, QStringLiteral("notes.txt"));
}

QTEST_MAIN(TestRules)
#include "test_rules.moc"
