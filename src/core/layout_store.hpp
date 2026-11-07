#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include "core/jsonstore.hpp"

namespace starfish::core {

// Owns the persisted layout: where the document lives, when it is read and when
// it is written back (T-008). It is the "single source of truth for which icon
// is in which fence, and where" of docs/technique.md section 2 -- minus the
// desktop: no widget, no Win32, so it is unit-tested.
//
// Two behaviours are deliberate:
//
//  * **Debounced writes.** A fence drag emits one `fenceRectChanged` per mouse
//    move -- about 60 per second. Writing the file that often is pure waste, so
//    every change re-arms a single-shot timer and a burst collapses into one
//    write once the gesture settles.
//  * **No write without a change.** `setLayout` compares the candidate against
//    the current document and returns false when they are identical, so a
//    click that only changes the selection (which is not persisted) does not
//    touch the disk.
class LayoutStore : public QObject
{
    Q_OBJECT

public:
    static constexpr int kDefaultDebounceMs = 500;

    explicit LayoutStore(QObject* parent = nullptr);

    // %APPDATA%/StarFish/layout.json (QStandardPaths::AppDataLocation), with a
    // home-directory fallback when the platform reports no location.
    static QString defaultFilePath();

    void setFilePath(const QString& path);
    QString filePath() const { return m_filePath; }

    void setDebounceInterval(int ms);
    int debounceInterval() const { return m_debounceMs; }

    const Layout& layout() const { return m_layout; }

    // Reads the file.
    //  * a missing file is not an error: it means "first run", the empty layout
    //    already in place is kept and the call returns true;
    //  * a file that exists but cannot be read, parsed, or carries an
    //    unsupported schema returns false and leaves the current layout
    //    untouched -- a corrupted file must never silently wipe the layout.
    bool load(QString* error = nullptr);

    // Replaces the in-memory layout. Returns true when it actually differed, in
    // which case the document is marked dirty (the disk is untouched until
    // saveNow/scheduleSave). `emit layoutChanged()` accompanies a true return.
    bool setLayout(const Layout& layout);

    // Re-arms the debounce timer. Safe to call on every change signal.
    void scheduleSave();
    void cancelPendingSave();

    // Writes now, cancelling any pending debounced write. Returns true and emits
    // `saved` when a write happened; an unchanged document is a no-op that
    // returns true without emitting (there is nothing to report).
    bool saveNow(QString* error = nullptr);

    bool isDirty() const { return m_dirty; }

    // --- session 85: safe across versions ----------------------------------
    // The version this build stamps into the document ("writtenBy"); a file
    // last written by another version is backed up before anything is changed.
    void setAppVersion(const QString& version) { m_appVersion = version; }
    // Read-only: the file came from a newer StarFish, or is broken with no good
    // backup. Nothing is ever written over it then (saveNow fails, says why).
    bool isReadOnly() const { return m_readOnly; }
    QString readOnlyReason() const { return m_readOnlyReason; }
    // Set by load() when the file was broken and a backup was used instead.
    QString recoveredFrom() const { return m_recoveredFrom; }
    // <dir of the document>/backups; the newest kMaxBackups are kept.
    static constexpr int kMaxBackups = 10;
    QString backupDir() const;
    // Copies the current file to backups/layout.v<schema>.<yyyyMMdd-HHmmss-zzz>.<reason>.bak.json
    // and prunes the oldest. Returns the copy's path ("" when there was no file).
    QString backupNow(const QString& reason);
    QStringList backups() const; // newest first
    bool hasPendingSave() const { return m_timer.isActive(); }

signals:
    void layoutLoaded(const starfish::core::Layout& layout);
    void layoutChanged();
    void saved(const QString& path);
    void saveFailed(const QString& path, const QString& error);

private:
    QString m_filePath;
    Layout m_layout;
    // Serialised form of m_layout: the identity used to detect a real change.
    QByteArray m_fingerprint;
    QTimer m_timer;
    int m_debounceMs = kDefaultDebounceMs;
    bool m_dirty = false;
    QJsonObject m_raw; // the document as last read / written (unknown keys live here)
    QString m_appVersion;
    bool m_readOnly = false;
    QString m_readOnlyReason;
    QString m_recoveredFrom;
    bool loadFromBytes(const QByteArray& data, QString* error);
};

} // namespace starfish::core
