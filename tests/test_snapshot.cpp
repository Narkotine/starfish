#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

#include "core/jsonstore.hpp"
#include "core/snapshot.hpp"

using namespace starfish::core;

namespace {
Layout sampleLayout()
{
    Layout l;
    l.schema = 1;
    Fence f;
    f.id = "f-main";
    f.title = "Main";
    f.rect = QRect(0, 0, 400, 300);
    f.items.append(FenceItem{QLatin1String("file"),QLatin1String("C:/Desktop/a.txt")});
    l.fences.append(f);
    return l;
}
QString readFile(const QString& path)
{
    if (!QFileInfo::exists(path))
        return QString();
    QFile f(path); f.open(QIODevice::ReadOnly); return QString::fromUtf8(f.readAll());
}
} // namespace

class TestSnapshot : public QObject
{
    Q_OBJECT

private slots:
    void invalidIdsAreRefused();
    void saveAndListRoundTrips();
    void loadReturnsTheLayoutAndMetadata();
    void loadRejectsAPlainLayoutFile();
    void loadRejectsAnUnknownId();
    void removeSnapshot();
    void twoSnapshotsAreIndependent();
    void exportAndImportRoundTrip();
    void exportRejectsASnapshotFile();
    void largeSnapshotIsNotALayoutFile();
    void snapshotsDirIsNextToTheLayout();
    void aSnapshotIsALayoutTheApplicationUnderstands();
};

void TestSnapshot::invalidIdsAreRefused()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    const QStringList bad = {QLatin1String(""),QLatin1String("."),QLatin1String(".."),
                           QLatin1String("a/b"),QLatin1String("a\\b"),QLatin1String("a:b"),
                           QLatin1String("a?"),QLatin1String("a*"),QLatin1String("a<b"),
                           QLatin1String("a>b"),QLatin1String("a|b"),QLatin1String("a."),
                           QLatin1String(" a")};
    for (const QString& id : bad) {
        QString e;
        const bool ok = m.saveSnapshot(dir, l, id, {}, &e);
        if (ok)
            QFAIL("bad id was accepted");
        if (e.isEmpty())
            QFAIL("no error message");
    }
}

void TestSnapshot::saveAndListRoundTrips()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, l, "before", "before the change"))
        QFAIL("save failed");
    if (!m.saveSnapshot(dir, l, "after", {}))
        QFAIL("save failed");

    const QStringList ids = m.listSnapshots(dir);
    if (ids.size() != 2)
        QFAIL("wrong count");
    if (!ids.contains("before"))
        QFAIL("missing before");
    if (!ids.contains("after"))
        QFAIL("missing after");

    if (!QFileInfo::exists(m.snapshotFilePath(dir, "before")))
        QFAIL("file missing");
    if (QFileInfo::exists(m.snapshotFilePath(dir, "ghost")))
        QFAIL("ghost exists");
}

void TestSnapshot::loadReturnsTheLayoutAndMetadata()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, l, "snap-1", "because"))
        QFAIL("save failed");

    Snapshot loaded;
    if (!m.loadSnapshot(dir, "snap-1", loaded))
        QFAIL("load failed");
    if (loaded.id != "snap-1")
        QFAIL("id wrong");
    if (loaded.note != "because")
        QFAIL("note wrong");
    if (loaded.createdAt.isEmpty())
        QFAIL("no createdAt");
    if (loaded.layout.fences.size() != 1)
        QFAIL("fence count");
    if (loaded.layout.fences.at(0).id != "f-main")
        QFAIL("fence id");
}

void TestSnapshot::loadRejectsAPlainLayoutFile()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!JsonStore::save(l, m.snapshotFilePath(dir, "plain")))
        QFAIL("save failed");

    Snapshot loaded;
    QString err;
    if (m.loadSnapshot(dir, "plain", loaded, &err))
        QFAIL("plain layout was accepted as a snapshot");
    if (err.isEmpty())
        QFAIL("no error message");
}

void TestSnapshot::loadRejectsAnUnknownId()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, l, "real"))
        QFAIL("save failed");

    Snapshot loaded;
    if (m.loadSnapshot(dir, "ghost", loaded))
        QFAIL("unknown id was accepted");
}

void TestSnapshot::removeSnapshot()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, l, "doomed"))
        QFAIL("save failed");
    if (!QFileInfo::exists(m.snapshotFilePath(dir, "doomed")))
        QFAIL("missing");

    if (!m.removeSnapshot(dir, "doomed"))
        QFAIL("remove failed");
    if (QFileInfo::exists(m.snapshotFilePath(dir, "doomed")))
        QFAIL("still there");

    if (m.removeSnapshot(dir, "doomed"))
        QFAIL("second remove should fail");
    if (m.removeSnapshot(dir, "a/b"))
        QFAIL("bad id removed");
}

void TestSnapshot::twoSnapshotsAreIndependent()
{
    SnapshotManager m;
    Layout first = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, first, "a"))
        QFAIL("save failed");

    Layout second = sampleLayout();
    second.fences[0].id = "f-other";
    second.fences[0].title = "Other";
    if (!m.saveSnapshot(dir, second, "b"))
        QFAIL("save failed");

    Snapshot a;
    if (!m.loadSnapshot(dir, "a", a))
        QFAIL("load a failed");
    Snapshot b;
    if (!m.loadSnapshot(dir, "b", b))
        QFAIL("load b failed");
    if (a.layout.fences.at(0).id != "f-main")
        QFAIL("a changed");
    if (b.layout.fences.at(0).id != "f-other")
        QFAIL("b wrong");

    const QStringList ids = m.listSnapshots(dir);
    if (ids.size() != 2)
        QFAIL("count");
}

