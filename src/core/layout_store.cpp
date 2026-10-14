#include "core/layout_store.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

namespace starfish::core {

namespace {
QLoggingCategory lc("starfish.layout");
}

LayoutStore::LayoutStore(QObject* parent)
    : QObject(parent)
    , m_filePath(defaultFilePath())
    , m_fingerprint(JsonStore::toJson(m_layout))
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(m_debounceMs);
    connect(&m_timer, &QTimer::timeout, this, [this] { saveNow(); });
}

QString LayoutStore::defaultFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.starfish");
    return dir + QStringLiteral("/layout.json");
}

void LayoutStore::setFilePath(const QString& path)
{
    if (path.isEmpty() || path == m_filePath)
        return;
    // A pending write belongs to the previous document.
    cancelPendingSave();
    m_filePath = path;
}

void LayoutStore::setDebounceInterval(int ms)
{
    m_debounceMs = qMax(0, ms);
    m_timer.setInterval(m_debounceMs);
}

bool LayoutStore::load(QString* error)
{
    if (!QFileInfo::exists(m_filePath)) {
        // First run: an empty layout is the correct state, not a failure.
        qCInfo(lc).noquote()
            << QStringLiteral("no layout file at %1, starting empty").arg(m_filePath);
        emit layoutLoaded(m_layout);
        return true;
    }

    QString localError;
    const auto loaded = JsonStore::load(m_filePath, &localError);
    if (!loaded) {
        // The current layout is kept on purpose: refusing to load is a
        // recoverable state, wiping the user's fences is not.
        if (error)
            *error = localError;
        qCWarning(lc).noquote()
            << QStringLiteral("could not load %1: %2 (layout kept as is)")
                   .arg(m_filePath, localError);
        return false;
    }

    m_layout = *loaded;
    m_fingerprint = JsonStore::toJson(m_layout);
    m_dirty = false;
    qCInfo(lc).noquote()
        << QStringLiteral("loaded %1 fence(s) from %2").arg(m_layout.fences.size()).arg(m_filePath);
    emit layoutLoaded(m_layout);
    return true;
}

bool LayoutStore::setLayout(const Layout& layout)
{
    const QByteArray candidate = JsonStore::toJson(layout);
    if (candidate == m_fingerprint)
        return false;

    m_layout = layout;
    m_fingerprint = candidate;
    m_dirty = true;
    emit layoutChanged();
    return true;
}

void LayoutStore::scheduleSave()
{
    if (!m_dirty)
        return;
    m_timer.start();
}

void LayoutStore::cancelPendingSave()
{
    m_timer.stop();
}

bool LayoutStore::saveNow(QString* error)
{
    cancelPendingSave();
    if (!m_dirty) {
        // Nothing changed since the last write: an identical file would only
        // cost I/O and disk wear.
        return true;
    }

    QString localError;
    if (!JsonStore::save(m_layout, m_filePath, &localError)) {
        if (error)
            *error = localError;
        // Stays dirty: the next change (or an explicit save) retries.
        qCWarning(lc).noquote()
            << QStringLiteral("could not save %1: %2").arg(m_filePath, localError);
        emit saveFailed(m_filePath, localError);
        return false;
    }

    m_dirty = false;
    qCInfo(lc).noquote()
        << QStringLiteral("saved %1 fence(s) to %2").arg(m_layout.fences.size()).arg(m_filePath);
    emit saved(m_filePath);
    return true;
}

} // namespace starfish::core
