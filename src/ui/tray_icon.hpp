#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QString>

class QAction;
class QLabel;
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

    // F-038 / T-057: which material the fences are shown in, as a menu submenu
    // entry. `material` is a FenceMaterial preset id, or an empty string when the
    // fences do not agree (a mixed desktop): the submenu then shows nothing as
    // current instead of picking one of them. The tray applies a material to
    // **all** fences -- it has no way to point at one -- and a fence's own menu
    // (T-076, ADR-16) is what chooses per fence.
    void setFenceMaterial(const QString& material);
    // Whether fence tiles show the shortcut arrow (a checkable entry).
    void setFenceShortcutArrows(bool shown);

    // ADR-13: StarFish never hides the desktop's icons, so the two entries that
    // exist for that ("Hide desktop icons" and the repair command beside it) are
    // not offered at all unless the caller asks for them. A menu entry the user
    // must never click, whose only effect is to change their desktop for the
    // worse, is a trap -- it is a development tool now, and it is hidden as one.
    void setIconHidingOffered(bool offered);
    bool iconHidingOffered() const { return m_offerIconHiding; }

    // T-088: the emergency exit on its own, because it has to be reachable exactly
    // when it can be needed -- i.e. whenever StarFish is allowed to be the one
    // drawing the desktop (the user's own switch, in the settings window). Kept
    // apart from setIconHidingOffered so that what the product grows is not the
    // developer path's "Hide desktop icons" toggle: "Restore my icons" is a plain
    // command (it asks the desktop, and asks it again whatever our bookkeeping
    // believes, T-046), and offering it costs the user nothing.
    void setRestoreIconsOffered(bool offered);
    void setFenceCount(int count);

    // A short balloon message, for an action whose effect is on the desktop and
    // therefore easy to miss.
    void notify(const QString& title, const QString& message);

signals:
    void newFenceRequested();
    void fencesVisibilityToggled(bool visible);
    void peekToggled(bool active);
    void desktopIconsToggled(bool hidden);
    // F-038 / T-057: a material was picked for the whole desktop (see
    // setFenceMaterial -- the tray has no way to point at one fence).
    void fenceMaterialRequested(const QString& material);
    void fenceShortcutArrowsToggled(bool shown);
    void contextMenuEditRequested();
    // The emergency exit (T-046): "give me my icons back", as a command rather
    // than a toggle. The product answers it by asking the desktop directly.
    void restoreIconsRequested();
    void settingsRequested();
    void quitRequested();

public:
    // StarFish's mark, every size (Qt resource from tools/make_icon.py): the
    // tray, and every window through QApplication::setWindowIcon.
    static QIcon buildIcon();
    // The menu, for the probe that photographs it (it pops it in-process).
    QMenu* menu() const { return m_menu; }

private:
    // Session 59: the menu in Windows 11's style, in the theme of the moment
    // (re-read each time it opens), with glyphs on its entries.
    void restyle();

    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QAction* m_newFence = nullptr;
    QAction* m_showFences = nullptr;
    QAction* m_peek = nullptr;
    QAction* m_hideIcons = nullptr;
    QAction* m_restoreIcons = nullptr;
    QAction* m_fenceArrows = nullptr;
    // ADR-13: whether the two desktop-icon entries are offered at all. See
    // setIconHidingOffered -- off unless the caller asks, and no caller in the
    // product does.
    bool m_offerIconHiding = false;
    // T-088: whether "Restore my icons" alone is offered. It folds into the flag
    // above, and it can be true while that one is false -- the way back without
    // the developer toggle.
    bool m_offerRestoreIcons = false;
    QAction* m_settings = nullptr;
    QLabel* m_headerCount = nullptr;
    int m_fenceCount = 0;
    int m_styledDark = -1; // the theme the menu was last styled for
    QHash<QAction*, QString> m_glyphs;
    // F-038 / T-057: the "Fence material" submenu and one entry per preset, keyed
    // by FenceMaterial id so setFenceMaterial() cannot drift from the menu.
    QMenu* m_materialMenu = nullptr;
    QHash<QString, QAction*> m_materials;
};

} // namespace starfish::ui
