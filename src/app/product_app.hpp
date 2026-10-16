#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "platform/win/desktop_menu.hpp"
#include "ui/screen_layout.hpp"

namespace starfish::core {
class DesktopWatcher;
class LayoutStore;
} // namespace starfish::core

namespace starfish::platform::win {
class HotkeyManager;
class ShellIconCache;
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

    // The emergency exit (F-034 / T-046): asks the desktop to show the native
    // icons right now, whatever our own bookkeeping believes. Deliberately not a
    // toggle -- the ordinary path only acts when the requested state differs from
    // the remembered one, which is exactly the situation a confused run cannot
    // rely on. Wired to the tray's "Restore my icons".
    void restoreDesktopIcons();

    // One line describing what is actually running: fences, rules, what is
    // watched, which hot keys Windows accepted. Used by the settings window and
    // by --product-probe, so both report the same thing.
    QString statusText() const;

    // Creates a fence in the first free spot, with a default title, and returns
    // its id.
    QString createFenceWithDefaultPlacement();

    // F-038 / T-057 -- the materials, as the product applies them.
    //
    // Both are public because they are the *whole* effect of a click: the tray's
    // submenu applies one material to every fence, the settings window's combo to
    // the selected one, and a probe drives the same two calls. What they do is a
    // style change (the material is the alpha of the fence's colour, which the
    // document already stores), so the auto-save follows on its own and nothing
    // else has to know about materials at all.
    void applyMaterialToFence(const QString& id, const QString& material);
    void applyMaterialToAllFences(const QString& material);

    // The material every fence shares, or an empty string when they disagree --
    // what the tray's check mark is allowed to claim.
    QString sharedFenceMaterial() const;

    // The default title of the next fence ("Fence 3"), the single naming policy
    // shared by the tray, the settings window and a fence drawn on the desktop
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
