#pragma once

namespace starfish::platform::win {

// Session 82: the light/dark switch, as a context-menu entry can run it.
//
// Windows keeps the choice in two values under HKCU (AppsUseLightTheme and
// SystemUsesLightTheme, 1 = light), and every window learns about a change from
// a WM_SETTINGCHANGE broadcast carrying "ImmersiveColorSet" -- that broadcast is
// what repaints the taskbar and the open apps without a logoff.
struct ThemeState {
    bool appsLight = true;
    bool systemLight = true;
    bool valid = false; // the values could be read at all
};

ThemeState readTheme();
// Writes both values and tells every window. False when the write failed.
bool applyTheme(bool light);

// The switch's decision, on its own: whatever it is, the other one. Pure, so the
// probe measures the choice without touching this machine's theme.
inline bool flippedLight(bool light)
{
    return !light;
}

} // namespace starfish::platform::win
