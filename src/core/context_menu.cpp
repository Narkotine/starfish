#include "core/context_menu.hpp"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace starfish::core {

namespace {

struct Known {
    const char* key;
    char16_t glyph; // 0: none
    const char* labels[2]; // lower case: fr-FR, en-US (a static table: no initializer_list)
};

// Labels as Windows writes them (fr-FR, en-US), measured on the desktop's own
// menus (build/context-menu-probe.txt).
const std::vector<Known>& knownEntries()
{
    static const std::vector<Known> known{
        {"view", 0xE8A9, {"affichage", "view"}},
        {"sort", 0xE8CB, {"trier par", "sort by"}},
        {"refresh", 0xE72C, {"actualiser", "refresh"}},
        {"paste", 0xE77F, {"coller", "paste"}},
        {"paste-shortcut", 0x0000, {"coller le raccourci", "paste shortcut"}},
        {"new", 0xE710, {"nouveau", "new"}},
        {"display-settings", 0xE7F4, {"paramètres d'affichage", "display settings"}},
        {"personalize", 0xE771, {"personnaliser", "personalize"}},
        {"terminal", 0xE756, {"ouvrir dans le terminal", "open in terminal"}},
        {"open", 0xE8E5, {"ouvrir", "open"}},
        {"open-new-window", 0xE8A7, {"ouvrir dans une nouvelle fenêtre", "open in new window"}},
        {"open-new-tab", 0xE8A7, {"ouvrir dans un nouvel onglet", "open in new tab"}},
        {"edit", 0xE70F, {"modifier", "edit"}},
        {"print", 0xE749, {"imprimer", "print"}},
        {"open-with", 0xE7AC, {"ouvrir avec", "open with"}},
        {"open-location", 0xE838, {"ouvrir l'emplacement du fichier", "open file location"}},
        {"run-as-admin", 0xE7EF, {"exécuter en tant qu'administrateur", "run as administrator"}},
        {"pin-quick-access", 0xE718, {"épingler à l'accès rapide", "pin to quick access"}},
        {"pin-start", 0xE718, {"épingler au menu démarrer", "pin to start"}},
        {"pin-taskbar", 0xE718, {"épingler à la barre des tâches", "pin to taskbar"}},
        {"favorites", 0xE734, {"ajouter aux favoris", "add to favorites"}},
        {"empty-recycle-bin", 0xE74D, {"vider la corbeille", "empty recycle bin"}},
        {"copy-path", 0xE8C8, {"copier en tant que chemin d'accès", "copy as path"}},
        {"share", 0xE72D, {"partager", "share"}},
        {"cut", 0xE8C6, {"couper", "cut"}},
        {"copy", 0xE8C8, {"copier", "copy"}},
        {"create-shortcut", 0xE71B, {"créer un raccourci", "create shortcut"}},
        {"delete", 0xE74D, {"supprimer", "delete"}},
        {"rename", 0xE8AC, {"renommer", "rename"}},
        {"properties", 0xE946, {"propriétés", "properties"}},
        {"send-to", 0xE724, {"envoyer vers", "send to"}},
        {"restore-versions", 0x0000, {"restaurer les versions précédentes", "restore previous versions"}},
        {"give-access", 0x0000, {"accorder l'accès à", "give access to"}},
    };
    return known;
}

// Windows 11's menus, group by group (an empty string = a separator).
const QStringList& windows11Order(ContextMenuKind kind)
{
    static const QStringList background{
        QStringLiteral("view"), QStringLiteral("sort"), QStringLiteral("refresh"), QString(),
        QStringLiteral("undo"), QStringLiteral("redo"), QString(),
        QStringLiteral("new"), QString(),
        QStringLiteral("display-settings"), QStringLiteral("personalize"), QString(),
        QStringLiteral("terminal")};
    static const QStringList selection{
        QStringLiteral("open"), QStringLiteral("open-new-tab"), QStringLiteral("open-new-window"),
        QStringLiteral("edit"), QStringLiteral("open-with"), QStringLiteral("open-location"),
        QStringLiteral("run-as-admin"), QStringLiteral("empty-recycle-bin"), QString(),
        QStringLiteral("pin-quick-access"), QStringLiteral("pin-start"), QStringLiteral("pin-taskbar"),
        QStringLiteral("favorites"), QString(),
        QStringLiteral("copy-path"), QStringLiteral("properties")};
    return kind == ContextMenuKind::Background ? background : selection;
}

const QStringList& windows11TopBar(ContextMenuKind kind)
{
    static const QStringList background{QStringLiteral("paste")};
    static const QStringList selection{QStringLiteral("cut"), QStringLiteral("copy"),
                                       QStringLiteral("rename"), QStringLiteral("share"),
                                       QStringLiteral("delete")};
    return kind == ContextMenuKind::Background ? background : selection;
}

void keyAll(std::vector<ContextMenuItem>& items)
{
    for (ContextMenuItem& item : items) {
        if (item.separator)
            continue;
        if (item.key.isEmpty())
            item.key = contextMenuKey(item.label, !item.children.empty());
        if (item.glyph.isEmpty())
            item.glyph = contextMenuGlyph(item.key);
        // Inside a submenu the keys only name, they do not filter: a child's
        // label is its own ("Nouveau > Dossier").
        for (ContextMenuItem& child : item.children) {
            if (!child.separator && child.key.isEmpty())
                child.key = item.key + QLatin1Char('/') + child.label;
        }
    }
}

void tidy(std::vector<ContextMenuItem>& items)
{
    std::vector<ContextMenuItem> out;
    for (ContextMenuItem& item : items) {
        if (item.separator && (out.empty() || out.back().separator))
            continue;
        out.push_back(std::move(item));
    }
    while (!out.empty() && out.back().separator)
        out.pop_back();
    items = std::move(out);
}

ContextMenuItem separator()
{
    ContextMenuItem s;
    s.separator = true;
    return s;
}

QJsonObject profileToJson(const ContextMenuProfile& p)
{
    QJsonArray custom;
    for (const ContextMenuCustomEntry& c : p.custom) {
        QJsonObject entry{{QStringLiteral("label"), c.label},
                          {QStringLiteral("program"), c.program},
                          {QStringLiteral("arguments"), QJsonArray::fromStringList(c.arguments)},
                          {QStringLiteral("glyph"), c.glyph}};
        // Session 82: an action is written only when there is one -- a document
        // that predates it must not gain an empty key on its first save.
        if (!c.action.isEmpty())
            entry.insert(QStringLiteral("action"), c.action);
        custom.append(entry);
    }
    return QJsonObject{{QStringLiteral("hidden"), QJsonArray::fromStringList(p.hidden)},
                       {QStringLiteral("shown"), QJsonArray::fromStringList(p.shown)},
                       {QStringLiteral("order"), QJsonArray::fromStringList(p.order)},
                       {QStringLiteral("custom"), custom}};
}

QStringList strings(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& e : v.toArray()) {
        if (e.isString())
            out << e.toString();
    }
    return out;
}

