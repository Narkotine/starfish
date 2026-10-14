// T-011 -- F-024: what a folder portal actually lists.
//
// The listing is the whole read half of a portal: no Shell, no Win32, just QDir
// and the local filesystem (exactly like itemFromPathOnly), so it is tested
// against a real temporary directory.
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/portal.hpp"

using namespace starfish::core;

namespace {

bool writeFile(const QString& path, const QByteArray& payload = QByteArrayLiteral("x"))
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(payload) == payload.size();
}

QStringList namesOf(const QList<DesktopItem>& items)
{
    QStringList names;
    for (const DesktopItem& item : items)
        names.append(item.fileName);
    return names;
}

} // namespace

class TestPortal : public QObject
{
    Q_OBJECT

private slots:
    void portalKindIsRecognised();
    void pathIsKeptVerbatim();
    void listsAFolderNaturally();
    void dotEntriesAreSkipped();
    void emptyFolderIsNotAnError();
    void aMissingFolderIsReportedNotFatal();
    void aFileIsNotAFolder();
    void sameFolderNormalisesSeparatorsAndCase();
};

void TestPortal::portalKindIsRecognised()
{
    QVERIFY(isPortalKind(QStringLiteral("portal")));
    QVERIFY(isPortalKind(QStringLiteral("PORTAL")));
    QVERIFY(isPortalKind(QStringLiteral("Portal")));
    QVERIFY(!isPortalKind(QStringLiteral("file")));
    // A hand-written document may omit `kind`: that is a plain file reference,
    // and documents written before T-011 have to keep loading.
    QVERIFY(!isPortalKind(QString()));
}

void TestPortal::pathIsKeptVerbatim()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Whatever the caller declared is what the document will hold: QDir would
    // normalise "C:\x" to "C:/x", and that would stop a document from
    // round-tripping byte for byte.
    const QString declared = dir.path() + QStringLiteral("/");
    const Portal portal = listPortal(declared);
    QCOMPARE(portal.path, declared);
    QVERIFY(portal.error.isEmpty());

    const QString missing = dir.filePath(QStringLiteral("nope"));
    QCOMPARE(listPortal(missing).path, missing);
}

void TestPortal::listsAFolderNaturally()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(QStringLiteral("shot10.png"))));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("shot9.png"))));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("notes.txt")), QByteArrayLiteral("12345")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sub")));

    const Portal portal = listPortal(dir.path());
    QVERIFY2(portal.error.isEmpty(), qPrintable(portal.error));

    // Natural order: "shot9" before "shot10", folders are entries like the rest.
    QCOMPARE(namesOf(portal.items),
             QStringList({QStringLiteral("notes.txt"), QStringLiteral("shot9.png"),
                          QStringLiteral("shot10.png"), QStringLiteral("sub")}));

    bool sawFolder = false;
    bool sawFile = false;
    for (const DesktopItem& item : portal.items) {
        if (item.fileName == QLatin1String("sub")) {
            sawFolder = item.isDir;
            QCOMPARE(item.sizeBytes, qint64(-1)); // a folder has no size
        }
        if (item.fileName == QLatin1String("notes.txt")) {
            sawFile = !item.isDir;
            QCOMPARE(item.sizeBytes, qint64(5));
            QVERIFY(item.modified.isValid());
        }
        QVERIFY(!item.path.isEmpty());
    }
    QVERIFY(sawFolder);
    QVERIFY(sawFile);
}

void TestPortal::dotEntriesAreSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(QStringLiteral(".secret"))));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("visible.txt"))));

    const Portal portal = listPortal(dir.path());
    QCOMPARE(namesOf(portal.items), QStringList({QStringLiteral("visible.txt")}));
}

void TestPortal::emptyFolderIsNotAnError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const Portal portal = listPortal(dir.path());
    QVERIFY(portal.error.isEmpty());
    QVERIFY(portal.items.isEmpty());
}

void TestPortal::aMissingFolderIsReportedNotFatal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const Portal portal = listPortal(dir.filePath(QStringLiteral("unplugged")));
    QVERIFY(!portal.error.isEmpty());
    QVERIFY(portal.items.isEmpty());
    // The path survives: the caller keeps the portal in the document and shows
    // nothing until the folder is reachable again.
    QCOMPARE(portal.path, dir.filePath(QStringLiteral("unplugged")));
}

void TestPortal::aFileIsNotAFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.txt"));
    QVERIFY(writeFile(path));

    const Portal portal = listPortal(path);
    QVERIFY(!portal.error.isEmpty());
    QVERIFY(portal.items.isEmpty());
}

void TestPortal::sameFolderNormalisesSeparatorsAndCase()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(sameFolder(dir.path(), dir.path()));
    // Separator style, redundant segments and case all describe one folder.
    QVERIFY(sameFolder(dir.path(), dir.path() + QStringLiteral("/")));
    QVERIFY(sameFolder(dir.path(), dir.path() + QStringLiteral("/./")));
    QVERIFY(sameFolder(dir.path().toUpper(), dir.path().toLower()));
    QVERIFY(!sameFolder(dir.path(), dir.filePath(QStringLiteral("elsewhere"))));
    QVERIFY(!sameFolder(QString(), dir.path()));
}

QTEST_MAIN(TestPortal)
#include "test_portal.moc"
