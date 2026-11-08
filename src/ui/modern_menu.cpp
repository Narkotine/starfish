#include "ui/modern_menu.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QLabel>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QToolButton>
#include <QWidgetAction>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace starfish::ui {

namespace {

QString iconFontFamily()
{
    static const QString family = [] {
        const QStringList families = QFontDatabase::families();
        if (families.contains(QStringLiteral("Segoe Fluent Icons")))
            return QStringLiteral("Segoe Fluent Icons");
        return QStringLiteral("Segoe MDL2 Assets");
    }();
    return family;
}

QString styleSheetFor(const ModernMenuStyle& s)
{
    // Measured on Windows 11 23H2 menus: 8 px corners (DWM), 4 px inside the
    // frame, 30-32 px rows, 4 px rounded highlight, Segoe UI 9 pt.
    const QString bg = s.dark ? QStringLiteral("#2C2C2C") : QStringLiteral("#F9F9F9");
    const QString fg = s.dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1A1A1A");
    const QString dim = s.dark ? QStringLiteral("#787878") : QStringLiteral("#A0A0A0");
    const QString hover = s.dark ? QStringLiteral("rgba(255,255,255,0.08)") : QStringLiteral("rgba(0,0,0,0.05)");
    const QString line = s.dark ? QStringLiteral("rgba(255,255,255,0.09)") : QStringLiteral("rgba(0,0,0,0.08)");
    const QString border = s.dark ? QStringLiteral("#3B3B3B") : QStringLiteral("#E0E0E0");
    return QStringLiteral(
               "QMenu { background: %1; color: %2; border: 1px solid %6; padding: 4px; "
               "font-family: 'Segoe UI Variable Text', 'Segoe UI'; font-size: 9pt; }"
               "QMenu::item { padding: 7px 28px 7px 12px; border-radius: 4px; margin: 0px; }"
               "QMenu::item:selected { background: %4; }"
               "QMenu::item:disabled { color: %3; }"
               "QMenu::icon { padding-left: 10px; }"
               "QMenu::separator { height: 1px; background: %5; margin: 4px -4px; }"
               "QMenu::right-arrow { width: 12px; height: 12px; }"
               "QToolButton { background: transparent; border: none; border-radius: 4px; "
               "padding: 6px; margin: 0px 2px; }"
               "QToolButton:hover { background: %4; }")
        .arg(bg, fg, dim, hover, line, border);
}

void roundCorners(QWidget* popup)
{
#ifdef Q_OS_WIN
    // Windows 11's own popups: small rounded corners and the system shadow.
    // DWMWA_WINDOW_CORNER_PREFERENCE (33) = DWMWCP_ROUNDSMALL (3); ignored on 10.
    const HWND hwnd = reinterpret_cast<HWND>(popup->winId());
    const DWORD preference = 3;
    ::DwmSetWindowAttribute(hwnd, 33, &preference, sizeof(preference));
#else
    Q_UNUSED(popup);
#endif
}

void fill(QMenu* menu, const std::vector<core::ContextMenuItem>& items, const ModernMenuStyle& style,
          const std::shared_ptr<std::function<void(const core::ContextMenuItem&)>>& chosen,
          QMenu* root)
{
    const QColor fg = style.dark ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x1A, 0x1A, 0x1A);
    const QColor dim = style.dark ? QColor(0x78, 0x78, 0x78) : QColor(0xA0, 0xA0, 0xA0);

    // The top bar: one row of icon buttons, named by their tooltip.
    QWidget* bar = nullptr;
    QHBoxLayout* barLayout = nullptr;
    for (const core::ContextMenuItem& item : items) {
        if (!item.topBar)
            continue;
        if (!bar) {
            bar = new QWidget(menu);
            barLayout = new QHBoxLayout(bar);
            barLayout->setContentsMargins(4, 2, 4, 2);
            barLayout->setSpacing(2);
        }
        auto* button = new QToolButton(bar);
        button->setIcon(glyphIcon(item.glyph, item.enabled ? fg : dim, 16));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(item.label);
        button->setAccessibleName(item.label);
        button->setEnabled(item.enabled);
        button->setObjectName(QStringLiteral("topbar:") + item.key);
        const core::ContextMenuItem copy = item;
        QObject::connect(button, &QToolButton::clicked, root, [root, chosen, copy] {
            root->setProperty("starfishChosen", true);
            root->close();
            (*chosen)(copy);
        });
        barLayout->addWidget(button);
    }
    if (bar) {
        barLayout->addStretch(1);
        auto* action = new QWidgetAction(menu);
        action->setDefaultWidget(bar);
        menu->addAction(action);
        menu->addSeparator();
    }

