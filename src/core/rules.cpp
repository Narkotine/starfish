#include "core/rules.hpp"

#include <QFileInfo>
#include <QRegularExpression>

namespace starfish::core {

static QString extensionOf(const FileMeta& file)
{
    return QFileInfo(file.name).suffix().toLower();
}

// A date criterion needs a known modification time. A file whose mtime could
// not be read (unreadable entry, folder portal on a dead share) must not
// accidentally satisfy "older than 30 days": it simply does not match.
static bool matchesDates(const Rule& rule, const FileMeta& file, const QDateTime& now)
{
    const bool wantsNewer = rule.newerThanDays && *rule.newerThanDays >= 0;
    const bool wantsOlder = rule.olderThanDays && *rule.olderThanDays >= 0;
    if (!wantsNewer && !wantsOlder)
        return true;
    if (!file.modified.isValid() || !now.isValid())
        return false;

    // Exact boundaries rather than a rounded age in days: "newer than 7 days"
    // means modified no earlier than now - 7 days, to the second.
    if (wantsNewer && file.modified < now.addDays(-*rule.newerThanDays))
        return false;
    if (wantsOlder && file.modified > now.addDays(-*rule.olderThanDays))
        return false;
    return true;
}

bool RuleEngine::matches(const Rule& rule, const FileMeta& file, const QDateTime& now)
{
    if (!rule.extensions.isEmpty() && !rule.extensions.contains(extensionOf(file)))
        return false;

    if (!rule.namePattern.isEmpty()) {
        const auto rx = QRegularExpression::fromWildcard(rule.namePattern,
                                                         Qt::CaseInsensitive);
        if (!rx.isValid() || !rx.match(file.name).hasMatch())
            return false;
    }

    return matchesDates(rule, file, now);
}

std::optional<QString> RuleEngine::classify(const FileMeta& file, const QList<Rule>& rules,
                                            const QDateTime& now)
{
    for (const auto& rule : rules) {
        if (matches(rule, file, now))
            return rule.targetFenceId;
    }
    return std::nullopt;
}

} // namespace starfish::core
