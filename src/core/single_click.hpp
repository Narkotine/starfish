#pragma once

#include <QString>

namespace starfish::core {

// Session 119: "" inherits. A Bouton with an empty value is on (session 115
// always launched on a single click). A classic zone or the desktop inherits
// the Settings default (off unless the user turns it on).
inline bool resolvedSingleClickLaunch(const QString& value, bool isButton, bool globalDefault)
{
    if (value == QLatin1String("on"))
        return true;
    if (value == QLatin1String("off"))
        return false;
    return isButton ? true : globalDefault;
}

} // namespace starfish::core
