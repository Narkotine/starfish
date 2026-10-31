#pragma once

// Session 65 (the user: "créateur de Template de fence"; T-081 / T-093 /
// T-097): a fence's look saved under a name, applied to another fence, and
// one of them, if chosen, given to every new fence. Stored in the settings
// (HKCU\Software\StarFish\fenceTemplates), the style as the layout writes it.

#include <QSettings>
#include <QStringList>
#include <QVariantMap>

#include "core/jsonstore.hpp"

namespace starfish::ui::FenceTemplates {

inline QSettings& store()
{
    static QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat);
    return settings;
}

inline QVariantMap all()
{
    return store().value(QStringLiteral("fenceTemplates")).toMap();
}

inline QStringList names()
{
    return all().keys();
}

inline bool contains(const QString& name)
{
    return all().contains(name);
}

inline core::FenceStyle style(const QString& name)
{
    return core::JsonStore::styleFromBytes(all().value(name).toString().toUtf8());
}

inline void save(const QString& name, const core::FenceStyle& style)
{
    QVariantMap map = all();
    map.insert(name, QString::fromUtf8(core::JsonStore::styleToBytes(style)));
    store().setValue(QStringLiteral("fenceTemplates"), map);
}

inline void remove(const QString& name)
{
    QVariantMap map = all();
    map.remove(name);
    store().setValue(QStringLiteral("fenceTemplates"), map);
    if (store().value(QStringLiteral("fenceTemplateDefault")).toString() == name)
        store().remove(QStringLiteral("fenceTemplateDefault"));
}

// Session 70 (T-103): a new name; the "for new fences" choice follows it.
inline bool rename(const QString& from, const QString& to)
{
    if (to.isEmpty() || from == to || !contains(from) || contains(to))
        return false;
    QVariantMap map = all();
    map.insert(to, map.take(from));
    store().setValue(QStringLiteral("fenceTemplates"), map);
    if (store().value(QStringLiteral("fenceTemplateDefault")).toString() == from)
        store().setValue(QStringLiteral("fenceTemplateDefault"), to);
    return true;
}

// The template new fences get ("" = none: the schema's defaults).
inline QString defaultName()
{
    const QString name = store().value(QStringLiteral("fenceTemplateDefault")).toString();
    return contains(name) ? name : QString();
}

inline void setDefaultName(const QString& name)
{
    store().setValue(QStringLiteral("fenceTemplateDefault"), name);
}

inline core::FenceStyle newFenceStyle()
{
    const QString name = defaultName();
    return name.isEmpty() ? core::FenceStyle() : style(name);
}

} // namespace starfish::ui::FenceTemplates