ContextMenuProfile profileFromJson(const QJsonObject& o)
{
    ContextMenuProfile p;
    p.hidden = strings(o.value(QStringLiteral("hidden")));
    p.shown = strings(o.value(QStringLiteral("shown")));
    p.order = strings(o.value(QStringLiteral("order")));
    for (const QJsonValue& v : o.value(QStringLiteral("custom")).toArray()) {
        const QJsonObject c = v.toObject();
        ContextMenuCustomEntry e;
        e.label = c.value(QStringLiteral("label")).toString();
        e.program = c.value(QStringLiteral("program")).toString();
        e.arguments = strings(c.value(QStringLiteral("arguments")));
        e.glyph = c.value(QStringLiteral("glyph")).toString();
        e.action = c.value(QStringLiteral("action")).toString();
        // An action entry has no program (the light/dark switch runs nothing).
        if (!e.label.isEmpty() && (!e.program.isEmpty() || !e.action.isEmpty()))
            p.custom << e;
    }
    return p;
}

} // namespace

QString contextMenuKey(const QString& label, bool hasChildren)
{
    // Windows writes some apostrophes typographic ("chemin d’accès"): compared as straight.
    const QString l = label.trimmed().toLower().replace(QChar(0x2019), QLatin1Char('\''));
    if (l.startsWith(QLatin1String("annuler")) || l.startsWith(QLatin1String("undo")))
        return QStringLiteral("undo");
    if (l.startsWith(QStringLiteral("rétablir")) || l.startsWith(QLatin1String("redo")))
        return QStringLiteral("redo");
    for (const Known& k : knownEntries()) {
        for (const char* text : k.labels) {
            if (l == QString::fromUtf8(text)) {
                const QString key = QString::fromLatin1(k.key);
                // "Ouvrir" the submenu would be "Ouvrir avec"'s cousin: a key
                // whose entry is a submenu in Windows names the submenu only.
                Q_UNUSED(hasChildren);
                return key;
            }
        }
    }
    return QStringLiteral("label:") + label.trimmed();
}

