#include "core/jsonstore.hpp"

#include <functional>
#include <QHash>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace starfish::core {

static QJsonObject styleToJson(const FenceStyle& s)
{
    QJsonObject o;
    o["background"] = s.background;
    o["radius"] = s.radius;
    o["padding"] = s.padding;
    o["iconSize"] = s.iconSize;
    o["gap"] = s.gap;
    o["titleBar"] = s.titleBar;
    if (!s.iconTint.isEmpty())
        o["iconTint"] = s.iconTint;
    // T-082: written for every fence, like titleBar -- an explicit `false` is what
    // the behaviour is, and a bool that is only written when true would make the
    // document depend on comparing it to a default.
    o["fadeIcons"] = s.fadeIcons;
    // T-080: written only when the fence has a band of its own. A document that
    // has never been asked for one keeps exactly the bytes it had -- which is what
    // LayoutStore compares to decide whether anything changed, and what makes a
    // disposition exported before T-080 identical to one exported after.
    if (!s.titleBarColor.isEmpty())
        o["titleBarColor"] = s.titleBarColor;
    o["defaultOpacity"] = s.defaultOpacity;
    // T-094: written only when the fence has its own (no change for the rest).
    if (s.borderWidth >= 0)
        o["borderWidth"] = s.borderWidth;
    if (!s.borderColor.isEmpty())
        o["borderColor"] = s.borderColor;
    if (s.cellWidth > 0)
        o["cellWidth"] = s.cellWidth;
    if (!s.effect.isEmpty())
        o["effect"] = s.effect;
    if (!s.titleAlign.isEmpty())
        o["titleAlign"] = s.titleAlign;
    if (!s.titleSide.isEmpty())
        o["titleSide"] = s.titleSide;
    if (!s.shape.isEmpty())
        o["shape"] = s.shape;
    if (!s.titleBarShape.isEmpty())
        o["titleBarShape"] = s.titleBarShape;
    if (!s.hoverEffect.isEmpty())
        o["hoverEffect"] = s.hoverEffect;
    if (!s.hoverUnroll.isEmpty())
        o["hoverUnroll"] = s.hoverUnroll;
    if (!s.rollToward.isEmpty())
        o["rollToward"] = s.rollToward;
    if (!s.snapToCells.isEmpty())
        o["snapToCells"] = s.snapToCells;
    if (!s.showLabels.isEmpty())
        o["showLabels"] = s.showLabels;
    if (!s.iconFlow.isEmpty())
        o["iconFlow"] = s.iconFlow;
    // Session 111: written only when the Bouton has its own (0 = default).
    if (s.buttonDiameter > 0)
        o["buttonDiameter"] = s.buttonDiameter;
    if (s.buttonRings > 0)
        o["buttonRings"] = s.buttonRings;
    if (!s.buttonSkin.isEmpty() && s.buttonSkin != QLatin1String("plain"))
        o["buttonSkin"] = s.buttonSkin;
    if (s.buttonSkin == QLatin1String("gif") && !s.buttonSkinPath.isEmpty())
        o["buttonSkinPath"] = s.buttonSkinPath;
    if (s.buttonSkinLayer == QLatin1String("front") || s.buttonSkinLayer == QLatin1String("behind"))
        o["buttonSkinLayer"] = s.buttonSkinLayer;
    if (s.buttonOpenMode == QLatin1String("reveal") || s.buttonOpenMode == QLatin1String("grow"))
        o["buttonOpenMode"] = s.buttonOpenMode;
    return o;
}

static QJsonObject itemToJson(const FenceItem& i)
{
    QJsonObject o;
    o["kind"] = i.kind;
    o["path"] = i.path;
    if (!i.fileId.isEmpty())
        o["fileId"] = i.fileId;
    if (!i.target.isEmpty())
        o["target"] = i.target;
    if (i.kind == QLatin1String("widget")) {
        QJsonObject w;
        w["type"] = i.widgetType;
        w["span"] = QJsonArray{i.span.width(), i.span.height()};
        w["settings"] = i.widgetSettings;
        o["widget"] = w;
    }
    return o;
}

