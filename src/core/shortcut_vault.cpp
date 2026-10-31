#include "core/shortcut_vault.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <chrono>
#include <filesystem>
#include <system_error>

#include "core/desktop_item.hpp"

namespace starfish::core {

namespace {

// Size and time of the entry itself (QFileInfo would read a .lnk's target).
bool statEntry(const QString& path, qint64* size, qint64* modified)
{
    std::error_code error;
    const std::filesystem::path p(path.toStdWString());
    const auto bytes = std::filesystem::file_size(p, error);
    if (error)
        return false;
    const auto time = std::filesystem::last_write_time(p, error);
    if (error)
        return false;
    *size = qint64(bytes);
    *modified = qint64(std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count());
    return true;
}

} // namespace

ShortcutVault::ShortcutVault(QString folder)
    : m_folder(std::move(folder))
{
}

bool ShortcutVault::isShortcut(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix();
    return suffix.compare(QLatin1String("lnk"), Qt::CaseInsensitive) == 0
           || suffix.compare(QLatin1String("url"), Qt::CaseInsensitive) == 0;
}

QString ShortcutVault::keyOf(const QString& path)
{
    const QByteArray hash = QCryptographicHash::hash(QDir::fromNativeSeparators(path).toLower().toUtf8(),
                                                     QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex().left(20));
}

QString ShortcutVault::copyOf(const QString& path) const
{
    return m_folder + QLatin1Char('/') + keyOf(path) + QLatin1Char('.') + QFileInfo(path).suffix().toLower();
}

void ShortcutVault::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    QFile file(m_folder + QStringLiteral("/index.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Entry entry;
        entry.path = o.value(QStringLiteral("path")).toString();
        entry.size = qint64(o.value(QStringLiteral("size")).toDouble(-1));
        entry.modified = qint64(o.value(QStringLiteral("modified")).toDouble());
        if (!entry.path.isEmpty())
            m_entries.insert(it.key(), entry);
    }
}

void ShortcutVault::save() const
{
    QJsonObject root;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("path"), it.value().path);
        o.insert(QStringLiteral("size"), double(it.value().size));
        o.insert(QStringLiteral("modified"), double(it.value().modified));
        root.insert(it.key(), o);
    }
    QDir().mkpath(m_folder);
    QSaveFile file(m_folder + QStringLiteral("/index.json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

int ShortcutVault::sync(const QStringList& fencedPaths)
{
    load();
    int changes = 0;
    QSet<QString> wanted;
    for (const QString& path : fencedPaths) {
        if (!isShortcut(path))
            continue; // a document, a folder, a program: the user's data, never copied
        const QString key = keyOf(path);
        wanted.insert(key);
        qint64 size = -1, modified = 0;
        if (!entryExists(path) || !statEntry(path, &size, &modified))
            continue; // gone: the copy (if any) is what can bring it back
        const QString copy = copyOf(path);
        const auto known = m_entries.constFind(key);
        if (known != m_entries.constEnd() && known->size == size && known->modified == modified
            && entryExists(copy))
            continue;
        QDir().mkpath(m_folder);
        const QString fresh = copy + QStringLiteral(".new");
        QFile::remove(fresh);
        if (!QFile::copy(path, fresh))
            continue;
        QFile::remove(copy);
        if (!QFile::rename(fresh, copy)) {
            QFile::remove(fresh);
            continue;
        }
        m_entries.insert(key, Entry{QDir::fromNativeSeparators(path), size, modified});
        ++changes;
    }
    // A tile the user took out of its fence: its copy goes too. Never on an
    // empty list -- a document that failed to load must not wipe every copy.
    for (auto it = fencedPaths.isEmpty() ? m_entries.end() : m_entries.begin(); it != m_entries.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        QFile::remove(copyOf(it.value().path));
        it = m_entries.erase(it);
        ++changes;
    }
    if (changes > 0)
        save();
    return changes;
}

bool ShortcutVault::has(const QString& path) const
{
    const_cast<ShortcutVault*>(this)->load();
    return isShortcut(path) && m_entries.contains(keyOf(path)) && entryExists(copyOf(path));
}

bool ShortcutVault::restore(const QString& path)
{
    if (!has(path) || entryExists(path))
        return false; // never over whatever is there now
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    return QFile::copy(copyOf(path), path);
}

} // namespace starfish::core
