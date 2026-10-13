#include <QtTest>

#include "core/zorder.hpp"

using namespace starfish::core;

namespace {
// QCOMPARE on a plain enum class relies on QTest::toString specialisations:
// compare as booleans to keep the test portable across Qt versions.
bool isBelow(int ourIndex, int defViewIndex)
{
    return evaluatePlacement(ourIndex, defViewIndex) == PlacementVerdict::BelowIconLayer;
}

bool isAbove(int ourIndex, int defViewIndex)
{
    return evaluatePlacement(ourIndex, defViewIndex) == PlacementVerdict::AboveIconLayer;
}

bool isUnknown(int ourIndex, int defViewIndex)
{
    return evaluatePlacement(ourIndex, defViewIndex) == PlacementVerdict::Unknown;
}
} // namespace

class TestZOrder : public QObject
{
    Q_OBJECT

private slots:
    void greaterIndexIsBelowIconLayer();
    void smallerIndexIsAboveIconLayer();
    void equalIndexIsAboveIconLayer();
    void missingWindowIsUnknown();
    void missingIconLayerIsUnknown();
    void verdictTextIsStable();
};

void TestZOrder::greaterIndexIsBelowIconLayer()
{
    // Measured in the T-003 spike: DefView [0] ... our window [3].
    QVERIFY(isBelow(3, 0));
    QVERIFY(!isAbove(3, 0));
}

void TestZOrder::smallerIndexIsAboveIconLayer()
{
    // The session-4 failure: our window ends up on top of the icons.
    QVERIFY(isAbove(0, 2));
}

void TestZOrder::equalIndexIsAboveIconLayer()
{
    // Same index means the same window: a failure, not a success.
    QVERIFY(isAbove(2, 2));
}

void TestZOrder::missingWindowIsUnknown()
{
    QVERIFY(isUnknown(-1, 0));
}

void TestZOrder::missingIconLayerIsUnknown()
{
    QVERIFY(isUnknown(1, -1));
}

void TestZOrder::verdictTextIsStable()
{
    QCOMPARE(QString::fromLatin1(placementVerdictText(PlacementVerdict::BelowIconLayer)),
             QStringLiteral("below icon layer"));
    QCOMPARE(QString::fromLatin1(placementVerdictText(PlacementVerdict::AboveIconLayer)),
             QStringLiteral("above icon layer"));
    QVERIFY(QString::fromLatin1(placementVerdictText(PlacementVerdict::Unknown))
                .startsWith(QStringLiteral("unknown")));
}

QTEST_MAIN(TestZOrder)
#include "test_zorder.moc"
