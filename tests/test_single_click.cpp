#include <QtTest/QtTest>
#include "core/single_click.hpp"

class TestSingleClick : public QObject
{
    Q_OBJECT
private slots:
    void inheritanceResolvesForZoneButtonAndDefault()
    {
        using starfish::core::resolvedSingleClickLaunch;
        // Explicit values win.
        QCOMPARE(resolvedSingleClickLaunch(QStringLiteral("on"), false, false), true);
        QCOMPARE(resolvedSingleClickLaunch(QStringLiteral("off"), true, true), false);
        QCOMPARE(resolvedSingleClickLaunch(QStringLiteral("on"), true, false), true);
        QCOMPARE(resolvedSingleClickLaunch(QStringLiteral("off"), false, true), false);
        // Empty: boutons default to on; zones follow the Settings default.
        QCOMPARE(resolvedSingleClickLaunch(QString(), true, false), true);
        QCOMPARE(resolvedSingleClickLaunch(QString(), true, true), true);
        QCOMPARE(resolvedSingleClickLaunch(QString(), false, false), false);
        QCOMPARE(resolvedSingleClickLaunch(QString(), false, true), true);
        // Unknown values inherit the same way as empty (backward compatible).
        QCOMPARE(resolvedSingleClickLaunch(QStringLiteral("maybe"), true, false), true);
        QCOMPARE(resolvedSingleClickLaunch(QStringLiteral("maybe"), false, false), false);
    }
};

QTEST_MAIN(TestSingleClick)
#include "test_single_click.moc"