    for (const core::ContextMenuItem& item : items) {
        if (item.topBar)
            continue;
        if (item.separator) {
            menu->addSeparator();
            continue;
        }
        QString text = item.label;
        text.replace(QLatin1Char('&'), QLatin1String("&&"));
        if (!item.shortcut.isEmpty())
            text += QLatin1Char('\t') + item.shortcut;
        if (!item.children.empty()) {
            QMenu* sub = menu->addMenu(glyphIcon(item.glyph, fg), text);
            sub->setStyleSheet(menu->styleSheet());
            sub->setWindowFlag(Qt::NoDropShadowWindowHint, false);
            sub->setAttribute(Qt::WA_TranslucentBackground, false);
            sub->setObjectName(QStringLiteral("submenu:") + item.key);
            QObject::connect(sub, &QMenu::aboutToShow, sub, [sub] { roundCorners(sub); });
            fill(sub, item.children, style, chosen, root);
            sub->setEnabled(item.enabled);
            continue;
        }
        const QIcon icon = !item.iconFile.isEmpty() ? QIcon(item.iconFile)
                                                    : glyphIcon(item.glyph, item.enabled ? fg : dim);
        QAction* action = menu->addAction(icon, text);
        action->setObjectName(item.key);
        action->setEnabled(item.enabled);
        if (item.checked) {
            action->setCheckable(true);
            action->setChecked(true);
            if (item.glyph.isEmpty())
                action->setIcon(glyphIcon(item.radio ? QString(QChar(0xE915)) : QString(QChar(0xE73E)), fg));
        }
        if (item.isDefault) {
            QFont bold = action->font();
            bold.setBold(true);
            action->setFont(bold);
        }
        const core::ContextMenuItem copy = item;
        QObject::connect(action, &QAction::triggered, root, [root, chosen, copy] {
            root->setProperty("starfishChosen", true);
            (*chosen)(copy);
        });
    }
}

} // namespace

ModernMenuStyle ModernMenuStyle::current()
{
    ModernMenuStyle style;
    const QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    style.dark = personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
    // STARFISH_THEME=dark|light: the probe photographs both themes without
    // touching the user's setting.
    const QByteArray forced = qgetenv("STARFISH_THEME");
    if (forced == "dark" || forced == "light")
        style.dark = forced == "dark";
    return style;
}

QIcon glyphIcon(const QString& glyph, const QColor& color, int px)
{
    if (glyph.isEmpty())
        return {};
    const qreal dpr = 2.0; // crisp at 200 %, scaled down elsewhere
    QPixmap pixmap(QSize(px, px) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font(iconFontFamily());
    font.setPixelSize(px);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRect(0, 0, px, px), Qt::AlignCenter, glyph);
    painter.end();
    return QIcon(pixmap);
}

std::vector<core::ContextMenuItem> dropMenuItems()
{
    std::vector<core::ContextMenuItem> items;
    const auto add = [&items](int id, const char* key, const char* text, ushort glyph) {
        core::ContextMenuItem item;
        item.key = QString::fromLatin1(key);
        item.label = QCoreApplication::translate("DropMenu", text);
        item.glyph = QString(QChar(glyph));
        item.ownId = id;
        items.push_back(item);
    };
    add(0, "drop:move", QT_TRANSLATE_NOOP("DropMenu", "Move here"), 0xE8DE);
    items.back().isDefault = true;
    add(1, "drop:copy", QT_TRANSLATE_NOOP("DropMenu", "Copy here"), 0xE8C8);
    add(2, "drop:link", QT_TRANSLATE_NOOP("DropMenu", "Create shortcuts here"), 0xE71B);
    core::ContextMenuItem line;
    line.separator = true;
    items.push_back(line);
    add(3, "drop:cancel", QT_TRANSLATE_NOOP("DropMenu", "Cancel"), 0xE711);
    return items;
}

QMenu* buildModernMenu(const std::vector<core::ContextMenuItem>& items, const ModernMenuStyle& style,
                       std::function<void(const core::ContextMenuItem&)> chosen, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("starfishModernMenu"));
    menu->setStyleSheet(styleSheetFor(style));
    menu->setSeparatorsCollapsible(true);
    menu->setToolTipsVisible(true);
    auto shared = std::make_shared<std::function<void(const core::ContextMenuItem&)>>(std::move(chosen));
    fill(menu, items, style, shared, menu);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { roundCorners(menu); });
    return menu;
}

QString modernMenuStyleSheet(const ModernMenuStyle& style)
{
    return styleSheetFor(style);
}

void styleModernMenu(QMenu* menu, const ModernMenuStyle& style)
{
    menu->setStyleSheet(styleSheetFor(style));
    menu->setSeparatorsCollapsible(false);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { roundCorners(menu); });
    for (QAction* action : menu->actions()) {
        if (QMenu* sub = action->menu())
            styleModernMenu(sub, style);
    }
}

void addMenuSection(QMenu* menu, const QString& title, const ModernMenuStyle& style)
{
    auto* label = new QLabel(title.toUpper(), menu);
    label->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 7.5pt; font-weight: 600; "
                                        "letter-spacing: 1px; padding: 8px 12px 2px 12px; background: transparent; }")
                             .arg(style.dark ? QStringLiteral("#9A9A9A") : QStringLiteral("#6B6B6B")));
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    action->setEnabled(false);
    menu->addAction(action);
}

