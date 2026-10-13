#include <QtTest>

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

QTEST_MAIN(TestRules)
#include "test_rules.moc"
