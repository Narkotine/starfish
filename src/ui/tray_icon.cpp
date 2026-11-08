#include "ui/tray_icon.hpp"

#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QWidgetAction>
#include <QMenu>
#include <QPainter>
#include <QRectF>
#include <QSystemTrayIcon>

#include "ui/fence_material.hpp"
#include "ui/modern_menu.hpp"

namespace starfish::ui {

TrayIcon::TrayIcon(QObject* parent)
    : QObject(parent)
{
}

TrayIcon::~TrayIcon()
{
    if (m_tray) {
        m_tray->hide();
        m_tray->setContextMenu(nullptr); // the menu is not owned by the tray icon
    }
    delete m_menu;
    m_menu = nullptr;
}

bool TrayIcon::isAvailable()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

QIcon TrayIcon::buildIcon()
{
    // Session 59: StarFish's own mark -- a coral starfish (tools/make_icon.py,
    // src/resources/starfish.svg), every size drawn for itself so the tray's
    // 16-24 px and a window's 32-48 px are each sharp. It replaces a miniature
    // fence in a dark box, which looked like another product's icon.
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/starfish-%1.png").arg(size), QSize(size, size));
    return icon;
}

// Session 97: the entries, built again when the language changes.
void TrayIcon::buildMenu()
{
    const auto glyphed = [this](QMenu* menu, const QString& text, ushort glyph) {
        QAction* action = menu->addAction(text);
        if (glyph)
            m_glyphs.insert(action, QString(QChar(glyph)));
        return action;
    };

    // The header: the mark, the name, how many fences.
    auto* header = new QWidget(m_menu);
    auto* logo = new QLabel(header);
    logo->setPixmap(buildIcon().pixmap(24, 24));
    auto* name = new QLabel(QStringLiteral("StarFish"), header);
    name->setObjectName(QStringLiteral("trayHeaderName"));
    m_headerCount = new QLabel(header);
    m_headerCount->setObjectName(QStringLiteral("trayHeaderCount"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 8, 16, 6);
    headerLayout->setSpacing(10);
    headerLayout->addWidget(logo);
    headerLayout->addWidget(name);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_headerCount);
    auto* headerAction = new QWidgetAction(m_menu);
    headerAction->setDefaultWidget(header); // enabled: a disabled one greys the mark
    m_menu->addAction(headerAction);
    m_menu->addSeparator();

    const ModernMenuStyle style = ModernMenuStyle::current();
    addMenuSection(m_menu, tr("Fences"), style);
    m_newFence = glyphed(m_menu, tr("New fence"), 0xE710);
    connect(m_newFence, &QAction::triggered, this, &TrayIcon::newFenceRequested);

    // Checkable state == "what is true now", so the signal and the
    // container's own setters speak the same language.
    m_showFences = glyphed(m_menu, tr("Show fences"), 0);
    m_showFences->setCheckable(true);
    m_showFences->setChecked(true);
    connect(m_showFences, &QAction::triggered, this, [this](bool checked) {
        emit fencesVisibilityToggled(checked);
    });

    // T-053 (session 65): Peek is gone -- the fences sit above the icons
    // anyway since ADR-15, so it had nothing left to show.

    // F-038 / T-057: the fence material, for every fence at once. The tray
    // cannot point at one fence; a fence's own menu chooses for one.
    m_materialMenu = m_menu->addMenu(tr("Fence material"));
    m_glyphs.insert(m_materialMenu->menuAction(), QString(QChar(0xE790)));
    // Exclusive, so clicking one entry unchecks the others even before the
    // product has reported the new state back (the menu closes first).
    auto* materialGroup = new QActionGroup(m_menu);
    materialGroup->setExclusive(true);
    for (const FenceMaterial::Preset& preset : FenceMaterial::presets()) {
        const QString id = QString::fromLatin1(preset.id);
        QAction* entry = m_materialMenu->addAction(FenceMaterial::presetLabel(id));
        entry->setCheckable(true);
        connect(entry, &QAction::triggered, this, [this, id] {
            emit fenceMaterialRequested(id);
        });
        materialGroup->addAction(entry);
        m_materials.insert(id, entry);
    }

    m_menu->addSeparator();
    addMenuSection(m_menu, tr("Desktop"), style);
    // Session 58: the shortcut arrow on fence tiles, a setting of its own.
    m_fenceArrows = glyphed(m_menu, tr("Shortcut arrows on fence icons"), 0);
    m_fenceArrows->setCheckable(true);
    m_fenceArrows->setChecked(true);
    connect(m_fenceArrows, &QAction::triggered, this,
            [this](bool checked) { emit fenceShortcutArrowsToggled(checked); });
    // T-092: the drawn right-click menu's editor (the settings window's page).
    connect(glyphed(m_menu, tr("Customize the right-click menu..."), 0xE70F), &QAction::triggered,
            this, [this] { emit contextMenuEditRequested(); });

    // ADR-13: the desktop's icons are never StarFish's to hide, so these
    // two entries do not exist unless a caller asks for them (the dev path
    // that still claims the desktop). They are built either way so the
    // actions are always valid pointers; visibility is what changes.
    m_hideIcons = glyphed(m_menu, tr("Hide desktop icons"), 0);
    m_hideIcons->setCheckable(true);
    connect(m_hideIcons, &QAction::triggered, this, [this](bool checked) {
        emit desktopIconsToggled(checked);
    });
    m_hideIcons->setVisible(m_offerIconHiding);

    // The emergency exit (T-046): a plain command, offered on its own flag
    // too (T-088) -- a user whose icons StarFish draws keeps a way back.
    m_restoreIcons = glyphed(m_menu, tr("Restore my icons"), 0xE777);
    connect(m_restoreIcons, &QAction::triggered, this, &TrayIcon::restoreIconsRequested);
    m_restoreIcons->setVisible(m_offerRestoreIcons);

    m_menu->addSeparator();

    m_settings = glyphed(m_menu, tr("Settings..."), 0xE713);
    connect(m_settings, &QAction::triggered, this, &TrayIcon::settingsRequested);

    QAction* quit = glyphed(m_menu, tr("Quit StarFish"), 0xE7E8);
    connect(quit, &QAction::triggered, this, &TrayIcon::quitRequested);

    restyle();
    setFenceCount(m_fenceCount);
}

void TrayIcon::retranslate()
{
    if (!m_menu) {
        m_menu = new QMenu;
        connect(m_menu, &QMenu::aboutToShow, this, [this] { restyle(); });
    } else {
        for (QAction* action : m_menu->actions())
            if (action->menu() && action->menu() != m_menu)
                action->menu()->deleteLater();
        m_menu->clear();
    }
    m_glyphs.clear();
    m_materials.clear();
    m_styledDark = -1;
    buildMenu();
    if (m_tray)
        m_tray->setToolTip(tr("StarFish -- %1 fence(s)").arg(m_fenceCount));
}

bool TrayIcon::show()
{
    if (!isAvailable())
        return false;

    if (!m_tray) {
        // Session 59: Windows 11's look (the same as the drawn desktop menu),
        // in sections -- what concerns the fences, what concerns the desktop,
        // then the window and the exit.
        m_menu = new QMenu;
        // The theme can change while StarFish runs: the menu follows it.
        connect(m_menu, &QMenu::aboutToShow, this, [this] { restyle(); });
        buildMenu();

        m_tray = new QSystemTrayIcon(buildIcon(), this);
        m_tray->setToolTip(tr("StarFish"));
        m_tray->setContextMenu(m_menu);
        // A left click opens the settings window: the icon is the only handle on
        // an application whose fences live on the desktop and which therefore has
        // no window of its own to click on.
        connect(m_tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger)
                        emit settingsRequested();
                });
    }

    m_tray->show();
    return m_tray->isVisible();
}

