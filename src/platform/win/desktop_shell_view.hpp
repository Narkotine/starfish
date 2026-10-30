#pragma once

#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

namespace starfish::platform::win {

// One item of the desktop as the shell's own view holds it (ADR-21).
struct ShellViewItem {
    QString parsingName; // SHGDN_FORPARSING: a path for a file, "::{GUID}" for This PC & co.
    QString displayName; // SHGDN_NORMAL: exactly what the desktop writes under the icon
    QPoint position;     // top-left of the item, physical *screen* pixels
};

// What the desktop's view says about itself, read in one go.
struct ShellViewState {
    bool ok = false;
    QVector<ShellViewItem> items;
    QSize spacing;          // the grid step the view uses (IFolderView::GetSpacing)
    int iconSize = 48;      // the icon size the user chose (View > Large/Medium/Small)
    bool iconsShown = true; // "Show desktop icons" (FWF_NOICONS cleared)
    bool autoArrange = false;
    bool snapToGrid = false;
};

// ADR-21 -- the desktop's **own** view, reached the documented way and driven
// rather than imitated.
//
// The desktop is a shell view (SHELLDLL_DefView) living in explorer.exe. Since
// ADR-21 StarFish draws the desktop, but every *decision* about it stays with
// that view: which items exist (This PC, the Recycle Bin and the Public desktop
// included), where each one sits, the icon size, the background menu (View,
// Sort by, Refresh, Paste, Undo, New, the extensions...) and the item menus.
// It is reached exactly as Raymond Chen documents it -- IShellWindows ->
// FindWindowSW(SWC_DESKTOP) -> SID_STopLevelBrowser -> QueryActiveShellView --
// and every call crosses into explorer through the system's COM proxies
// (IShellView, IFolderView2, IContextMenu{,2,3} all have registered proxies;
// verified on this machine). No LVM_* message, no pointer handed to another
// process (T-071).
//
// Measured 2026-10-30 (experiments/spike_folderview.cpp): 87 items with their
// positions, spacing 95x98, icon size 48, the background menu with its 14
// entries, and its "New", "View" and "Sort by" submenus filled through
// IContextMenu3::HandleMenuMsg2(WM_INITMENUPOPUP).
class DesktopShellView
{
public:
    DesktopShellView();
    ~DesktopShellView();

    DesktopShellView(const DesktopShellView&) = delete;
    DesktopShellView& operator=(const DesktopShellView&) = delete;

    // Reads the items, their positions and the view settings. `ok` is false when
    // the desktop view cannot be reached (Explorer restarting): the caller keeps
    // what it had.
    ShellViewState read();

    // Makes the native view's selection the given items (and nothing else), so
    // the menus and verbs that act on "the selection" act on ours.
    bool select(const QStringList& parsingNames);

    // An entry StarFish adds on top of a shell menu. `id` is the caller's own,
    // returned when the entry is chosen.
    struct MenuEntry {
        int id = 0;
        QString label;
        bool separatorAfter = false;
        bool checked = false;
        std::vector<MenuEntry> children; // non-empty: a submenu, `id` unused
    };

    // The desktop's background menu, the real one (SVGIO_BACKGROUND), with our
    // entries on top and the shell verbs named in `hiddenVerbs` removed. Blocks
    // until the popup closes and invokes the shell verb chosen (at `screen`, so a
    // "New folder" lands where the user clicked). Returns the id of one of our
    // entries, or -1 (a shell verb ran, or nothing was chosen).
    int showBackgroundMenu(const QPoint& screen, quintptr owner, const QList<MenuEntry>& extra,
                           const QStringList& hiddenVerbs = {});

    // The menu of the selection (SVGIO_SELECTION) after select(parsingNames).
    // Same contract. `renameRequested` is set when the user chose the shell's
    // own "Rename": the native view is hidden, so the edit is the caller's.
    int showSelectionMenu(const QStringList& parsingNames, const QPoint& screen, quintptr owner,
                          const QList<MenuEntry>& extra, bool* renameRequested = nullptr);

