#include "ui/tray_icon.hpp"

#include <QAction>
#include <QMenu>
#include <QPainter>
#include <QRectF>
#include <QSystemTrayIcon>

namespace starfish::ui {

namespace {

// The fence panel colours, so the icon reads as "a StarFish fence" rather than
// as a generic glyph.
const QColor kPanel(0x1f, 0x29, 0x37);
const QColor kTitleBar(0x60, 0xa5, 0xfa);
const QColor kTile(0xe5, 0xe7, 0xeb);

} // namespace

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
    // A miniature fence: rounded panel, title bar, three tiles. Drawn rather
    // than loaded so the build has no resource bundle to get wrong.
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    p.setBrush(kPanel);
    p.drawRoundedRect(QRectF(4, 5, 56, 54), 10, 10);
    p.setBrush(kTitleBar);
    p.drawRoundedRect(QRectF(4, 5, 56, 13), 6, 6);

    p.setBrush(kTile);
    p.drawRoundedRect(QRectF(11, 26, 13, 13), 3, 3);
    p.drawRoundedRect(QRectF(28, 26, 13, 13), 3, 3);
    p.drawRoundedRect(QRectF(11, 43, 13, 13), 3, 3);
    p.end();

    return QIcon(pm);
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

        m_menu->addSeparator();

        m_hideIcons = m_menu->addAction(tr("Hide desktop icons"));
        m_hideIcons->setCheckable(true);
        connect(m_hideIcons, &QAction::triggered, this, [this](bool checked) {
            emit desktopIconsToggled(checked);
        });

        // The emergency exit (T-046), next to the toggle on purpose: it is a
        // plain command, and it exists for the states the toggle cannot reach
        // (the icons hidden by a run that never came back).
        m_restoreIcons = m_menu->addAction(tr("Restore my icons"));
        connect(m_restoreIcons, &QAction::triggered, this, &TrayIcon::restoreIconsRequested);

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
