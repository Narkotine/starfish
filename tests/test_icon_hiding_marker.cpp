#include <QtTest>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDir>

#include "core/icon_hiding_marker.hpp"

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

// T-046: the memory that outlives a crash. The point of the marker is that it is
// decided by the *file*, not by any object state -- so the second half of every
// case below reads it back through a brand-new marker object.
class TestIconHidingMarker : public QObject
{
    Q_OBJECT

private slots:
    void aFreshMarkerIsNotPresent();
    void writingCreatesTheFileAndItsDirectory();
    void theMarkerRecordsWhoAndWhen();
    void removalIsIdempotent();
    void presenceIsDecidedByTheFileNotByItsContent();
    void writingTwiceOverwritesCleanly();
    void aSecondReaderSeesTheMarker();
    void theDefaultPathNamesTheProductState();
};

void TestIconHidingMarker::aFreshMarkerIsNotPresent()
{
    QTemporaryDir dir;
    const IconHidingMarker marker(dir.path() + QLatin1String("/icons-hidden.marker"));
    QVERIFY(!marker.isPresent());
    QVERIFY(marker.description().isEmpty());
}

void TestIconHidingMarker::writingCreatesTheFileAndItsDirectory()
{
    QTemporaryDir dir;
    // A nested directory on purpose: the product's state directory may not
    // exist yet on a machine that has never run StarFish.
    const QString path = dir.path() + QLatin1String("/StarFish/icons-hidden.marker");
    IconHidingMarker marker(path);

    QVERIFY(marker.write());
    QVERIFY(QFileInfo::exists(path));

    const IconHidingMarker second(path);
    QVERIFY(second.isPresent());
}

void TestIconHidingMarker::theMarkerRecordsWhoAndWhen()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/icons-hidden.marker");
    IconHidingMarker marker(path);
    QVERIFY(marker.write());

    const QJsonDocument document = QJsonDocument::fromJson(readFile(path).toUtf8());
    QVERIFY(document.isObject());
    const QJsonObject object = document.object();
    QCOMPARE(object.value(QStringLiteral("pid")).toInt(),
             static_cast<int>(QCoreApplication::applicationPid()));
    QVERIFY(!object.value(QStringLiteral("hiddenAt")).toString().isEmpty());

    // The description is what a log line or a probe report shows.
    QVERIFY(marker.description().contains(QStringLiteral("pid")));
}

void TestIconHidingMarker::removalIsIdempotent()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/icons-hidden.marker");
    IconHidingMarker marker(path);

    // Removing a marker that was never written is success, not failure: the
    // restore path must be able to run unconditionally.
    QVERIFY(marker.remove());

    QVERIFY(marker.write());
    QVERIFY(marker.remove());
    QVERIFY(!marker.isPresent());
    QVERIFY(marker.remove());
}

void TestIconHidingMarker::presenceIsDecidedByTheFileNotByItsContent()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/icons-hidden.marker");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not the JSON we write");
    }

    // A marker we cannot parse still counts: "was the layer hidden?" must never
    // depend on reading the note back.
    const IconHidingMarker marker(path);
    QVERIFY(marker.isPresent());
    QCOMPARE(marker.description(), QStringLiteral("unreadable marker"));
}

void TestIconHidingMarker::writingTwiceOverwritesCleanly()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/icons-hidden.marker");
    IconHidingMarker marker(path);

    QVERIFY(marker.write());
    QVERIFY(marker.write()); // a second hiding must not trip on its own note
    QVERIFY(marker.isPresent());
    QVERIFY(QJsonDocument::fromJson(readFile(path).toUtf8()).isObject());
}

void TestIconHidingMarker::aSecondReaderSeesTheMarker()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QLatin1String("/icons-hidden.marker");

    // What a *later run* sees, with no object state carried over.
    {
        IconHidingMarker writer(path);
        QVERIFY(writer.write());
    }
    const IconHidingMarker reader(path);
    QVERIFY(reader.isPresent());
    QVERIFY(!reader.description().isEmpty());
}

void TestIconHidingMarker::theDefaultPathNamesTheProductState()
{
    const QString path = IconHidingMarker::defaultPath();
    QVERIFY(!path.isEmpty());
    QVERIFY(path.endsWith(QLatin1String("/icons-hidden.marker")));
    // Same directory as the layout document, whatever the platform reports.
    const QString dir = QFileInfo(path).absolutePath();
    QVERIFY(!dir.isEmpty());
}

QTEST_MAIN(TestIconHidingMarker)
#include "test_icon_hiding_marker.moc"
