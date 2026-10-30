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

} // namespace starfish::ui