QColor accentColor()
{
    const QSettings dwm(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM"),
                        QSettings::NativeFormat);
    bool ok = false;
    const quint32 abgr = dwm.value(QStringLiteral("AccentColor")).toUInt(&ok);
    if (!ok || abgr == 0)
        return QColor(0x00, 0x78, 0xD4);
    return QColor(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
}

QColor themeText(const ModernMenuStyle& style)
{
    return style.dark ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x1A, 0x1A, 0x1A);
}

QString modernWindowStyleSheet(const ModernMenuStyle& s)
{
    const QString bg = s.dark ? QStringLiteral("#202020") : QStringLiteral("#F3F3F3");
    const QString card = s.dark ? QStringLiteral("#2B2B2B") : QStringLiteral("#FFFFFF");
    const QString cardLine = s.dark ? QStringLiteral("#1D1D1D") : QStringLiteral("#E5E5E5");
    const QString fg = s.dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1A1A1A");
    const QString dim = s.dark ? QStringLiteral("#C5C5C5") : QStringLiteral("#5C5C5C");
    const QString hover = s.dark ? QStringLiteral("rgba(255,255,255,0.06)") : QStringLiteral("rgba(0,0,0,0.04)");
    const QString press = s.dark ? QStringLiteral("rgba(255,255,255,0.09)") : QStringLiteral("rgba(0,0,0,0.06)");
    const QString control = s.dark ? QStringLiteral("#2D2D2D") : QStringLiteral("#FBFBFB");
    const QString controlLine = s.dark ? QStringLiteral("#3A3A3A") : QStringLiteral("#D9D9D9");
    const QColor accent = accentColor();
    const QString acc = accent.name();
    const QString accText = accent.lightness() > 150 ? QStringLiteral("#000000") : QStringLiteral("#FFFFFF");
    return QStringLiteral(
               "QWidget { color: %4; font-family: 'Segoe UI Variable Text', 'Segoe UI'; font-size: 10pt; }"
               "QDialog, QWidget#page, QStackedWidget, QScrollArea, QWidget#scrollBody { background: %1; }"
               "QLabel { background: transparent; }"
               "QLabel#pageTitle { font-family: 'Segoe UI Variable Display', 'Segoe UI'; font-size: 20pt; font-weight: 600; }"
               "QLabel#sectionTitle { font-size: 10.5pt; font-weight: 600; padding-top: 8px; }"
               "QLabel#hint { color: %5; font-size: 9pt; }"
               "QFrame#card { background: %2; border: 1px solid %3; border-radius: 8px; }"
               "QListWidget#nav { background: transparent; border: none; outline: none; }"
               "QListWidget#nav::item { padding: 9px 10px; margin: 1px 6px; border-radius: 5px; }"
               "QListWidget#nav::item:hover { background: %6; }"
               "QListWidget#nav::item:selected { background: %7; color: %4; }"
               "QPushButton { background: %8; border: 1px solid %9; border-radius: 5px; padding: 6px 14px; }"
               "QPushButton:hover { background: %6; }"
               "QPushButton:pressed { background: %7; }"
               "QPushButton#accent { background: %10; color: %11; border: 1px solid %10; }"
               "QPushButton:disabled { color: %5; }"
               "QToolButton { background: transparent; border: none; border-radius: 4px; padding: 4px; }"
               "QToolButton:hover { background: %6; }"
               "QComboBox, QLineEdit { background: %8; border: 1px solid %9; border-radius: 5px; padding: 5px 8px; }"
               "QComboBox QAbstractItemView { background: %2; border: 1px solid %9; selection-background-color: %6; }"
               "QListWidget#entries { background: %2; border: 1px solid %3; border-radius: 8px; outline: none; padding: 4px; }"
               "QListWidget#entries::item { padding: 6px 6px; border-radius: 4px; }"
               "QListWidget#entries::item:hover { background: %6; }"
               "QListWidget#entries::item:selected { background: %7; color: %4; }"
               "QCheckBox { spacing: 8px; }"
               "QRadioButton { spacing: 10px; padding: 4px 0; }"
               "QRadioButton::indicator { width: 18px; height: 18px; border-radius: 10px; border: 1px solid %5; background: %8; }"
               "QRadioButton::indicator:hover { border: 1px solid %4; }"
               "QRadioButton::indicator:checked { width: 10px; height: 10px; border-radius: 10px; border: 5px solid %10; background: %11; }"
               "QTabBar::tab { background: transparent; padding: 6px 14px; border-bottom: 2px solid transparent; }"
               "QTabBar::tab:selected { border-bottom: 2px solid %10; font-weight: 600; }"
               "QScrollBar:vertical { background: transparent; width: 8px; }"
               "QScrollBar::handle:vertical { background: %9; border-radius: 4px; min-height: 30px; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
        .arg(bg, card, cardLine, fg, dim, hover, press, control, controlLine)
        .arg(acc, accText);
}

void applyWindowTheme(QWidget* window, const ModernMenuStyle& style)
{
    window->setStyleSheet(modernWindowStyleSheet(style));
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL dark = style.dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    const DWORD round = 2;                                  // DWMWCP_ROUND
    ::DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
#endif
}

} // namespace starfish::ui
