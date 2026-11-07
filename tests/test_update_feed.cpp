#include "core/update_feed.hpp"

#include <QTemporaryDir>
#include <QFile>
#include <QtTest>

using namespace starfish::core;

class TestUpdateFeed : public QObject {
    Q_OBJECT
private slots:
    void parsesAValidFeed()
    {
        const QByteArray json = R"({"schema":1,"latest":{"version":"0.2.0","url":"https://example.invalid/s.exe",
            "sha256":"A2D54459633E8947BC1A65A5F88FC65C296DCF08BE1535026830F11C3628C1DC","notes":"x","future":1}})";
        UpdateRelease r;
        QVERIFY(UpdateFeed::parse(json, &r));
        QCOMPARE(r.version, QStringLiteral("0.2.0"));
        QCOMPARE(r.sha256.size(), 64);
        QVERIFY(UpdateFeed::isNewer(r, QStringLiteral("0.1.0")));
        QVERIFY(!UpdateFeed::isNewer(r, QStringLiteral("0.2")));
    }
    void refusesUnsafeFeeds()
    {
        QVERIFY(!UpdateFeed::parse(R"({"schema":1,"latest":{"version":"1","url":"http://x/s.exe","sha256":"00"}})", nullptr));
        QVERIFY(!UpdateFeed::parse(R"({"schema":2,"latest":{}})", nullptr));
        QVERIFY(!UpdateFeed::parse("garbage", nullptr));
    }
    void comparesVersions()
    {
        QVERIFY(UpdateFeed::compareVersions(QStringLiteral("0.10.0"), QStringLiteral("0.9.9")) > 0);
        QCOMPARE(UpdateFeed::compareVersions(QStringLiteral("1.0"), QStringLiteral("1.0.0")), 0);
    }
    void verifiesTheHash()
    {
        QTemporaryDir dir;
        const QString p = dir.filePath(QStringLiteral("f.bin"));
        QFile f(p); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("abc"); f.close();
        const QString abc = QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        QVERIFY(UpdateFeed::verifySha256(p, abc));
        QVERIFY(!UpdateFeed::verifySha256(p, QString(64, QLatin1Char('0'))));
    }
    void isOffByDefault()
    {
        QVERIFY(!UpdateSettings{}.enabled);
    }
};

QTEST_MAIN(TestUpdateFeed)
#include "test_update_feed.moc"