QStringList windows11Keys(ContextMenuKind kind)
{
    QStringList keys = windows11TopBar(kind);
    for (const QString& key : windows11Order(kind)) {
        if (!key.isEmpty())
            keys << key;
    }
    return keys;
}

bool shownByDefault(const QString& key, ContextMenuKind kind)
{
    return windows11Keys(kind).contains(key) || key.startsWith(QLatin1String("app:"))
           || key.startsWith(QLatin1String("starfish:")) || key.startsWith(QLatin1String("custom:"))
           || key == QLatin1String("more-options");
}

bool isShown(const QString& key, ContextMenuKind kind, const ContextMenuProfile& profile)
{
    if (profile.hidden.contains(key, Qt::CaseInsensitive))
        return false;
    return shownByDefault(key, kind) || profile.shown.contains(key, Qt::CaseInsensitive);
}

QString contextMenuGlyph(const QString& key)
{
    if (key == QLatin1String("undo"))
        return QString(QChar(0xE7A7));
    if (key == QLatin1String("redo"))
        return QString(QChar(0xE7A6));
    for (const Known& k : knownEntries()) {
        if (key == QLatin1String(k.key))
            return k.glyph ? QString(QChar(k.glyph)) : QString();
    }
    return {};
}

QString ContextMenuConfig::defaultPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("StarFish/context-menu.json"));
}

ContextMenuConfig ContextMenuConfig::load(const QString& path)
{
    ContextMenuConfig config;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return config;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty())
        return config;
    config.modern = root.value(QStringLiteral("modern")).toBool(true);
    config.background = profileFromJson(root.value(QStringLiteral("background")).toObject());
    config.items = profileFromJson(root.value(QStringLiteral("items")).toObject());
    return config;
}

