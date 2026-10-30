#include "ui/modern_menu.hpp"

#include <QAction>
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

QString styleSheet(const ModernMenuStyle& s)
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
        QAction* action = menu->addAction(glyphIcon(item.glyph, item.enabled ? fg : dim), text);
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

QMenu* buildModernMenu(const std::vector<core::ContextMenuItem>& items, const ModernMenuStyle& style,
                       std::function<void(const core::ContextMenuItem&)> chosen, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("starfishModernMenu"));
    menu->setStyleSheet(styleSheet(style));
    menu->setSeparatorsCollapsible(true);
    menu->setToolTipsVisible(true);
    auto shared = std::make_shared<std::function<void(const core::ContextMenuItem&)>>(std::move(chosen));
    fill(menu, items, style, shared, menu);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { roundCorners(menu); });
    return menu;
}

} // namespace starfish::ui
