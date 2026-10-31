#pragma once

#include <QHash>
#include <QMap>
#include <QPoint>
#include <QSet>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "core/overlap_planner.hpp"
#include "core/context_menu.hpp"
#include "core/desktop_item.hpp"
#include "core/shortcut_vault.hpp"
#include "platform/win/desktop_menu.hpp"
#include "ui/fence_menu.hpp"
#include "ui/screen_layout.hpp"

class QLineEdit;

namespace starfish::core {
class DesktopWatcher;
class LayoutStore;
} // namespace starfish::core

namespace starfish::platform::win {
class HotkeyManager;
class ShellIconCache;
class DesktopShellView;
struct NativeDesktopIcon;
} // namespace starfish::platform::win

namespace starfish::ui {
class FenceContainer;
class SettingsDialog;
class TrayIcon;
} // namespace starfish::ui

namespace starfish::app {

// The product mode (T-038): the assembly that turns the pieces into an
// application somebody can launch and use.
//
// Every part of it already existed -- fences, hosting, the document, the
// watcher, the hot keys. What was missing was the product's own sequence: read
// the document *before* the first frame, put the fences on the desktop, follow
// the desktop folder, keep the tray in step with the real state, and write
// everything back on the way out. This class owns that sequence and nothing
// else; the pieces keep owning their own behaviour.
//
// Two properties are deliberate:
//  * **It never touches the files.** Files are shown, never moved (AGENTS.md
//    golden rule). The only thing it changes outside its own windows is the
//    visibility of the native desktop icons, and it puts that back.
//  * **The mode is testable without a desk.** `hostOnDesktop`, `tray` and
//    `hotkeys` can each be turned off, so the same sequence runs headless in
//    --product-probe.
class ProductApp : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QString layoutPath;        // empty = %APPDATA%/StarFish/layout.json
        bool hostOnDesktop = true; // put the fences on the real desktop
        bool tray = true;
        bool settingsWindow = true;   // create the settings window
        bool showSettingsOnStart = true; // ...and open it at launch
        bool hotkeys = true;
        bool watchDesktop = true;  // F-018 on the real Desktop folder
        // T-071: the desktop's own menu entry asked for a fence (`--new-fence`)
        // and nothing was running to answer it, so *this* launch is the answer.
        // Honoured even when the document already holds fences -- that is what the
        // entry is for.
        bool newFence = false;
        bool starterFence = true;  // materialise one fence on a first launch
        // T-056 used to be on here: StarFish took the desktop at launch by hiding
        // the native icon layer and drawing the desktop itself. ADR-13 took it
        // back, and the reason is the user's own: "the normal mode is having the
        // wallpaper displayed", with the icons they arranged. The desktop now
        // stays theirs -- nothing is hidden, nothing is replaced -- and what
        // StarFish owns is the *bare* desktop's right button, not the desktop.
        // Off by default; a probe or a future option can still ask for the old
        // behaviour (and then the tray offers the entries that undo it).
        bool claimDesktop = false;
        // Session 62: an existing "start with Windows" entry follows the exe
        // if it moved -- the real launch only, never a probe.
        bool refreshAutostart = false;

    };

    explicit ProductApp(QObject* parent = nullptr);
    ~ProductApp() override;

    ProductApp(const ProductApp&) = delete;
    ProductApp& operator=(const ProductApp&) = delete;

    // Builds everything and starts it. Returns false with a message when the
    // desktop is unreachable, in which case nothing is left half-built.
    bool start(const Options& options, QString* error = nullptr);

    // Writes the document, restores the native icons and tears the rest down.
    // Idempotent, and called by the destructor.
    void shutdown();

    ui::FenceContainer* container() const { return m_container; }
    core::LayoutStore* store() const { return m_store; }
    ui::SettingsDialog* settingsDialog() const { return m_settings; }
    ui::TrayIcon* tray() const { return m_tray; }
    core::DesktopWatcher* watcher() const { return m_watcher; }
    platform::win::HotkeyManager* hotkeys() const { return m_hotkeys; }

    bool desktopIconsHidden() const;
    void setDesktopIconsHidden(bool hidden);

    // ADR-21: is StarFish drawing the desktop right now, and through which view.
    bool drawnDesktop() const { return m_shellView != nullptr; }
    platform::win::DesktopShellView* shellView() const { return m_shellView.get(); }
    // The backdrop's window, which owns the shell menus (0 when not hosted).
    quintptr backdropOwner() const;
    // T-090, for the probe: how many times the desktop window was lost, and how
    // many times the shell showed its list again behind our back.
    int desktopLosses() const { return m_desktopLosses; }
    int listReshownCount() const { return m_listReshown; }
    bool reclaimPending() const { return m_reclaimTimer.isActive(); }
    // The same path a monitor change takes, callable without one.
    void screensChangedForProbe() { refreshScreens(); }
    // Session 58 (the user's decision): the shortcut arrow on fence tiles is a
    // setting, HKCU\Software\StarFish\fenceShortcutArrows, on by default.
    static bool fenceShortcutArrowsSetting();
    void setFenceShortcutArrows(bool shown);
    bool fenceShortcutArrows() const { return m_fenceShortcutArrows; }
    // Session 60 (the user's feedback), HKCU\Software\StarFish as well:
    // - sortIconsOnAdd (off): a drop is sorted by name only if asked, otherwise
    //   it goes where it fell and the order stays the user's;
    // - desktopShortcutArrows (on, as Windows): the arrow on the drawn desktop's
    //   icons -- StarFish's drawing only, the shell's overlay is never touched;
    // - allowDesktopOverlap (off): an icon never stays under a fence -- it goes
    //   to the nearest free slot (the shell view keeps the place).
    static bool sortOnAddSetting();
    static bool desktopShortcutArrowsSetting();
    static bool allowDesktopOverlapSetting();
    void setSortOnAdd(bool sorted);
    // Session 62: "Démarrer avec Windows" -- HKCU Run\StarFish, read each time.
    void setStartWithWindows(bool on);
    void setDesktopShortcutArrows(bool shown);
    void setAllowDesktopOverlap(bool allowed);
    bool sortOnAdd() const { return m_sortOnAdd; }
    bool desktopShortcutArrows() const { return m_desktopShortcutArrows; }
    bool allowDesktopOverlap() const { return m_allowDesktopOverlap; }
    // Moves the free desktop icons out from under the fences now; returns how
    // many moved. Public for the probe (runs in this process, no input).
    int resolveDesktopOverlaps();
    // Session 60: a fence's name is edited on the title itself (double-click on
    // the name, or the menu's Rename). Return or leaving commits, Escape cancels.
    void startFenceTitleEdit(const QString& id);
    void finishFenceTitleEdit(bool commit);
    QLineEdit* fenceTitleEditor() const { return m_titleEdit; }
    QString fenceTitleEditId() const { return m_titleEdit ? m_titleEditId : QString(); }

    // T-092 (lot 4) -- the right-click menu drawn by StarFish, Windows 11's
    // look, filled from the desktop's real menus and configurable
    // (%LOCALAPPDATA%/StarFish/context-menu.json). Windows 10, the "modern": false
    // setting and Shift+right-click keep the classic native menu.
    static bool modernContextMenuWanted();
    // The composed menu, the shell's menu left open for runContextMenuEntry().
    std::vector<core::ContextMenuItem> composeContextMenu(core::ContextMenuKind kind,
                                                          const QStringList& paths,
                                                          const std::vector<core::ContextMenuItem>& ours,
                                                          const QStringList& hiddenShellEntries = {});
    // Runs what was picked: a shell entry (by the menu that offered it), a user
    // entry, or "Show more options" (the classic menu). Returns the ownId of a
    // StarFish entry to act on, -1 otherwise.
    int runContextMenuEntry(const core::ContextMenuItem& item, core::ContextMenuKind kind,
                            const QStringList& paths, const QPoint& point,
                            const std::vector<core::ContextMenuItem>& ours, bool* renameRequested);
    void editContextMenu();

    // ADR-21 lot 3 (T-091) -- the drawn desktop's gestures, handed to the shell.
    // Public so the probe drives them in this process (no input is injected).
    // F2: our own edit over the caption; the rename itself is IFileOperation.
    void startRename(const QString& path, const QString& fenceId = QString()); // session 74: or a fence tile
    void finishRename(bool commit);
    QLineEdit* renameEditor() const { return m_renameEdit; }
    QString renamePath() const { return m_renameEdit ? m_renamePath : QString(); }
    bool renameDesktopItem(const QString& path, const QString& newName, QString* newPath = nullptr);
    // A key the backdrop passed on (F2, Delete, Return, Ctrl+C/X/V/Z/Y, F5...).
    void onDesktopKey(int key, Qt::KeyboardModifiers modifiers);
    bool deleteDesktopItems(const QStringList& paths, bool permanent, bool silent = false);
    // Moves icons on the desktop by `delta` (the view keeps the new place).
    bool moveDesktopItems(const QStringList& paths, const QPoint& delta);
    // "New folder" where the shell puts it, its name open for editing.
    void newDesktopFolder();
    // The drop of our own icons back onto the desktop (the drop target's half).
    void onInternalDrop(const QPoint& point, const QStringList& paths, const QPoint& origin);
    // Session 61: a path as the drawn desktop spells it (C:/...) or as the
    // shell does (C:\...) -- both name the same icon.
    QPoint desktopItemPosition(const QString& path) const { return m_itemPositions.value(shellNameOf(path)); }
    QString shellNameOf(const QString& path) const;
    void syncShellViewForProbe() { m_shellSignature.clear(); syncShellView(); }
    bool desktopSnapsToGrid() const { return m_shellSnap; }
    bool desktopAutoArranges() const { return m_shellAutoArrange; }
    // A drop from elsewhere is about to happen at `point` (physical screen):
    // what appears next on the desktop is placed there.
    void onForeignDrop(const QPoint& point, const QStringList& stems = {});
    // The desktop's background menu at `point`, as a right-click opens it.
    void showDesktopMenu(const QPoint& point) { onDesktopMenu(QRect(), point); }

    // The emergency exit (F-034 / T-046): asks the desktop to show the native
    // icons right now, whatever our own bookkeeping believes. Deliberately not a
    // toggle -- the ordinary path only acts when the requested state differs from
    // the remembered one, which is exactly the situation a confused run cannot
    // rely on. Wired to the tray's "Restore my icons".
    void restoreDesktopIcons();

    // One line describing what is actually running: fences, rules, what is
    // watched, which hot keys Windows accepted. Shown by the settings window and
    // read back by --product-probe, so both report the same thing.
    QString statusText() const;

    // Creates a fence in the first free spot, with a default title, and returns
    // its id.
    QString createFenceWithDefaultPlacement();

    // F-038 / T-057 -- the materials, as the product applies them.
    //
    // Both are public because they are the *whole* effect of a click: the tray's
    // submenu applies one material to every fence, a fence's own menu (T-076) to
    // the one under the cursor, and a probe drives the same two calls. What they
    // do is a style change (the material is the alpha of the fence's colour, which
    // the document already stores), so the auto-save follows on its own and
    // nothing else has to know about materials at all.
    void applyMaterialToFence(const QString& id, const QString& material);
    void applyMaterialToAllFences(const QString& material);

    // The material every fence shares, or an empty string when they disagree --
    // what the tray's check mark is allowed to claim.
    QString sharedFenceMaterial() const;
    // T-098 (session 63): the default transparency (percent, HKCU), and a
    // fence's own one through its dialog.
    static int defaultTransparencySetting();
    // Session 64: fences may overlap (default on, as before).
    static bool allowFenceOverlapSetting();
    void setAllowFenceOverlap(bool allowed);
    void setDefaultFenceTransparency(int percent);
    void editFenceTransparency(const QString& id);

    // The default title of the next fence ("Fence 3"), the single naming policy
    // shared by the tray, a fence's own menu and a fence drawn on the desktop
    // (T-041).
    QString nextFenceTitle() const;

    // The monitors, as the fence geometry needs them (T-014 / F-032): physical
    // rects in virtual-screen coordinates, each with its device pixel ratio.
    // Public because it is *the* definition of "where the screens are" for this
    // process -- a probe that built its own could measure a different desktop
    // than the product works on.
    static ui::ScreenLayout screenLayout();

