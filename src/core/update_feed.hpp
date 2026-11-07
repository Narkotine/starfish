#pragma once
// Session 85: the decision-independent half of the updater (docs/mises-a-jour.md).
// No network, no token, no UI: parse a release feed, compare versions, verify a
// downloaded installer's SHA-256. Off by default; the feed URL is configuration.
#include <QByteArray>
#include <QString>

namespace starfish::core {

struct UpdateRelease {
    QString version;   // "0.2.0"
    QString url;       // https:// installer URL
    QString sha256;    // 64 lowercase hex chars
    QString notes;
};

struct UpdateSettings {
    bool enabled = false; // opt-in
    QString feedUrl;      // empty: nothing to check
    static UpdateSettings read();             // HKCU\Software\StarFish\Updates
};

class UpdateFeed {
public:
    // {"schema":1,"channel":"stable","latest":{"version","url","sha256","notes"}}
    static bool parse(const QByteArray& json, UpdateRelease* out, QString* error = nullptr);
    // <0, 0, >0 like strcmp; numeric dot-separated, missing parts are 0.
    static int compareVersions(const QString& a, const QString& b);
    static bool isNewer(const UpdateRelease& r, const QString& current) { return compareVersions(r.version, current) > 0; }
    static bool verifySha256(const QString& filePath, const QString& expectedHex, QString* error = nullptr);
};

} // namespace starfish::core