void TestSnapshot::exportAndImportRoundTrip()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    const QString exportPath = dir + "/exported.json";
    if (!m.exportLayout(l, exportPath))
        QFAIL("export failed");

    const auto imported = m.importLayout(exportPath);
    if (!imported.has_value())
        QFAIL("import failed");
    if (imported->fences.size() != 1)
        QFAIL("fence count");
    if (imported->fences.at(0).id != "f-main")
        QFAIL("fence id");

    // A snapshot file is not a layout file: it carries a `snapshot` envelope a
    // disposition does not, and importing one would drop that metadata. The
    // inverse of export is import; the inverse of save is restore.
    if (!m.saveSnapshot(dir, l, "snap"))
        QFAIL("save failed");
    if (m.importLayout(m.snapshotFilePath(dir, "snap")).has_value())
        QFAIL("a snapshot was imported as a layout");
}

void TestSnapshot::exportRejectsASnapshotFile()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, l, "taken"))
        QFAIL("save failed");

    if (m.exportLayout(l, m.snapshotFilePath(dir, "taken")))
        QFAIL("export overwrote a snapshot");

    Snapshot stillThere;
    if (!m.loadSnapshot(dir, "taken", stillThere))
        QFAIL("snapshot lost");
}

void TestSnapshot::largeSnapshotIsNotALayoutFile()
{
    // The `snapshot` key sorts last in a QJsonObject, so a guard that scans only
    // the first bytes of the file would miss it on a big layout. Prove the
    // refusal is size-independent: a large snapshot is still not a layout file.
    SnapshotManager m;
    Layout big = sampleLayout();
    for (int i = 0; i < 200; ++i) {
        big.fences[0].items.append(
            FenceItem{QStringLiteral("file"),
                      QStringLiteral("C:/Desktop/a-fairly-long-file-name-%1.txt").arg(i)});
    }

    QTemporaryDir d;
    const QString dir = d.path();
    if (!m.saveSnapshot(dir, big, "big"))
        QFAIL("save failed");

    const QString path = m.snapshotFilePath(dir, "big");
    if (QFileInfo(path).size() <= 4096)
        QFAIL("fixture is not big enough to exercise the guard");

    // The premise of the regression: `snapshot` sorts last, so with the old
    // byte-window guard the envelope fell outside the first 4096 bytes, the
    // scan found nothing, and import *and* export both accepted the snapshot.
    // Keep proving the fixture still reproduces that situation.
    {
        QFile probe(path);
        if (!probe.open(QIODevice::ReadOnly))
            QFAIL("cannot read the snapshot file");
        if (probe.read(4096).contains("\"snapshot\""))
            QFAIL("fixture does not exercise the byte-window bug");
    }

    if (m.importLayout(path).has_value())
        QFAIL("a large snapshot was imported as a layout");
    if (m.exportLayout(big, path))
        QFAIL("export overwrote a large snapshot");

    Snapshot intact;
    if (!m.loadSnapshot(dir, "big", intact))
        QFAIL("snapshot lost");
    if (intact.layout.fences.at(0).items.size() != big.fences.at(0).items.size())
        QFAIL("snapshot content changed");
}

void TestSnapshot::snapshotsDirIsNextToTheLayout()
{
    QTemporaryDir d;
    const QString layoutPath = d.path() + "/layout.json";

    const QString dir = SnapshotManager::snapshotsDirFor(layoutPath);
    if (dir != d.path() + "/snapshots")
        QFAIL("wrong dir");

    const QString def = SnapshotManager::snapshotsDirFor({});
    if (def.isEmpty())
        QFAIL("empty default");
    if (!def.endsWith("/snapshots"))
        QFAIL("default not snapshots");
}

void TestSnapshot::aSnapshotIsALayoutTheApplicationUnderstands()
{
    SnapshotManager m;
    const Layout l = sampleLayout();
    QTemporaryDir d;
    const QString dir = d.path();

    if (!m.saveSnapshot(dir, l, "round-trip"))
        QFAIL("save failed");

    Snapshot loaded;
    if (!m.loadSnapshot(dir, "round-trip", loaded))
        QFAIL("load failed");

    const QByteArray bytes = JsonStore::toJson(loaded.layout);
    const auto reparsed = JsonStore::fromJson(bytes);
    if (!reparsed.has_value())
        QFAIL("reparse failed");
    if (reparsed->fences.size() != 1)
        QFAIL("fence count");

    const QString snapPath = m.snapshotFilePath(dir, "round-trip");
    if (!QFileInfo::exists(snapPath))
        QFAIL("snapshot file does not exist on disk");
    const QString raw = readFile(snapPath);
    if (raw.isEmpty()) {
        const QString err =QFile(snapPath).errorString();
        QFAIL(qPrintable("readFile returned empty: " + err));
    }
    if (!raw.contains("\"snapshot\""))
        QFAIL("no envelope");
    if (!raw.contains("\"schema\""))
        QFAIL("no schema key");
}

QTEST_MAIN(TestSnapshot)
#include "test_snapshot.moc"
