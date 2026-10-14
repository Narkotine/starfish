#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace starfish::core {

// Minimal metadata needed to evaluate rules. Shell integration
// (real file icons, sizes from disk) fills this later.
//
// `name` is the *on-disk* file name, extension included -- deliberately not the
// Shell display name. The Shell hides the extension of known types ("photo.png"
// is drawn "photo"), and an extension criterion that cannot see the extension
// would never match. See docs/technique.md section 12.
struct FileMeta {
    QString name;
    QString path;
    bool isDir = false;
    qint64 sizeBytes = 0;
    QDateTime modified;
};

// One automation rule (docs/produit.md F-017). The criteria are ANDed and an
// empty criterion means "any"; the first matching rule wins, so the order of
// the list is the priority order.
//
// Date criteria are expressed in *days relative to now* rather than as absolute
// timestamps: "modified within the last 7 days" keeps its meaning over time,
// and a layout restored on another machine does not suddenly match nothing.
struct Rule {
    QString id;
    QString name;
    QStringList extensions;  // lower-case, no dot; empty = any
    QString namePattern;     // wildcard, e.g. "screenshot*"; empty = any
    std::optional<int> newerThanDays; // modified within the last N days
    std::optional<int> olderThanDays; // modified at least N days ago
    QString targetFenceId;
    bool moveFiles = false;  // opt-in only: see docs/technique.md section 5
};

class RuleEngine
{
public:
    // Returns the target fence id of the first matching rule, or nullopt.
    // `now` is injectable so the date criteria can be tested without waiting.
    static std::optional<QString> classify(const FileMeta& file, const QList<Rule>& rules,
                                           const QDateTime& now = QDateTime::currentDateTime());

    // True if the rule matches the given file. Exposed for tests.
    static bool matches(const Rule& rule, const FileMeta& file,
                        const QDateTime& now = QDateTime::currentDateTime());
};

} // namespace starfish::core
