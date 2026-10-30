#include "ui/tray_icon.hpp"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QPainter>
#include <QRectF>
#include <QSystemTrayIcon>

#include "ui/fence_material.hpp"

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

bool TrayIcon::show()
{
    if (!isAvailable())
        return false;

    if (!m_tray) {
        m_menu = new QMenu;

        m_newFence = m_menu->addAction(tr("New fence"));
        connect(m_newFence, &QAction::triggered, this, &TrayIcon::newFenceRequested);

        m_menu->addSeparator();

        // Checkable state == "what is true now", so the signal and the
        // container's own setters speak the same language.
        m_showFences = m_menu->addAction(tr("Show fences"));
        m_showFences->setCheckable(true);
        m_showFences->setChecked(true);
        connect(m_showFences, &QAction::triggered, this, [this](bool checked) {
            emit fencesVisibilityToggled(checked);
        });

        m_peek = m_menu->addAction(tr("Peek (raise above the icons)"));
        m_peek->setCheckable(true);
        connect(m_peek, &QAction::triggered, this, [this](bool checked) {
            emit peekToggled(checked);
        });

        // F-038 / T-057: the fence material, for every fence at once. The tray
        // cannot point at one fence, and that is exactly what makes it useful
        // here: "make them all translucent" is one click. The settings window is
        // where a single fence is chosen.
        m_materialMenu = m_menu->addMenu(tr("Fence material"));
        // Exclusive, so clicking one entry unchecks the others even before the
        // product has reported the new state back (the menu closes first).
        auto* materialGroup = new QActionGroup(this);
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

        // Session 58: the shortcut arrow on fence tiles, a setting of its own.
        m_fenceArrows = m_menu->addAction(tr("Shortcut arrows on fence icons"));
        m_fenceArrows->setCheckable(true);
        m_fenceArrows->setChecked(true);
        connect(m_fenceArrows, &QAction::triggered, this,
                [this](bool checked) { emit fenceShortcutArrowsToggled(checked); });
        // T-092: the drawn right-click menu's configuration (hide, reorder, add).
        connect(m_menu->addAction(tr("Customize the right-click menu...")), &QAction::triggered,
                this, [this] { emit contextMenuEditRequested(); });

        // ADR-13: the desktop's icons are never StarFish's to hide, so these
        // two entries do not exist unless a caller asks for them (the dev path
        // that still claims the desktop). They are built either way so the
        // actions are always valid pointers; visibility is what changes.
        m_menu->addSeparator();

        m_hideIcons = m_menu->addAction(tr("Hide desktop icons"));
        m_hideIcons->setCheckable(true);
        connect(m_hideIcons, &QAction::triggered, this, [this](bool checked) {
            emit desktopIconsToggled(checked);
        });
        m_hideIcons->setVisible(m_offerIconHiding);

        // The emergency exit (T-046), next to the toggle on purpose: it is a
        // plain command, and it exists for the states the toggle cannot reach
        // (the icons hidden by a run that never came back). T-088: it is offered
        // on its own flag as well, because a user who switches the icons over to
        // StarFish must keep a way back.
        m_restoreIcons = m_menu->addAction(tr("Restore my icons"));
        connect(m_restoreIcons, &QAction::triggered, this, &TrayIcon::restoreIconsRequested);
        m_restoreIcons->setVisible(m_offerRestoreIcons);

        m_menu->addSeparator();

        m_settings = m_menu->addAction(tr("Settings..."));
        connect(m_settings, &QAction::triggered, this, &TrayIcon::settingsRequested);

        m_menu->addAction(tr("Quit StarFish"));
        // The last action is the quit one; wire it by position so it cannot be
        // confused with a member that means something else.
        connect(m_menu->actions().constLast(), &QAction::triggered, this,
                &TrayIcon::quitRequested);

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
    if (!m_tray)
        return;
    m_tray->setToolTip(tr("StarFish -- %1 fence(s)").arg(count));
}

void TrayIcon::notify(const QString& title, const QString& message)
{
    if (m_tray && m_tray->isVisible())
        m_tray->showMessage(title, message, QSystemTrayIcon::Information, 4000);
}

} // namespace starfish::ui
