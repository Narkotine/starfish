#include "platform/win/windows_theme.hpp"

#include <QSettings>
#include <QString>

#include <windows.h>

namespace starfish::platform::win {

namespace {

constexpr auto kPersonalize =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

QSettings personalize()
{
    return QSettings(QString::fromLatin1(kPersonalize), QSettings::NativeFormat);
}

} // namespace

ThemeState readTheme()
{
    QSettings settings = personalize();
    ThemeState state;
    const QVariant apps = settings.value(QStringLiteral("AppsUseLightTheme"));
    const QVariant system = settings.value(QStringLiteral("SystemUsesLightTheme"));
    // A key that was never written (a fresh profile) reads as light, which is
    // Windows' own default -- but `valid` still says whether anything was read.
    state.valid = apps.isValid() || system.isValid();
    state.appsLight = apps.isValid() ? apps.toInt() != 0 : true;
    state.systemLight = system.isValid() ? system.toInt() != 0 : true;
    return state;
}

bool applyTheme(bool light)
{
    QSettings settings = personalize();
    settings.setValue(QStringLiteral("AppsUseLightTheme"), light ? 1 : 0);
    settings.setValue(QStringLiteral("SystemUsesLightTheme"), light ? 1 : 0);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return false;
    // Every top-level window hears this one: the taskbar, Explorer, the open
    // applications. Without it the change only shows on the next logon.
    DWORD_PTR ignored = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                          reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 1000, &ignored);
    return true;
}

} // namespace starfish::platform::win
