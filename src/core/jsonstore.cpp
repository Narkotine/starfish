#include "core/jsonstore.hpp"

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
    return o;
}

static QJsonObject itemToJson(const FenceItem& i)
{
    QJsonObject o;
    o["kind"] = i.kind;
    o["path"] = i.path;
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
    const auto items = o["items"].toArray();
    for (const auto& v : items) {
        const auto io = v.toObject();
        f.items.append({io["kind"].toString(), io["path"].toString()});
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
    const auto root = doc.object();
    if (root["schema"].toInt() != Layout{}.schema) {
        if (error)
            *error = QStringLiteral("unsupported schema: %1").arg(root["schema"].toInt());
        return std::nullopt;
    }

    Layout layout;
    for (const auto& v : root["fences"].toArray())
        layout.fences.append(fenceFromJson(v.toObject()));
    // Absent `rules` is not an error: documents written before T-010 simply
    // carry no rules, and must keep loading.
    for (const auto& v : root["rules"].toArray())
        layout.rules.append(ruleFromJson(v.toObject()));
    return layout;
}

bool JsonStore::save(const Layout& layout, const QString& filePath, QString* error)
{
    const QByteArray payload = toJson(layout);

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

} // namespace starfish::core
