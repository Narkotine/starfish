#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace starfish::core {

// Watches folders for entries appearing in them (F-018). This is the trigger of
// the rule engine: what the watcher reports goes to FenceContainer::applyRulesTo.
// It also reports, more coarsely, *that* a folder changed (T-011), which is what
// a folder portal follows: a mirror re-lists its folder instead of applying a
// diff, so a file leaving the folder matters as much as a file arriving.
//
// QFileSystemWatcher only says "this directory changed", never how, so the
// watcher keeps a snapshot of the entries and diffs it. That also absorbs two
// Windows quirks: creating a single file produces several notifications, and
// the file may still be being written when the first one arrives. A short
// debounce collapses the burst, so one dropped file yields one `itemsAdded()`.
//
// Qt Core only -- no Win32, no widget -- so it is unit-tested (the pure diff
// against a synthetic listing, the wiring against a temporary directory).
class DesktopWatcher : public QObject
{
    Q_OBJECT

public:
    static constexpr int kDefaultDebounceMs = 400;

    explicit DesktopWatcher(QObject* parent = nullptr);

    void setFolders(const QStringList& folders);
    QStringList folders() const { return m_folders; }

    void setDebounceInterval(int ms);
    int debounceInterval() const { return m_debounceMs; }

    // Takes the current listing as the baseline, then starts watching. Entries
    // already there are deliberately *not* reported: a rule fires on what
    // appears after the watch begins, not on the whole desktop at startup.
    void start();

    // Stops watching and drops the baseline.
    void stop();

    bool isWatching() const;

    // Pure: absolute paths present in `current` but not in `known`. Order
    // follows `current`, duplicates dropped.
    static QStringList newEntries(const QStringList& known, const QStringList& current);

    // Pure: the mirror image -- paths in `known` that are no longer in
    // `current`. Used to prune the baseline, so a file deleted and recreated
    // under the same name is reported as new again (T-011).
    static QStringList removedEntries(const QStringList& known, const QStringList& current);

    // The entries currently in the watched folders (absolute paths).
    QStringList snapshot() const;

signals:
    // New entries, debounced. Paths are absolute and cleaned.
    void itemsAdded(const QStringList& paths);

    // T-011: "this folder changed", debounced, whatever changed. Additions,
    // deletions and renames all produce it, which is what a folder portal
    // needs -- it re-lists the folder rather than applying a diff. The path is
    // the watched folder, cleaned.
    void folderChanged(const QString& folder);

private:
    void onDirectoryChanged(const QString& path);
    void flush();

    QFileSystemWatcher m_watcher;
    QStringList m_folders;
    // The baseline: what was in the folders when the watch started, plus
    // everything reported since. A flat list, because newEntries() hashes it
    // anyway and the order carries no meaning.
    QStringList m_known;
    // Folders that notified since the last flush, in notification order. Kept
    // as a list rather than a set so a report reads the same way twice.
    QStringList m_dirtyFolders;
    QTimer m_debounce;
    int m_debounceMs = kDefaultDebounceMs;
};

} // namespace starfish::core
