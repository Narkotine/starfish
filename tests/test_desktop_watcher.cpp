// T-010 -- F-018: noticing that a file appeared in a watched folder.
//
// The diff itself is pure and tested against synthetic listings; the rest is
// tested against a real temporary directory, because the whole point of the
// class is how QFileSystemWatcher behaves on this platform.
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "core/desktop_watcher.hpp"

using namespace starfish::core;

namespace {

bool writeFile(const QString& path, const QByteArray& payload = QByteArrayLiteral("x"))
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(payload) == payload.size();
}

} // namespace

class TestDesktopWatcher : public QObject
{
    Q_OBJECT

private slots:
    void newEntriesIsAPureDiff();
    void startDoesNotReportWhatIsAlreadyThere();
    void reportsAFileThatAppears();
    void collapsesABurstIntoOneSignal();
    void deletingAFileIsNotAnAddition();
};

void TestDesktopWatcher::newEntriesIsAPureDiff()
{
    QCOMPARE(DesktopWatcher::newEntries({}, {}), QStringList{});
    QCOMPARE(DesktopWatcher::newEntries({QStringLiteral("a")}, {QStringLiteral("a")}),
             QStringList{});
    QCOMPARE(DesktopWatcher::newEntries({QStringLiteral("a")},
                                        {QStringLiteral("a"), QStringLiteral("b")}),
             QStringList{QStringLiteral("b")});
    // Order follows the current listing; duplicates are dropped.
    QCOMPARE(DesktopWatcher::newEntries({}, {QStringLiteral("b"), QStringLiteral("a"),
                                             QStringLiteral("b")}),
             QStringList({QStringLiteral("b"), QStringLiteral("a")}));
}

void TestDesktopWatcher::startDoesNotReportWhatIsAlreadyThere()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(QStringLiteral("before.txt"))));

    DesktopWatcher watcher;
    watcher.setFolders({dir.path()});
    watcher.setDebounceInterval(50);

    QSignalSpy spy(&watcher, &DesktopWatcher::itemsAdded);
    watcher.start();
    QVERIFY(watcher.isWatching());

    // The baseline holds the pre-existing entry, so a rule cannot re-file the
    // whole desktop on every launch.
    QCOMPARE(watcher.snapshot().size(), 1);

    // Touch the folder so a notification does arrive: it must still be silent,
    // because nothing new appeared.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("before.txt")), QByteArrayLiteral("changed")));
    QTest::qWait(300);
    QCOMPARE(spy.count(), 0);

    watcher.stop();
    QVERIFY(!watcher.isWatching());
}

void TestDesktopWatcher::reportsAFileThatAppears()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    DesktopWatcher watcher;
    watcher.setFolders({dir.path()});
    watcher.setDebounceInterval(50);
    watcher.start();

    QSignalSpy spy(&watcher, &DesktopWatcher::itemsAdded);
    const QString dropped = dir.filePath(QStringLiteral("shot-1.png"));
    QVERIFY(writeFile(dropped));

    QVERIFY(spy.wait(5000));
    const QStringList added = spy.at(0).at(0).toStringList();
    QCOMPARE(added.size(), 1);
    QCOMPARE(QFileInfo(added.at(0)).fileName(), QStringLiteral("shot-1.png"));
    QCOMPARE(QFileInfo(added.at(0)).absolutePath(), QFileInfo(dropped).absolutePath());

    watcher.stop();
}

void TestDesktopWatcher::collapsesABurstIntoOneSignal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    DesktopWatcher watcher;
    watcher.setFolders({dir.path()});
    watcher.setDebounceInterval(150);
    watcher.start();

    QSignalSpy spy(&watcher, &DesktopWatcher::itemsAdded);
    // Three files in a row, without spinning the event loop in between: this is
    // what dropping several files at once looks like, and Windows sends several
    // notifications per file.
    for (int i = 0; i < 3; ++i)
        QVERIFY(writeFile(dir.filePath(QStringLiteral("shot-%1.png").arg(i))));

    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.at(0).at(0).toStringList().size(), 3);

    // And the debounce really did collapse them: no second batch follows.
    QTest::qWait(600);
    QCOMPARE(spy.count(), 1);

    watcher.stop();
}

void TestDesktopWatcher::deletingAFileIsNotAnAddition()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    DesktopWatcher watcher;
    watcher.setFolders({dir.path()});
    watcher.setDebounceInterval(50);
    watcher.start();

    QSignalSpy spy(&watcher, &DesktopWatcher::itemsAdded);
    const QString path = dir.filePath(QStringLiteral("gone.txt"));
    QVERIFY(writeFile(path));
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.count(), 1);

    // A removal changes the directory too, but it is not a new entry -- and the
    // watcher must survive it (the baseline shrinks).
    spy.clear();
    QVERIFY(QFile::remove(path));
    QTest::qWait(300);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(watcher.snapshot().size(), 0);

    // The watch is still live afterwards.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("again.txt"))));
    QVERIFY(spy.wait(5000));
    QCOMPARE(QFileInfo(spy.at(0).at(0).toStringList().at(0)).fileName(),
             QStringLiteral("again.txt"));

    watcher.stop();
}

QTEST_MAIN(TestDesktopWatcher)
#include "test_desktop_watcher.moc"
