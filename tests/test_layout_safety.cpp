// Session 85 -- the layout survives updates: version stamp, backups before a
// change of version, a newer document never overwritten, unknown keys kept, a
// broken file recovered from the last good backup. Pure Qt Core.
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/jsonstore.hpp"
#include "core/layout_store.hpp"

using namespace starfish::core;

namespace {

// A document as StarFish 0.1.0 (no "writtenBy") writes it today: the fixture
// every later version must keep reading.
const char* kV1Fixture = R"({
  "schema": 1,
  "fences": [
    { "id": "f1", "title": "Docs", "monitor": "\\\\.\\DISPLAY1",
      "rect": {"x": 10, "y": 20, "w": 300, "h": 200},
      "style": {"background": "#CC1F2937", "radius": 8, "padding": 6, "iconSize": 48, "gap": 8,
                "titleBar": true, "fadeIcons": false, "defaultOpacity": true},
      "sort": "manual", "sortAscending": true, "rolledUp": false,
      "items": [
        {"kind": "file", "path": "C:/tmp/a.txt"},
        {"kind": "widget", "path": "widget:n1",
         "widget": {"type": "notes", "span": [2, 2], "settings": {"text": "hello"}}}
      ] }
  ],
  "rules": []
})";

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeAll(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

} // namespace

