#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <functional>
#include <vector>

#include "core/context_menu.hpp"

class QMenu;
class QWidget;

namespace starfish::ui {

// T-092 -- the right-click menu drawn the way Windows 11 draws its own: a
// rounded popup (DWM corners and shadow), Segoe UI, Segoe Fluent Icons glyphs,
// the row of icon buttons on top (Cut, Copy, Rename, Share, Delete), the
// light or dark theme the user picked for apps. A QMenu underneath, so
// keyboard navigation, submenus and accessibility come for free.
//
// `chosen` runs after the menu closed, with the entry picked.
struct ModernMenuStyle {
    bool dark = false;
    static ModernMenuStyle current(); // the "apps" theme from the registry
};

QMenu* buildModernMenu(const std::vector<core::ContextMenuItem>& items, const ModernMenuStyle& style,
                       std::function<void(const core::ContextMenuItem&)> chosen,
                       QWidget* parent = nullptr);

// A Segoe Fluent Icons (or Segoe MDL2 Assets) glyph as an icon.
QIcon glyphIcon(const QString& glyph, const QColor& color, int px = 16);

// Session 59 -- the same look for StarFish's other surfaces (the tray menu,
// the settings window), so the product has one style, Windows 11's.
QString modernMenuStyleSheet(const ModernMenuStyle& style);
// Styles a menu someone else built (the tray's): sheet, rounded corners, and
// the same for its submenus as they open.
void styleModernMenu(QMenu* menu, const ModernMenuStyle& style);
// A small grey section title inside a menu ("ZONES"), not clickable.
void addMenuSection(QMenu* menu, const QString& title, const ModernMenuStyle& style);
// A window in the Windows 11 manner: the theme's colours, a dark title bar
// when the theme is dark (DWMWA_USE_IMMERSIVE_DARK_MODE), Mica where offered.
QString modernWindowStyleSheet(const ModernMenuStyle& style);
void applyWindowTheme(QWidget* window, const ModernMenuStyle& style);
// The user's accent colour (DWM), Windows' default blue when unreadable.
QColor accentColor();
// The foreground colour of the theme, for glyph icons.
QColor themeText(const ModernMenuStyle& style);

} // namespace starfish::ui
