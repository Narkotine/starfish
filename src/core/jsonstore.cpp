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

QByteArray JsonStore::toJson(const Layout& layout)
{
    QJsonObject root;
    root["schema"] = layout.schema;
    QJsonArray fences;
    for (const auto& f : layout.fences)
        fences.append(fenceToJson(f));
    root["fences"] = fences;
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
