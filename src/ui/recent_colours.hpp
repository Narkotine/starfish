#pragma once

// Session 65 (the user: "auto enregistrement des couleurs utilisées"): every
// colour picked in StarFish is remembered, most recent first, and offered
// again as the colour dialog's custom colours (its 16 slots).

#include <QColor>
#include <QColorDialog>
#include <QSettings>
#include <QStringList>

namespace starfish::ui::RecentColours {

constexpr int kMax = 16; // QColorDialog::customCount()

// The list with `colour` (#AARRGGBB) in front, once, at most kMax long.
inline QStringList pushed(QStringList list, const QString& colour)
{
    const QString key = colour.toUpper();
    list.removeAll(key);
    list.prepend(key);
    while (list.size() > kMax)
        list.removeLast();
    return list;
}

inline QSettings& store()
{
    static QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat);
    return settings;
}

inline QStringList load()
{
    return store().value(QStringLiteral("recentColours")).toStringList();
}

// QColorDialog::getColor, with the recent colours in its custom slots, and the
// picked one remembered. Invalid when cancelled.
inline QColor pick(const QColor& initial, const QString& title, QWidget* parent = nullptr)
{
    const QStringList recent = load();
    for (int i = 0; i < QColorDialog::customCount(); ++i)
        QColorDialog::setCustomColor(i, i < recent.size() ? QColor(recent.at(i)) : QColor(Qt::white));
    const QColor picked = QColorDialog::getColor(initial, parent, title, QColorDialog::ShowAlphaChannel);
    if (picked.isValid())
        store().setValue(QStringLiteral("recentColours"), pushed(recent, picked.name(QColor::HexArgb)));
    return picked;
}

} // namespace starfish::ui::RecentColours