class TestLayoutSafety : public QObject
{
    Q_OBJECT

private slots:
    void readsTheV1FixtureAndStampsTheVersion()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("layout.json"));
        writeAll(path, kV1Fixture);
        LayoutStore store;
        store.setFilePath(path);
        store.setAppVersion(QStringLiteral("9.9.9"));
        QVERIFY(store.load());
        QCOMPARE(store.layout().fences.size(), 1);
        QCOMPARE(store.layout().fences.at(0).items.size(), 2);
        QVERIFY(!store.isReadOnly());
        // The unversioned file was copied before anything could change it.
        QCOMPARE(store.backups().size(), 1);
        QVERIFY(store.backups().first().contains(QLatin1String("layout.v1.")));
        QCOMPARE(readAll(store.backups().first()), QByteArray(kV1Fixture));
        Layout changed = store.layout();
        changed.fences[0].title = QStringLiteral("Docs 2");
        QVERIFY(store.setLayout(changed));
        QVERIFY(store.saveNow());
        const QJsonObject root = QJsonDocument::fromJson(readAll(path)).object();
        QCOMPARE(root["writtenBy"].toString(), QStringLiteral("9.9.9"));
        QCOMPARE(root["schema"].toInt(), 1);
        // Same version next time: no new backup.
        LayoutStore again;
        again.setFilePath(path);
        again.setAppVersion(QStringLiteral("9.9.9"));
        QVERIFY(again.load());
        QCOMPARE(again.backups().size(), 1);
    }

    void keepsKeysItDoesNotKnow()
    {
        QJsonObject root = QJsonDocument::fromJson(kV1Fixture).object();
        root["futureRoot"] = 42;
        QJsonArray fences = root["fences"].toArray();
        QJsonObject fence = fences[0].toObject();
        fence["futureFence"] = QStringLiteral("x");
        QJsonObject style = fence["style"].toObject();
        style["futureStyle"] = true;
        fence["style"] = style;
        QJsonArray items = fence["items"].toArray();
        QJsonObject tile = items[0].toObject();
        tile["futureTile"] = QJsonArray{1, 2};
        items[0] = tile;
        QJsonObject widget = items[1].toObject();
        QJsonObject w = widget["widget"].toObject();
        w["futureWidget"] = 7;
        widget["widget"] = w;
        items[1] = widget;
        fence["items"] = items;
        fences[0] = fence;
        root["fences"] = fences;

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("layout.json"));
        writeAll(path, QJsonDocument(root).toJson());
        LayoutStore store;
        store.setFilePath(path);
        QVERIFY(store.load());
        Layout changed = store.layout();
        changed.fences[0].rect.moveLeft(99); // a real edit, so the file is rewritten
        QVERIFY(store.setLayout(changed));
        QVERIFY(store.saveNow());
        const QJsonObject out = QJsonDocument::fromJson(readAll(path)).object();
        const QJsonObject f = out["fences"].toArray()[0].toObject();
        QCOMPARE(out["futureRoot"].toInt(), 42);
        QCOMPARE(f["futureFence"].toString(), QStringLiteral("x"));
        QCOMPARE(f["style"].toObject()["futureStyle"].toBool(), true);
        QCOMPARE(f["items"].toArray()[0].toObject()["futureTile"].toArray().size(), 2);
        QCOMPARE(f["items"].toArray()[1].toObject()["widget"].toObject()["futureWidget"].toInt(), 7);
        QCOMPARE(f["rect"].toObject()["x"].toInt(), 99);
        // A key this build owns and now omits is not resurrected from the old file.
        QVERIFY(!f["style"].toObject().contains(QLatin1String("borderWidth")));
    }

    void neverWritesOverANewerDocument()
    {
        QByteArray newer = kV1Fixture;
        newer.replace("\"schema\": 1", "\"schema\": 5");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("layout.json"));
        writeAll(path, newer);
        LayoutStore store;
        store.setFilePath(path);
        QVERIFY(store.load());
        QVERIFY(store.isReadOnly());
        QCOMPARE(store.layout().fences.size(), 1); // shown as far as understood
        Layout changed = store.layout();
        changed.fences[0].title = QStringLiteral("overwrite?");
        store.setLayout(changed);
        QString error;
        QVERIFY(!store.saveNow(&error));
        QVERIFY(error.contains(QLatin1String("newer")));
        QCOMPARE(readAll(path), newer);
    }

    void recoversABrokenFileFromTheLastGoodBackup()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("layout.json"));
        writeAll(path, kV1Fixture);
        LayoutStore first;
        first.setFilePath(path);
        QVERIFY(!first.backupNow(QStringLiteral("manual")).isEmpty());
        writeAll(path, "{ \"schema\": 1, \"fences\": [ {"); // truncated mid-write
        LayoutStore store;
        store.setFilePath(path);
        QVERIFY(store.load());
        QVERIFY(!store.recoveredFrom().isEmpty());
        QCOMPARE(store.layout().fences.size(), 1);
        QVERIFY(store.isDirty()); // the good copy goes back to disk at the next save
        bool keptBroken = false;
        for (const QString& b : store.backups())
            keptBroken = keptBroken || b.contains(QLatin1String(".corrupt."));
        QVERIFY(keptBroken);
        QVERIFY(store.saveNow());
        QVERIFY(JsonStore::fromJson(readAll(path)).has_value());
    }

    void protectsABrokenFileWithNoBackup()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("layout.json"));
        writeAll(path, "not json");
        LayoutStore store;
        store.setFilePath(path);
        QVERIFY(!store.load());
        QVERIFY(store.isReadOnly());
        Layout l;
        l.fences.append(Fence{});
        store.setLayout(l);
        QVERIFY(!store.saveNow());
        QCOMPARE(readAll(path), QByteArray("not json"));
    }

    void keepsOnlyTheNewestBackups()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("layout.json"));
        writeAll(path, kV1Fixture);
        LayoutStore store;
        store.setFilePath(path);
        for (int i = 0; i < LayoutStore::kMaxBackups + 4; ++i) {
            QVERIFY(!store.backupNow(QStringLiteral("n%1").arg(i)).isEmpty());
            QTest::qWait(15); // distinct timestamps and mtimes
        }
        QCOMPARE(store.backups().size(), LayoutStore::kMaxBackups);
        QVERIFY(store.backups().first().contains(QStringLiteral(".n%1.").arg(LayoutStore::kMaxBackups + 3)));
    }

    void migrationPipelineRefusesAMissingStep()
    {
        QJsonObject root = QJsonDocument::fromJson(kV1Fixture).object();
        QString error;
        QVERIFY(JsonStore::migrate(root, JsonStore::kCurrentSchema, &error)); // nothing to do
        QVERIFY(!JsonStore::migrate(root, 0, &error) || JsonStore::kCurrentSchema == 0);
    }
};

QTEST_MAIN(TestLayoutSafety)
#include "test_layout_safety.moc"
