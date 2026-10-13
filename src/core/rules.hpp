#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace starfish::core {

// Minimal metadata needed to evaluate rules. Shell integration
// (real file icons, sizes from disk) fills this later.
struct FileMeta {
    QString name;
    QString path;
    bool isDir = false;
    qint64 sizeBytes = 0;
    QDateTime modified;
};

// One automation rule. First matching rule wins (see docs/produit.md F-017).
struct Rule {
    QString id;
    QString name;
    QStringList extensions;  // lower-case, no dot; empty = any
    QString namePattern;     // wildcard, e.g. "screenshot*"; empty = any
    QString targetFenceId;
    bool moveFiles = false;  // opt-in only: see docs/technique.md section 5
};

class RuleEngine
{
public:
    // Returns the target fence id of the first matching rule, or nullopt.
    static std::optional<QString> classify(const FileMeta& file, const QList<Rule>& rules);

    // True if the rule matches the given file. Exposed for tests.
    static bool matches(const Rule& rule, const FileMeta& file);
};

} // namespace starfish::core