static QJsonObject fenceToJson(const Fence& f)
{
    QJsonObject o;
    o["id"] = f.id;
    o["title"] = f.title;
    o["monitor"] = f.monitor;
    QJsonObject r;
    r["x"] = f.rect.x();
    r["y"] = f.rect.y();
    r["w"] = f.rect.width();
    r["h"] = f.rect.height();
    o["rect"] = r;
    o["style"] = styleToJson(f.style);
    o["sort"] = f.sortBy;
    o["sortAscending"] = f.sortAscending;
    o["rolledUp"] = f.rolledUp;
    if (f.locked)
        o["locked"] = true;
    QJsonArray items;
    for (const auto& i : f.items)
        items.append(itemToJson(i));
    o["items"] = items;
    return o;
}

static FenceStyle styleFromJson(const QJsonObject& o)
{
    FenceStyle s;
    if (o.contains("background"))
        s.background = o["background"].toString();
    s.radius = o["radius"].toInt(s.radius);
    s.padding = o["padding"].toInt(s.padding);
    s.iconSize = o["iconSize"].toInt(s.iconSize);
    s.gap = o["gap"].toInt(s.gap);
    if (o.contains("titleBar"))
        s.titleBar = o["titleBar"].toBool();
    s.iconTint = o["iconTint"].toString();
    // T-082: absent means false -- which is the whole point of the key (a
    // document written before it exists keeps the fence it describes, and stays
    // valid: no migration, no schema bump).
    s.fadeIcons = o["fadeIcons"].toBool(false);
    // T-080: absent -- or empty, which is the same thing -- is "no band of its
    // own", i.e. the title drawn on the panel, exactly as before this key existed.
    s.titleBarColor = o["titleBarColor"].toString();
    s.borderWidth = o["borderWidth"].toInt(-1);
    s.borderColor = o["borderColor"].toString();
    s.cellWidth = o["cellWidth"].toInt(0);
    s.effect = o["effect"].toString();
    s.titleAlign = o["titleAlign"].toString();
    s.titleSide = o["titleSide"].toString();
    s.shape = o["shape"].toString();
    s.titleBarShape = o["titleBarShape"].toString();
    s.hoverEffect = o["hoverEffect"].toString();
    s.hoverUnroll = o["hoverUnroll"].toString();
    s.rollToward = o["rollToward"].toString();
    s.snapToCells = o["snapToCells"].toString();
    s.showLabels = o["showLabels"].toString();
    s.iconFlow = o["iconFlow"].toString();
    s.buttonDiameter = o["buttonDiameter"].toInt(0);
    s.buttonRings = o["buttonRings"].toInt(0);
    s.buttonSkin = o["buttonSkin"].toString();
    s.buttonSkinPath = o["buttonSkinPath"].toString();
    s.buttonSkinLayer = o["buttonSkinLayer"].toString();
    s.buttonOpenMode = o["buttonOpenMode"].toString();
    // T-098: absent -> an opaque fence follows the default (which starts
    // opaque: nothing changes on screen), a translucent one keeps its own.
    if (o.contains("defaultOpacity")) {
        s.defaultOpacity = o["defaultOpacity"].toBool(true);
    } else {
        // `#AARRGGBB` (8 digits) carries an alpha; `#RRGGBB` is opaque.
        bool ok = false;
        const int alpha = s.background.size() == 9 ? s.background.mid(1, 2).toInt(&ok, 16) : 255;
        s.defaultOpacity = !ok ? true : alpha == 255;
    }
    return s;
}

