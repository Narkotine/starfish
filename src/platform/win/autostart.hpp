#pragma once

#include <QString>

namespace starfish::platform::win {

// Session 62 -- "Démarrer avec Windows". The registry is the truth, nothing is
// cached: HKCU\...\CurrentVersion\Run\StarFish holds the quoted executable,
// and Explorer's StartupApproved\Run\StarFish says whether the user turned it
// off (Task Manager > Startup apps: first byte odd = disabled). The key paths
// are parameters so the unit test works on scratch keys, never on the user's.
struct AutostartState {
    bool registered = false;      // the Run value exists
    bool disabledByUser = false;  // ... but Task Manager turned it off
    QString command;              // the Run value as written
    bool enabled() const { return registered && !disabledByUser; }
};

class Autostart
{
public:
    static constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr const wchar_t* kApprovedKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

    explicit Autostart(QString runKey = QString::fromWCharArray(kRunKey),
                       QString approvedKey = QString::fromWCharArray(kApprovedKey),
                       QString valueName = QStringLiteral("StarFish"));

    AutostartState read() const;
    // On: the Run value, and a Task Manager "disabled" lifted (the user asked
    // for it here). Off: the Run value and our StartupApproved entry go.
    bool setEnabled(bool on, const QString& exePath);
    // At launch: an existing entry follows the executable if it moved.
    // Returns true when the value was rewritten. Never creates one.
    bool refresh(const QString& exePath);

    static QString commandFor(const QString& exePath);
    static QString exeOf(const QString& command);

private:
    QString m_runKey;
    QString m_approvedKey;
    QString m_valueName;
};

} // namespace starfish::platform::win
