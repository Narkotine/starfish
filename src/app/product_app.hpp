#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace starfish::core {
class DesktopWatcher;
class LayoutStore;
} // namespace starfish::core

namespace starfish::platform::win {
class HotkeyManager;
class ShellIconCache;
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
        bool starterFence = true;  // materialise one fence on a first launch
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

    // One line describing what is actually running: fences, rules, what is
    // watched, which hot keys Windows accepted. Used by the settings window and
    // by --product-probe, so both report the same thing.
    QString statusText() const;

    // Creates a fence in the first free spot, with a default title, and returns
    // its id.
    QString createFenceWithDefaultPlacement();

signals:
    void quitRequested();

private:
    void applyDocument();
    // Wires every document-level signal to `onChange`, so auto-save and the
    // settings list cannot drift apart.
    void connectLifecycle(const std::function<void()>& onChange);
    void onLifecycleChanged();

    void wirePersistence();
    void wireTray();
    void wireSettings();
    void wireWatcher();
    void wireHotkeys();

    void syncWatchedFolders();
    void refreshStatus();
    void showSettings();

    void onNewFence();
    void onRenameFence(const QString& id, const QString& title);
    void onRemoveFence(const QString& id);
    void onRollUpToggled(const QString& id, bool rolledUp);
    void onDesktopIconsToggled(bool hidden);

    static QSize desktopBounds();

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
    ui::TrayIcon* m_tray = nullptr;
    ui::SettingsDialog* m_settings = nullptr;
    // Not a QObject: owned here, and it must outlive every fence that asks it
    // for an icon.
    std::unique_ptr<platform::win::ShellIconCache> m_icons;

    // The folders currently watched, so a portal appearing or leaving only
    // re-arms the watcher when the set really changed.
    QStringList m_watchedFolders;
    QStringList m_portalFolders;
};

} // namespace starfish::app
