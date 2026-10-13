#include "core/rules.hpp"

#include <QFileInfo>
#include <QRegularExpression>

namespace starfish::core {

static QString extensionOf(const FileMeta& file)
{
    return QFileInfo(file.name).suffix().toLower();
}

bool RuleEngine::matches(const Rule& rule, const FileMeta& file)
{
    if (!rule.extensions.isEmpty() && !rule.extensions.contains(extensionOf(file)))
        return false;

    if (!rule.namePattern.isEmpty()) {
        const auto rx = QRegularExpression::fromWildcard(rule.namePattern,
                                                         Qt::CaseInsensitive);
        if (!rx.isValid() || !rx.match(file.name).hasMatch())
            return false;
    }

    return true;
}

std::optional<QString> RuleEngine::classify(const FileMeta& file, const QList<Rule>& rules)
{
    for (const auto& rule : rules) {
        if (matches(rule, file))
            return rule.targetFenceId;
    }
    return std::nullopt;
}

} // namespace starfish::core