bool ContextMenuConfig::save(const QString& path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QJsonObject root{{QStringLiteral("modern"), modern},
                           {QStringLiteral("background"), profileToJson(background)},
                           {QStringLiteral("items"), profileToJson(items)}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

std::vector<ContextMenuItem> composeWindows11Menu(const std::vector<ContextMenuItem>& classicIn,
                                                  ContextMenuKind kind,
                                                  const ContextMenuConfig& config,
                                                  const std::vector<ContextMenuItem>& oursIn,
                                                  const QString& moreOptionsLabel,
                                                  QStringList* omitted,
                                                  const std::vector<ContextMenuItem>& packaged)
{
    const ContextMenuProfile& profile = config.profile(kind);
    const auto hidden = [&](const QString& key) {
        return profile.hidden.contains(key, Qt::CaseInsensitive);
    };

    std::vector<ContextMenuItem> classic = classicIn;
    keyAll(classic);
    // First of each key wins (two "Copier"... no: "copy" and "copy-path" differ).
    QHash<QString, const ContextMenuItem*> byKey;
    for (const ContextMenuItem& item : classic) {
        if (!item.separator && !byKey.contains(item.key))
            byKey.insert(item.key, &item);
    }

    std::vector<ContextMenuItem> out;
    QStringList used;
    const auto take = [&](const QString& key) -> const ContextMenuItem* {
        const ContextMenuItem* item = byKey.value(key, nullptr);
        if (!item || hidden(key) || used.contains(key))
            return nullptr;
        used << key;
        return item;
    };

    // The top bar.
    for (const QString& key : windows11TopBar(kind)) {
        const ContextMenuItem* item = take(key);
        if (!item || !item->enabled)
            continue; // Windows 11 leaves a Paste with nothing to paste out
        ContextMenuItem top = *item;
        top.topBar = true;
        out.push_back(top);
    }

    for (const ContextMenuItem& item : packaged) {
        if (!byKey.contains(item.key))
            byKey.insert(item.key, &item);
    }
    std::vector<ContextMenuItem> customs;
    customs.reserve(static_cast<size_t>(profile.custom.size()));
    for (int i = 0; i < profile.custom.size(); ++i) {
        const ContextMenuCustomEntry& c = profile.custom.at(i);
        ContextMenuItem item;
        item.key = QStringLiteral("custom:") + c.label;
        item.label = c.label;
        item.glyph = c.glyph;
        item.customIndex = i;
        customs.push_back(item);
    }
    for (const ContextMenuItem& item : customs) {
        if (!byKey.contains(item.key))
            byKey.insert(item.key, &item);
    }
    for (const ContextMenuItem& item : oursIn) {
        if (!item.separator && !byKey.contains(item.key))
            byKey.insert(item.key, &item);
    }

    // The user's order first. Two neighbours from different groups of Windows
    // 11 keep a separator between them, so a reordered menu still reads in groups.
    const auto groupOf = [&](const QString& key) {
        int group = 0;
        for (const QString& k : windows11Order(kind)) {
            if (k.isEmpty())
                ++group;
            else if (k == key)
                return group;
        }
        return key.startsWith(QLatin1String("app:")) ? 100
               : key.startsWith(QLatin1String("starfish:")) || key.startsWith(QLatin1String("custom:")) ? 101
                                                                                                          : 99;
    };
    bool orderedAny = false;
    int lastGroup = -1;
    for (const QString& key : profile.order) {
        if (const ContextMenuItem* item = take(key)) {
            if (orderedAny && groupOf(key) != lastGroup)
                out.push_back(separator());
            lastGroup = groupOf(key);
            out.push_back(*item);
            orderedAny = true;
        }
    }
    if (orderedAny)
        out.push_back(separator());

    // Windows 11's own entries, in its groups.
    for (const QString& key : windows11Order(kind)) {
        if (key.isEmpty()) {
            out.push_back(separator());
            continue;
        }
        if (const ContextMenuItem* item = take(key))
            out.push_back(*item);
    }

    // What Windows 11 keeps for "Show more options", unless brought back.
    out.push_back(separator());
    for (const ContextMenuItem& item : classic) {
        if (item.separator || used.contains(item.key))
            continue;
        if (profile.shown.contains(item.key, Qt::CaseInsensitive) && !hidden(item.key)) {
            used << item.key;
            out.push_back(item);
        } else if (omitted && !omitted->contains(item.key)) {
            *omitted << item.key;
        }
    }

    // The packaged apps' entries (Terminal, Notepad, Photos...).
    out.push_back(separator());
    for (const ContextMenuItem& item : packaged) {
        if (const ContextMenuItem* taken = take(item.key))
            out.push_back(*taken);
    }

    // StarFish's entries and the user's own.
    out.push_back(separator());
    for (const ContextMenuItem& item : oursIn) {
        if (item.separator)
            continue;
        if (const ContextMenuItem* taken = take(item.key))
            out.push_back(*taken);
    }
    for (const ContextMenuItem& item : customs) {
        if (const ContextMenuItem* taken = take(item.key))
            out.push_back(*taken);
    }

    // "Show more options", always there and always last.
    out.push_back(separator());
    ContextMenuItem more;
    more.key = QStringLiteral("more-options");
    more.label = moreOptionsLabel;
    more.glyph = QString(QChar(0xE8A7));
    more.ownId = kMoreOptionsId;
    out.push_back(more);

    // Separators: none at the edges of the list, none doubled, none right after the top bar.
    std::vector<ContextMenuItem> list;
    std::vector<ContextMenuItem> bar;
    for (ContextMenuItem& item : out)
        (item.topBar ? bar : list).push_back(std::move(item));
    tidy(list);
    bar.insert(bar.end(), list.begin(), list.end());
    return bar;
}

QStringList expandCustomArguments(const ContextMenuCustomEntry& entry, const QStringList& paths,
                                  const QString& desktop)
{
    QStringList out;
    for (const QString& arg : entry.arguments) {
        if (arg == QLatin1String("%paths%")) {
            for (const QString& p : paths)
                out << QDir::toNativeSeparators(p);
            continue;
        }
        QString a = arg;
        a.replace(QLatin1String("%path%"), paths.isEmpty() ? QString() : QDir::toNativeSeparators(paths.first()));
        a.replace(QLatin1String("%desktop%"), QDir::toNativeSeparators(desktop));
        out << a;
    }
    return out;
}

namespace {

// Session 82: the icon pool. The code points are the ones the menu already
// carries where the two lists overlap (measured on this PC's own menus, see
// knownEntries above); the rest are the font's own standard names. A probe asks
// the installed icon font whether each one exists, so a wrong number shows up as
// a red check instead of an empty box in the menu.
struct PoolRow {
    const char* name;
    char16_t glyph;
    const char* label; // QT_TRANSLATE_NOOP: translated by menuGlyphPool()
};

const std::vector<PoolRow>& poolRows()
{
    static const std::vector<PoolRow> rows{
        {"settings", 0xE713, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Settings")},
        {"apps", 0xE71D, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Applications")},
        {"uninstall", 0xE74D, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Uninstall")},
        {"appearance", 0xE771, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Appearance")},
        {"theme", 0xE706, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Light or dark")},
        {"folder", 0xE8B7, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Folder")},
        {"terminal", 0xE756, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Terminal")},
        {"star", 0xE734, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Star")},
        {"browser", 0xE774, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Browser")},
        {"mail", 0xE715, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Mail")},
        {"search", 0xE721, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Search")},
        {"calendar", 0xE787, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Calendar")},
        {"clock", 0xE917, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Clock")},
        {"lock", 0xE72E, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Lock")},
        {"pin", 0xE718, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Pin")},
        {"share", 0xE72D, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Share")},
        {"copy", 0xE8C8, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Copy")},
        {"cut", 0xE8C6, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Cut")},
        {"edit", 0xE70F, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Edit")},
        {"print", 0xE749, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Print")},
        {"properties", 0xE946, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Properties")},
        {"link", 0xE71B, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Link")},
        {"refresh", 0xE72C, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Refresh")},
        {"power", 0xE7E8, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Power")},
        {"display", 0xE7F4, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Display")},
        {"network", 0xE839, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Network")},
        {"wifi", 0xE701, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Wi-Fi")},
        {"bluetooth", 0xE702, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Bluetooth")},
        {"sound", 0xE767, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Sound")},
        {"download", 0xE896, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Download")},
        {"upload", 0xE898, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Upload")},
        {"game", 0xE7FC, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Game")},
        {"store", 0xE719, QT_TRANSLATE_NOOP("ContextMenuGlyphs", "Store")},
    };
    return rows;
}

} // namespace

QList<MenuGlyph> menuGlyphPool()
{
    QList<MenuGlyph> pool;
    for (const PoolRow& row : poolRows()) {
        MenuGlyph glyph;
        glyph.name = QString::fromLatin1(row.name);
        glyph.glyph = QString(QChar(row.glyph));
        glyph.label = QCoreApplication::translate("ContextMenuGlyphs", row.label);
        pool.append(glyph);
    }
    return pool;
}

QString glyphForName(const QString& name)
{
    for (const PoolRow& row : poolRows()) {
        if (name == QLatin1String(row.name))
            return QString(QChar(row.glyph));
    }
    return {};
}

QString nameForGlyph(const QString& glyph)
{
    if (glyph.isEmpty())
        return {};
    for (const PoolRow& row : poolRows()) {
        if (glyph == QString(QChar(row.glyph)))
            return QString::fromLatin1(row.name);
    }
    return {};
}

QList<MenuPreset> menuPresets()
{
    // Settings pages go through explorer.exe: the URI handler is the shell's, and
    // running it through a program keeps one shape for every entry (a program and
    // its arguments). The light/dark switch is ours -- no program at all.
    const QString explorer = QStringLiteral("explorer.exe");
    QList<MenuPreset> presets;
    const auto add = [&presets](const QString& label, const QString& program, const QStringList& arguments,
                                const QString& glyph, const QString& action) {
        MenuPreset preset;
        preset.label = label;
        preset.program = program;
        preset.arguments = arguments;
        preset.glyph = glyph;
        preset.action = action;
        presets.append(preset);
    };
    add(QCoreApplication::translate("ContextMenuPresets", "Windows settings"), explorer,
        {QStringLiteral("ms-settings:")}, QStringLiteral("settings"), {});
    add(QCoreApplication::translate("ContextMenuPresets", "Installed applications"), explorer,
        {QStringLiteral("ms-settings:appsfeatures")}, QStringLiteral("apps"), {});
    add(QCoreApplication::translate("ContextMenuPresets", "Appearance and themes"), explorer,
        {QStringLiteral("ms-settings:personalization")}, QStringLiteral("appearance"), {});
    add(QCoreApplication::translate("ContextMenuPresets", "Light or dark mode"), {}, {}, QStringLiteral("theme"),
        QStringLiteral("theme"));
    add(QCoreApplication::translate("ContextMenuPresets", "Network and Internet"), explorer,
        {QStringLiteral("ms-settings:network")}, QStringLiteral("network"), {});
    add(QCoreApplication::translate("ContextMenuPresets", "Windows Update"), explorer,
        {QStringLiteral("ms-settings:windowsupdate")}, QStringLiteral("refresh"), {});
    return presets;
}

} // namespace starfish::core
