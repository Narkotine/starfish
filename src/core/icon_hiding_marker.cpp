#include "core/icon_hiding_marker.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>

namespace starfish::core {

namespace {

QLoggingCategory lc("starfish.marker");

constexpr int kSchemaVersion = 1;

} // namespace

QString IconHidingMarker::defaultPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.starfish");
    return dir + QStringLiteral("/icons-hidden.marker");
}

IconHidingMarker::IconHidingMarker(const QString& path)
    : m_path(path)
{
}

bool IconHidingMarker::isPresent() const
{
    return QFileInfo(m_path).isFile();
}

QString IconHidingMarker::description() const
{
    if (!isPresent())
        return QString();

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("unreadable marker");

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return QStringLiteral("unreadable marker");

    const QJsonObject object = document.object();
    const QString when = object.value(QStringLiteral("hiddenAt")).toString();
    const QString pid = object.contains(QStringLiteral("pid"))
                            ? QString::number(object.value(QStringLiteral("pid")).toInt())
                            : QStringLiteral("?");
    return when.isEmpty() ? QStringLiteral("pid %1").arg(pid)
                          : QStringLiteral("pid %1, hidden at %2").arg(pid, when);
}

bool IconHidingMarker::write()
{
    const QString dir = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(dir)) {
        qCWarning(lc).noquote()
            << QStringLiteral("could not create %1 for the icon-hiding marker").arg(dir);
        return false;
    }

    QJsonObject object;
    object.insert(QStringLiteral("version"), kSchemaVersion);
    object.insert(QStringLiteral("pid"),
                  QJsonValue(QCoreApplication::applicationPid()));
    object.insert(QStringLiteral("hiddenAt"),
                  QDateTime::currentDateTime().toString(Qt::ISODate));

    // Atomic, like every other write in this codebase: the marker either exists
    // complete or does not exist at all, and the layer is hidden only once it is
    // really on disk.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lc).noquote()
            << QStringLiteral("could not write the icon-hiding marker %1: %2")
                   .arg(m_path, file.errorString());
        return false;
    }
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        qCWarning(lc).noquote()
            << QStringLiteral("could not write the icon-hiding marker %1: %2")
                   .arg(m_path, file.errorString());
        return false;
    }

    qCInfo(lc).noquote() << QStringLiteral("icon-hiding marker written: %1").arg(m_path);
    return true;
}

bool IconHidingMarker::remove()
{
    if (!isPresent())
        return true;

    if (!QFile::remove(m_path)) {
        qCWarning(lc).noquote()
            << QStringLiteral("could not remove the icon-hiding marker %1").arg(m_path);
        return false;
    }

    qCInfo(lc).noquote() << QStringLiteral("icon-hiding marker removed: %1").arg(m_path);
    return true;
}

} // namespace starfish::core
