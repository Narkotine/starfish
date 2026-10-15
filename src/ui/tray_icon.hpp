#pragma once

#include <QIcon>
#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace starfish::ui {

// The notification-area presence of the product (F-034).
//
// Once the fences live on the desktop, StarFish has no ordinary window: the tray
// icon is what keeps it reachable, and it is the only place a user can create a
// fence, hide them all, or quit. It therefore owns exactly one responsibility --
// *reporting* what the user asked for. Every entry emits a signal and the product
// decides what it means; the same split as everywhere else in this codebase (the
// widget signals, the owner decides).
//
// The labels go through tr() with English source strings: interface
// localisation is F-036 / T-014, and marking them now means translating later is
// a translation file rather than a hunt.
class TrayIcon : public QObject
{
    Q_OBJECT

public:
    explicit TrayIcon(QObject* parent = nullptr);
    ~TrayIcon() override;

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // True when this session has a system tray at all (an Explorer shell). The
    // caller decides how to survive without one instead of failing silently.
    static bool isAvailable();

    // Creates the icon and its menu. Returns false when the tray could not be
    // shown.
    bool show();

    // Keeps the check marks in step with the real state. The product calls these
    // whenever the container's navigation state changes, including when it
    // changes from something other than the menu (a hot key).
    void setFencesVisible(bool visible);
    void setPeekActive(bool active);
    void setDesktopIconsHidden(bool hidden);
    void setFenceCount(int count);

    // A short balloon message, for an action whose effect is on the desktop and
    // therefore easy to miss.
    void notify(const QString& title, const QString& message);

signals:
    void newFenceRequested();
    void fencesVisibilityToggled(bool visible);
    void peekToggled(bool active);
    void desktopIconsToggled(bool hidden);
    void settingsRequested();
    void quitRequested();

private:
    // Drawn in code rather than shipped as a resource: StarFish has no resource
    // bundle yet, and an icon that cannot go missing cannot break the tray.
    static QIcon buildIcon();

    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QAction* m_newFence = nullptr;
    QAction* m_showFences = nullptr;
    QAction* m_peek = nullptr;
    QAction* m_hideIcons = nullptr;
    QAction* m_settings = nullptr;
};

} // namespace starfish::ui