signals:
    void quitRequested();

private:
    void applyDocument();
    // The safety net (T-046): if a previous run hid the native icon layer and
    // never came back, the marker it left is the only surviving evidence. Any
    // launch -- whichever mode -- repairs it before anything else is built.
    void repairIconHidingFromPreviousRun();
    // Wires every document-level signal to `onChange`, so auto-save and the
    // settings list cannot drift apart.
    void connectLifecycle(const std::function<void()>& onChange);
    void onLifecycleChanged();

    void wirePersistence();
    void wireTray();
    void wireSettings();
    void wireWatcher();
    void wireHotkeys();
    // T-041: the empty desktop's own gestures, carried by the backdrop. Wired
    // once at startup like the tray's, because they are the same kind of thing --
    // a request from a surface, answered by the product.
    void wireBackdrop();
    // T-076: the fence's own context menu. Wired like the tray's entries -- the
    // container reports which fence and where, the product decides what each entry
    // does.
    void wireFenceMenu();

    void syncWatchedFolders();
    void refreshStatus();
    void showSettings();

    // T-039: reads the real desktop folder and hands it to the container, which
    // is what the backdrop subtracts the fences' own icons from. Called at
    // startup and whenever a desktop folder changes -- the backdrop is a view of
    // the desktop, so it has to follow it exactly like a portal follows its
    // folder.
    void refreshDesktopItems();

    // ADR-13: reads the native desktop icon list -- positions, slots and names --
    // and hands it to the backdrop, which is what lets our surfaces step the mouse
    // aside over the user's icons, and lets the layout draw a fence-less icon where
    // the user put it when the icons end up being ours. **Nothing is moved.**
    //
    // Idempotent and cheap (a handful of SendMessage per icon), so it runs on a
    // timer as well: the user may drag one of their icons, and the click zones have
    // to follow. It reads only while the shell is drawing its icons -- a hidden
    // list view answers empty names and positions no desktop has (measured).
    void syncNativeIcons();

    // ADR-21 -- the drawn desktop, the product's desktop since session 58. The
    // shell's own view (DesktopShellView) says what is on the desktop and where;
    // the native list is hidden behind the T-046 marker; the backdrop draws the
    // wallpaper and the icons and answers every click, with the shell's own menus
    // and verbs. claimDrawnDesktop() refuses -- and leaves the desktop native --
    // whenever one step of it cannot be taken.
    bool claimDrawnDesktop();
    void releaseDrawnDesktop();
    // Re-reads the view and pushes what changed to the backdrop. Cheap when
    // nothing moved (a signature comparison).
    void syncShellView();
    void refreshWallpaper();
    // ADR-21 lot 2 (T-090). The desktop window went away under us (Explorer
    // restarted): a new shell shows its own list, so the drawn state is dropped
    // at once -- nothing of ours is left claiming a desktop that is gone -- and
    // claimed again once the desktop has settled (tryReclaimDrawnDesktop, on a
    // timer, for a minute at most; after that the desktop stays native).
    void onDesktopLost();
    void tryReclaimDrawnDesktop();
    // The screens changed (a monitor, a resolution, a scaling): the backdrop
    // re-attached at the new size, so the wallpaper is captured again.
    void refreshDrawnDesktopForScreens();
    void onIconMenu(const QStringList& paths, const QPoint& point);
    // Session 65: the menu of tiles in a fence -- the desktop's (Windows 11
    // style, real shell commands) plus "Put back on the desktop".
    void onFenceItemMenu(const QString& fenceId, const QStringList& paths, const QPoint& point);
    void openDesktopItem(const QString& path);

    // The fallback ADR-13 needs and does not want: an icon a fence holds has to
    // stop being drawn by the desktop, and moving it is how that is done. When the
    // desktop refuses to be moved one icon at a time (auto-arrange on), the whole
    // list is taken over instead -- hidden, and drawn here at the positions just
    // read, so the desktop still looks like the user's own.
    //
    // T-066: this is now **only** the claimDesktop path (`--claim-desktop`, the
    // development mode and the probes). In the product it never runs, because the
    // icons we would draw in the user's place are not yet usable (T-042) -- a
    // desktop whose icons answer nothing is broken, while a duplicated icon is
    // merely annoying. Called and given back by syncNativeIcons.
    void takeOverDesktopIcons();
    void giveBackDesktopIcons();

    // T-039: turns the backdrop on, and reports whether it really took. Hiding
    // the native icon layer on the strength of a backdrop that could not be
    // hosted would leave the user with no icons at all -- the same "refuse the
    // risky step when the safe one cannot be done" discipline as the crash
    // marker (T-046).
    bool enableBackdrop();

    // Called when the set of screens changes: the fences are revalidated onto
    // the screens that are actually there, so unplugging a monitor brings its
    // fences back instead of stranding them off-screen.
    void refreshScreens();

    // True while the icons are ours to draw (see takeOverDesktopIcons).
    bool m_ownsIcons = false;
    std::unique_ptr<platform::win::DesktopShellView> m_shellView;
    QTimer m_shellTimer;   // the view's periodic re-read (positions, renames, View > size)
    QTimer m_shellResync;  // a re-read soon after something we know changed the desktop
    QString m_shellSignature;
    quint64 m_wallpaperSignature = 0;
    int m_shellTicks = 0;
    int m_recycleBinState = -2; // session 66: -2 not read yet
    QHash<QString, core::DesktopItem> m_itemCache;
    QTimer m_reclaimTimer; // T-090: claims the desktop again after Explorer came back
    int m_reclaimTries = 0;
    int m_reclaimSettled = 0;
    int m_desktopLosses = 0;
    int m_listReshown = 0;
    bool m_fenceShortcutArrows = true;
    bool m_sortOnAdd = false;
    // Session 69: double-click on the empty desktop hides fences and icons
    // (setting, off by default), and whether they are hidden that way now.
    bool m_doubleClickHides = false;
    bool m_desktopHiddenByUser = false;
    void setDesktopHiddenByUser(bool hidden);
    bool m_desktopShortcutArrows = true;
    bool m_allowDesktopOverlap = false;
    // Session 75: copies of the fenced shortcuts, to put one back.
    std::unique_ptr<core::ShortcutVault> m_vault;
    QTimer m_vaultTimer;
    bool m_autoRestoreShortcuts = false; // Settings, off by default
    bool m_autoRestoreDone = false;      // only at startup
    void syncShortcutVault();
    QTimer m_overlapTimer; // after a fence moved, grew or came back: the sweep, once
    void scheduleOverlapSweep();
    QSize m_shellSpacing;
    int m_shellIconSize = 0;
    bool m_shellIconsShown = false;
    QLineEdit* m_titleEdit = nullptr;
    QString m_titleEditId;
    // T-092: the classic entries' keys seen so far, for context-menu-keys.txt.
    QMap<QString, QString> m_seenMenuKeys;
    int showModernContextMenu(core::ContextMenuKind kind, const QStringList& paths, const QPoint& point,
                              const std::vector<core::ContextMenuItem>& ours, bool* renameRequested,
                              const QStringList& hiddenShellEntries = {});
    void writeSeenMenuKeys();
    void loadSeenMenuKeys();
    void dragDesktopItems(const QStringList& paths, const QPoint& origin);
    // A new item the shell is about to create (New > Folder...) opens for
    // renaming when it appears, as on the real desktop.
    void armNewItemRename();
    QLineEdit* m_renameEdit = nullptr;
    QString m_renamePath;
    QString m_renameFenceId; // session 74: the fence of the tile being renamed
    QHash<QString, QPoint> m_itemPositions;   // by the shell's parsing name
    QHash<QString, QString> m_shellNames;     // BackdropLayout::pathKey -> parsing name
    bool m_shellSnap = false;
    bool m_shellAutoArrange = false;
    QList<QRect> desktopWorkAreas() const;
    core::OverlapSlot desktopSlot() const;
    qint64 m_renameNewUntil = 0;
    QSet<QString> m_namesBeforeNew;
    // T-091 follow-up: a drop from elsewhere, placed under the cursor.
    QPoint m_foreignDropAt;
    qint64 m_foreignDropUntil = 0;
    QSet<QString> m_namesBeforeDrop;
    QStringList m_dropStems;

    void onNewFence();
    // F-001 / T-055: the user right-clicked or right-dragged the bare desktop.
    // `rect` is the fence the drag described, or a null rect for a plain
    // right-click; `point` is where the button came up. Opens the desktop's own
    // context menu with a StarFish entry on top, and creates the fence only if
    // that entry was chosen.
    void onDesktopMenu(const QRect& rect, const QPoint& point);
    void onRenameFence(const QString& id, const QString& title);
    void onRemoveFence(const QString& id);
    void onRollUpToggled(const QString& id, bool rolledUp);
    void onDesktopIconsToggled(bool hidden);

    // T-076: the fence's **own** menu (ui::FenceMenu says what is in it). Shown at
    // a screen point and applied through the same calls the tray and the settings
    // window use, so a menu choice cannot reach the document by a third route.
    //
    // Blocker by design: the menu runs its own event loop, exactly like the
    // Shell's menu the tiles already show (T-006) -- a right-click is the only
    // thing that needs it.
    void showFenceMenu(const QString& id, const QPoint& globalPos);
    // `value` is the entry's own value, and only Action::Radius uses it (T-080):
    // a radius has no id to switch on, so it travels as the number the document
    // stores.
    void runFenceMenuCommand(const QString& id, ui::FenceMenu::Action action, int value = -1);

    Options m_options;
    bool m_started = false;
    bool m_shutDown = false;
    // Whether *this* application hid the native icons. Restoring is only ever
    // undoing our own doing: a user who hid their desktop icons themselves must
    // not have them shown back on the way out.
    bool m_iconsHidden = false;

    core::LayoutStore* m_store = nullptr;
    ui::FenceContainer* m_container = nullptr;
    core::DesktopWatcher* m_watcher = nullptr;
    platform::win::HotkeyManager* m_hotkeys = nullptr;
    // T-071: the desktop's own menu entry asks a *running* product for a fence
    // through a named event -- the entry can be clicked while we are already here,
    // and a second process would be refused by the single-instance guard (T-044).
    platform::win::NewFenceWatcher* m_newFence = nullptr;

    ui::TrayIcon* m_tray = nullptr;
    ui::SettingsDialog* m_settings = nullptr;
    // Not a QObject: owned here, and it must outlive every fence that asks it
    // for an icon.
    std::unique_ptr<platform::win::ShellIconCache> m_icons;

    // The folders currently watched, so a portal appearing or leaving only
    // re-arms the watcher when the set really changed.
    QStringList m_watchedFolders;
    QStringList m_portalFolders;
    // The desktop folders themselves, kept apart from the portal folders so a
    // change notification can be routed to the right consumer (T-039).
    QStringList m_desktopFolders;
};

} // namespace starfish::app