static Fence fenceFromJson(const QJsonObject& o)
{
    Fence f;
    f.id = o["id"].toString();
    f.title = o["title"].toString();
    f.monitor = o["monitor"].toString();
    const auto r = o["rect"].toObject();
    f.rect = QRect(r["x"].toInt(), r["y"].toInt(),
                   r["w"].toInt(), r["h"].toInt());
    f.style = styleFromJson(o["style"].toObject());
    f.sortBy = o["sort"].toString(f.sortBy);
    f.sortAscending = o["sortAscending"].toBool(f.sortAscending);
    f.rolledUp = o["rolledUp"].toBool();
    f.locked = o["locked"].toBool();
    const auto items = o["items"].toArray();
    for (const auto& v : items) {
        const auto io = v.toObject();
        FenceItem item{io["kind"].toString(), io["path"].toString()};
        item.fileId = io["fileId"].toString();
        item.target = io["target"].toString();
        if (item.kind == QLatin1String("widget")) {
            const QJsonObject w = io["widget"].toObject();
            item.widgetType = w["type"].toString();
            const QJsonArray span = w["span"].toArray();
            item.span = QSize(qMax(1, span.at(0).toInt(1)), qMax(1, span.at(1).toInt(1)));
            item.widgetSettings = w["settings"].toObject();
            if (item.widgetType.isEmpty() || !item.path.startsWith(QLatin1String("widget:")))
                continue; // not a widget we can even keep
        }
        f.items.append(item);
    }
    return f;
}

static QJsonObject ruleToJson(const Rule& r)
{
    QJsonObject o;
    o["id"] = r.id;
    o["name"] = r.name;

    // The criteria live in their own object so that a future criterion (size,
    // folder-only, ...) is an addition rather than a schema break. Empty
    // criteria are omitted; an empty `match` is the documented catch-all.
    QJsonObject match;
    if (!r.extensions.isEmpty()) {
        QJsonArray types;
        for (const QString& extension : r.extensions)
            types.append(extension);
        match["type"] = types;
    }
    if (!r.namePattern.isEmpty())
        match["name"] = r.namePattern;
    if (r.newerThanDays)
        match["newerThanDays"] = *r.newerThanDays;
    if (r.olderThanDays)
        match["olderThanDays"] = *r.olderThanDays;
    o["match"] = match;

    o["targetFence"] = r.targetFenceId;
    o["moveFiles"] = r.moveFiles;
    return o;
}

static Rule ruleFromJson(const QJsonObject& o)
{
    Rule r;
    r.id = o["id"].toString();
    r.name = o["name"].toString();

    const QJsonObject match = o["match"].toObject();
    for (const auto& value : match["type"].toArray()) {
        const QString extension = value.toString().toLower();
        if (!extension.isEmpty())
            r.extensions.append(extension);
    }
    r.namePattern = match["name"].toString();
    if (match.contains("newerThanDays"))
        r.newerThanDays = match["newerThanDays"].toInt();
    if (match.contains("olderThanDays"))
        r.olderThanDays = match["olderThanDays"].toInt();

    r.targetFenceId = o["targetFence"].toString();
    r.moveFiles = o["moveFiles"].toBool(false);
    return r;
}

QByteArray JsonStore::toJson(const Layout& layout)
{
    QJsonObject root;
    root["schema"] = layout.schema;
    QJsonArray fences;
    for (const auto& f : layout.fences)
        fences.append(fenceToJson(f));
    root["fences"] = fences;
    QJsonArray rules;
    for (const auto& r : layout.rules)
        rules.append(ruleToJson(r));
    root["rules"] = rules;
    // Session 91: only when there is one -- a document without desktop widgets
    // stays byte for byte what it was.
    if (!layout.desktopWidgets.isEmpty()) {
        QJsonArray widgets;
        for (const DesktopWidgetEntry& w : layout.desktopWidgets) {
            QJsonObject o = itemToJson(w.item);
            o.remove("kind");
            o["x"] = w.pos.x();
            o["y"] = w.pos.y();
            widgets.append(o);
        }
        root["desktopWidgets"] = widgets;
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<Layout> JsonStore::fromJson(const QByteArray& data, QString* error)
{
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = parseError.errorString();
        return std::nullopt;
    }
    QJsonObject root = doc.object();
    const int schema = root["schema"].toInt();
    if (schema < 1) {
        if (error)
            *error = QStringLiteral("unsupported schema: %1").arg(schema);
        return std::nullopt;
    }
    // Session 85: an older document is migrated in memory (LayoutStore backs
    // the file up first); a newer one is read as far as this build understands
    // it -- LayoutStore then refuses to write over it.
    if (schema < kCurrentSchema && !migrate(root, schema, error))
        return std::nullopt;

    Layout layout;
    for (const auto& v : root["fences"].toArray())
        layout.fences.append(fenceFromJson(v.toObject()));
    // Absent `rules` is not an error: documents written before T-010 simply
    // carry no rules, and must keep loading.
    for (const auto& v : root["rules"].toArray())
        layout.rules.append(ruleFromJson(v.toObject()));
    // Session 91: widgets on the desktop (absent before: none).
    for (const auto& v : root["desktopWidgets"].toArray()) {
        const QJsonObject o = v.toObject();
        DesktopWidgetEntry entry;
        entry.item = FenceItem{QStringLiteral("widget"), o["path"].toString()};
        const QJsonObject w = o["widget"].toObject();
        entry.item.widgetType = w["type"].toString();
        const QJsonArray span = w["span"].toArray();
        entry.item.span = QSize(qMax(1, span.at(0).toInt(1)), qMax(1, span.at(1).toInt(1)));
        entry.item.widgetSettings = w["settings"].toObject();
        entry.pos = QPoint(o["x"].toInt(), o["y"].toInt());
        if (entry.item.widgetType.isEmpty() || !entry.item.path.startsWith(QLatin1String("widget:")))
            continue;
        layout.desktopWidgets.append(entry);
    }
    return layout;
}

JsonStore::Info JsonStore::peek(const QByteArray& data, QString* error)
{
    Info info;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                                  : QStringLiteral("not a JSON object");
        return info;
    }
    info.valid = true;
    info.root = doc.object();
    info.schema = info.root["schema"].toInt();
    info.writtenBy = info.root["writtenBy"].toString();
    return info;
}

