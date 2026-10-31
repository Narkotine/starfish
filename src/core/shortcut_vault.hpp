#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace starfish::core {

// Session 75 -- the user: "a way to keep the desktop's shortcuts in StarFish
// even when they are deleted from the desktop". A copy of every shortcut
// (.lnk / .url) a fence holds, in StarFish's own folder; refreshed when the
// shortcut changes, dropped when the tile leaves its fence. Only shortcuts:
// any other file is the user's data and is never copied or recreated.
// Nothing here ever deletes or overwrites a file outside its own folder.
class ShortcutVault
{
public:
    explicit ShortcutVault(QString folder);

    static bool isShortcut(const QString& path);

    // Copies the fenced shortcuts that are new or changed, forgets those no
    // fence holds any more. A missing shortcut keeps its copy. Returns the
    // number of copies written or dropped.
    int sync(const QStringList& fencedPaths);
    bool has(const QString& path) const;
    // Puts the shortcut back where it was -- only when nothing is there.
    bool restore(const QString& path);

    QString folder() const { return m_folder; }
    QString copyOf(const QString& path) const; // the backup file (may not exist)

private:
    struct Entry {
        QString path;
        qint64 size = -1;
        qint64 modified = 0;
    };
    static QString keyOf(const QString& path);
    void load();
    void save() const;

    QString m_folder;
    QHash<QString, Entry> m_entries; // key -> what the copy is of
    bool m_loaded = false;
};

} // namespace starfish::core