    // Runs a verb of the selection's / the background's menu without showing it
    // ("open", "delete", "copy", "cut", "paste", "undo", "refresh", ...). The
    // default verb (what a double-click does) is `QString()`.
    bool invokeSelectionVerb(const QStringList& parsingNames, const QString& verb);
    bool invokeBackgroundVerb(const QString& verb);
    // The shell verb the last menu shown ran ("NewFolder", "paste", ...), or
    // empty when one of our entries -- or nothing -- was chosen.
    QString lastChosenVerb() const;
    // Ctrl+Z / Ctrl+Y: the shell's own "Undo ..." / "Redo ..." entry of the
    // background menu, when it offers one.
    bool invokeBackgroundUndo(bool redo);

    // Moves an item in the native view (IFolderView::SelectAndPositionItems) --
    // the view applies its own grid and keeps the position, as if the user had
    // dragged the icon. `screen` is the item's new top-left.
    bool positionItem(const QString& parsingName, const QPoint& screen);

    // The "View" settings of the desktop, written to its own view -- the very
    // calls Explorer's View submenu makes, so the choice survives StarFish. For
    // the configurable menu (lot 4) and the keyboard; the shell's own "View"
    // submenu is in the background menu and needs none of this.
    enum class ViewFlag { AutoArrange, SnapToGrid, ShowIcons };
    bool setIconSize(int px);
    bool setViewFlag(ViewFlag flag, bool on);

    // T-092 -- the same menus read as data, for the menu StarFish draws itself.
    // The shell's IContextMenu stays alive until closeMenuModel() (or the next
    // open), so an entry chosen later is run by the very object that offered it.
    // Submenus are filled the way a popup fills them (WM_INITMENUPOPUP through
    // IContextMenu3, plain values only -- T-071).
    struct ShellMenuItem {
        QString verb;     // canonical verb ("open", "properties"...), may be empty
        QString label;    // the text, accelerator '&' removed
        QString shortcut; // what followed a tab ("Ctrl+Z"), may be empty
        bool separator = false;
        bool enabled = true;
        bool checked = false;
        bool radio = false;
        bool isDefault = false;
        int command = -1; // offset to invoke, -1 for a separator or a submenu
        std::vector<ShellMenuItem> children;
    };
    std::vector<ShellMenuItem> openBackgroundMenuModel(const QStringList& hiddenVerbs = {});
    std::vector<ShellMenuItem> openSelectionMenuModel(const QStringList& parsingNames,
                                                      bool extended = false);
    // Runs an entry of the open model (at `screen`, owned by `owner`); remembers
    // its verb for lastChosenVerb(). False when no model is open or it failed.
    bool invokeMenuCommand(int command, const QPoint& screen, quintptr owner);
    void closeMenuModel();

    // Diagnostics for the probes: a menu's labels, built and destroyed without a
    // popup. Submenu entries follow their parent as "Parent > Entry", filled the
    // way a popup fills them (WM_INITMENUPOPUP through IContextMenu3).
    QStringList backgroundMenuLabels();
    QStringList selectionMenuLabels(const QStringList& parsingNames);

    // The desktop as it is on screen, our surfaces included:
    // PrintWindow(PW_RENDERFULLCONTENT) on the window that hosts them (Progman).
    // The probes' window on a desktop the user may be covering with his own.
    static QImage captureDesktop(const QRect& area);

    // The wallpaper as the desktop paints it, for the drawn desktop's frame:
    // PrintWindow(PW_RENDERFULLCONTENT) on SHELLDLL_DefView, which keeps
    // rendering the wallpaper while its list view is hidden. `area` is in
    // virtual-screen pixels; the result has its size. Null on failure.
    static QImage captureWallpaper(const QRect& area);

private:
    struct Impl;
    Impl* d;
};

} // namespace starfish::platform::win
