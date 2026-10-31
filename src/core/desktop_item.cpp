#include <filesystem>
#include <system_error>
#include "core/desktop_item.hpp"

#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace starfish::core {

namespace {

// Width used to zero-pad digit runs. 18 digits covers every realistic file
// name; longer runs fall back to the natural length and still compare
// lexicographically.
constexpr int kDigitRunWidth = 18;

// U+0001 sorts below both digits ('0' is U+0030) and letters, which is what
// puts "shot9" before "shot.png".
constexpr QChar kDigitRunMarker(u'\x01');

QString extensionOf(const DesktopItem& item)
{
    const int dot = item.fileName.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? item.fileName.mid(dot + 1).toLower() : QString();
}

qint64 comparableSize(const DesktopItem& item)
{
    return item.sizeBytes < 0 ? 0 : item.sizeBytes;
}

// Primary comparison for a sort mode; 0 means "tie, break on the name".
int comparePrimary(const DesktopItem& a, const DesktopItem& b, const QString& by)
{
    if (by == QLatin1String("size")) {
        const qint64 sa = comparableSize(a);
        const qint64 sb = comparableSize(b);
        if (sa != sb)
            return sa < sb ? -1 : 1;
        return 0;
    }

    if (by == QLatin1String("date")) {
        if (a.modified != b.modified)
            return a.modified < b.modified ? -1 : 1;
        return 0;
    }

    if (by == QLatin1String("type")) {
        // Folders first, then by extension, like the Shell's own "Type" order.
        if (a.isDir != b.isDir)
            return a.isDir ? -1 : 1;
        const QString ea = extensionOf(a);
        const QString eb = extensionOf(b);
        if (ea != eb)
            return ea < eb ? -1 : 1;
        return 0;
    }

    // "name", and any unknown value: natural order on the display name.
    const QString ka = naturalSortKey(a.name);
    const QString kb = naturalSortKey(b.name);
    if (ka != kb)
        return ka < kb ? -1 : 1;
    return 0;
}

} // namespace

bool isDotHiddenName(const QString& fileName)
{
    return fileName.size() > 1 && fileName.startsWith(QLatin1Char('.'));
}

QString naturalSortKey(const QString& text)
{
    const QString lowered = text.toLower();
    QString key;
    key.reserve(lowered.size() + 8);

    int i = 0;
    while (i < lowered.size()) {
        if (!lowered.at(i).isDigit()) {
            key.append(lowered.at(i));
            ++i;
            continue;
        }

        int end = i;
        while (end < lowered.size() && lowered.at(end).isDigit())
            ++end;

        // Drop leading zeros so that "01" and "1" land on the same key.
        QString digits = lowered.mid(i, end - i);
        while (digits.size() > 1 && digits.startsWith(QLatin1Char('0')))
            digits.remove(0, 1);

        key.append(kDigitRunMarker);
        if (digits.size() < kDigitRunWidth)
            key.append(QString(kDigitRunWidth - digits.size(), QLatin1Char('0')));
        key.append(digits);

        i = end;
    }
    return key;
}

bool lessThanForDisplay(const DesktopItem& a, const DesktopItem& b,
                        const QString& by, bool ascending)
{
    const int primary = comparePrimary(a, b, by);
    if (primary != 0)
        return ascending ? primary < 0 : primary > 0;

    // Stable tiebreak, always ascending: the same two items keep the same
    // relative order whatever the direction chosen.
    return a.path < b.path;
}

void sortForDisplay(QList<DesktopItem>& items, const QString& by, bool ascending)
{
    if (by == QLatin1String("manual"))
        return; // session 60: the user's own order, kept as it is
    std::stable_sort(items.begin(), items.end(), [&by, ascending](const DesktopItem& a,
                                                                 const DesktopItem& b) {
        return lessThanForDisplay(a, b, by, ascending);
    });
}

QList<DesktopItem> mergeDesktopItems(const QList<DesktopItem>& items)
{
    QList<DesktopItem> merged;
    merged.reserve(items.size());

    QSet<QString> seen;
    // Two passes so that the per-user entry wins whatever the input order is.
    for (int pass = 0; pass < 2; ++pass) {
        const bool publicPass = (pass == 1);
        for (const DesktopItem& item : items) {
            if (item.fromPublicDesktop != publicPass)
                continue;
            const QString key = item.fileName.toLower();
            if (seen.contains(key))
                continue;
            seen.insert(key);
            merged.append(item);
        }
    }
    return merged;
}

bool entryExists(const QString& path)
{
    if (path.isEmpty())
        return false;
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()), error));
}

DesktopItem itemFromPathOnly(const QString& path)
{
    DesktopItem item;
    item.path = path;

    const QFileInfo info(path);
    item.fileName = info.fileName().isEmpty() ? path : info.fileName();
    item.name = item.fileName;
    item.isDir = info.isDir();
    item.isHidden = isDotHiddenName(item.fileName);
    if (info.exists() && !item.isDir)
        item.sizeBytes = info.size();
    if (info.exists())
        item.modified = info.lastModified();
    return item;
}

FileMeta fileMetaFromItem(const DesktopItem& item)
{
    FileMeta meta;
    meta.name = item.fileName.isEmpty() ? item.name : item.fileName;
    meta.path = item.path;
    meta.isDir = item.isDir;
    meta.sizeBytes = item.sizeBytes;
    meta.modified = item.modified;
    return meta;
}

} // namespace starfish::core
