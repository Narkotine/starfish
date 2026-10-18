#include "core/preferences.hpp"

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

QLoggingCategory lc("starfish.preferences");

constexpr int kSchemaVersion = 1;

const QLatin1String kDesktopIconsOwned("desktopIconsOwned");

} // namespace

QString Preferences::defaultFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.starfish");
    return dir + QStringLiteral("/preferences.json");
}

Preferences::Preferences(const QString& path)
    : m_path(path)
{
}

bool Preferences::load(QString* error)
{
    if (!QFileInfo::exists(m_path)) {
        // First run: the defaults *are* the state, not a failure. Nothing is
        // written, so a launch that never chooses anything leaves no file behind.
        qCInfo(lc).noquote()
            << QStringLiteral("no preferences file at %1, using the defaults").arg(m_path);
        return true;
    }

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("could not read %1: %2").arg(m_path, file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("%1 is not the JSON we wrote: %2")
                         .arg(m_path, parseError.errorString());
        // The defaults stand: see the class comment -- the wrong answer here is
        // the one that takes the user's icons away.
        return false;
    }

    const QJsonObject object = document.object();
    // Every key is read as "absent means the default", so a file written by an
    // older run keeps describing the choices it described (ADR-5's discipline for
    // the document, applied to the preferences).
    m_desktopIconsOwned = object.value(kDesktopIconsOwned).toBool(m_desktopIconsOwned);

    qCInfo(lc).noquote()
        << QStringLiteral("preferences read from %1 (desktopIconsOwned=%2)")
               .arg(m_path,
                    m_desktopIconsOwned ? QStringLiteral("true") : QStringLiteral("false"));
    return true;
}

bool Preferences::save(QString* error) const
{
    const QString dir = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("could not create %1").arg(dir);
        return false;
    }

    QJsonObject object;
    object.insert(QStringLiteral("version"), kSchemaVersion);
    object.insert(kDesktopIconsOwned, m_desktopIconsOwned);

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("could not write %1: %2").arg(m_path, file.errorString());
        return false;
    }
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
        if (error)
            *error = QStringLiteral("could not write %1: %2").arg(m_path, file.errorString());
        return false;
    }

    qCInfo(lc).noquote() << QStringLiteral("preferences written to %1").arg(m_path);
    return true;
}

} // namespace starfish::core