bool JsonStore::migrate(QJsonObject& root, int from, QString* error)
{
    // One entry per step: steps[n - 1] takes schema n to n + 1. Empty while the
    // only schema is 1 -- the pipeline (backup, step, re-check) is what exists.
    static const QList<std::function<bool(QJsonObject&)>> steps{};
    for (int schema = from; schema < kCurrentSchema; ++schema) {
        if (schema < 1 || schema - 1 >= steps.size() || !steps.at(schema - 1)(root)) {
            if (error)
                *error = QStringLiteral("no migration from schema %1").arg(schema);
            return false;
        }
        root["schema"] = schema + 1;
    }
    return true;
}

namespace {

// The keys this build writes, level by level. A key in a previous document
// that is not listed here belongs to someone else (a newer StarFish) and is
// carried over; a listed key is always this build's to write or to omit.
const QStringList kRootKeys{"schema", "fences", "rules", "writtenBy", "desktopWidgets"};
const QStringList kDesktopWidgetKeys{"path", "x", "y", "widget", "fileId", "target"};
const QStringList kFenceKeys{"id", "title", "monitor", "rect", "style", "sort", "sortAscending", "rolledUp", "locked", "items"};
const QStringList kStyleKeys{"background", "radius", "padding", "iconSize", "gap", "titleBar", "iconTint",
                             "fadeIcons", "titleBarColor", "defaultOpacity", "borderWidth", "borderColor",
                             "cellWidth", "effect", "titleAlign", "titleSide", "titleVertical" /* ignored since s103 */, "titleBarShape", "shape", "hoverEffect",
                             "hoverUnroll", "rollToward", "snapToCells", "showLabels", "iconFlow",
                             "buttonDiameter", "buttonRings",
                             "buttonSkin", "buttonSkinPath", "buttonSkinLayer", "buttonOpenMode"};
const QStringList kItemKeys{"kind", "path", "fileId", "target", "widget"};
const QStringList kWidgetKeys{"type", "span", "settings"};
const QStringList kRuleKeys{"id", "name", "match", "targetFence", "moveFiles"};
const QStringList kMatchKeys{"type", "name", "newerThanDays", "olderThanDays"};

void keepUnknown(QJsonObject& into, const QJsonObject& previous, const QStringList& known)
{
    for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
        if (!known.contains(it.key()) && !into.contains(it.key()))
            into.insert(it.key(), it.value());
    }
}

QHash<QString, QJsonObject> byKey(const QJsonArray& array, const char* key)
{
    QHash<QString, QJsonObject> map;
    for (const QJsonValue& v : array) {
        const QJsonObject o = v.toObject();
        const QString k = o[QLatin1String(key)].toString();
        if (!k.isEmpty() && !map.contains(k))
            map.insert(k, o);
    }
    return map;
}

} // namespace

