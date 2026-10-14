#include "core/desktop_watcher.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

namespace starfish::core {

namespace {

QLoggingCategory lc("starfish.watcher");

// Everything but "." and "..", hidden and system entries included: the desktop
// draws those too, and a rule may legitimately target them.
QStringList entriesOf(const QString& folder)
{
    const QFileInfoList infos =
        QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                       | QDir::System,
                                   QDir::NoSort);
    QStringList paths;
    paths.reserve(infos.size());
    for (const QFileInfo& info : infos)
        paths.append(QDir::cleanPath(info.absoluteFilePath()));
    return paths;
}

QString cleanFolder(const QString& folder)
{
    return QDir::cleanPath(QDir(folder).absolutePath());
}

} // namespace

DesktopWatcher::DesktopWatcher(QObject* parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &DesktopWatcher::flush);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            &DesktopWatcher::onDirectoryChanged);
}

void DesktopWatcher::setFolders(const QStringList& folders)
{
    const bool restart = isWatching();
    stop();
    m_folders.clear();
    for (const QString& folder : folders) {
        if (folder.isEmpty())
            continue;
        const QString clean = cleanFolder(folder);
        if (!m_folders.contains(clean))
            m_folders.append(clean);
    }
    if (restart)
        start();
}

void DesktopWatcher::setDebounceInterval(int ms)
{
    m_debounceMs = qMax(0, ms);
}

bool DesktopWatcher::isWatching() const
{
    return !m_watcher.directories().isEmpty();
}

QStringList DesktopWatcher::snapshot() const
{
    QStringList all;
    for (const QString& folder : m_folders)
        all += entriesOf(folder);
    return all;
}

QStringList DesktopWatcher::newEntries(const QStringList& known, const QStringList& current)
{
    QSet<QString> knownSet;
    for (const QString& path : known)
        knownSet.insert(path);

    QStringList fresh;
    for (const QString& path : current) {
        if (!knownSet.contains(path) && !fresh.contains(path))
            fresh.append(path);
    }
    return fresh;
}

QStringList DesktopWatcher::removedEntries(const QStringList& known, const QStringList& current)
{
    QSet<QString> currentSet;
    for (const QString& path : current)
        currentSet.insert(path);

    QStringList gone;
    for (const QString& path : known) {
        if (!currentSet.contains(path) && !gone.contains(path))
            gone.append(path);
    }
    return gone;
}

void DesktopWatcher::start()
{
    m_debounce.stop();
    m_known.clear();

    for (const QString& folder : m_folders) {
        if (!QFileInfo(folder).isDir()) {
            // Not fatal: a folder can appear later (an external drive). Say so
            // rather than watching nothing in silence.
            qCWarning(lc).noquote()
                << QStringLiteral("%1 is not a folder, not watched").arg(folder);
            continue;
        }
        for (const QString& path : entriesOf(folder)) {
            if (!m_known.contains(path))
                m_known.append(path);
        }
        if (!m_watcher.addPath(folder))
            qCWarning(lc).noquote() << QStringLiteral("could not watch %1").arg(folder);
    }

    qCInfo(lc).noquote() << QStringLiteral("watching %1 folder(s), %2 known entry/entries")
                                .arg(m_watcher.directories().size())
                                .arg(m_known.size());
}

void DesktopWatcher::stop()
{
    m_debounce.stop();
    const QStringList watched = m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    m_known.clear();
    m_dirtyFolders.clear();
}

void DesktopWatcher::onDirectoryChanged(const QString& path)
{
    // QFileSystemWatcher forgets a path that disappears (folder deleted and
    // recreated, drive unmounted). Re-adding keeps the watch alive instead of
    // silently going deaf.
    const QString clean = cleanFolder(path);
    if (QFileInfo(clean).isDir()) {
        bool stillWatched = false;
        for (const QString& watched : m_watcher.directories()) {
            if (cleanFolder(watched) == clean) {
                stillWatched = true;
                break;
            }
        }
        if (!stillWatched)
            m_watcher.addPath(clean);
    }
    if (!m_dirtyFolders.contains(clean))
        m_dirtyFolders.append(clean);
    m_debounce.start(m_debounceMs);
}

void DesktopWatcher::flush()
{
    // Taken now rather than at notification time: a folder can notify twice in
    // one debounce window, and the folder portals (T-011) want one refresh per
    // burst, not one per notification.
    const QStringList dirty = m_dirtyFolders;
    m_dirtyFolders.clear();

    const QStringList current = snapshot();
    const QStringList fresh = newEntries(m_known, current);
    if (!fresh.isEmpty()) {
        for (const QString& path : fresh) {
            if (!m_known.contains(path))
                m_known.append(path);
        }
        qCInfo(lc).noquote() << QStringLiteral("%1 new entry/entries").arg(fresh.size());
        emit itemsAdded(fresh);
    } else {
        // A deletion or a rename: drop what is gone from the baseline, so a
        // file recreated under the same name counts as new again.
        const QStringList gone = removedEntries(m_known, current);
        for (const QString& path : gone)
            m_known.removeAll(path);
        if (!gone.isEmpty())
            qCInfo(lc).noquote() << QStringLiteral("%1 entry/entries gone").arg(gone.size());
    }

    // Emitted for every folder that notified, additions or not: a portal has
    // to re-list its folder when a file *disappears* too, and it has no
    // business knowing which of the two happened.
    for (const QString& folder : dirty)
        emit folderChanged(folder);
}

} // namespace starfish::core
