#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/preferences.hpp"

using namespace starfish::core;

namespace {
QString readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}
} // namespace

// T-088: the user's own choices, and the two things that matter about them.
//
// The first is that a *later run* sees them -- that is the whole point of the
// file, and it is why every case below reads its value back through a brand-new
// object instead of trusting the one that wrote it.
//
// The second is the direction of the failure. The one preference that exists
// decides whether StarFish takes the desktop's icons over, so both a missing file
// and a corrupt one have to land on the **defaults** -- never on a guess. A file
// that turns the takeover on by accident is a desktop that answers nothing (T-066).
class TestPreferences : public QObject
{
    Q_OBJECT

private slots:
    void aFreshFileKeepsTheDefaults();
    void theDefaultPathNamesTheProductState();
    void savingAndReadingBackSurvivesANewObject();
    void aMissingKeyReadsAsTheDefault();
    void aCorruptFileFallsBackToTheDefaults();
    void aCorruptFileIsReported();
    void savingCreatesTheDirectory();
    void theFileIsTheJsonWeWrote();
};

void TestPreferences::aFreshFileKeepsTheDefaults()
{
    QTemporaryDir dir;
    Preferences prefs(dir.path() + QLatin1String("/preferences.json"));

    // A missing file is "nothing has been chosen yet", not a failure: first launch
    // must not be an error path.
    QString error;
    QVERIFY(prefs.load(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(!prefs.desktopIconsOwned());
}

void TestPreferences::theDefaultPathNamesTheProductState()
{
    const QString path = Preferences::defaultFilePath();
    QVERIFY(!path.isEmpty());
    // It names *the file*, and it is not the layout document: a preference must not
    // travel inside an exported layout (F-028).
    QVERIFY(path.endsWith(QLatin1String("/preferences.json")));
    QVERIFY(!QFileInfo(path).absolutePath().isEmpty());
}

void TestPreferences::savingAndReadingBackSurvivesANewObject()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/preferences.json");

    {
        Preferences prefs(path);
        prefs.setDesktopIconsOwned(true);
        QVERIFY(prefs.save());
    }

    // What the *next* run reads: no object state carried over anywhere.
    Preferences later(path);
    QVERIFY(later.load());
    QVERIFY(later.desktopIconsOwned());

    // And back off again, because the switch has to be reversible.
    later.setDesktopIconsOwned(false);
    QVERIFY(later.save());
    Preferences third(path);
    QVERIFY(third.load());
    QVERIFY(!third.desktopIconsOwned());
}

void TestPreferences::aMissingKeyReadsAsTheDefault()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/preferences.json");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // A file written by a run that knew a different set of keys (ADR-5's
        // discipline for the document, applied here): an absent key describes the
        // state a document was written with, so it reads as the default.
        file.write("{\"version\":1}");
    }

    Preferences prefs(path);
    QVERIFY(prefs.load());
    QVERIFY(!prefs.desktopIconsOwned());
}

void TestPreferences::aCorruptFileFallsBackToTheDefaults()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/preferences.json");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not the JSON we wrote");
    }

    Preferences prefs(path);
    QVERIFY(!prefs.load()); // reported...
    QVERIFY(!prefs.desktopIconsOwned()); // ...and the safe answer stands
}

void TestPreferences::aCorruptFileIsReported()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/preferences.json");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[1, 2, 3]"); // valid JSON, wrong shape
    }

    Preferences prefs(path);
    QString error;
    QVERIFY(!prefs.load(&error));
    QVERIFY(error.contains(path));
}

void TestPreferences::savingCreatesTheDirectory()
{
    QTemporaryDir dir;
    // A nested directory on purpose: the product's state directory may not exist
    // yet on a machine that has never run StarFish.
    const QString path = dir.path() + QLatin1String("/StarFish/preferences.json");

    Preferences prefs(path);
    prefs.setDesktopIconsOwned(true);
    QVERIFY(prefs.save());
    QVERIFY(QFileInfo::exists(path));

    Preferences reader(path);
    QVERIFY(reader.load());
    QVERIFY(reader.desktopIconsOwned());
}

void TestPreferences::theFileIsTheJsonWeWrote()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/preferences.json");

    Preferences prefs(path);
    prefs.setDesktopIconsOwned(true);
    QVERIFY(prefs.save());

    const QJsonDocument document = QJsonDocument::fromJson(readFile(path).toUtf8());
    QVERIFY(document.isObject());
    const QJsonObject object = document.object();
    // The value is a real JSON boolean, not a string that happens to look like one:
    // a user reading the file has to see what the switch means.
    QCOMPARE(object.value(QStringLiteral("desktopIconsOwned")), QJsonValue(true));
    QVERIFY(object.contains(QStringLiteral("version")));
}

QTEST_MAIN(TestPreferences)
#include "test_preferences.moc"
