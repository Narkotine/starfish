#include "core/update_feed.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QUrl>

namespace starfish::core {

UpdateSettings UpdateSettings::read()
{
    QSettings s(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish\\Updates"), QSettings::NativeFormat);
    UpdateSettings out;
    out.enabled = s.value(QStringLiteral("Enabled"), false).toBool();
    out.feedUrl = s.value(QStringLiteral("FeedUrl")).toString();
    return out;
}

bool UpdateFeed::parse(const QByteArray& json, UpdateRelease* out, QString* error)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject())
        return fail(QStringLiteral("not a JSON object: %1").arg(pe.errorString()));
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schema")).toInt() != 1)
        return fail(QStringLiteral("unsupported feed schema"));
    const QJsonObject latest = root.value(QStringLiteral("latest")).toObject();
    UpdateRelease r;
    r.version = latest.value(QStringLiteral("version")).toString();
    r.url = latest.value(QStringLiteral("url")).toString();
    r.sha256 = latest.value(QStringLiteral("sha256")).toString().toLower();
    r.notes = latest.value(QStringLiteral("notes")).toString();
    static const QRegularExpression ver(QStringLiteral("^\\d+(\\.\\d+){0,3}$"));
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!ver.match(r.version).hasMatch())
        return fail(QStringLiteral("bad version"));
    const QUrl u(r.url);
    if (!u.isValid() || u.scheme() != QLatin1String("https"))
        return fail(QStringLiteral("installer URL must be https"));
    if (!hex.match(r.sha256).hasMatch())
        return fail(QStringLiteral("bad sha256"));
    if (out)
        *out = r;
    return true;
}

int UpdateFeed::compareVersions(const QString& a, const QString& b)
{
    const QStringList pa = a.split(QLatin1Char('.')), pb = b.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < qMax(pa.size(), pb.size()); ++i) {
        const int x = i < pa.size() ? pa.at(i).toInt() : 0;
        const int y = i < pb.size() ? pb.at(i).toInt() : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

bool UpdateFeed::verifySha256(const QString& filePath, const QString& expectedHex, QString* error)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        if (error) *error = QStringLiteral("read failed");
        return false;
    }
    const bool ok = QString::fromLatin1(h.result().toHex()) == expectedHex.toLower();
    if (!ok && error) *error = QStringLiteral("sha256 mismatch");
    return ok;
}

} // namespace starfish::core
