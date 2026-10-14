#include "core/portal.hpp"

#include <QDir>
#include <QFileInfo>

namespace starfish::core {

bool isPortalKind(const QString& kind)
{
    return kind.compare(QLatin1String("portal"), Qt::CaseInsensitive) == 0;
}

Portal listPortal(const QString& folderPath)
{
    Portal portal;
    // Declared verbatim: this is what the document will hold.
    portal.path = folderPath;
    if (folderPath.isEmpty()) {
        portal.error = QStringLiteral("empty path");
        return portal;
    }

    const QFileInfo info(folderPath);
    if (!info.exists()) {
        portal.error = QStringLiteral("no such folder");
        return portal;
    }
    if (!info.isDir()) {
        portal.error = QStringLiteral("not a folder");
        return portal;
    }

    // Hidden and system entries are listed too (the desktop model keeps them);
    // only the dot convention is skipped, and it is skipped here rather than
    // filtered afterwards so a dotted name never even reaches the grid.
    const QFileInfoList entries =
        QDir(folderPath).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                           | QDir::System,
                                       QDir::NoSort);
    portal.items.reserve(entries.size());
    for (const QFileInfo& entry : entries) {
        if (isDotHiddenName(entry.fileName()))
            continue;
        portal.items.append(itemFromPathOnly(entry.absoluteFilePath()));
    }

    // The grid sorts what it shows anyway; sorting here keeps a portal's own
    // listing stable for callers that only look at the model (the probe, and
    // the "what does this portal contribute" question).
    sortForDisplay(portal.items, QStringLiteral("name"), true);
    return portal;
}

bool sameFolder(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    const QString ca = QDir::cleanPath(QDir(a).absolutePath());
    const QString cb = QDir::cleanPath(QDir(b).absolutePath());
    return ca.compare(cb, Qt::CaseInsensitive) == 0;
}

} // namespace starfish::core