QByteArray JsonStore::toJsonPreserving(const Layout& layout, const QJsonObject& previous, const QString& writtenBy)
{
    QJsonObject root = QJsonDocument::fromJson(toJson(layout)).object();
    if (!writtenBy.isEmpty())
        root["writtenBy"] = writtenBy;
    if (!previous.isEmpty()) {
        keepUnknown(root, previous, kRootKeys);
        const auto oldFences = byKey(previous["fences"].toArray(), "id");
        QJsonArray fences;
        for (const QJsonValue& v : root["fences"].toArray()) {
            QJsonObject fence = v.toObject();
            const QJsonObject old = oldFences.value(fence["id"].toString());
            if (!old.isEmpty()) {
                keepUnknown(fence, old, kFenceKeys);
                QJsonObject style = fence["style"].toObject();
                keepUnknown(style, old["style"].toObject(), kStyleKeys);
                fence["style"] = style;
                const auto oldItems = byKey(old["items"].toArray(), "path");
                QJsonArray items;
                for (const QJsonValue& iv : fence["items"].toArray()) {
                    QJsonObject item = iv.toObject();
                    const QJsonObject oldItem = oldItems.value(item["path"].toString());
                    if (!oldItem.isEmpty()) {
                        keepUnknown(item, oldItem, kItemKeys);
                        if (item.contains("widget") && oldItem.contains("widget")) {
                            QJsonObject widget = item["widget"].toObject();
                            keepUnknown(widget, oldItem["widget"].toObject(), kWidgetKeys);
                            item["widget"] = widget;
                        }
                    }
                    items.append(item);
                }
                fence["items"] = items;
            }
            fences.append(fence);
        }
        root["fences"] = fences;
        const auto oldRules = byKey(previous["rules"].toArray(), "id");
        QJsonArray rules;
        for (const QJsonValue& v : root["rules"].toArray()) {
            QJsonObject rule = v.toObject();
            const QJsonObject old = oldRules.value(rule["id"].toString());
            if (!old.isEmpty()) {
                keepUnknown(rule, old, kRuleKeys);
                QJsonObject match = rule["match"].toObject();
                keepUnknown(match, old["match"].toObject(), kMatchKeys);
                rule["match"] = match;
            }
            rules.append(rule);
        }
        root["rules"] = rules;
        // Session 91: per desktop widget (matched by path), its unknown keys.
        if (root.contains("desktopWidgets")) {
            const auto oldWidgets = byKey(previous["desktopWidgets"].toArray(), "path");
            QJsonArray widgets;
            for (const QJsonValue& v : root["desktopWidgets"].toArray()) {
                QJsonObject entry = v.toObject();
                const QJsonObject old = oldWidgets.value(entry["path"].toString());
                if (!old.isEmpty()) {
                    keepUnknown(entry, old, kDesktopWidgetKeys);
                    QJsonObject widget = entry["widget"].toObject();
                    keepUnknown(widget, old["widget"].toObject(), kWidgetKeys);
                    entry["widget"] = widget;
                }
                widgets.append(entry);
            }
            root["desktopWidgets"] = widgets;
        }
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool JsonStore::save(const Layout& layout, const QString& filePath, QString* error)
{
    return saveBytes(toJson(layout), filePath, error);
}

bool JsonStore::saveBytes(const QByteArray& payload, const QString& filePath, QString* error)
{

    // The layout lives in %APPDATA%/StarFish/, which does not exist on a fresh
    // profile: create it rather than failing the very first save.
    const QString dir = QFileInfo(filePath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("could not create %1").arg(dir);
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(payload) != payload.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

std::optional<Layout> JsonStore::load(const QString& filePath, QString* error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    return fromJson(file.readAll(), error);
}

QByteArray JsonStore::styleToBytes(const FenceStyle& style)
{
    return QJsonDocument(styleToJson(style)).toJson(QJsonDocument::Compact);
}

FenceStyle JsonStore::styleFromBytes(const QByteArray& data)
{
    return styleFromJson(QJsonDocument::fromJson(data).object());
}

} // namespace starfish::core