void TrayIcon::setFencesVisible(bool visible)
{
    if (m_showFences)
        m_showFences->setChecked(visible);
}

void TrayIcon::setPeekActive(bool active)
{
    if (m_peek)
        m_peek->setChecked(active);
}

void TrayIcon::setDesktopIconsHidden(bool hidden)
{
    if (m_hideIcons)
        m_hideIcons->setChecked(hidden);
}

void TrayIcon::setFenceShortcutArrows(bool shown)
{
    if (m_fenceArrows)
        m_fenceArrows->setChecked(shown);
}

void TrayIcon::setFenceMaterial(const QString& material)
{
    // An empty material means the fences disagree: nothing is checked, which is
    // the only honest state for a menu that speaks for all of them at once.
    for (auto it = m_materials.constBegin(); it != m_materials.constEnd(); ++it)
        it.value()->setChecked(it.key() == material && !material.isEmpty());
}

void TrayIcon::setIconHidingOffered(bool offered)
{
    m_offerIconHiding = offered;
    // The actions may already exist (show() has run) or not yet: setVisibility
    // covers both, because show() reads the flag when it builds them.
    if (m_hideIcons)
        m_hideIcons->setVisible(offered);
    if (m_restoreIcons)
        m_restoreIcons->setVisible(offered);
}

void TrayIcon::setRestoreIconsOffered(bool offered)
{
    // Only the plain command, and only that one: "Hide desktop icons" stays where
    // T-062/ADR-13 put it -- behind the developer path that still claims the
    // desktop -- while the way back is offered whenever StarFish may take the
    // desktop's icons (T-088).
    m_offerRestoreIcons = offered || m_offerIconHiding;
    if (m_restoreIcons)
        m_restoreIcons->setVisible(m_offerRestoreIcons);
}

void TrayIcon::setFenceCount(int count)
{
    m_fenceCount = count;
    if (m_headerCount)
        m_headerCount->setText(tr("%1 fence(s)").arg(count));
    if (!m_tray)
        return;
    m_tray->setToolTip(tr("StarFish -- %1 fence(s)").arg(count));
}

void TrayIcon::restyle()
{
    if (!m_menu)
        return;
    const ModernMenuStyle style = ModernMenuStyle::current();
    if (m_styledDark == int(style.dark))
        return;
    m_styledDark = int(style.dark);
    styleModernMenu(m_menu, style);
    const QColor glyph = themeText(style);
    for (auto it = m_glyphs.cbegin(); it != m_glyphs.cend(); ++it)
        it.key()->setIcon(glyphIcon(it.value(), glyph));
    // The section titles and the header carry their own colours.
    for (QLabel* label : m_menu->findChildren<QLabel*>()) {
        if (label->text() == label->text().toUpper() && !label->text().isEmpty() && label->pixmap().isNull())
            label->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 7.5pt; font-weight: 600; "
                                                "padding: 8px 12px 2px 12px; background: transparent; }")
                                     .arg(style.dark ? QStringLiteral("#9A9A9A") : QStringLiteral("#6B6B6B")));
    }
    if (auto* name = m_menu->findChild<QLabel*>(QStringLiteral("trayHeaderName")))
        name->setStyleSheet(QStringLiteral("color: %1; font-weight: 600; font-size: 10pt; background: transparent;")
                                .arg(themeText(style).name()));
    if (m_headerCount)
        m_headerCount->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                         .arg(style.dark ? QStringLiteral("#B0B0B0") : QStringLiteral("#5C5C5C")));
}

void TrayIcon::notify(const QString& title, const QString& message)
{
    if (m_tray && m_tray->isVisible())
        m_tray->showMessage(title, message, QSystemTrayIcon::Information, 4000);
}

} // namespace starfish::ui
