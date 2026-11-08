#include "app/product_app.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QGuiApplication>
#include <QDateTime>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <windows.h>

#include "core/context_menu.hpp"
#include "platform/win/autostart.hpp"
#include "core/desktop_item.hpp"
#include "core/desktop_watcher.hpp"
#include "core/icon_hiding_marker.hpp"
#include "core/layout_store.hpp"
#include "core/overlap_planner.hpp"
#include "platform/win/desktop_icons.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/desktop_shell_view.hpp"
#include "platform/win/desktop_surface.hpp"
#include "platform/win/hotkey_manager.hpp"
#include "platform/win/packaged_commands.hpp"
#include "platform/win/shell_context_menu.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "platform/win/shell_operations.hpp"
#include "ui/context_menu_editor.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/fence_material.hpp"
#include "ui/fence_transparency_dialog.hpp"
#include "ui/fence_appearance_dialog.hpp"
#include "ui/fence_templates.hpp"
#include "ui/recent_colours.hpp"
#include "ui/fence_view.hpp"
#include "ui/percent_slider.hpp"
#include "ui/fence_menu.hpp"
#include "ui/icon_grid.hpp"
#include "ui/modern_menu.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/splash_screen.hpp"
#include "ui/fence_widgets.hpp"
#include "ui/system_monitor.hpp"
#include "ui/widget_settings_dialog.hpp"
#include "ui/note_editor.hpp"
#include <QActionGroup>
#include "platform/win/startup_timeline.hpp"
#include "platform/win/windows_theme.hpp"
#include "ui/tray_icon.hpp"

QLoggingCategory lc("starfish.product");

namespace starfish::app {

namespace {
QPoint logicalFromPhysical(const QPoint& physical); // defined below

// F2's edit: Escape cancels, Return and leaving it commit (as Explorer's own).
class RenameEdit : public QLineEdit
{
public:
    std::function<void()> onCancel;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            if (onCancel)
                onCancel();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};


// F-021 asks for Win+Space, but Windows reserves that for the language switcher
// and refuses to register it (measured, session 15). Win+Alt+Space is close
// enough to remember and is actually accepted.

// F-022's documented trigger is a double click on the *empty* desktop, which
// belongs to SysListView32 and therefore needs a global mouse hook (T-028).
// Until then, the state is reachable from the tray and from this key.
constexpr int kHideHotkeyId = 2;
constexpr Qt::KeyboardModifiers kHideModifiers = Qt::MetaModifier | Qt::ShiftModifier;
constexpr Qt::Key kHideKey = Qt::Key_H;

constexpr int kDefaultFenceWidth = 320;
constexpr int kDefaultFenceHeight = 240;

// How much of the screen a fence created from the tray keeps clear of the
// screen's own edges -- and, on a normal desktop, of the first row of the user's
// icons (T-066). The search grid adds its own gap between candidates.
constexpr int kPlacementMargin = 80;

} // namespace

ProductApp::ProductApp(QObject* parent)
    : QObject(parent)
{
}

ProductApp::~ProductApp()
{
    shutdown();
}

ui::ScreenLayout ProductApp::screenLayout()
{
    ui::ScreenLayout layout;
    QScreen* primary = QGuiApplication::primaryScreen();
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (!screen)
            continue;
        ui::ScreenInfo info;
        // Qt hands over *logical* (device-independent) pixels; every coordinate
        // in StarFish is physical, because that is Progman's client space and
        // what the Win32 window calls take (T-014). Qt's logical origin is the
        // primary monitor's top-left, so scaling by the ratio keeps the primary
        // at (0,0) -- exactly Windows' virtual-screen convention.
        const qreal dpr = screen->devicePixelRatio();
        const QRect logical = screen->geometry();
        info.geometry = QRect(qRound(logical.x() * dpr), qRound(logical.y() * dpr),
                              qRound(logical.width() * dpr), qRound(logical.height() * dpr));
        info.devicePixelRatio = dpr;
        info.name = screen->name();
        info.primary = (screen == primary);
        layout.addScreen(info);
    }
    // Screens can also be reported while a monitor is being switched off; an
    // empty list means "no information", so the last known layout is kept
    // (FenceContainer::setScreens refuses it) rather than stranding everything.
    return layout;
}

void ProductApp::refreshScreens()
{
    if (!m_container)
        return;
    const ui::ScreenLayout layout = screenLayout();
    if (layout.isEmpty()) {
        qCWarning(lc) << "no screen reported: keeping the last known arrangement";
        return;
    }
    qCInfo(lc).noquote() << QStringLiteral("screens changed: %1 screen(s), virtual desktop %2,%3 "
                                           "%4x%5")
                                .arg(layout.count())
                                .arg(layout.bounds().x())
                                .arg(layout.bounds().y())
                                .arg(layout.bounds().width())
                                .arg(layout.bounds().height());
    // setScreens() revalidates every fence, so a monitor that just disappeared
    // brings its fences back on a screen that exists -- and only the fences that
    // really moved emit fenceRectChanged(), so the document is written only then.
    m_container->setScreens(layout);
    refreshDrawnDesktopForScreens();
}

bool ProductApp::start(const Options& options, QString* error)
{
    ui::SystemMonitor::install(); // session 88: system widgets read it (idle until one paints)
    if (m_started)
        return true;

    m_options = options;
    if (m_options.splash) {
        platform::win::StartupTimeline::enable();
        platform::win::StartupTimeline::mark("ProductApp::start");
    }
    if (m_options.splash
        && QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
               .value(QStringLiteral("showSplash"), true).toBool()) {
        // Session 78: FIRST, before anything of the desktop changes (the icon
        // repair below, the fences, the icon list hidden, the drawn desktop):
        // every monitor where the desktop shows is covered, and the cover is
        // on screen (painted, composited) before the next line runs.
        m_splash = new ui::SplashScreen(this);
        if (!m_splash->showAll()) // nothing to cover: the desktop is not in sight
            delete m_splash.data();
    }
    if (m_options.refreshAutostart
        && platform::win::Autostart().refresh(QCoreApplication::applicationFilePath()))
        qCInfo(lc) << "the start-with-Windows entry now points to" << QCoreApplication::applicationFilePath();

    // The safety net, first (T-046). If a previous run hid the native icon layer
    // and never came back -- crash, kill, power cut -- the desktop has been
    // without icons since, and the marker that run left is the only surviving
    // evidence of why. Repair it before anything else exists, and regardless of
    // the mode: the icons being gone is a fact about the machine, not about this
    // document or this options set.
    repairIconHidingFromPreviousRun();

    if (m_options.hostOnDesktop && !platform::win::DesktopSurface::isDesktopAvailable()) {
        if (error)
            *error = tr("the Windows desktop is not reachable (is Explorer running?)");
        return false;
    }

    // The fences live on the desktop, so the tray icon is the window the user
    // closes: without this, opening and closing the settings window would end
    // the application.
    QApplication::setQuitOnLastWindowClosed(false);

    // --- the document ------------------------------------------------------
    m_store = new core::LayoutStore(this);
    m_store->setAppVersion(QCoreApplication::applicationVersion()); // session 85: "writtenBy", backups on change
    if (!m_options.layoutPath.isEmpty())
        m_store->setFilePath(m_options.layoutPath);
    // Session 75: next to the document (a probe's own document, its own copies).
    m_vault = std::make_unique<core::ShortcutVault>(QFileInfo(m_store->filePath()).absolutePath()
                                                    + QStringLiteral("/shortcuts"));

    // --- the fences --------------------------------------------------------
    m_container = new ui::FenceContainer(this);
    // The real monitors, read before the first frame: a restored fence must come
    // back on the screen it belongs to, and a fence must never be clamped onto
    // the primary monitor just because that used to be the only one (T-014).
    m_container->setScreens(screenLayout());
    m_container->setHostOnDesktop(m_options.hostOnDesktop);
    m_container->setSnapThreshold(8);
    m_container->setMinimumFenceSize(QSize(160, 120));

    // The very icons the Shell draws, so a hosted fence looks like the desktop
    // rather than like a mock-up.
    m_icons = std::make_unique<platform::win::ShellIconCache>();
    platform::win::ShellIconCache* icons = m_icons.get();
    // The shortcut arrow on a fence's tiles is the user's setting (session 58,
    // default on: the tile then looks like the desktop icon it was); the
    // drawn desktop has its own setting (session 60).
    m_fenceShortcutArrows = fenceShortcutArrowsSetting();
    m_container->setIconProvider([this, icons](const core::DesktopItem& item, int sizePx) {
        return icons->iconFor(item.path, sizePx, m_fenceShortcutArrows);
    });
    m_desktopShortcutArrows = desktopShortcutArrowsSetting();
    m_container->setBackdropIconProvider([this, icons](const core::DesktopItem& item, int sizePx) {
        return icons->iconFor(item.path, sizePx, m_desktopShortcutArrows);
    });
    m_sortOnAdd = sortOnAddSetting();
    m_container->setSortOnAdd(m_sortOnAdd);
    m_container->setDefaultFenceAlpha(ui::PercentSlider::alphaFromPercent(defaultTransparencySetting()));
    m_container->setAllowFenceOverlap(allowFenceOverlapSetting());
    {
        const QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat);
        m_container->setHoverHighlight(settings.value(QStringLiteral("fenceHoverHighlight"), true).toBool());
        m_container->setHoverUnrollDefault(settings.value(QStringLiteral("fenceHoverUnroll"), false).toBool());
        m_container->setRollOppositeDefault(settings.value(QStringLiteral("fenceRollOpposite"), false).toBool());
        m_container->setAnimateRoll(settings.value(QStringLiteral("fenceRollAnimated"), true).toBool());
        m_container->setRollSpeed(settings.value(QStringLiteral("fenceRollSpeed"),
                                                 ui::FenceContainer::kRollSpeedDefault).toInt()); // session 95
        m_container->setSnapToCellsDefault(settings.value(QStringLiteral("fenceSnapToCells"), false).toBool());
        m_autoRestoreShortcuts = settings.value(QStringLiteral("fenceAutoRestoreShortcuts"), false).toBool();
        m_desktopDragGrid = settings.value(QStringLiteral("desktopDragGrid"), true).toBool(); // session 80
        m_container->setShowLabelsDefault(settings.value(QStringLiteral("fenceShowLabels"), true).toBool());
        m_container->setIconFlowDefault(
            ui::IconLayout::flowFromKey(settings.value(QStringLiteral("fenceIconFlow")).toString()));
        m_container->setNewFenceStyle(ui::FenceTemplates::newFenceStyle());
        m_container->setDefaultBorder(settings.value(QStringLiteral("fenceBorderWidth"), 1).toInt(),
                                      settings.value(QStringLiteral("fenceBorderColor"), QStringLiteral("#FF3B82F6")).toString());
    }
    m_allowDesktopOverlap = allowDesktopOverlapSetting();
    // The Shell resolves a restored path exactly as it resolves a dropped one, so
    // a tile means the same thing wherever it came from (ADR-7).
    m_container->setItemResolver([](const QString& path) {
        return platform::win::DesktopScanner::itemForPath(path);
    });

    // Read the document before the first frame: what the user sees must be what
    // the document says, never an empty desktop that fills in a moment later.
    // Wiring comes after, so loading never looks like a change and never
    // schedules a write.
    applyDocument();

    wirePersistence();

    // A first launch materialised a starter fence that the file has never seen;
    // this is what puts it there. It is a no-op when the document already held
    // the fences.
    m_store->setLayout(m_container->toLayout());
    m_store->scheduleSave();

    wireTray();
    wireSettings();
    wireWatcher();
    wireHotkeys();
    wireBackdrop();
    wireFenceMenu();

    // ⚠️ T-071 / ADR-14 -- and the product puts **no window of ours on the desktop
    // at all**. Four user reports in a row came out of the one we used to host
    // there: it took the mouse from the fences (T-068), it took it from the
    // desktop's icons (T-069, patched with alpha holes), it went on refusing the
    // *drop* half of every icon drag -- Windows looks for a drop target under the
    // cursor, finds our window, and we register none on purpose (T-052), so an
    // icon could be picked up and never put down -- and, on the bare desktop, our
    // window held the left button (the shell's selection rectangle disappeared)
    // and the right one (which replaced the desktop's own menu with one we
    // rebuild, T-055).
    //
    // The user's decision: keep the desktop **normal** and move the gesture
    // somewhere that costs nothing. So nothing of ours is hosted, nothing is read
    // from the shell on a timer, nothing steps the mouse aside: the wallpaper, the
    // icons, their drags, the shell's menu and its rubber band are the shell's, by
    // construction, and creating a fence is an entry in the desktop's **own**
    // menu, which Explorer builds and we only answer.
    //
    // ⚠️ ADR-21 (session 58) reverses ADR-20: the user's first need is that an
    // icon filed into a fence **leaves the desktop** without its file moving, and
    // that every icon is back if StarFish stops. Only a desktop StarFish draws can
    // do both without injecting anything into Explorer -- so the drawn desktop is
    // the product again, this time driven by the shell's own view (positions,
    // items, menus, verbs) instead of an imitation of it. `--native-desktop` keeps
    // the ADR-20 desktop.
    if (m_options.claimDesktop && !claimDrawnDesktop())
        qCWarning(lc) << "the drawn desktop could not be set up: the desktop stays native";

    // The screen arrangement changes while StarFish is running: a monitor is
    // unplugged, docked, re-arranged, or its scaling changes. Qt is the
    // authority on what exists; the fences are revalidated onto whatever is
    // really there (T-014 / F-032).
    // T-090: and a screen that stays can change too -- its resolution, its
    // place in the arrangement, its scaling.
    const auto watchScreen = [this](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, [this](const QRect&) { refreshScreens(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                [this](qreal) { refreshScreens(); });
    };
    for (QScreen* screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, watchScreen](QScreen* screen) {
        watchScreen(screen);
        refreshScreens();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) { refreshScreens(); });
    connect(qApp, &QGuiApplication::primaryScreenChanged, this,
            [this](QScreen*) { refreshScreens(); });

    // T-071: `--new-fence` with nothing running. The named event cannot carry the
    // request here -- the object dies with the CLI's handle, before this process
    // opens it -- so the request is the *option*: this launch owes a fence.
    if (m_options.newFence) {
        const QString created = createFenceWithDefaultPlacement();
        qCInfo(lc).noquote() << (created.isEmpty()
                                     ? QStringLiteral("the desktop menu asked for a fence and it "
                                                      "could not be created")
                                     : QStringLiteral("the desktop menu asked for a fence: %1")
                                           .arg(m_container->fenceTitle(created)));
    }

    // T-071: and the same entry can be clicked again at any time while we run.
    m_newFence = new platform::win::NewFenceWatcher(this);
    connect(m_newFence, &platform::win::NewFenceWatcher::requested, this, [this] {
        const QString id = createFenceWithDefaultPlacement();
        qCInfo(lc).noquote() << (id.isEmpty()
                                     ? QStringLiteral("the desktop menu asked for a fence, and it "
                                                      "could not be created")
                                     : QStringLiteral("the desktop menu asked for a fence: %1")
                                           .arg(m_container->fenceTitle(id)));
        refreshStatus();
    });
    m_newFence->start();

    refreshStatus();
    m_started = true;
    platform::win::StartupTimeline::mark("startup done (fences and drawn desktop set up)");
    if (m_splash) {
        QList<QWidget*> views;
        for (const QString& fid : m_container->fenceIds())
            views << m_container->view(fid);
        m_splash->finishWhenPainted(views);
    }

    if (m_options.settingsWindow && m_options.showSettingsOnStart)
        showSettings();

    return true;
}

void ProductApp::repairIconHidingFromPreviousRun()
{
    core::IconHidingMarker marker;
    if (!marker.isPresent())
        return;

    qCWarning(lc).noquote()
        << QStringLiteral("a previous run hid the desktop icons and never restored them "
                          "(%1): restoring")
               .arg(marker.description());

    if (platform::win::DesktopSurface::setDesktopIconsHidden(false)) {
        qCInfo(lc) << "the desktop icons are back";
        return;
    }

    // The marker is deliberately kept: this run could not repair the desktop (no
    // Explorer, say), and the next launch -- or the tray's "Restore my icons" --
    // must still find out that it has to.
    qCWarning(lc) << "the desktop icons could not be restored; the marker stays on disk "
                     "so a later run can still repair this";
}

void ProductApp::applyDocument()
{
    QString loadError;
    if (!m_store->load(&loadError)) {
        // A corrupt or unknown document is refused and the current (empty)
        // layout is kept: never replace a user's layout with nothing.
        qCWarning(lc).noquote() << "document refused, starting empty:" << loadError;
    }
    if (m_store->isReadOnly())
        qCWarning(lc).noquote() << "layout.json is protected, nothing will be written:" << m_store->readOnlyReason();
    if (!m_store->recoveredFrom().isEmpty())
        qCWarning(lc).noquote() << "layout.json was unreadable, recovered from" << m_store->recoveredFrom();

    QString applyError;
    if (!m_container->applyLayout(m_store->layout(), &applyError)) {
        qCWarning(lc).noquote() << "layout could not be applied:" << applyError;
        return;
    }

    if (m_container->fenceCount() == 0 && m_options.starterFence) {
        // One fence, so that launching the application is visibly something.
        // Deliberately left EMPTY: filing the whole desktop into it would claim
        // files the user never put there, and there is still no representation of
        // an icon outside a fence (F-011/F-007, untranched).
        const QString id = createFenceWithDefaultPlacement();
        qCInfo(lc).noquote() << "first launch: created" << id;
    }
}

void ProductApp::connectLifecycle(const std::function<void()>& onChange)
{
    ui::FenceContainer* c = m_container;
    connect(c, &ui::FenceContainer::fenceCreated, this, [onChange](const QString&) { onChange(); });
    connect(c, &ui::FenceContainer::fenceRemoved, this, [onChange](const QString&) { onChange(); });
    connect(c, &ui::FenceContainer::fenceRectChanged, this,
            [onChange](const QString&, const QRect&) { onChange(); });
    connect(c, &ui::FenceContainer::fenceContentChanged, this,
            [onChange](const QString&) { onChange(); });
    connect(c, &ui::FenceContainer::fenceRolledUpChanged, this,
            [onChange](const QString&, bool) { onChange(); });
    connect(c, &ui::FenceContainer::fenceLockedChanged, this,
            [onChange](const QString&, bool) { onChange(); }); // session 94
    connect(c, &ui::FenceContainer::fencesVisibilityChanged, this,
            [onChange](bool) { onChange(); });
    connect(c, &ui::FenceContainer::peekChanged, this, [onChange](bool) { onChange(); });
    connect(c, &ui::FenceContainer::rulesChanged, this, onChange);
    connect(c, &ui::FenceContainer::desktopWidgetsChanged, this, onChange); // session 91
    // Session 64: the stacking is the fences' order in the document.
    connect(c, &ui::FenceContainer::fenceStackChanged, this, [onChange](const QString&) { onChange(); });
}

void ProductApp::wirePersistence()
{
    connectLifecycle([this] { onLifecycleChanged(); });

    // Session 60: what can put a fence over desktop icons -- a fence made,
    // moved, grown, unrolled or shown again, or an icon leaving one.
    m_overlapTimer.setSingleShot(true);
    m_overlapTimer.setInterval(400);
    connect(&m_overlapTimer, &QTimer::timeout, this, [this] { resolveDesktopOverlaps(); });
    ui::FenceContainer* c = m_container;
    connect(c, &ui::FenceContainer::fenceCreated, this, [this](const QString&) { scheduleOverlapSweep(); });
    connect(c, &ui::FenceContainer::fenceRectChanged, this,
            [this](const QString&, const QRect&) { scheduleOverlapSweep(); });
    connect(c, &ui::FenceContainer::fenceRolledUpChanged, this,
            [this](const QString&, bool rolledUp) { if (!rolledUp) scheduleOverlapSweep(); });
    connect(c, &ui::FenceContainer::fencesVisibilityChanged, this,
            [this](bool visible) { if (visible) scheduleOverlapSweep(); });
    connect(c, &ui::FenceContainer::fenceContentChanged, this,
            [this](const QString&) { scheduleOverlapSweep(); });
    connect(c, &ui::FenceContainer::fenceTitleEditRequested, this,
            &ProductApp::startFenceTitleEdit);
    // Session 75: the shortcut copies follow what the fences hold.
    m_vaultTimer.setSingleShot(true);
    m_vaultTimer.setInterval(1500);
    connect(&m_vaultTimer, &QTimer::timeout, this, &ProductApp::syncShortcutVault);
    connect(c, &ui::FenceContainer::fenceContentChanged, this, [this](const QString&) { m_vaultTimer.start(); });
    connect(c, &ui::FenceContainer::fenceRemoved, this, [this](const QString&) { m_vaultTimer.start(); });

    connect(m_store, &core::LayoutStore::saveFailed, this,
            [](const QString& path, const QString& error) {
                qCWarning(lc).noquote()
                    << QStringLiteral("could not write %1: %2").arg(path, error);
            });
}

void ProductApp::onLifecycleChanged()
{
    // Exactly the wiring --persist-demo exercises: a change updates the document
    // and the store decides whether anything really differed and when to write.
    // A portal refresh re-emitting contentChanged() therefore writes nothing --
    // toLayout() drops the portal listing (T-011).
    m_store->setLayout(m_container->toLayout());
    m_store->scheduleSave();

    if (m_tray) {
        m_tray->setFenceCount(m_container->fenceCount());
        m_tray->setFencesVisible(m_container->fencesVisible());
        m_tray->setPeekActive(m_container->peekActive());
        m_tray->setDesktopIconsHidden(desktopIconsHidden());
        // F-038 / T-057: the material entry follows the document like the other
        // menu states -- a change made from a fence's own menu shows up here.
        m_tray->setFenceMaterial(sharedFenceMaterial());
    }

    // A portal appearing or leaving changes what has to be watched (F-018). The
    // cheap comparison keeps a fence drag (60 signals a second) from re-listing
    // the desktop folders.
    if (m_watcher && m_container->portalFolders() != m_portalFolders) {
        m_portalFolders = m_container->portalFolders();
        syncWatchedFolders();
    }

    // A fence that just took an icon (or let one go) changes which native icons
    // have to move out of the way: react now rather than at the next timer tick.
    syncNativeIcons();

    refreshStatus();
}

void ProductApp::wireTray()
{
    if (!m_options.tray)
        return;

    if (!ui::TrayIcon::isAvailable()) {
        // No notification area at all (a bare session, or the shell restarting):
        // there is no other way into the application, so say so instead of
        // running invisibly.
        qCWarning(lc) << "no system tray available: StarFish has no visible handle";
        return;
    }

    m_tray = new ui::TrayIcon(this);
    // ADR-13: the entries that hide the desktop's icons are only offered on the
    // development path that still claims the desktop (--claim-desktop / the
    // probes). In the product they do not exist, because clicking them does
    // nothing the user could want.
    m_tray->setIconHidingOffered(m_options.claimDesktop);
    // T-046: the way back follows the same condition -- a desktop whose icons
    // StarFish may have hidden is a desktop whose way back must exist. Nothing in
    // the product hides them any more (ADR-20), so in the product this entry is
    // not offered; the crash marker (T-046) repairs a previous run at launch.
    m_tray->setRestoreIconsOffered(m_options.claimDesktop);
    connect(m_tray, &ui::TrayIcon::newFenceRequested, this, &ProductApp::onNewFence);
    connect(m_tray, &ui::TrayIcon::settingsRequested, this, &ProductApp::showSettings);
    connect(m_tray, &ui::TrayIcon::quitRequested, this, &ProductApp::quitRequested);
    connect(m_tray, &ui::TrayIcon::fencesVisibilityToggled, this,
            [this](bool visible) { m_container->setFencesVisible(visible); });
    connect(m_tray, &ui::TrayIcon::desktopIconsToggled, this,
            &ProductApp::onDesktopIconsToggled);
    connect(m_tray, &ui::TrayIcon::restoreIconsRequested, this,
            &ProductApp::restoreDesktopIcons);
    // F-038 / T-057: the tray speaks for the whole desktop, so its material entry
    // applies to every fence (a fence's own menu is the per-fence one).
    connect(m_tray, &ui::TrayIcon::fenceMaterialRequested, this,
            &ProductApp::applyMaterialToAllFences);
    connect(m_tray, &ui::TrayIcon::fenceShortcutArrowsToggled, this,
            &ProductApp::setFenceShortcutArrows);
    connect(m_tray, &ui::TrayIcon::contextMenuEditRequested, this, &ProductApp::editContextMenu);

    if (!m_tray->show())
        qCWarning(lc) << "the tray icon could not be shown";
    m_tray->setFenceShortcutArrows(m_fenceShortcutArrows);

    m_tray->setFenceCount(m_container->fenceCount());
    m_tray->setFencesVisible(m_container->fencesVisible());
    m_tray->setPeekActive(m_container->peekActive());
    m_tray->setDesktopIconsHidden(desktopIconsHidden());
    m_tray->setFenceMaterial(sharedFenceMaterial());
}

void ProductApp::wireSettings()
{
    if (!m_options.settingsWindow)
        return;

    // T-059: the window is no longer the fence manager, so it takes no container
    // and no connection at all. Nothing here can change a fence: creating one is
    // the tray's (or the desktop menu's, F-001), and everything that belongs to
    // *one* fence -- rename, roll up, remove, material, colours, radius -- is the
    // fence's own menu (T-076 / ADR-16). What the window carries is the status
    // line, and the rules screen (T-032) is assigned to it.
    m_settings = new ui::SettingsDialog(nullptr);
    // Session 59: the window grew pages, and every control on them speaks for
    // the whole desktop -- the tray's entries, wired to the same slots, so the
    // two surfaces cannot disagree. Still no fence list (T-059): one fence is
    // its own menu's.
    connect(m_settings, &ui::SettingsDialog::newFenceRequested, this, &ProductApp::onNewFence);
    connect(m_settings, &ui::SettingsDialog::fencesVisibilityToggled, this,
            [this](bool visible) { m_container->setFencesVisible(visible); });
    connect(m_settings, &ui::SettingsDialog::fenceMaterialRequested, this,
            &ProductApp::applyMaterialToAllFences);
    connect(m_settings, &ui::SettingsDialog::defaultTransparencyChanged, this,
            &ProductApp::setDefaultFenceTransparency);
    connect(m_settings, &ui::SettingsDialog::fenceOverlapToggled, this, &ProductApp::setAllowFenceOverlap);
    connect(m_settings, &ui::SettingsDialog::languageChosen, this, [](const QString& code) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("language"), code);
    });
    connect(m_settings, &ui::SettingsDialog::desktopDoubleClickHidesToggled, this, [this](bool on) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("desktopDoubleClickHides"), on);
        m_doubleClickHides = on;
        if (!on)
            setDesktopHiddenByUser(false); // nothing stays hidden behind a switched-off gesture
    });
    const auto saveSwitch = [this](const QString& key, bool on, void (ui::FenceContainer::*apply)(bool)) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat).setValue(key, on);
        if (m_container)
            (m_container->*apply)(on);
    };
    connect(m_settings, &ui::SettingsDialog::rollOppositeToggled, this, [saveSwitch](bool on) {
        saveSwitch(QStringLiteral("fenceRollOpposite"), on, &ui::FenceContainer::setRollOppositeDefault);
    });
    connect(m_settings, &ui::SettingsDialog::rollAnimatedToggled, this, [saveSwitch](bool on) {
        saveSwitch(QStringLiteral("fenceRollAnimated"), on, &ui::FenceContainer::setAnimateRoll);
    });
    connect(m_settings, &ui::SettingsDialog::autoRestoreShortcutsToggled, this, [this](bool on) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("fenceAutoRestoreShortcuts"), on);
        m_autoRestoreShortcuts = on;
    });
    connect(m_settings, &ui::SettingsDialog::rollSpeedChosen, this, [this](int speed) { // session 95
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("fenceRollSpeed"), speed);
        if (m_container)
            m_container->setRollSpeed(speed);
    });
    connect(m_settings, &ui::SettingsDialog::iconFlowChosen, this, [this](const QString& key) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("fenceIconFlow"), key);
        m_container->setIconFlowDefault(ui::IconLayout::flowFromKey(key));
    });
    connect(m_settings, &ui::SettingsDialog::desktopDragGridToggled, this, [this](bool on) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("desktopDragGrid"), on);
        setDesktopDragGrid(on);
    });
    connect(m_settings, &ui::SettingsDialog::splashToggled, this, [](bool on) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("showSplash"), on);
    });
    connect(m_settings, &ui::SettingsDialog::showLabelsToggled, this, [saveSwitch](bool on) {
        saveSwitch(QStringLiteral("fenceShowLabels"), on, &ui::FenceContainer::setShowLabelsDefault);
    });
    connect(m_settings, &ui::SettingsDialog::snapToCellsToggled, this, [saveSwitch](bool on) {
        saveSwitch(QStringLiteral("fenceSnapToCells"), on, &ui::FenceContainer::setSnapToCellsDefault);
    });
    connect(m_settings, &ui::SettingsDialog::hoverUnrollToggled, this, [this](bool on) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("fenceHoverUnroll"), on);
        if (m_container)
            m_container->setHoverUnrollDefault(on);
    });
    connect(m_settings, &ui::SettingsDialog::hoverHighlightToggled, this, [this](bool on) {
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat).setValue(QStringLiteral("fenceHoverHighlight"), on);
        if (m_container)
            m_container->setHoverHighlight(on);
    });
    connect(m_settings, &ui::SettingsDialog::defaultBorderChanged, this, [this](int width, const QString& color) {
        QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat);
        settings.setValue(QStringLiteral("fenceBorderWidth"), width);
        settings.setValue(QStringLiteral("fenceBorderColor"), color);
        if (m_container)
            m_container->setDefaultBorder(width, color);
    });
    connect(m_settings, &ui::SettingsDialog::allFencesToDefaultRequested, this, [this] {
        m_container->resetFencesToDefaultOpacity();
        qCInfo(lc) << "every fence follows the default transparency";
    });
    connect(m_settings, &ui::SettingsDialog::fenceShortcutArrowsToggled, this,
            &ProductApp::setFenceShortcutArrows);
    connect(m_settings, &ui::SettingsDialog::sortOnAddToggled, this, &ProductApp::setSortOnAdd);
    connect(m_settings, &ui::SettingsDialog::startWithWindowsToggled, this,
            &ProductApp::setStartWithWindows);
    connect(m_settings, &ui::SettingsDialog::desktopShortcutArrowsToggled, this,
            &ProductApp::setDesktopShortcutArrows);
    connect(m_settings, &ui::SettingsDialog::desktopOverlapToggled, this,
            &ProductApp::setAllowDesktopOverlap);
    connect(m_settings, &ui::SettingsDialog::restoreIconsRequested, this,
            &ProductApp::restoreDesktopIcons);
    connect(m_settings, &ui::SettingsDialog::resetDesktopRequested, this, &ProductApp::resetDesktop);
    m_settings->setRestoreIconsOffered(m_options.claimDesktop);
    loadSeenMenuKeys();
    m_settings->menuEditor()->setSeenEntries(m_seenMenuKeys);
    refreshStatus();
}

void ProductApp::wireWatcher()
{
    if (!m_options.watchDesktop)
        return;

    m_watcher = new core::DesktopWatcher(this);

    // F-018: what the rules are offered. `itemsAdded` is the addition-only signal
    // the rules consume; `folderChanged` is the "this folder changed, however it
    // changed" signal the portals re-list on (T-011).
    connect(m_watcher, &core::DesktopWatcher::itemsAdded, this,
            [this](const QStringList& paths) {
                QList<core::DesktopItem> items;
                items.reserve(paths.size());
                for (const QString& path : paths)
                    items.append(m_container->itemForPath(path));
                const int filed = m_container->applyRulesTo(items);
                if (filed > 0 && m_tray)
                    m_tray->notify(tr("StarFish"), tr("%1 item(s) filed by the rules").arg(filed));
                // T-039: a file dropped on the desktop must appear on the
                // backdrop too, whether or not a rule claimed it.
                if (m_container->backdropEnabled())
                    refreshDesktopItems();
            });

    connect(m_watcher, &core::DesktopWatcher::folderChanged, this,
            [this](const QString& folder) {
                m_container->refreshPortalsFor(folder);
                // The backdrop is a view of the desktop folder: a file leaving it
                // has to leave the backdrop as well, and only this signal says so
                // (itemsAdded never will).
                if (m_container->backdropEnabled()
                    && m_desktopFolders.contains(folder, Qt::CaseInsensitive))
                    refreshDesktopItems();
            });

    m_portalFolders = m_container->portalFolders();
    syncWatchedFolders();
}

void ProductApp::syncWatchedFolders()
{
    if (!m_watcher)
        return;

    QStringList folders = m_options.watchDesktop
                              ? platform::win::DesktopScanner::desktopFolders()
                              : QStringList();
    // Kept so a folderChanged() can tell "the desktop changed" (the backdrop
    // follows) from "a portal's folder changed" (it does not).
    m_desktopFolders = folders;
    folders += m_container->portalFolders();
    folders.removeDuplicates();

    if (folders == m_watchedFolders)
        return;
    m_watchedFolders = folders;

    if (folders.isEmpty()) {
        m_watcher->stop();
        return;
    }

    m_watcher->setFolders(folders);
    // start() takes the listing as the baseline: what is already on the desktop
    // is not "new", so a rule cannot re-file the whole desktop at every launch.
    m_watcher->start();
    qCInfo(lc).noquote()
        << QStringLiteral("watching %1 folder(s)").arg(folders.size());
}

void ProductApp::wireHotkeys()
{
    if (!m_options.hotkeys)
        return;

    m_hotkeys = new platform::win::HotkeyManager(this);

    // T-053 (session 65): no Peek hot key any more. A failure below is
    // reported, never swallowed (session 15).
    if (!m_hotkeys->registerHotkey(kHideHotkeyId, kHideModifiers, kHideKey)) {
        qCWarning(lc).noquote()
            << QStringLiteral("could not register %1 for quick hide (already taken?)")
                   .arg(platform::win::HotkeyManager::describe(kHideModifiers, kHideKey));
    }

    connect(m_hotkeys, &platform::win::HotkeyManager::triggered, this, [this](int id) {
        if (id == kHideHotkeyId)
            m_container->toggleFencesVisible();
    });
}

QString ProductApp::createFenceWithDefaultPlacement()
{
    if (!m_container)
        return QString();

    // A fence created under an existing one looks like nothing happened, so the
    // spot is *searched*: the first place on the primary monitor (the screen the
    // user is looking at) that no fence covers (T-066). This used to be "to the
    // right of everything, wrapping to a new row", and the wrap put every later
    // fence on the same point: measured on the user's own document, three fences
    // at (80, 1200) -- stacked on each other at the bottom of the screen, looking
    // like one. The margin keeps the screen's edges (and its first icons) clear,
    // and the container clamps whatever comes back.
    const ui::ScreenInfo* primaryScreen = m_container->screens().primary();
    const QRect bounds = primaryScreen ? primaryScreen->geometry
                                       : QRect(QPoint(0, 0), QSize(1920, 1080));
    QList<QRect> others;
    const QStringList ids = m_container->fenceIds();
    others.reserve(ids.size());
    for (const QString& id : ids)
        others.append(m_container->fenceRect(id));

    const QRect spot = ui::FenceGeometry::freeSpot(
        others, QSize(kDefaultFenceWidth, kDefaultFenceHeight),
        bounds.adjusted(kPlacementMargin, kPlacementMargin, -kPlacementMargin, -kPlacementMargin));

    return m_container->createFence(nextFenceTitle(), spot);
}

QString ProductApp::nextFenceTitle() const
{
    // One naming policy, shared by the tray, the fence's own menu and a fence
    // drawn on the desktop: a fence created from anywhere is named the same way,
    // and only this function decides it.
    return tr("Fence %1").arg(m_container ? m_container->fenceCount() + 1 : 1);
}

void ProductApp::onNewFence()
{
    // Session 82 (the user's ask): no notification when a fence is built, wherever
    // it is built from. The fence appearing is the answer.
    createFenceWithDefaultPlacement();
}

void ProductApp::onDesktopMenu(const QRect& rect, const QPoint& point)
{
    if (!m_container)
        return;

    ui::DesktopBackdrop* backdrop = m_container->backdrop();
    // Session 82 (the user's ask): the rectangle he drew stays on screen while the
    // menu is open, so he sees the fence he is about to build. It goes away when
    // this function returns -- chosen, dismissed, created or not.
    struct PreviewGuard {
        ui::DesktopBackdrop* backdrop;
        ~PreviewGuard()
        {
            if (backdrop)
                backdrop->clearFencePreview();
        }
    } previewGuard{backdrop};

    // What the entry would do, said before anything happens: a drawn rectangle
    // offers *that* rectangle, a plain right-click offers a default-size fence
    // where the user aimed. The draw has already refused to offer a rectangle too
    // small to be a fence, so the label can never promise a 40x40 fence.
    const bool drawn = rect.isValid() && !rect.isEmpty();
    const QRect target = drawn ? rect : QRect(point, QSize(kDefaultFenceWidth, kDefaultFenceHeight));
    const QString label = drawn ? tr("Create a fence here (%1 x %2)")
                                      .arg(target.width())
                                      .arg(target.height())
                                : tr("Create a fence here");

    // The popup belongs to the hosted surface, so it behaves like a menu of the
    // real on-screen window rather than of a hidden helper.
    void* owner = nullptr;
    if (backdrop) {
        if (platform::win::DesktopSurface* surface = backdrop->surface())
            owner = reinterpret_cast<void*>(surface->hwnd());
    }

    if (drawnDesktop()) {
        // ADR-21: the desktop's **real** background menu -- the one its own view
        // builds (SVGIO_BACKGROUND), with View, Sort by, Refresh, Paste, Undo, New
        // and every extension, its submenus filled by the shell itself. Our entry
        // goes on top; the registry verb that says the same thing (T-071) is taken
        // out, since this one creates the fence where the user clicked.
        QStringList hidden{QStringLiteral("StarFish"), platform::win::desktopMenuLabel()};
        const QString registered =
            QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\DesktopBackground\\"
                                     "Shell\\StarFish"),
                      QSettings::NativeFormat)
                .value(QStringLiteral("."))
                .toString();
        if (!registered.isEmpty())
            hidden << registered;
        constexpr int kCreateFence = 1;
        constexpr int kAddWidget = 100; // session 91: + the kind's index
        const QList<ui::widgets::Kind> kinds = ui::widgets::kinds();
        const QString addWidget = tr("Add a widget");
        int chosen = -1;
        if (modernContextMenuWanted()) {
            // T-092: Windows 11's menu, drawn by StarFish from the real one.
            core::ContextMenuItem create;
            create.key = QStringLiteral("starfish:create-fence");
            create.label = label;
            create.glyph = QString(QChar(0xE80A));
            create.ownId = kCreateFence;
            core::ContextMenuItem widgetsEntry;
            widgetsEntry.key = QStringLiteral("starfish:add-widget");
            widgetsEntry.label = addWidget;
            widgetsEntry.glyph = QString(QChar(0xE71D));
            for (int i = 0; i < kinds.size(); ++i) {
                core::ContextMenuItem kind;
                kind.key = QStringLiteral("starfish:add-widget:") + kinds.at(i).type;
                kind.label = kinds.at(i).label;
                kind.ownId = kAddWidget + i;
                widgetsEntry.children.push_back(kind);
            }
            chosen = showModernContextMenu(core::ContextMenuKind::Background, {}, point, {create, widgetsEntry},
                                           nullptr, hidden);
        } else {
            platform::win::DesktopShellView::MenuEntry widgetsEntry{0, addWidget, true, false, {}};
            for (int i = 0; i < kinds.size(); ++i)
                widgetsEntry.children.push_back({kAddWidget + i, kinds.at(i).label, false, false, {}});
            chosen = m_shellView->showBackgroundMenu(
                point, backdropOwner(), {{kCreateFence, label, false, false, {}}, widgetsEntry}, hidden);
        }
        if (chosen >= kAddWidget && chosen < kAddWidget + kinds.size()) {
            addDesktopWidgetAt(kinds.at(chosen - kAddWidget).type, point);
            return;
        }
        // Whatever the shell ran (a new folder, a paste, View > Small icons)
        // shows up on the next read; asked for now rather than in 1.5 s.
        const QString verb = m_shellView->lastChosenVerb();
        // Through the proxy a verb arrives cut to its first letter (T-092):
        // "N" for NewFolder / NewLink, "." for a new document.
        if (verb.startsWith(QLatin1Char('N')) || verb.startsWith(QLatin1Char('.')))
            armNewItemRename();
        m_shellResync.start();
        if (chosen != kCreateFence)
            return;
    } else {
    const platform::win::ShellContextMenu::BackgroundChoice choice =
        platform::win::ShellContextMenu::showForDesktopBackground(point, owner, label);

    // Only our own entry creates anything. The shell's verbs have already run
    // inside the menu call, and a dismissed menu creates nothing at all.
    if (choice != platform::win::ShellContextMenu::BackgroundChoice::StarfishVerb)
        return;
    }

    // Session 64: overlaps off, a fence drawn over another one goes to the
    // nearest free place (the log says so).
    QRect placed = target;
    if (!m_container->allowFenceOverlap()) {
        QList<QRect> others;
        for (const QString& other : m_container->fenceIds())
            others.append(m_container->fenceRect(other));
        placed = ui::FenceGeometry::nearestFree(target, others, m_container->screens().clampRectFor(target));
        if (placed != target)
            qCInfo(lc).noquote() << QStringLiteral("overlaps are off: the new fence moved to a free place");
    }
    const QString id = m_container->createFence(nextFenceTitle(), placed);
    if (id.isEmpty())
        return;
    qCInfo(lc).noquote() << QStringLiteral("created %1 at %2,%3 %4x%5 from the desktop menu")
                                .arg(id)
                                .arg(target.x())
                                .arg(target.y())
                                .arg(target.width())
                                .arg(target.height());
    // Session 82 (the user's ask): no notification when a fence is built. The
    // fence appearing *is* the answer; a balloon said it twice.
}

void ProductApp::wireBackdrop()
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    if (!backdrop)
        return;

    // The smallest thing a draw may offer as a fence is the smallest fence the
    // container will keep, taken from it rather than repeated here: the gesture
    // and the container cannot then disagree about what a fence is.
    ui::BackdropGesture::Metrics metrics;
    metrics.minimumFence = m_container->minimumFenceSize();
    backdrop->setGestureMetrics(metrics);

    connect(backdrop, &ui::DesktopBackdrop::desktopMenuRequested, this,
            &ProductApp::onDesktopMenu);
    // ADR-21: the drawn desktop's item menu, and the view's re-reads.
    connect(backdrop, &ui::DesktopBackdrop::iconMenuRequested, this, &ProductApp::onIconMenu);
    // Session 91: the desktop widgets -- menu, drag (with the grid), drop, edit.
    connect(backdrop, &ui::DesktopBackdrop::desktopWidgetMenuRequested, this, &ProductApp::showDesktopWidgetMenu);
    connect(backdrop, &ui::DesktopBackdrop::desktopWidgetDragged, this,
            [this](const QString&, const QRect& rect, bool active) {
                // Session 95 (T-150): over a fence, its insertion bar instead of the grid.
                const bool overFence = active && !m_container->fenceAt(rect.center()).isEmpty();
                m_container->showWidgetDropAt(rect.center(), active);
                if (active && !overFence)
                    showDesktopWidgetGrid(rect);
                else
                    hideDesktopDropGrid();
            });
    connect(backdrop, &ui::DesktopBackdrop::desktopWidgetDropped, this, [this](const QString& path, const QPoint& pos) {
        core::DesktopItem item;
        for (const ui::DesktopWidget& widget : m_container->desktopWidgets())
            if (widget.item.path == path)
                item = widget.item;
        const QSize span = item.isWidget() ? item.span : QSize(1, 1);
        // Session 95 (T-150): let go over a fence, the widget moves into it (a
        // locked one refuses; it then stays on the desktop, where it was let go).
        const QSize cell = desktopSlot().spacing;
        const QPoint centre = pos + QPoint(span.width() * cell.width() / 2, span.height() * cell.height() / 2);
        m_container->showWidgetDropAt(centre, false);
        const QString fence = m_container->fenceAt(centre);
        if (item.isWidget() && !fence.isEmpty() && m_container->fileIntoAt(fence, {item}, centre) > 0) {
            m_container->removeDesktopWidget(path);
            qCInfo(lc).noquote() << QStringLiteral("desktop widget %1 moved into %2").arg(path, fence);
            return;
        }
        m_container->moveDesktopWidget(path, snapDesktopWidget(pos, span));
    });
    connect(backdrop, &ui::DesktopBackdrop::desktopWidgetEditRequested, this,
            [this](const QString& path) { startWidgetEdit(desktopWidgetOwner(), path); });
    syncDesktopWidgetCell();
    // ADR-21 lot 3 (T-091): the keyboard, the shell's drag loop, and the drop
    // target that moves our icons or hands a foreign drop to the desktop folder.
    connect(backdrop, &ui::DesktopBackdrop::keyCommand, this, &ProductApp::onDesktopKey);
    connect(backdrop, &ui::DesktopBackdrop::iconDragRequested, this,
            &ProductApp::dragDesktopItems);
    backdrop->setDropTargetFactory([this](quintptr hwnd) {
        return platform::win::createDesktopDropTarget(
            hwnd,
            [this](const QPoint& point, const QStringList& paths, const QPoint& origin) {
                onInternalDrop(point, paths, origin);
            },
            [this](const QPoint& point, const QStringList& stems) { onForeignDrop(point, stems); },
            [this](const QPoint& point, bool internal, bool active) {
                // Session 80: the grid follows a drag over the drawn desktop.
                if (!active) {
                    hideDesktopDropGrid();
                } else if (internal) {
                    const QStringList paths = platform::win::internalDragPaths();
                    showDesktopDropGrid(paths, point - m_dragOrigin, point, m_dragOrigin);
                } else {
                    showDesktopDropGrid({}, {}, point);
                }
            });
    });
    // T-090: the desktop window died under us (Explorer restarted).
    connect(backdrop, &ui::DesktopBackdrop::desktopLost, this, &ProductApp::onDesktopLost);
    m_reclaimTimer.setInterval(2000);
    connect(&m_reclaimTimer, &QTimer::timeout, this, &ProductApp::tryReclaimDrawnDesktop);
    m_shellTimer.setInterval(1500);
    connect(&m_shellTimer, &QTimer::timeout, this, [this, backdrop] {
        if (drawnDesktop() && !backdrop->isAttached()) {
            onDesktopLost(); // whatever took our window, the desktop is not drawn any more
            return;
        }
        // T-090: the shell can show its list again on its own (it rebuilt its
        // view, a setting was applied): the icons would be drawn twice. Hidden
        // again, marker first, exactly like the first time.
        if (drawnDesktop() && m_iconsHidden && platform::win::nativeDesktopIconsVisible()) {
            ++m_listReshown;
            qCWarning(lc) << "the shell showed its icon list again: hidden again";
            platform::win::DesktopSurface::setDesktopIconsHidden(true);
            m_shellSignature.clear();
        }
        syncShellView();
        // Session 66: icons that change without their file changing -- the
        // Recycle Bin (empty / full), and icons the shell could not give yet.
        if (m_shellTicks % 2 == 0 && m_icons) {
            bool redraw = m_icons->retryFailed();
            const int bin = platform::win::ShellIconCache::recycleBinState();
            if (bin != m_recycleBinState) {
                if (m_recycleBinState != -2)
                    redraw = m_icons->forget(platform::win::ShellIconCache::recycleBinPath()) || redraw;
                m_recycleBinState = bin;
            }
            if (redraw) {
                m_container->refreshBackdropIcons();
                m_container->refreshFenceIcons();
            }
        }
        // The wallpaper changes far less often (a slideshow, Spotlight, the user
        // picking a new one): checked every ~9 s, pushed only when it differs.
        if (++m_shellTicks % 6 == 0)
            refreshWallpaper();
    });
    m_shellResync.setSingleShot(true);
    m_shellResync.setInterval(300);
    connect(&m_shellResync, &QTimer::timeout, this, &ProductApp::syncShellView);
    // F-022: the desktop's own double-click, the gesture the feature was always
    // specified with. It is the same toggle the tray and the hot key use.
    // Session 69: only when the setting is on (off by default), and it hides
    // the drawn icons with the fences -- drawing only: no file, no position.
    m_doubleClickHides = QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
                             .value(QStringLiteral("desktopDoubleClickHides"), false).toBool();
    connect(backdrop, &ui::DesktopBackdrop::quickHideRequested, this, [this] {
        if (m_doubleClickHides)
            setDesktopHiddenByUser(!m_desktopHiddenByUser);
    });

    // -------------------------------------------------------------------------
    // T-042 (first slice) -- the icons the backdrop draws answer the mouse.
    //
    // This is what F-040 made necessary: hiding the native list takes away the
    // *source* of every drag, so with the switch on there was nothing left to
    // pick up, and "file this icon in a fence" -- the whole point of the mode --
    // became impossible. The backdrop reads the gesture (the cells are its
    // business) and this decides what it means, like every other gesture here.
    // -------------------------------------------------------------------------
    connect(backdrop, &ui::DesktopBackdrop::iconDropRequested, this,
            [this](const QString& path, const QPoint& point) {
                if (!m_container)
                    return;
                // ADR-21 (user's decision, session 58): This PC, the Recycle Bin and
                // the other virtual items are filed like any icon -- the tile keeps
                // the parsing name ("::{CLSID}") and opens through the shell.
                // What the button came up *on* decides: a fence takes it. Letting
                // go on the bare desktop is how a drag is called off -- the entry
                // stays where it is, because an icon is never moved and never
                // copied (the golden rule).
                const QString id = m_container->fenceAt(point);
                if (id.isEmpty()) {
                    qCInfo(lc).noquote()
                        << QStringLiteral("\"%1\" was let go on the bare desktop: nothing "
                                          "filed")
                               .arg(path);
                    return;
                }
                // Session 60: where it was let go, not at the end (unless the
                // user asked for alphabetical order).
                const int filed =
                    m_container->fileIntoAt(id, {m_container->itemForPath(path)}, point);
                qCInfo(lc).noquote()
                    << QStringLiteral("dropped \"%1\" on %2: %3 item(s) filed")
                           .arg(path, id)
                           .arg(filed);
            });

    // The desktop's oldest gesture, and ours to provide: the icon is only pixels
    // here, so there is no shell list behind it to open anything.
    connect(backdrop, &ui::DesktopBackdrop::iconActivateRequested, this,
            [this](const QString& path) {
                if (!m_container)
                    return;
                if (drawnDesktop()) {
                    openDesktopItem(path);
                    return;
                }
                // The same resolution a fence's tile uses (F-006/T-005): a .lnk or
                // a .url opens through its target, never through the link file.
                // Session 76: the item itself, through the shell -- a shortcut's
                // arguments, app id or URI are the shell's to apply, not ours.
                if (platform::win::openShellItem(path))
                    return;
                const QString target = ui::IconGrid::launchPath(m_container->itemForPath(path));
                if (target.isEmpty() || target.startsWith(QLatin1String("::")))
                    return;
                qCInfo(lc).noquote() << QStringLiteral("opening \"%1\"").arg(target);
                QDesktopServices::openUrl(QUrl::fromLocalFile(target));
            });
}

void ProductApp::onRenameFence(const QString& id, const QString& title)
{
    m_container->setFenceTitle(id, title); // emits contentChanged -> auto-save
}

// T-076 / T-064 -- the fence's own menu.
//
// The user asked for it in session 42 (« un menu rien qu'à nous »); what it costs
// the desktop is nothing, because it opens on a right-click inside a fence we
// already own. The tiles keep the Shell's menu (T-006) -- FenceView decides which
// of the two a right-click is, and this is the half that is ours.
//
// The content is ui::FenceMenu's (pure, and checked without a desktop); what the
// product adds is the mechanism and, above all, that every entry *calls the same
// method the tray and a fence's own menu call*. A menu choice that reached the
// document by its own route is exactly how two sources of truth start.
void ProductApp::wireFenceMenu()
{
    connect(m_container, &ui::FenceContainer::fenceMenuRequested, this,
            &ProductApp::showFenceMenu);
    connect(m_container, &ui::FenceContainer::fenceItemMenuRequested, this,
            &ProductApp::onFenceItemMenu);
    // Session 81: a double-click on a note opens its editor over the widget.
    connect(m_container, &ui::FenceContainer::fenceWidgetEditRequested, this,
            &ProductApp::startWidgetEdit);
    // Session 73: Delete on a fence's selection -- the Shell's own delete (Recycle
    // Bin and undo; Shift: its permanent delete with its confirmation). The
    // tiles whose file is gone leave the fence; a cancelled delete leaves all.
    // Session 76: tiles dropped out of a fence go where they were let go (on
    // the grid, never under a fence unless allowed), not to the place the
    // hidden icon last had on the desktop.
    // Session 95 (T-150): a widget dragged out of a fence lands on the desktop.
    connect(m_container, &ui::FenceContainer::widgetsDroppedOnDesktop, this,
            [this](const QString& sourceId, const QList<core::DesktopItem>& widgets, const QPoint& point) {
                if (!m_container->backdrop())
                    return; // no desktop layer to hold it: the widget stays in its fence
                syncDesktopWidgetCell();
                QStringList moved;
                QPoint at = point;
                for (const core::DesktopItem& widget : widgets) {
                    const QSize cell = desktopSlot().spacing;
                    const QPoint topLeft = snapDesktopWidget(
                        at - QPoint(widget.span.width() * cell.width() / 2, widget.span.height() * cell.height() / 2),
                        widget.span);
                    if (m_container->addDesktopWidget(widget, topLeft))
                        moved << widget.path;
                    at += QPoint(cell.width(), 0);
                }
                if (!moved.isEmpty())
                    m_container->releaseItems(sourceId, moved);
            });
    connect(m_container, &ui::FenceContainer::tilesDroppedOnDesktop, this,
            [this](const QStringList& paths, const QPoint& point) {
                if (!drawnDesktop() || !m_shellView || paths.isEmpty())
                    return;
                const QString first = shellNameOf(paths.first());
                if (first.isEmpty() || !m_itemPositions.contains(first))
                    return;
                const int icon = qMax(16, m_shellIconSize);
                const QPoint wanted = point - QPoint(icon / 2, icon / 2);
                QHash<QString, QPoint> before;
                for (const QString& path : paths)
                    before.insert(path, m_itemPositions.value(shellNameOf(path)));
                moveDesktopItems(paths, wanted - m_itemPositions.value(first));
                // Session 86 (the user: the icon flashed at its old desktop place
                // before jumping to the drop): the drawn desktop's boxes still held
                // the old place until the next shell read. Moved here, before the
                // fence releases the tiles, so they first appear where they land.
                ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
                if (!backdrop)
                    return;
                QHash<QString, QRect> boxes = backdrop->nativeIconBoxes();
                bool changed = false;
                for (const QString& path : paths) {
                    const QString name = shellNameOf(path);
                    const QString key = ui::BackdropLayout::pathKey(path);
                    if (name.isEmpty() || !boxes.contains(key) || !m_itemPositions.contains(name))
                        continue;
                    const QPoint shift = m_itemPositions.value(name) - before.value(path);
                    if (shift.isNull())
                        continue;
                    boxes[key].translate(shift);
                    changed = true;
                }
                if (changed)
                    backdrop->setNativeIconBoxes(boxes);
            });
    // Session 80: tiles held over the bare desktop show its grid, and the slot
    // they would take (the same sum as the drop above).
    connect(m_container, &ui::FenceContainer::tilesHoverDesktop, this,
            [this](const QStringList& paths, const QPoint& point, bool active) {
                QStringList files;
                for (const QString& path : paths)
                    if (!core::isWidgetPath(path))
                        files << path;
                const QString first = files.isEmpty() ? QString() : shellNameOf(files.first());
                if (!active || first.isEmpty() || !m_itemPositions.contains(first)) {
                    hideDesktopDropGrid();
                    return;
                }
                const int icon = qMax(16, m_shellIconSize);
                const QPoint wanted = point - QPoint(icon / 2, icon / 2);
                showDesktopDropGrid(files, wanted - m_itemPositions.value(first), point);
            });
    // Session 96: a widget held out of a fence over the bare desktop shows the
    // grid and the slots it would cover, as a desktop widget's own drag does.
    connect(m_container, &ui::FenceContainer::widgetHoversDesktop, this,
            [this](const QSize& span, const QPoint& point, bool active) {
                const QSize cell = desktopSlot().spacing;
                if (!active || !cell.isValid() || span.isEmpty()) {
                    hideDesktopDropGrid();
                    return;
                }
                syncDesktopWidgetCell();
                const QSize size(span.width() * cell.width(), span.height() * cell.height());
                showDesktopWidgetGrid(QRect(point - QPoint(size.width() / 2, size.height() / 2), size));
            });
    // Session 74: F2 on a fence's selection -- the same edit as the desktop's.
    connect(m_container, &ui::FenceContainer::fenceRenameRequested, this,
            [this](const QString& id, const QString& path) {
                if (!core::isWidgetPath(path))
                    startRename(path, id);
            });
    connect(m_container, &ui::FenceContainer::fenceDeleteRequested, this,
            [this](const QString& id, const QStringList& requested, bool permanent) {
                using platform::win::DeleteMode;
                QStringList paths; // session 79: a widget path never reaches the shell
                for (const QString& path : requested)
                    if (!core::isWidgetPath(path))
                        paths << path;
                if (paths.isEmpty())
                    return;
                platform::win::deleteShellItems(paths, permanent ? DeleteMode::Permanent : DeleteMode::RecycleBin,
                                                backdropOwner());
                if (m_container)
                    m_container->forgetMissingItems(id, paths);
                m_shellResync.start();
            });
}

void ProductApp::showFenceMenu(const QString& id, const QPoint& globalPos)
{
    if (!m_container || !m_container->contains(id))
        return;

    ui::FenceMenu::State state;
    state.rolledUp = m_container->fenceRolledUp(id);
    state.locked = m_container->fenceLocked(id); // session 94
    // Read from the fence every time the menu opens rather than remembered here:
    // the check mark must follow the document, and the document may have been
    // changed by the tray a moment ago.
    state.material = m_container->fenceMaterial(id);
    // T-082: what the material covers. Both come from the document -- the material
    // *is* the fence's alpha -- so the menu cannot drift from what is on screen.
    state.materialIsTranslucent = ui::FenceMaterial::isTranslucent(m_container->fenceEffectiveAlpha(id));
    state.fadeIcons = m_container->fenceIconsFollowMaterial(id);
    // T-080: the fence's own look, read from the document each time the menu
    // opens -- the radius entry marked is the one the fence *is*, and the way back
    // to "no band" only exists when there is a band.
    const core::FenceStyle fenceStyle = m_container->fenceStyle(id);
    state.radius = fenceStyle.radius;
    state.hasTitleBarColor = !fenceStyle.titleBarColor.isEmpty();
    state.titleBarShown = fenceStyle.titleBar;
    state.hoverUnroll = fenceStyle.hoverUnroll;
    state.hoverUnrollDefault = m_container->hoverUnrollDefault();
    state.snapToCells = fenceStyle.snapToCells; // session 76
    state.snapToCellsDefault = m_container->snapToCellsDefault();
    state.templates = ui::FenceTemplates::names();
    for (const ui::widgets::Kind& kind : ui::widgets::kinds()) // session 79
        state.widgetKinds << kind.label;
    if (const ui::FenceView* view = m_container->view(id))
        state.sortBy = view->sortBy();

    QMenu menu;
    for (const ui::FenceMenu::Entry& entry : ui::FenceMenu::entries(state)) {
        switch (entry.kind) {
        case ui::FenceMenu::Kind::Separator:
            menu.addSeparator();
            break;
        case ui::FenceMenu::Kind::Submenu: {
            QMenu* submenu = menu.addMenu(entry.label);
            submenu->setEnabled(entry.enabled);
            // Which children a submenu shows is the entry's own action, so a
            // submenu added to FenceMenu is a case here and nothing else.
            QList<ui::FenceMenu::Entry> children;
            switch (entry.action) {
            case ui::FenceMenu::Action::Material:
                children = ui::FenceMenu::materialEntries(state);
                break;
            case ui::FenceMenu::Action::Colours:
                children = ui::FenceMenu::colourEntries(state);
                break;
            case ui::FenceMenu::Action::Radius:
                children = ui::FenceMenu::radiusEntries(state);
                break;
            case ui::FenceMenu::Action::Sort:
                children = ui::FenceMenu::sortEntries(state);
                break;
            case ui::FenceMenu::Action::HoverUnroll:
                children = ui::FenceMenu::hoverUnrollEntries(state);
                break;
            case ui::FenceMenu::Action::Resize:
                children = ui::FenceMenu::resizeEntries(state);
                break;
            case ui::FenceMenu::Action::Template:
                children = ui::FenceMenu::templateEntries(state);
                break;
            case ui::FenceMenu::Action::AddWidget:
                children = ui::FenceMenu::widgetEntries(state);
                break;
            default:
                break;
            }
            // Exclusive where the choice is one of a set: a fence is shown in one
            // material and has one radius, and a radio mark that can mark two at
            // once would be describing a state the document cannot hold (the
            // material *is* the alpha of the fence's colour). The colours submenu
            // is commands, not a state, so it gets no group at all.
            const bool radio = entry.action != ui::FenceMenu::Action::Colours
                               && entry.action != ui::FenceMenu::Action::Template
                               && entry.action != ui::FenceMenu::Action::AddWidget;
            auto* group = radio ? new QActionGroup(submenu) : nullptr;
            if (group)
                group->setExclusive(true);
            for (const ui::FenceMenu::Entry& child : children) {
                QAction* choice = submenu->addAction(child.label);
                choice->setCheckable(child.checkable);
                choice->setChecked(child.checked);
                choice->setEnabled(child.enabled);
                if (group)
                    group->addAction(choice);
                if (child.action == ui::FenceMenu::Action::Material) {
                    const QString material = child.material;
                    connect(choice, &QAction::triggered, this,
                            [this, id, material] { applyMaterialToFence(id, material); });
                } else {
                    const ui::FenceMenu::Action what = child.action;
                    const int value = child.value;
                    const QString name = child.label; // a template is applied by name
                    connect(choice, &QAction::triggered, this, [this, id, what, value, name] {
                        if (what == ui::FenceMenu::Action::ApplyTemplate) {
                            if (ui::FenceTemplates::contains(name))
                                m_container->setFenceStyle(id, ui::FenceTemplates::style(name));
                            return;
                        }
                        runFenceMenuCommand(id, what, value);
                    });
                }
            }
            break;
        }
        case ui::FenceMenu::Kind::Action: {
            QAction* action = menu.addAction(entry.label);
            action->setCheckable(entry.checkable);
            action->setChecked(entry.checked);
            action->setEnabled(entry.enabled);
            const ui::FenceMenu::Action what = entry.action;
            const int value = entry.value;
            connect(action, &QAction::triggered, this,
                    [this, id, what, value] { runFenceMenuCommand(id, what, value); });
            break;
        }
        }
    }

    // Blocking, like the Shell's menu the tiles show: a context menu is the one
    // place where the product waits for the user.
    menu.exec(globalPos);
}

void ProductApp::runFenceMenuCommand(const QString& id, ui::FenceMenu::Action action, int value)
{
    // The fence may be gone by now -- the menu held the pointer for as long as the
    // user took to choose, and the tray can remove a fence in the meantime.
    if (!m_container || !m_container->contains(id))
        return;

    switch (action) {
    case ui::FenceMenu::Action::ToggleRoll:
        onRollUpToggled(id, !m_container->fenceRolledUp(id));
        break;
    case ui::FenceMenu::Action::ToggleLock: // session 94
        m_container->setFenceLocked(id, !m_container->fenceLocked(id));
        break;
    case ui::FenceMenu::Action::Rename:
        // Session 60: on the title itself, no dialog.
        startFenceTitleEdit(id);
        break;
    case ui::FenceMenu::Action::Remove:
        onRemoveFence(id);
        break;
    case ui::FenceMenu::Action::ToggleTitleBar: {
        core::FenceStyle style = m_container->fenceStyle(id);
        style.titleBar = !style.titleBar;
        m_container->setFenceStyle(id, style);
        break;
    }
    case ui::FenceMenu::Action::FadeIcons:
        // T-082: the option that puts the material on the fence's icons as well.
        // One call, through setFenceStyle: the container re-applies the material,
        // creates or drops the icon overlay, and the document follows.
        m_container->setFenceIconsFollowMaterial(id, !m_container->fenceIconsFollowMaterial(id));
        break;
    // T-080: the fence's own appearance. Like the material, every one of these is
    // a style change through setFenceStyle: the container repaints, the region
    // follows the new radius, and the auto-save sees one document change. Nothing
    // is remembered here -- the colour pickers read the *document* for their
    // starting colour and cancel changes nothing at all.
    case ui::FenceMenu::Action::PanelColor: {
        const core::FenceStyle current = m_container->fenceStyle(id);
        const QColor picked = ui::RecentColours::pick(QColor(current.background), tr("Panel colour"));
        if (!picked.isValid())
            break;
        // The same `#AARRGGBB` spelling the presets write, so a colour chosen here
        // and a material chosen there describe the same field the same way.
        const QString background = picked.name(QColor::HexArgb).toUpper();
        if (background == current.background)
            break; // re-picking the colour it already has is not a document change
        core::FenceStyle style = current;
        style.background = background;
        m_container->setFenceStyle(id, style);
        break;
    }
    case ui::FenceMenu::Action::TitleBarColor: {
        const core::FenceStyle current = m_container->fenceStyle(id);
        // The dialog opens on what the fence shows now: its band if it has one, and
        // otherwise its panel -- because that is the colour the eye sees up there.
        const QColor start = current.titleBarColor.isEmpty() ? QColor(current.background)
                                                             : QColor(current.titleBarColor);
        const QColor picked = ui::RecentColours::pick(start, tr("Title bar colour"));
        if (!picked.isValid())
            break;
        const QString band = picked.name(QColor::HexArgb).toUpper();
        if (band == current.titleBarColor)
            break;
        core::FenceStyle style = current;
        style.titleBarColor = band;
        m_container->setFenceStyle(id, style);
        break;
    }
    case ui::FenceMenu::Action::ClearTitleBarColor: {
        core::FenceStyle style = m_container->fenceStyle(id);
        if (style.titleBarColor.isEmpty())
            break;
        // Empty, not a colour of its own: "no band" is what the fence showed
        // before T-080 existed, and the key simply disappears from the document.
        style.titleBarColor.clear();
        m_container->setFenceStyle(id, style);
        break;
    }
    case ui::FenceMenu::Action::Radius: {
        if (value < 0)
            break;
        core::FenceStyle style = m_container->fenceStyle(id);
        if (style.radius == value)
            break;
        style.radius = value;
        m_container->setFenceStyle(id, style);
        break;
    }
    case ui::FenceMenu::Action::Transparency:
        editFenceTransparency(id);
        break;
    case ui::FenceMenu::Action::SortBy:
        if (value >= 0 && value < ui::FenceMenu::sortKeys().size())
            m_container->setFenceSort(id, ui::FenceMenu::sortKeys().at(value), true);
        break;
    case ui::FenceMenu::Action::ResizeChoice:
        if (value >= 0 && value < ui::FenceMenu::resizeKeys().size()) {
            core::FenceStyle style = m_container->fenceStyle(id);
            style.snapToCells = ui::FenceMenu::resizeKeys().at(value);
            m_container->setFenceStyle(id, style);
        }
        break;
    case ui::FenceMenu::Action::AddWidgetChoice: // session 79
        if (value >= 0 && value < ui::widgets::kinds().size()) {
            core::DesktopItem widget = ui::widgets::create(ui::widgets::kinds().at(value).type);
            if (widget.widgetType == QLatin1String("image")) { // session 95: the picture first
                const QString picture = ui::WidgetSettingsDialog::chooseImage(nullptr, {});
                if (picture.isEmpty())
                    break;
                widget.widgetSettings.insert(QStringLiteral("path"), picture);
            }
            m_container->addWidget(id, widget);
        }
        break;
    case ui::FenceMenu::Action::AddWidget:
    case ui::FenceMenu::Action::Resize:
    case ui::FenceMenu::Action::Template:
    case ui::FenceMenu::Action::ApplyTemplate: // by name, in showFenceMenu
        break;
    case ui::FenceMenu::Action::Appearance: {
        QDialog* dialog = ui::createFenceAppearanceDialog(m_container, id, nullptr);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        // Session 76: a double-click on the title meanwhile opened the inline
        // rename over a fence the dialog was restyling -- off until it closes.
        m_container->setFenceTitleLocked(id, true);
        dialog->exec();
        if (m_container->contains(id))
            m_container->setFenceTitleLocked(id, false);
        m_container->setNewFenceStyle(ui::FenceTemplates::newFenceStyle()); // it may have changed
        // Session 96: the dialog also sets the (global) rolling speed.
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
            .setValue(QStringLiteral("fenceRollSpeed"), m_container->rollSpeed());
        if (m_settings)
            m_settings->setRollSpeed(m_container->rollSpeed());
        break;
    }
    case ui::FenceMenu::Action::HoverUnrollChoice:
        if (value >= 0 && value < ui::FenceMenu::hoverUnrollKeys().size()) {
            core::FenceStyle style = m_container->fenceStyle(id);
            style.hoverUnroll = ui::FenceMenu::hoverUnrollKeys().at(value);
            m_container->setFenceStyle(id, style);
        }
        break;
    case ui::FenceMenu::Action::HoverUnroll: // carried by the submenu's own entries
    case ui::FenceMenu::Action::Sort: // carried by the submenu's own entries
    case ui::FenceMenu::Action::Material: // carried by the submenu's own entries
    case ui::FenceMenu::Action::Colours:  // ... and so are the colours
    case ui::FenceMenu::Action::None:
        break;
    }
}

void ProductApp::onRemoveFence(const QString& id)
{
    m_container->removeFence(id); // the files are never touched
}

void ProductApp::onRollUpToggled(const QString& id, bool rolledUp)
{
    m_container->setFenceRolledUp(id, rolledUp);
}

void ProductApp::onDesktopIconsToggled(bool hidden)
{
    if (m_options.claimDesktop && m_options.hostOnDesktop) {
        // ADR-21: "hide the desktop icons" is the drawn desktop, and showing them
        // again is the native one.
        if (hidden)
            claimDrawnDesktop();
        else
            releaseDrawnDesktop();
        if (m_tray)
            m_tray->setDesktopIconsHidden(m_iconsHidden);
        refreshStatus();
        return;
    }
    setDesktopIconsHidden(hidden);
}

// F-038 / T-057: the materials. Both are a one-line style change on purpose --
// the container turns the preset into an alpha on the fence's colour (which the
// document already carries), keeps the hosted surface on the per-pixel path (it
// is the only one that can show a rounded panel, T-075) and emits the lifecycle
// signal that auto-saves and refreshes the tray and the status line. There
// is nothing left for the product to remember.
void ProductApp::applyMaterialToFence(const QString& id, const QString& material)
{
    m_container->setFenceMaterial(id, material);
}

void ProductApp::applyMaterialToAllFences(const QString& material)
{
    const QStringList ids = m_container->fenceIds();
    for (const QString& id : ids)
        m_container->setFenceMaterial(id, material);
    qCInfo(lc).noquote()
        << QStringLiteral("material %1 -> %2 fence(s)").arg(material).arg(ids.size());
}

QString ProductApp::sharedFenceMaterial() const
{
    if (!m_container)
        return QString();
    QString shared;
    bool first = true;
    for (const QString& id : m_container->fenceIds()) {
        const QString material = m_container->fenceMaterial(id);
        if (first) {
            shared = material;
            first = false;
        } else if (material != shared) {
            // A desktop whose fences do not agree: the tray says so by claiming
            // nothing rather than by picking one of them.
            return QString();
        }
    }
    return shared;
}

bool ProductApp::desktopIconsHidden() const
{
    return m_iconsHidden;
}

void ProductApp::setDesktopIconsHidden(bool hidden)
{
    // The icon layer is only touched on a real change, and the state is only
    // remembered when the change actually took effect -- so the checkbox cannot
    // claim something the desktop is not doing.
    if (hidden != m_iconsHidden) {
        if (!m_options.hostOnDesktop) {
            m_iconsHidden = hidden; // windowed or headless: track the intent only
        } else if (hidden && !enableBackdrop()) {
            // T-039: from here on the native icon layer is the only thing drawing
            // the desktop. Taking it away without a backdrop that can be
            // transparent would leave an empty wallpaper -- so the icons stay.
            qCCritical(lc) << "the desktop backdrop could not be hosted; the native icons "
                              "stay visible";
        } else if (platform::win::DesktopSurface::setDesktopIconsHidden(hidden)) {
            m_iconsHidden = hidden;
            // Same state, same consequences: whoever hid the list, StarFish is the
            // one drawing the icons now (or no longer is). See takeOverDesktopIcons.
            m_ownsIcons = hidden;
            if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr)
                backdrop->setDrawFreeIcons(hidden);
            if (!hidden) {
                // The native layer is back first, then ours goes: the desktop is
                // never left without icons, not even for a frame.
                m_container->setBackdropEnabled(false);
            }
        } else {
            qCWarning(lc) << "could not change the desktop icon visibility";
            if (hidden)
                m_container->setBackdropEnabled(false);
        }
    }

    if (m_tray)
        m_tray->setDesktopIconsHidden(m_iconsHidden);

    refreshStatus();
}

bool ProductApp::enableBackdrop()
{
    if (!m_container)
        return false;
    // The desktop is re-read first: a file added since the last scan must be on
    // the backdrop the moment the native layer goes away, not after the next
    // watcher tick.
    refreshDesktopItems();
    if (m_container->setBackdropEnabled(true)) {
        // T-041: the backdrop is also what carries the empty desktop's own
        // gestures (F-001 draw a fence, F-022 quick hide). They go on with it,
        // because they only make sense while StarFish is the one drawing the
        // desktop -- and they are what turns the surface from a decoration into
        // the desktop's input surface.
        if (ui::DesktopBackdrop* backdrop = m_container->backdrop())
            backdrop->setGestureEnabled(true);
        return true;
    }

    qCWarning(lc) << "the backdrop could not be attached (no desktop, or the per-pixel "
                     "push was refused)";
    m_container->setBackdropEnabled(false);
    return false;
}

void ProductApp::syncNativeIcons()
{
    if (!m_container || !m_options.hostOnDesktop)
        return;
    // ADR-21: the drawn desktop reads the shell's view, not the list view, and
    // the icons a fence holds simply leave the backdrop (BackdropLayout::freeItems).
    if (drawnDesktop())
        return;
    ui::DesktopBackdrop* backdrop = m_container->backdrop();
    if (!backdrop)
        return;

    // The names a fence holds, i.e. the icons StarFish has to stop the desktop
    // from drawing because it draws its own copy inside a fence. `name` is the
    // shell's own display name on both sides (the scanner asks the shell too), and
    // the on-disk name is kept as a fallback for the types the shell strips the
    // extension from. Computed from the container, so it needs no desktop read.
    QSet<QString> held;
    for (const core::DesktopItem& item : backdrop->desktopItems()) {
        if (!backdrop->occupiedPaths().contains(ui::BackdropLayout::pathKey(item.path)))
            continue;
        held.insert(item.name);
        held.insert(item.fileName);
    }

    // Nothing is read from a list view that is not being drawn -- and that is a
    // measurement, not a precaution. With the icon layer hidden, the very same
    // view answers `LVM_GETITEMCOUNT` = 88, **every name empty**, and positions
    // running 1,1 / 1,2 / 1,3: a layout no desktop has. Two cases, and neither
    // wants a read:
    //   * the icons are ours (we hid the list), so the last good read is still the
    //     right one and the click zones keep working;
    //   * another instance is holding the desktop, and the honest thing is to
    //     touch nothing at all.
    const bool listDrawn = platform::win::nativeDesktopIconsVisible();
    if (listDrawn) {
        const QVector<platform::win::NativeDesktopIcon> icons =
            platform::win::readNativeDesktopIcons();

        // Where the user's icons are, in screen pixels: the rectangles our surfaces
        // step aside for, so every one of them keeps its own click, drag and menu.
        // The same read in the other shape is what the layout draws into when the
        // icons end up being ours (takeOverDesktopIcons).
        QVector<QRect> rects;
        QHash<QString, QRect> boxes;
        rects.reserve(icons.size());
        boxes.reserve(icons.size());
        for (const platform::win::NativeDesktopIcon& icon : icons) {
            const QRect box(icon.position, icon.slot);
            rects.append(box);
            boxes.insert(icon.name, box);
        }
        backdrop->setNativeIconBoxes(boxes);

        // Only while the user's icons are the ones on screen does the surface step
        // aside over them. With the list hidden there is nothing behind this window,
        // so a step-aside would hand the click to the wallpaper and turn a click on
        // an icon into a click on the bare desktop.
        backdrop->setNativeIconRects(m_ownsIcons ? QVector<QRect>() : rects);
    }

    // A fence that holds an icon draws it, so the desktop's own copy has to stop
    // being drawn. It is *not* moved out of the way: an icon is only ever moved by
    // a position write, and a write was measured landing the item in the top-left
    // corner of the desktop instead of the parked point (LVM_SETITEMPOSITION with
    // (-32000,-32000) reading back as (8,2)) -- which is a worse outcome than the
    // duplicate it was meant to avoid, and it cannot be undone once the record of
    // where the icon was is gone.
    //
    // ⚠️ T-066 -- and StarFish does **not** take the list over either. Hiding the
    // desktop's icons and drawing them ourselves means the icons on screen stop
    // being the user's: they cannot be clicked, dragged or right-clicked (that is
    // T-042, unwritten). [T-066, session 36: what triggered it was not a mode the
    // user chose but the *first* icon dropped into a fence -- the whole point of
    // the application -- so the desktop was taken over minutes after a normal
    // launch, with no way back except quitting. The user reported exactly that:
    // "starfish ne masque pas les icônes du bureau, affiche les fences sur le
    // bureau normal, et que ce soit comme ça dès le démarrage".]
    //
    // ADR-20 (session 57) removed the switch T-088 had added: the user tried it and
    // rejected it -- "je pense que le plus simple c'est de supprimer cette option
    // parce qu'elle ne convient pas" -- so in the **product** the takeover no longer
    // has a handle at all. It survives on `claimDesktop` only (the development path
    // and the probes), which is where a future model of the desktop gets measured.
    // Filing an icon into a fence is therefore, again, a **copy** in the product:
    // the icon shows twice, once where the desktop keeps it and once as the fence's
    // tile (F-039, assumed and stated).
    if (!m_options.claimDesktop) {
        if (m_ownsIcons)
            giveBackDesktopIcons(); // left over from the claim path, or an older run
        return;
    }

    if (!held.isEmpty() && !m_ownsIcons)
        takeOverDesktopIcons();
    else if (held.isEmpty() && m_ownsIcons)
        giveBackDesktopIcons();
}

void ProductApp::takeOverDesktopIcons()
{
    if (m_ownsIcons)
        return;

    // T-039 / T-088: the backdrop is what draws the icon we are about to stop
    // showing, so it goes on screen **first** -- and if it cannot be, the desktop
    // keeps its icons. Same discipline as the crash marker (T-046): when the safe
    // step cannot be taken, the risky one is refused. (Before T-088 the claim path
    // happened to attach it a line later; the product's own switch must not depend
    // on that ordering.)
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr) {
        if (!backdrop->enabled() && !enableBackdrop()) {
            qCWarning(lc) << "the desktop backdrop could not be hosted; the desktop keeps its "
                             "icons and a fenced icon will show twice";
            return;
        }
    }

    if (!platform::win::DesktopSurface::setDesktopIconsHidden(true)) {
        qCWarning(lc) << "could not take the desktop's icon list away; the icons a fence holds "
                         "will show twice";
        return;
    }
    m_ownsIcons = true;
    m_iconsHidden = true;
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr)
        backdrop->setDrawFreeIcons(true);
    if (m_tray)
        m_tray->setDesktopIconsHidden(true);
    refreshStatus();
}

void ProductApp::giveBackDesktopIcons()
{
    if (!m_ownsIcons)
        return;
    // The static call, not setDesktopIconsHidden(): the backdrop is *not* to be
    // detached here -- it is the bare desktop's input surface (ADR-13), and it has
    // to outlive the icons StarFish was drawing.
    if (!platform::win::DesktopSurface::setDesktopIconsHidden(false)) {
        qCWarning(lc) << "could not give the desktop's icon list back";
        return;
    }
    m_ownsIcons = false;
    m_iconsHidden = false;
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr)
        backdrop->setDrawFreeIcons(false);
    if (m_tray)
        m_tray->setDesktopIconsHidden(false);
    refreshStatus();
}

void ProductApp::refreshDesktopItems()
{
    if (!m_container)
        return;
    // ADR-21: the shell's view is the authority on what the desktop shows (the
    // Public desktop and This PC included). The watcher only says "something
    // changed"; the view needs a moment to take the change in.
    if (drawnDesktop()) {
        m_shellResync.start();
        return;
    }
    QString error;
    QList<core::DesktopItem> items = platform::win::DesktopScanner::scan(&error);
    if (!error.isEmpty())
        qCWarning(lc).noquote() << QStringLiteral("could not read the desktop: %1").arg(error);
    // The Windows desktop sorts by name unless the user turned auto-arrange off.
    // This is only the *enumeration* order: since ADR-13 the positions come from
    // the shell itself (syncNativeIcons -> the backdrop's native rectangles), and
    // the icons on screen are the native ones, in the order the user put them.
    core::sortForDisplay(items, QStringLiteral("name"), true);
    m_container->reconcileMissingItems(items); // session 74: renamed while we were away
    syncShortcutVault();
    m_container->setDesktopItems(std::move(items));
}

void ProductApp::resetDesktop(bool wipeAll)
{
    if (!m_container || !m_store)
        return;
    if (m_store->isReadOnly()) {
        qCWarning(lc).noquote() << "reset refused: the layout is read-only:" << m_store->readOnlyReason();
        return;
    }
    // The current state first on disk, then copied aside: the reset can always
    // be undone by putting that backup back as layout.json.
    m_store->setLayout(m_container->toLayout());
    m_store->saveNow();
    const QString backup = m_store->backupNow(wipeAll ? QStringLiteral("before-wipe")
                                                      : QStringLiteral("before-reset"));
    if (backup.isEmpty() && QFileInfo::exists(m_store->filePath())) {
        qCWarning(lc) << "reset cancelled: no backup could be written";
        return;
    }
    // Zones only: no file moves, Explorer's own places are left as they are, so
    // the released icons show where Windows keeps them.
    for (const QString& id : m_container->fenceIds())
        m_container->removeFence(id);
    // Session 91: the Windows desktop has no widgets -- both resets remove the
    // desktop's (the backup above keeps them).
    m_container->clearDesktopWidgets();
    if (wipeAll)
        m_container->setRules({});
    m_store->setLayout(m_container->toLayout());
    m_store->saveNow();
    m_shellSignature.clear(); // read Explorer's layout again
    m_shellResync.start();
    qCInfo(lc).noquote() << QStringLiteral("StarFish desktop %1 (backup %2)")
                                .arg(wipeAll ? QStringLiteral("wiped") : QStringLiteral("reset from Windows"), backup);
    refreshStatus();
}

void ProductApp::restoreDesktopIcons()
{
    // The emergency exit (F-034 / T-046), reachable from the tray. It is a
    // *command*, not a state toggle: it asks the desktop to show the icons and
    // believes the answer, so it also reaches the states a toggle cannot -- the
    // layer hidden by an earlier run while this one remembers nothing about it,
    // for instance. It is the one place that touches the real layer even when the
    // product is not hosting (--no-host): "give me my icons back" is a request
    // from the user to the desktop, not a piece of our own bookkeeping.
    qCInfo(lc) << "restoring the desktop icons on request";
    // ADR-21: the drawn desktop goes with them -- it only exists over a hidden list.
    if (drawnDesktop()) {
        m_shellTimer.stop();
        m_shellResync.stop();
        m_shellView.reset();
        m_ownsIcons = false;
        if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr) {
            backdrop->setWallpaper({});
            backdrop->setDrawFreeIcons(false);
        }
    }
    if (platform::win::DesktopSurface::setDesktopIconsHidden(false)) {
        m_iconsHidden = false;
    } else {
        qCWarning(lc) << "the desktop icons could not be restored";
    }
    // Whatever happened to the native layer, the backdrop has no reason to stay:
    // it exists to stand in for the icons, not to double them (T-039).
    if (m_container)
        m_container->setBackdropEnabled(false);

    if (m_tray)
        m_tray->setDesktopIconsHidden(m_iconsHidden);

    refreshStatus();
}

void ProductApp::showSettings()
{
    if (!m_settings)
        return;
    // T-059: there is nothing to rebuild any more -- the window holds no list of
    // fences to follow. Its one line is refreshed like everywhere else, so it is
    // current the moment it appears.
    refreshStatus();
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
}

void ProductApp::refreshStatus()
{
    if (!m_settings)
        return;
    m_settings->setStatusText(statusText());
    if (m_container) {
        m_settings->setFencesVisible(m_container->fencesVisible());
        m_settings->setFenceMaterial(sharedFenceMaterial());
    }
    m_settings->setDefaultTransparency(defaultTransparencySetting());
    m_settings->setFenceOverlapAllowed(allowFenceOverlapSetting());
    m_settings->setLanguage(QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
                                .value(QStringLiteral("language")).toString());
    if (m_container) {
        m_settings->setHoverHighlight(m_container->hoverHighlight());
        m_settings->setDesktopDoubleClickHides(m_doubleClickHides);
        m_settings->setHoverUnroll(m_container->hoverUnrollDefault());
        m_settings->setRollOpposite(m_container->rollOppositeDefault());
        m_settings->setRollAnimated(m_container->animateRoll());
        m_settings->setSnapToCells(m_container->snapToCellsDefault());
        m_settings->setAutoRestoreShortcuts(m_autoRestoreShortcuts);
        m_settings->setShowLabels(m_container->showLabelsDefault());
        m_settings->setSplash(QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish"), QSettings::NativeFormat)
                                  .value(QStringLiteral("showSplash"), true).toBool());
        m_settings->setIconFlow(ui::IconLayout::flowKeys().value(m_container->iconFlowDefault()));
        m_settings->setRollSpeed(m_container->rollSpeed()); // session 95
        m_settings->setDesktopDragGrid(m_desktopDragGrid);
        m_settings->setWidgetEngineStatus(widgetEngineSummary());
        m_settings->setDefaultBorder(m_container->defaultBorderWidth(), m_container->defaultBorderColor());
    }
    m_settings->setFenceShortcutArrows(m_fenceShortcutArrows);
    m_settings->setSortOnAdd(m_sortOnAdd);
    {
        const platform::win::AutostartState autostart = platform::win::Autostart().read();
        m_settings->setStartWithWindows(autostart.registered, autostart.disabledByUser);
    }
    m_settings->setDesktopShortcutArrows(m_desktopShortcutArrows);
    m_settings->setDesktopOverlapAllowed(m_allowDesktopOverlap);
}

QString ProductApp::statusText() const
{
    if (!m_container)
        return tr("not started");

    QStringList parts;
    parts << tr("%1 fence(s)").arg(m_container->fenceCount());
    // F-038 / T-057: what the fences are made of. Stated once, here, because the
    // status line is the only place the product describes itself in words -- and
    // a material that is a document value has to be readable without a screenshot.
    if (m_container->fenceCount() > 0) {
        const QString material = sharedFenceMaterial();
        parts << (material.isEmpty()
                      ? tr("materials: mixed")
                      : tr("material: %1").arg(ui::FenceMaterial::presetLabel(material)));
    }
    parts << tr("%1 rule(s)").arg(m_container->rules().size());
    parts << (m_iconsHidden ? tr("desktop icons hidden") : tr("desktop icons visible"));
    if (m_container->backdrop() && m_container->backdrop()->enabled())
        parts << m_container->backdrop()->statusText();
    parts << (m_watcher && m_watcher->isWatching()
                  ? tr("watching %1 folder(s)").arg(m_watcher->folders().size())
                  : tr("not watching any folder"));

    if (m_hotkeys) {
        if (m_hotkeys->isRegistered(kHideHotkeyId))
            parts << tr("Hide: %1").arg(
                platform::win::HotkeyManager::describe(kHideModifiers, kHideKey));
    }

    if (!m_store->filePath().isEmpty())
        parts << tr("document: %1").arg(QDir::toNativeSeparators(m_store->filePath()));

    return parts.join(QStringLiteral("  |  "));
}

void ProductApp::shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;

    // Order matters. Stop reacting to the desktop first, then write the document
    // while the fences still exist to describe, then destroy the fences -- which
    // detaches every hosted surface -- and only then give the user their icons
    // back: restoring them earlier would leave the fences drawn over the real
    // icon layer for a moment.
    if (m_newFence)
        m_newFence->stop();
    if (m_watcher)
        m_watcher->stop();
    if (m_hotkeys)
        m_hotkeys->unregisterAll();

    // Session 81: a note being written is part of the document -- it goes in
    // before the last write, not after it.
    finishWidgetEdit(true);

    if (m_store) {
        QString error;
        if (!m_store->saveNow(&error))
            qCWarning(lc).noquote() << QStringLiteral("last write failed: %1").arg(error);
    }

    delete m_settings;
    m_settings = nullptr;
    delete m_tray;
    m_tray = nullptr;

    // ADR-21: under the drawn desktop the list comes back *first* -- it appears
    // under our opaque backdrop, invisible, and is what shows the moment the
    // backdrop goes. The other order would flash a desktop without icons.
    finishRename(false);
    m_shellTimer.stop();
    m_shellResync.stop();
    m_reclaimTimer.stop();
    if (drawnDesktop() && m_iconsHidden) {
        platform::win::DesktopSurface::setDesktopIconsHidden(false);
        m_iconsHidden = false;
    }
    m_shellView.reset();

    delete m_container;
    m_container = nullptr;

    // The layer we hid -- if we hid one -- comes back last, after the fences are
    // gone, so the desktop is never left showing an icon twice or none at all. The
    // icons themselves were never moved, so there is nothing else to put back.

    // Restore only what we hid ourselves (see setDesktopIconsHidden).
    if (m_iconsHidden) {
        platform::win::DesktopSurface::setDesktopIconsHidden(false);
        m_iconsHidden = false;
    }

    m_started = false;
}

// ---------------------------------------------------------------------------
// ADR-21 -- the drawn desktop
// ---------------------------------------------------------------------------

namespace {

// A cheap fingerprint of a frame: every 97th pixel. Enough to tell one
// wallpaper from another, at a fraction of a full comparison.
quint64 imageSignature(const QImage& image)
{
    if (image.isNull())
        return 0;
    const QImage rgb = image.format() == QImage::Format_ARGB32 ? image
                                                              : image.convertToFormat(QImage::Format_ARGB32);
    const auto* pixels = reinterpret_cast<const quint32*>(rgb.constBits());
    const qsizetype count = rgb.sizeInBytes() / 4;
    quint64 h = 1469598103934665603ULL;
    for (qsizetype i = 0; i < count; i += 97) {
        h ^= pixels[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// The wallpaper as the shell renders it with its list hidden. The view repaints
// in its own process, so the capture is retried until two in a row agree --
// a frame caught mid-repaint would still carry icons.
QImage settledWallpaper(const QRect& area)
{
    QImage previous;
    for (int attempt = 0; attempt < 6; ++attempt) {
        QThread::msleep(120);
        QImage current = platform::win::DesktopShellView::captureWallpaper(area);
        if (current.isNull())
            return {};
        if (!previous.isNull() && imageSignature(current) == imageSignature(previous))
            return current;
        previous = std::move(current);
    }
    return previous;
}

} // namespace

quintptr ProductApp::backdropOwner() const
{
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr) {
        if (platform::win::DesktopSurface* surface = backdrop->surface())
            return surface->hwnd();
    }
    return 0;
}

void ProductApp::refreshDrawnDesktopForScreens()
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    if (!backdrop || !drawnDesktop())
        return;
    // The metrics depend on the primary monitor's scale and the boxes on where
    // the monitors are: the next read rebuilds both, whatever its signature.
    m_shellSignature.clear();
    m_shellResync.start();
    if (backdrop->wallpaper().size() == backdrop->area().size())
        return;
    // A wallpaper the size of the old virtual screen would be drawn at the
    // wrong scale: captured again, the list being hidden (ADR-21).
    const QImage wallpaper = settledWallpaper(backdrop->area());
    if (wallpaper.isNull()) {
        qCWarning(lc) << "the screens changed and the wallpaper could not be captured again";
        return;
    }
    m_wallpaperSignature = imageSignature(wallpaper);
    backdrop->setWallpaper(wallpaper);
    qCInfo(lc).noquote() << QStringLiteral("the screens changed: the drawn desktop is now %1x%2")
                                .arg(wallpaper.width())
                                .arg(wallpaper.height());
}

bool ProductApp::claimDrawnDesktop()
{
    if (drawnDesktop())
        return true;
    if (!m_container || !m_options.hostOnDesktop)
        return false;
    ui::DesktopBackdrop* backdrop = m_container->backdrop();
    if (!backdrop)
        return false;

    // 1. What the desktop shows, read while it still shows it.
    m_shellView = std::make_unique<platform::win::DesktopShellView>();
    m_shellSignature.clear();
    syncShellView();
    if (m_shellSignature.isEmpty()) {
        qCWarning(lc) << "the desktop's shell view is unreachable; the desktop stays native";
        m_shellView.reset();
        return false;
    }

    // 2. The list goes -- marker first (T-046): from here on a crash leaves a
    //    record, and the next launch, the tray and an Explorer restart all bring
    //    the icons back.
    if (!platform::win::DesktopSurface::setDesktopIconsHidden(true)) {
        qCWarning(lc) << "the desktop's icon list could not be hidden; the desktop stays native";
        m_shellView.reset();
        return false;
    }
    m_iconsHidden = true;
    m_ownsIcons = true;

    // 3. The wallpaper as the shell renders it without its icons, then our
    //    desktop over it. Any failure gives the list back.
    const QImage wallpaper = settledWallpaper(backdrop->area());
    if (!wallpaper.isNull()) {
        backdrop->setWallpaper(wallpaper);
        m_wallpaperSignature = imageSignature(wallpaper);
        backdrop->setDrawFreeIcons(true);
    }
    if (wallpaper.isNull() || !enableBackdrop() || !backdrop->drawsWholeDesktop()) {
        qCWarning(lc) << "the drawn desktop could not be hosted (wallpaper"
                      << (wallpaper.isNull() ? "missing)" : "ok)") << "; the icons come back";
        platform::win::DesktopSurface::setDesktopIconsHidden(false);
        m_iconsHidden = false;
        m_ownsIcons = false;
        backdrop->setWallpaper({});
        backdrop->setDrawFreeIcons(false);
        m_container->setBackdropEnabled(false);
        m_shellView.reset();
        return false;
    }

    m_shellTimer.start();
    if (m_tray)
        m_tray->setDesktopIconsHidden(true);
    qCInfo(lc).noquote() << QStringLiteral("the desktop is drawn by StarFish: %1 icon(s)")
                                .arg(backdrop->freeCount());
    refreshStatus();
    return true;
}

namespace {
const QString kSettingsKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\StarFish");
const QString kFenceArrows = QStringLiteral("fenceShortcutArrows");
const QString kSortOnAdd = QStringLiteral("sortIconsOnAdd");
const QString kDesktopArrows = QStringLiteral("desktopShortcutArrows");
const QString kDesktopOverlap = QStringLiteral("allowDesktopOverlap");
const QString kFenceTransparency = QStringLiteral("fenceDefaultTransparency");
const QString kFenceOverlap = QStringLiteral("allowFenceOverlap");
} // namespace

bool ProductApp::allowFenceOverlapSetting()
{
    return QSettings(kSettingsKey, QSettings::NativeFormat).value(kFenceOverlap, true).toBool();
}

void ProductApp::setAllowFenceOverlap(bool allowed)
{
    QSettings(kSettingsKey, QSettings::NativeFormat).setValue(kFenceOverlap, allowed);
    if (m_container)
        m_container->setAllowFenceOverlap(allowed);
    if (m_settings)
        m_settings->setFenceOverlapAllowed(allowed);
    qCInfo(lc) << "fences may overlap:" << allowed;
}

int ProductApp::defaultTransparencySetting()
{
    // 0 % when never set: the fences look as they always did.
    return qBound(0, QSettings(kSettingsKey, QSettings::NativeFormat).value(kFenceTransparency, 0).toInt(), 100);
}

void ProductApp::setDefaultFenceTransparency(int percent)
{
    percent = qBound(0, percent, 100);
    QSettings(kSettingsKey, QSettings::NativeFormat).setValue(kFenceTransparency, percent);
    if (m_container)
        m_container->setDefaultFenceAlpha(ui::PercentSlider::alphaFromPercent(percent));
    qCInfo(lc).noquote() << QStringLiteral("default fence transparency %1 %").arg(percent);
}

void ProductApp::editFenceTransparency(const QString& id)
{
    if (!m_container || !m_container->fenceIds().contains(id))
        return;
    QDialog* dialog = ui::createFenceTransparencyDialog(m_container, id, nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->exec();
}

bool ProductApp::sortOnAddSetting()
{
    return QSettings(kSettingsKey, QSettings::NativeFormat).value(kSortOnAdd, false).toBool();
}

bool ProductApp::desktopShortcutArrowsSetting()
{
    return QSettings(kSettingsKey, QSettings::NativeFormat).value(kDesktopArrows, true).toBool();
}

bool ProductApp::allowDesktopOverlapSetting()
{
    return QSettings(kSettingsKey, QSettings::NativeFormat).value(kDesktopOverlap, false).toBool();
}

void ProductApp::setStartWithWindows(bool on)
{
    platform::win::Autostart autostart;
    const bool done = autostart.setEnabled(on, QCoreApplication::applicationFilePath());
    const platform::win::AutostartState state = autostart.read();
    if (m_settings)
        m_settings->setStartWithWindows(state.registered, state.disabledByUser);
    qCInfo(lc).noquote() << QStringLiteral("start with Windows: %1%2")
                                .arg(state.enabled() ? QLatin1String("on") : QLatin1String("off"))
                                .arg(done ? QString() : QStringLiteral(" (the registry refused)"));
}

void ProductApp::setSortOnAdd(bool sorted)
{
    QSettings(kSettingsKey, QSettings::NativeFormat).setValue(kSortOnAdd, sorted);
    m_sortOnAdd = sorted;
    if (m_container)
        m_container->setSortOnAdd(sorted);
    if (m_settings)
        m_settings->setSortOnAdd(sorted);
    qCInfo(lc) << "icons sorted by name when added to a fence:" << sorted;
}

void ProductApp::setDesktopShortcutArrows(bool shown)
{
    QSettings(kSettingsKey, QSettings::NativeFormat).setValue(kDesktopArrows, shown);
    if (m_desktopShortcutArrows == shown)
        return;
    m_desktopShortcutArrows = shown;
    if (m_container)
        m_container->refreshBackdropIcons();
    if (m_settings)
        m_settings->setDesktopShortcutArrows(shown);
    qCInfo(lc) << "shortcut arrows on desktop icons:" << (shown ? "shown" : "hidden");
}

void ProductApp::setAllowDesktopOverlap(bool allowed)
{
    QSettings(kSettingsKey, QSettings::NativeFormat).setValue(kDesktopOverlap, allowed);
    m_allowDesktopOverlap = allowed;
    if (m_settings)
        m_settings->setDesktopOverlapAllowed(allowed);
    qCInfo(lc) << "desktop icons may sit under a fence:" << allowed;
    if (!allowed)
        scheduleOverlapSweep();
}

void ProductApp::scheduleOverlapSweep()
{
    if (!m_allowDesktopOverlap && drawnDesktop())
        m_overlapTimer.start();
}

int ProductApp::resolveDesktopOverlaps()
{
    if (m_allowDesktopOverlap || !drawnDesktop() || !m_shellView || !m_container
        || !m_shellIconsShown || m_itemPositions.isEmpty())
        return 0;
    ui::DesktopBackdrop* backdrop = m_container->backdrop();
    if (!backdrop)
        return 0;
    // Not while a button is down: a fence still being dragged would push the
    // icons around under the user's hand. Tried again a moment later.
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) {
        m_overlapTimer.start();
        return 0;
    }
    const QList<QRect> obstacles = m_container->fenceObstacleRects();
    if (obstacles.isEmpty())
        return 0;
    const QList<QRect> areas = desktopWorkAreas();
    const core::OverlapSlot slot = desktopSlot();
    // Filed icons are not on the desktop (hidden there): never moved.
    const QSet<QString> filed = m_container->filedPathKeys();
    QList<QPair<QString, QPoint>> items;
    for (auto it = m_itemPositions.constBegin(); it != m_itemPositions.constEnd(); ++it) {
        if (!filed.contains(ui::BackdropLayout::pathKey(it.key())))
            items.append({it.key(), it.value()});
    }
    const QList<core::OverlapMove> moves = core::OverlapPlanner::plan(items, obstacles, areas, slot);
    int moved = 0;
    for (const core::OverlapMove& move : moves) {
        if (m_shellView->positionItem(move.key, move.to)) {
            m_itemPositions.insert(move.key, move.to);
            ++moved;
        }
    }
    if (moved > 0) {
        qCInfo(lc) << moved << "desktop icon(s) moved out from under a fence";
        m_shellSignature.clear();
        m_shellResync.start();
    }
    return moved;
}

QList<QRect> ProductApp::desktopWorkAreas() const
{
    // The work areas, physical (the same convention as screenLayout()).
    QList<QRect> areas;
    for (QScreen* screen : QGuiApplication::screens()) {
        const qreal dpr = screen->devicePixelRatio();
        const QRect a = screen->availableGeometry();
        areas.append(QRect(qRound(a.x() * dpr), qRound(a.y() * dpr), qRound(a.width() * dpr),
                           qRound(a.height() * dpr)));
    }
    return areas;
}

core::OverlapSlot ProductApp::desktopSlot() const
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    const ui::ScreenInfo* primary = backdrop ? backdrop->screens().primary() : nullptr;
    const qreal scale = primary ? qMax(1.0, primary->devicePixelRatio) : 1.0;
    core::OverlapSlot slot;
    slot.iconSize = qMax(16, m_shellIconSize);
    slot.spacing = m_shellSpacing.isValid() && !m_shellSpacing.isEmpty()
                       ? m_shellSpacing
                       : QSize(slot.iconSize + 47, slot.iconSize + 50);
    slot.top = qRound(5 * scale);
    return slot;
}

QString ProductApp::shellNameOf(const QString& path) const
{
    if (m_itemPositions.contains(path))
        return path;
    return m_shellNames.value(ui::BackdropLayout::pathKey(path));
}

void ProductApp::startFenceTitleEdit(const QString& id)
{
    if (!m_container || !m_container->contains(id) || m_container->fenceLocked(id))
        return;
    finishFenceTitleEdit(false);
    const QRect title = m_container->fenceTitleScreenRect(id); // logical
    if (title.isNull())
        return;
    auto* edit = new RenameEdit;
    edit->setObjectName(QStringLiteral("fenceTitleEdit"));
    edit->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    // The fence's title, bold as it is drawn, on a light field so the edit reads
    // as one.
    QFont font(QStringLiteral("Segoe UI"), 9);
    font.setBold(true);
    edit->setFont(font);
    edit->setStyleSheet(QStringLiteral("QLineEdit { background: #FFFFFF; color: #000000; "
                                       "border: 1px solid #0078D7; border-radius: 3px; "
                                       "selection-background-color: #0078D7; "
                                       "selection-color: #FFFFFF; padding: 0 3px; }"));
    edit->setText(m_container->fenceTitle(id));
    edit->selectAll();
    // Session 68: a vertical title (left / right bar) is edited across, laid
    // over the fence from its bar.
    if (title.height() > title.width()) {
        const int width = qMax(title.height() + 48, 160);
        const bool right = m_container->fenceStyle(id).titleSide == QLatin1String("right");
        edit->setGeometry(right ? title.right() + 4 - width : title.left() - 4, title.center().y() - 12, width, 24);
    } else {
        const int width = qMax(title.width() + 48, 160);
        edit->setGeometry(title.left() - 4, title.center().y() - 12, width, 24);
    }
    edit->onCancel = [this] { finishFenceTitleEdit(false); };
    connect(edit, &QLineEdit::editingFinished, this, [this] { finishFenceTitleEdit(true); });
    m_titleEdit = edit;
    m_titleEditId = id;
    edit->show();
    edit->raise();
    edit->activateWindow();
    edit->setFocus();
    qCInfo(lc).noquote() << QStringLiteral("renaming fence %1 on its title").arg(id);
}

void ProductApp::finishFenceTitleEdit(bool commit)
{
    if (!m_titleEdit)
        return;
    QLineEdit* edit = m_titleEdit;
    m_titleEdit = nullptr; // editingFinished fires again on hide
    const QString id = m_titleEditId;
    const QString name = edit->text().trimmed();
    // Session 62: a late editingFinished of this edit (focus lost while it
    // waits for deleteLater) must not close the next one with its old text.
    edit->disconnect(this);
    edit->hide();
    edit->deleteLater();
    // An empty or unchanged name changes nothing; stored trimmed (a title of
    // spaces would look unnamed).
    if (commit && m_container && m_container->contains(id) && !name.isEmpty()
        && name != m_container->fenceTitle(id))
        onRenameFence(id, name);
}

bool ProductApp::fenceShortcutArrowsSetting()
{
    return QSettings(kSettingsKey, QSettings::NativeFormat).value(kFenceArrows, true).toBool();
}

void ProductApp::setFenceShortcutArrows(bool shown)
{
    QSettings(kSettingsKey, QSettings::NativeFormat).setValue(kFenceArrows, shown);
    if (m_fenceShortcutArrows == shown)
        return;
    m_fenceShortcutArrows = shown;
    if (m_container)
        m_container->refreshFenceIcons();
    if (m_tray)
        m_tray->setFenceShortcutArrows(shown);
    if (m_settings)
        m_settings->setFenceShortcutArrows(shown);
    qCInfo(lc) << "shortcut arrows on fence tiles:" << (shown ? "shown" : "hidden");
}

void ProductApp::onDesktopLost()
{
    if (!drawnDesktop())
        return;
    ++m_desktopLosses;
    qCWarning(lc) << "the desktop window went away (Explorer restarting?): the drawn desktop "
                     "is dropped until the desktop is back";
    releaseDrawnDesktop();
    m_reclaimTries = 0;
    m_reclaimSettled = 0;
    m_reclaimTimer.start();
}

void ProductApp::tryReclaimDrawnDesktop()
{
    if (drawnDesktop()) {
        m_reclaimTimer.stop();
        return;
    }
    ++m_reclaimTries;
    // Explorer comes back in stages -- Progman, the view, then its items: a
    // desktop seen on two ticks in a row is one that has settled.
    m_reclaimSettled =
        platform::win::DesktopSurface::isDesktopAvailable() ? m_reclaimSettled + 1 : 0;
    if (m_reclaimSettled >= 2 && claimDrawnDesktop()) {
        m_reclaimTimer.stop();
        qCInfo(lc) << "the desktop is back: drawn by StarFish again";
        return;
    }
    if (m_reclaimTries >= 30) {
        m_reclaimTimer.stop();
        // A list hidden by a half-finished attempt is given back, marker last.
        if (m_iconsHidden && platform::win::DesktopSurface::setDesktopIconsHidden(false))
            m_iconsHidden = false;
        qCWarning(lc) << "the desktop did not come back in a minute: it stays native";
        refreshStatus();
    }
}

void ProductApp::releaseDrawnDesktop()
{
    m_reclaimTimer.stop();
    m_shellTimer.stop();
    m_shellResync.stop();
    // The list first, under our opaque frame; then our frame goes (see shutdown).
    if (m_iconsHidden && platform::win::DesktopSurface::setDesktopIconsHidden(false))
        m_iconsHidden = false;
    m_ownsIcons = false;
    m_shellView.reset();
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr) {
        backdrop->setWallpaper({});
        backdrop->setDrawFreeIcons(false);
    }
    if (m_container)
        m_container->setBackdropEnabled(false);
    refreshStatus();
}

void ProductApp::syncShellView()
{
    if (!m_shellView || !m_container)
        return;
    ui::DesktopBackdrop* backdrop = m_container->backdrop();
    if (!backdrop)
        return;

    const platform::win::ShellViewState state = m_shellView->read();
    if (!state.ok)
        return; // Explorer away for a moment: the last good desktop stays on screen

    // Nothing to do when nothing moved -- this runs every 1.5 s.
    QString signature = QStringLiteral("%1x%2/%3/%4|")
                            .arg(state.spacing.width())
                            .arg(state.spacing.height())
                            .arg(state.iconSize)
                            .arg(state.iconsShown);
    for (const platform::win::ShellViewItem& item : state.items)
        signature += QStringLiteral("%1\x1f%2\x1f%3,%4\x1e")
                         .arg(item.parsingName, item.displayName)
                         .arg(item.position.x())
                         .arg(item.position.y());
    if (signature == m_shellSignature)
        return;
    m_shellSignature = signature;
    m_shellSpacing = state.spacing;
    m_shellIconSize = state.iconSize;
    syncDesktopWidgetCell(); // session 91: a desktop widget spans icon slots
    m_shellIconsShown = state.iconsShown;
    // Session 60: a new icon (an installer's shortcut) or one put back may land
    // under a fence -- checked once the view is quiet.
    scheduleOverlapSweep();
    m_itemPositions.clear();
    m_shellNames.clear();
    m_shellSnap = state.snapToGrid;
    m_shellAutoArrange = state.autoArrange;
    for (const platform::win::ShellViewItem& item : state.items) {
        m_itemPositions.insert(item.parsingName, item.position);
        m_shellNames.insert(ui::BackdropLayout::pathKey(item.parsingName), item.parsingName);
    }
    // A drop from elsewhere: what it created goes under the cursor, side by
    // side, as on the native desktop (the hidden view would put it in a free slot).
    if (m_foreignDropUntil > 0) {
        if (QDateTime::currentMSecsSinceEpoch() > m_foreignDropUntil) {
            m_foreignDropUntil = 0;
        } else {
            int placed = 0;
            const int stepX = qMax(state.spacing.width(), 1);
            for (const platform::win::ShellViewItem& item : state.items) {
                if (m_namesBeforeDrop.contains(item.parsingName))
                    continue;
                // Only what the drop made: a file of the same name, its copy
                // ("x - Copie") or its shortcut ("x - Raccourci"). A drop with no
                // file names (virtual items) takes what appears first.
                const QString stem = QFileInfo(item.parsingName).completeBaseName();
                bool ours = m_dropStems.isEmpty() && placed == 0;
                for (const QString& s : std::as_const(m_dropStems))
                    ours = ours || stem.startsWith(s, Qt::CaseInsensitive);
                if (!ours)
                    continue;
                m_namesBeforeDrop.insert(item.parsingName);
                const QPoint topLeft = m_foreignDropAt
                                       - QPoint(stepX / 2, state.iconSize / 2 + 8)
                                       + QPoint(placed * stepX, 0);
                if (m_shellView->positionItem(item.parsingName, topLeft))
                    ++placed;
            }
            if (placed > 0) {
                qCInfo(lc) << placed << "dropped item(s) placed under the cursor";
                m_shellSignature.clear(); // read the positions the view kept
                m_shellResync.start();
            }
        }
    }
    if (m_renameNewUntil > 0) {
        if (QDateTime::currentMSecsSinceEpoch() > m_renameNewUntil) {
            m_renameNewUntil = 0;
        } else {
            for (const platform::win::ShellViewItem& item : state.items) {
                if (!m_namesBeforeNew.contains(item.parsingName)) {
                    m_renameNewUntil = 0;
                    const QString created = item.parsingName;
                    // Once the backdrop has laid it out (its caption is where
                    // the edit goes).
                    QTimer::singleShot(200, this, [this, created] { startRename(created); });
                    break;
                }
            }
        }
    }

    // The metrics the user chose (View > Large / Medium / Small icons), in the
    // backdrop's logical units: it scales them back by the primary monitor's ratio.
    const ui::ScreenInfo* primary = backdrop->screens().primary();
    const qreal scale = primary ? qMax(1.0, primary->devicePixelRatio) : 1.0;
    const int iconSize = qMax(16, state.iconSize);
    const QSize spacing = state.spacing.isValid() && !state.spacing.isEmpty()
                              ? state.spacing
                              : QSize(iconSize + 47, iconSize + 50);
    const int top = qRound(5 * scale); // the icon sits a few pixels under its slot's top (measured: 5 at 100 %)
    ui::BackdropLayout::Metrics metrics = backdrop->metrics();
    metrics.iconSize = qRound(iconSize / scale);
    metrics.cellWidth = qRound(spacing.width() / scale);
    metrics.labelHeight = qMax(0, qRound((spacing.height() - iconSize - top) / scale));
    backdrop->setMetrics(metrics);

    QList<core::DesktopItem> items;
    QHash<QString, QRect> boxes;
    if (state.iconsShown && !m_desktopHiddenByUser) {
        items.reserve(state.items.size());
        for (const platform::win::ShellViewItem& view : state.items) {
            core::DesktopItem item;
            const auto cached = m_itemCache.constFind(view.parsingName);
            if (cached != m_itemCache.constEnd()) {
                item = cached.value();
            } else {
                if (!view.parsingName.startsWith(QLatin1String("::"))
                    && core::entryExists(view.parsingName)) { // session 75: not the .lnk's target
                    item = platform::win::DesktopScanner::itemForPath(view.parsingName);
                }
                if (item.path.isEmpty()) {
                    item.path = view.parsingName;
                    item.fileName = view.displayName;
                }
                m_itemCache.insert(view.parsingName, item);
            }
            // The view's own spelling wins, and what it shows is shown -- hidden
            // files included when the user asked Explorer to show them.
            item.name = view.displayName;
            item.isHidden = false;
            items.append(item);
            boxes.insert(ui::BackdropLayout::pathKey(item.path),
                         QRect(view.position.x() - (spacing.width() - iconSize) / 2,
                               view.position.y() + top, spacing.width(),
                               spacing.height() - top));
        }
    }
    backdrop->setNativeIconBoxes(boxes);
    // Session 74: a fenced file renamed (here or while StarFish was closed)
    // is found again before the desktop draws it as a free icon.
    if (!items.isEmpty()) {
        m_container->reconcileMissingItems(items);
        syncShortcutVault(); // session 75
    }
    m_container->setDesktopItems(std::move(items));
}

void ProductApp::setDesktopHiddenByUser(bool hidden)
{
    if (!m_container)
        return;
    m_desktopHiddenByUser = hidden;
    if (m_container->fencesVisible() == hidden)
        m_container->toggleFencesVisible();
    m_shellSignature.clear();
    syncShellView();
    qCInfo(lc) << (hidden ? "desktop hidden by a double-click" : "desktop shown again");
}

void ProductApp::refreshWallpaper()
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    if (!backdrop || !drawnDesktop() || platform::win::nativeDesktopIconsVisible())
        return; // never a capture with the icons in it
    const QImage wallpaper = platform::win::DesktopShellView::captureWallpaper(backdrop->area());
    const quint64 signature = imageSignature(wallpaper);
    if (wallpaper.isNull() || signature == m_wallpaperSignature)
        return;
    m_wallpaperSignature = signature;
    backdrop->setWallpaper(wallpaper);
    qCInfo(lc) << "the wallpaper changed: redrawn";
}

void ProductApp::onIconMenu(const QStringList& paths, const QPoint& point)
{
    if (!drawnDesktop() || paths.isEmpty())
        return;
    // ADR-21: the item's **real** menu -- the native view's selection is made
    // ours, and its own menu (SVGIO_SELECTION) is shown: Open, Run as
    // administrator, Pin, Send to, Copy, Delete, Properties and every extension.
    bool rename = false;
    if (modernContextMenuWanted())
        showModernContextMenu(core::ContextMenuKind::Selection, paths, point, {}, &rename);
    else
        m_shellView->showSelectionMenu(paths, point, backdropOwner(), {}, &rename);
    if (rename && paths.size() >= 1)
        startRename(paths.first()); // the shell's edit would happen in the hidden list
    m_shellResync.start();
}

void ProductApp::syncShortcutVault()
{
    if (!m_vault || !m_container)
        return;
    const QStringList fenced = m_container->fencedFilePaths();
    m_vault->sync(fenced);
    if (!m_autoRestoreDone) {
        m_autoRestoreDone = true;
        if (m_autoRestoreShortcuts) {
            // Session 75, optional (Settings): at startup, the fenced shortcuts
            // deleted while StarFish was away come back from their copies.
            int restored = 0;
            for (const QString& path : fenced) {
                if (!core::entryExists(path) && m_vault->restore(path)) {
                    ++restored;
                    qCInfo(lc).noquote() << QStringLiteral("shortcut restored at startup: %1").arg(path);
                }
            }
            if (restored > 0)
                m_container->reconcileMissingItems({});
        }
    }
    m_container->markRestorable([this](const QString& path) { return m_vault->has(path); });
}

void ProductApp::showWidgetMenu(const QString& fenceId, const QString& path, const QPoint& point)
{
    ui::FenceView* view = m_container->view(fenceId);
    if (!view)
        return;
    core::DesktopItem widget;
    for (const core::DesktopItem& item : view->items())
        if (item.path == path)
            widget = item;
    if (!widget.isWidget())
        return;
    QMenu menu;
    ui::styleModernMenu(&menu, ui::ModernMenuStyle::current());
    QAction* title = menu.addAction(widget.name);
    title->setEnabled(false);
    menu.addSeparator();
    // Session 81: a note is written here (or by a double-click on it).
    QAction* edit = ui::widgets::isEditable(widget.widgetType)
                        ? menu.addAction(QCoreApplication::translate("ProductApp", "Edit the note"))
                        : nullptr;
    // Session 88: every option of the widget, in a window of its own.
    QAction* configure = menu.addAction(QCoreApplication::translate("ProductApp", "Configure widget…"));
    QList<QPair<QAction*, QString>> toggles;
    if (widget.widgetType == QLatin1String("clock")) {
        const auto toggle = [&](const QString& key, const QString& label) {
            QAction* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(widget.widgetSettings.value(key).toBool());
            toggles.append({action, key});
        };
        toggle(QStringLiteral("seconds"), QCoreApplication::translate("ProductApp", "Show seconds"));
        toggle(QStringLiteral("date"), QCoreApplication::translate("ProductApp", "Show the date"));
        toggle(QStringLiteral("h12"), QCoreApplication::translate("ProductApp", "12-hour clock"));
    }
    QMenu* sizes = menu.addMenu(QCoreApplication::translate("ProductApp", "Size"));
    ui::styleModernMenu(sizes, ui::ModernMenuStyle::current());
    auto* group = new QActionGroup(sizes);
    QHash<QAction*, QSize> spanOf;
    for (const QSize& span : ui::widgets::spanChoices()) {
        QAction* action = sizes->addAction(QStringLiteral("%1 \u00D7 %2").arg(span.width()).arg(span.height()));
        action->setCheckable(true);
        action->setChecked(span == widget.span);
        group->addAction(action);
        spanOf.insert(action, span);
    }
    sizes->addSeparator(); // session 96: any other size, in the widget's settings
    QAction* otherSize = sizes->addAction(QCoreApplication::translate("ProductApp", "Other size…"));
    menu.addSeparator();
    QAction* remove = menu.addAction(QCoreApplication::translate("ProductApp", "Remove the widget"));
    QAction* chosen = menu.exec(logicalFromPhysical(point));
    if (!chosen || !m_container->contains(fenceId))
        return;
    if (edit && chosen == edit) {
        startWidgetEdit(fenceId, path);
        return;
    }
    if (chosen == remove) {
        m_container->releaseItems(fenceId, {path}); // the widget only: no file behind it
        return;
    }
    if (chosen == configure || chosen == otherSize) {
        ui::WidgetSettingsDialog dialog(widget);
        dialog.move(logicalFromPhysical(point));
        if (dialog.exec() == QDialog::Accepted && m_container->contains(fenceId))
            m_container->replaceFenceItem(fenceId, path, dialog.result());
        return;
    }
    for (const auto& toggle : std::as_const(toggles)) {
        if (toggle.first == chosen)
            widget.widgetSettings.insert(toggle.second, chosen->isChecked());
    }
    if (spanOf.contains(chosen))
        widget.span = spanOf.value(chosen);
    m_container->replaceFenceItem(fenceId, path, widget);
}

// --- session 91: widgets on the desktop ---------------------------------------

void ProductApp::syncDesktopWidgetCell()
{
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr)
        backdrop->setDesktopWidgetCell(desktopSlot().spacing);
}

QPoint ProductApp::snapDesktopWidget(const QPoint& pos, const QSize& span) const
{
    // As an icon: on the desktop's grid when Explorer aligns to it, kept on a
    // work area. `pos` is the widget's top-left = its first slot's box.
    const core::OverlapSlot slot = desktopSlot();
    const QList<QRect> areas = desktopWorkAreas();
    QPoint at = pos;
    if (m_shellSnap && !m_shellAutoArrange) {
        QList<QPair<QString, QPoint>> items;
        for (auto it = m_itemPositions.constBegin(); it != m_itemPositions.constEnd(); ++it)
            items.append({it.key(), it.value()});
        // The slot's box -> the icon position the grid is defined on, and back.
        const QPoint toIcon((slot.spacing.width() - slot.iconSize) / 2, -slot.top);
        const QPoint icon = core::OverlapPlanner::snapToGrid(
            pos + toIcon + QPoint(slot.spacing.width() / 2, slot.spacing.height() / 2) - QPoint(slot.spacing.width() / 2, slot.spacing.height() / 2),
            core::OverlapPlanner::gridOrigin(items, areas, slot), slot);
        at = slot.boxAt(icon).topLeft();
    }
    const QSize size(span.width() * slot.spacing.width(), span.height() * slot.spacing.height());
    QRect area;
    for (const QRect& a : areas)
        if (a.contains(pos + QPoint(size.width() / 2, size.height() / 2)))
            area = a;
    if (area.isEmpty() && !areas.isEmpty())
        area = areas.first();
    if (!area.isEmpty()) {
        at.setX(qBound(area.left(), at.x(), qMax(area.left(), area.right() + 1 - size.width())));
        at.setY(qBound(area.top(), at.y(), qMax(area.top(), area.bottom() + 1 - size.height())));
    }
    return at;
}

void ProductApp::showDesktopWidgetGrid(const QRect& rect)
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    if (!backdrop)
        return;
    if (!m_desktopDragGrid || !drawnDesktop() || m_shellAutoArrange || !m_shellSnap) {
        backdrop->clearDropGrid();
        return;
    }
    const core::OverlapSlot slot = desktopSlot();
    const QList<QRect> areas = desktopWorkAreas();
    const QSet<QString> filed = m_container->filedPathKeys();
    QList<QPair<QString, QPoint>> items;
    for (auto it = m_itemPositions.constBegin(); it != m_itemPositions.constEnd(); ++it) {
        if (!filed.contains(ui::BackdropLayout::pathKey(it.key())))
            items.append({it.key(), it.value()});
    }
    const QSize span(qMax(1, rect.width() / qMax(1, slot.spacing.width())),
                     qMax(1, rect.height() / qMax(1, slot.spacing.height())));
    const QPoint at = snapDesktopWidget(rect.topLeft(), span);
    backdrop->setDropGrid(core::OverlapPlanner::gridSlots(items, areas, slot), QRect(at, rect.size()));
}

QString ProductApp::addDesktopWidgetAt(const QString& type, const QPoint& point)
{
    if (!m_container)
        return {};
    core::DesktopItem widget = ui::widgets::create(type);
    if (!widget.isWidget())
        return {};
    if (type == QLatin1String("image")) { // session 95: the picture first
        const QString picture = ui::WidgetSettingsDialog::chooseImage(nullptr, {});
        if (picture.isEmpty())
            return {};
        widget.widgetSettings.insert(QStringLiteral("path"), picture);
    }
    syncDesktopWidgetCell();
    // Centred on the click's slot, then on the grid like an icon.
    const QPoint at = snapDesktopWidget(point - QPoint(desktopSlot().spacing.width() / 2, desktopSlot().spacing.height() / 2),
                                        widget.span);
    if (!m_container->addDesktopWidget(widget, at))
        return {};
    qCInfo(lc).noquote() << QStringLiteral("desktop widget %1 (%2) at %3,%4").arg(widget.path, type).arg(at.x()).arg(at.y());
    return widget.path;
}

void ProductApp::showDesktopWidgetMenu(const QString& path, const QPoint& point)
{
    if (!m_container)
        return;
    core::DesktopItem widget;
    for (const ui::DesktopWidget& d : m_container->desktopWidgets())
        if (d.item.path == path)
            widget = d.item;
    if (!widget.isWidget())
        return;
    QMenu menu;
    ui::styleModernMenu(&menu, ui::ModernMenuStyle::current());
    QAction* title = menu.addAction(widget.name);
    title->setEnabled(false);
    menu.addSeparator();
    QAction* edit = ui::widgets::isEditable(widget.widgetType)
                        ? menu.addAction(QCoreApplication::translate("ProductApp", "Edit the note"))
                        : nullptr;
    QAction* configure = menu.addAction(QCoreApplication::translate("ProductApp", "Configure widget…"));
    QAction* plate = menu.addAction(QCoreApplication::translate("ProductApp", "Readable backdrop"));
    plate->setCheckable(true);
    plate->setChecked(widget.widgetSettings.value(QStringLiteral("backdrop")).toBool());
    QMenu* sizes = menu.addMenu(QCoreApplication::translate("ProductApp", "Size"));
    ui::styleModernMenu(sizes, ui::ModernMenuStyle::current());
    auto* group = new QActionGroup(sizes);
    QHash<QAction*, QSize> spanOf;
    for (const QSize& span : ui::widgets::spanChoices()) {
        QAction* action = sizes->addAction(QStringLiteral("%1 \u00D7 %2").arg(span.width()).arg(span.height()));
        action->setCheckable(true);
        action->setChecked(span == widget.span);
        group->addAction(action);
        spanOf.insert(action, span);
    }
    sizes->addSeparator(); // session 96: any other size, in the widget's settings
    QAction* otherSize = sizes->addAction(QCoreApplication::translate("ProductApp", "Other size…"));
    menu.addSeparator();
    QAction* remove = menu.addAction(QCoreApplication::translate("ProductApp", "Remove the widget"));
    QAction* chosen = menu.exec(logicalFromPhysical(point));
    if (!chosen)
        return;
    if (edit && chosen == edit) {
        startWidgetEdit(desktopWidgetOwner(), path);
        return;
    }
    if (chosen == remove) {
        m_container->removeDesktopWidget(path);
        return;
    }
    if (chosen == configure || chosen == otherSize) {
        ui::WidgetSettingsDialog dialog(widget);
        dialog.move(logicalFromPhysical(point));
        if (dialog.exec() == QDialog::Accepted)
            m_container->replaceDesktopWidget(path, dialog.result());
        return;
    }
    if (chosen == plate)
        widget.widgetSettings.insert(QStringLiteral("backdrop"), chosen->isChecked());
    if (spanOf.contains(chosen))
        widget.span = spanOf.value(chosen);
    m_container->replaceDesktopWidget(path, widget);
}

void ProductApp::onFenceItemMenu(const QString& fenceId, const QStringList& paths, const QPoint& point)
{
    if (paths.isEmpty() || !m_container)
        return;
    // Session 79: a widget's own menu (its settings, its size, take it out).
    if (core::isWidgetPath(paths.first())) {
        showWidgetMenu(fenceId, paths.first(), point);
        return;
    }
    // Session 74: a tile whose file is not found has no shell menu -- it gets
    // the one thing to do with it (its file is never touched).
    QStringList missing;
    for (const QString& path : paths) {
        if (!path.startsWith(QLatin1String("::")) && !core::entryExists(path))
            missing << path;
    }
    if (!missing.isEmpty()) {
        QMenu menu;
        ui::styleModernMenu(&menu, ui::ModernMenuStyle::current());
        QAction* info = menu.addAction(QCoreApplication::translate("ProductApp", "Not found: %1")
                                           .arg(QFileInfo(missing.first()).fileName()));
        info->setEnabled(false);
        // Session 75: a shortcut StarFish kept a copy of can be put back.
        QStringList restorable;
        for (const QString& path : std::as_const(missing)) {
            if (m_vault && m_vault->has(path))
                restorable << path;
        }
        QAction* restore = restorable.isEmpty()
                               ? nullptr
                               : menu.addAction(QCoreApplication::translate("ProductApp", "Restore the shortcut"));
        QAction* remove = menu.addAction(QCoreApplication::translate("ProductApp", "Remove from the fence"));
        QAction* chosen = menu.exec(logicalFromPhysical(point));
        if (chosen == remove) {
            m_container->releaseItems(fenceId, missing);
        } else if (chosen && chosen == restore) {
            for (const QString& path : std::as_const(restorable)) {
                const bool done = m_vault->restore(path);
                qCInfo(lc).noquote() << QStringLiteral("restore \"%1\": %2").arg(path, done ? QStringLiteral("done")
                                                                                           : QStringLiteral("failed"));
            }
            m_container->reconcileMissingItems({});
            syncShortcutVault();
            m_shellResync.start();
        }
        return;
    }
    // The shell's selection menu needs items of the desktop's view; a tile of
    // a portal (a folder elsewhere) gets its own file's classic menu.
    bool onDesktop = drawnDesktop() && m_shellView;
    for (const QString& path : paths)
        onDesktop = onDesktop && !shellNameOf(path).isEmpty();
    if (!onDesktop) {
        platform::win::ShellContextMenu::showForPath(paths.first(), point,
                                                     reinterpret_cast<void*>(backdropOwner()));
        return;
    }
    constexpr int kPutBack = 1;
    const QString label = QCoreApplication::translate("ProductApp", "Put back on the desktop");
    bool rename = false;
    int chosen = -1;
    if (modernContextMenuWanted()) {
        core::ContextMenuItem back;
        back.key = QStringLiteral("starfish:put-back");
        back.label = label;
        back.glyph = QString(QChar(0xE8A7)); // OpenInNewWindow
        back.ownId = kPutBack;
        chosen = showModernContextMenu(core::ContextMenuKind::Selection, paths, point, {back}, &rename);
    } else {
        chosen = m_shellView->showSelectionMenu(paths, point, backdropOwner(),
                                                {{kPutBack, label, true, false, {}}}, &rename);
    }
    if (chosen == kPutBack)
        m_container->releaseItems(fenceId, paths);
    if (rename && !paths.isEmpty())
        startRename(paths.first(), fenceId); // session 74: the shell's own edit would be in the hidden list
    m_shellResync.start();
}

void ProductApp::openDesktopItem(const QString& path)
{
    // The default verb of the item itself, as the desktop's double-click runs it:
    // a shortcut keeps its arguments and working folder, This PC opens, a
    // document opens with its own application.
    if (m_shellView && m_shellView->invokeSelectionVerb({path}, QString())) {
        qCInfo(lc).noquote() << QStringLiteral("opened \"%1\" (its default verb)").arg(path);
        return;
    }
    if (path.startsWith(QLatin1String("::"))) {
        platform::win::openShellItem(path);
        return;
    }
    qCInfo(lc).noquote() << QStringLiteral("opening \"%1\" through the URL handler").arg(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}


void ProductApp::startRename(const QString& path, const QString& fenceId)
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    QRect label;
    if (!fenceId.isEmpty()) {
        // Session 74 (T-101): a fence's tile -- its caption on screen.
        if (!m_container)
            return;
        label = m_container->fenceItemLabelRect(fenceId, path);
    } else {
        if (!backdrop || !drawnDesktop())
            return;
        label = backdrop->labelRectOf(path);
    }
    const QString name = platform::win::editingNameOf(path);
    if (label.isNull() || name.isEmpty()) {
        qCInfo(lc).noquote() << QStringLiteral("\"%1\" cannot be renamed here").arg(path);
        return;
    }
    finishRename(false);

    auto* edit = new RenameEdit;
    edit->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    edit->setAlignment(Qt::AlignHCenter);
    edit->setFont(QFont(QStringLiteral("Segoe UI"), 9));
    // Explorer's own edit, whatever the application theme: white, blue frame.
    edit->setStyleSheet(QStringLiteral("QLineEdit { background: #FFFFFF; color: #000000; "
                                       "border: 1px solid #0078D7; selection-background-color: "
                                       "#0078D7; selection-color: #FFFFFF; padding: 0 2px; }"));
    edit->setText(name);
    // As Explorer does: the name without its extension is what is selected.
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (!path.startsWith(QLatin1String("::")) && !QFileInfo(path).isDir() && dot > 0)
        edit->setSelection(0, dot);
    else
        edit->selectAll();
    // The caption is in physical pixels; a Qt window is placed in logical ones.
    const ui::ScreenInfo* primary = backdrop ? backdrop->screens().primary() : nullptr;
    const qreal dpr = primary ? qMax(1.0, primary->devicePixelRatio)
                              : qMax(1.0, QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0);
    const int width = qMax(qRound(label.width() / dpr) + 24, 140);
    const QPoint centre(qRound(label.center().x() / dpr), qRound(label.top() / dpr));
    edit->setGeometry(centre.x() - width / 2, centre.y(), width, 24);
    edit->onCancel = [this] { finishRename(false); };
    connect(edit, &QLineEdit::editingFinished, this, [this] { finishRename(true); });
    m_renameEdit = edit;
    m_renamePath = path;
    m_renameFenceId = fenceId;
    edit->show();
    edit->raise();
    edit->activateWindow();
    edit->setFocus();
    qCInfo(lc).noquote() << QStringLiteral("renaming \"%1\"").arg(path);
}

void ProductApp::finishRename(bool commit)
{
    if (!m_renameEdit)
        return;
    QLineEdit* edit = m_renameEdit;
    m_renameEdit = nullptr; // editingFinished fires again on hide: once is enough
    const QString path = m_renamePath;
    const QString fenceId = m_renameFenceId;
    m_renameFenceId.clear();
    const QString text = edit->text().trimmed();
    edit->hide();
    edit->deleteLater();
    if (commit && !text.isEmpty() && text != platform::win::editingNameOf(path)) {
        QString newPath;
        if (renameDesktopItem(path, text, &newPath) && !fenceId.isEmpty() && m_container) {
            // Session 74: the tile follows at once, where it was -- the desktop
            // must not see the new name as a free icon in between.
            core::DesktopItem renamed = platform::win::DesktopScanner::itemForPath(
                newPath.isEmpty() ? path : QDir::fromNativeSeparators(newPath));
            if (!renamed.path.isEmpty())
                m_container->replaceFenceItem(fenceId, path, renamed);
        }
    }
}

void ProductApp::startWidgetEdit(const QString& fenceId, const QString& path)
{
    if (!m_container)
        return;
    core::DesktopItem widget;
    QRect cell;
    if (fenceId == desktopWidgetOwner()) { // session 91: a note on the desktop
        for (const ui::DesktopWidget& d : m_container->desktopWidgets()) {
            if (d.item.path == path) {
                widget = d.item;
                if (m_container->backdrop())
                    cell = m_container->backdrop()->desktopWidgetRect(d);
            }
        }
    } else {
        if (!m_container->contains(fenceId))
            return;
        ui::FenceView* view = m_container->view(fenceId);
        if (!view)
            return;
        for (const core::DesktopItem& item : view->items()) {
            if (item.path == path)
                widget = item;
        }
        // The widget's whole cell: the editor covers exactly what the fence paints.
        cell = m_container->fenceItemRect(fenceId, path);
    }
    if (!widget.isWidget() || !ui::widgets::isEditable(widget.widgetType))
        return;
    if (cell.isEmpty()) {
        qCInfo(lc).noquote() << QStringLiteral("\"%1\" cannot be written here").arg(path);
        return;
    }
    finishWidgetEdit(true); // one editor at a time, and the previous one is saved first

    auto* edit = new ui::NoteEditor(
        ui::widgets::noteText(widget), [this](const QString& text) { applyWidgetText(text); },
        [this] { finishWidgetEdit(true); });
    edit->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    edit->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    // The fence's own colours: this editor *is* the note, not a white box
    // flashing over the desktop (the rename box is Explorer's; this one is ours).
    edit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #1F2937; color: #E5E7EB; border: 1px solid #3B82F6; "
        "selection-background-color: #3B82F6; selection-color: #FFFFFF; padding: 4px; }"));
    // The cell is in physical pixels; a Qt window is placed in logical ones.
    const ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    const ui::ScreenInfo* primary = backdrop ? backdrop->screens().primary() : nullptr;
    const qreal dpr = primary ? qMax(1.0, primary->devicePixelRatio)
                              : qMax(1.0, QGuiApplication::primaryScreen()
                                              ? QGuiApplication::primaryScreen()->devicePixelRatio()
                                              : 1.0);
    const QRect frame(qRound(cell.x() / dpr), qRound(cell.y() / dpr), qRound(cell.width() / dpr),
                      qRound(cell.height() / dpr));
    edit->setGeometry(frame.adjusted(0, 0, 0, 0));
    m_noteEdit = edit;
    m_noteEditFenceId = fenceId;
    m_noteEditPath = path;
    edit->show();
    edit->raise();
    edit->activateWindow();
    edit->setFocus();
    qCInfo(lc).noquote() << QStringLiteral("writing the note \"%1\"").arg(path);
}

void ProductApp::applyWidgetText(const QString& text)
{
    if (!m_container || m_noteEditFenceId.isEmpty())
        return;
    if (m_noteEditFenceId == desktopWidgetOwner()) { // session 91
        for (const ui::DesktopWidget& d : m_container->desktopWidgets())
            if (d.item.path == m_noteEditPath)
                m_container->replaceDesktopWidget(m_noteEditPath, ui::widgets::withNoteText(d.item, text));
        return;
    }
    ui::FenceView* view = m_container->view(m_noteEditFenceId);
    if (!view)
        return;
    for (const core::DesktopItem& item : view->items()) {
        if (item.path != m_noteEditPath)
            continue;
        // One document change: the fence repaints, the auto-save follows.
        m_container->replaceFenceItem(m_noteEditFenceId, m_noteEditPath, ui::widgets::withNoteText(item, text));
        return;
    }
}

void ProductApp::flushWidgetEdit()
{
    if (m_noteEdit)
        m_noteEdit->flush();
}

void ProductApp::finishWidgetEdit(bool commit)
{
    if (!m_noteEdit)
        return;
    ui::NoteEditor* edit = m_noteEdit;
    if (commit)
        edit->flush(); // the last keystrokes belong to the note
    m_noteEdit = nullptr;
    m_noteEditFenceId.clear();
    m_noteEditPath.clear();
    edit->hide();
    edit->deleteLater();
}

void ProductApp::runMenuAction(const QString& action)
{
    if (action != QLatin1String("theme")) {
        qCInfo(lc).noquote() << QStringLiteral("unknown menu action \"%1\"").arg(action);
        return;
    }
    // Session 82: the light/dark switch. Windows keeps it in two values and every
    // window learns about the change from a broadcast (see windows_theme.cpp), so
    // the taskbar and the open apps follow without a logon.
    const platform::win::ThemeState now = platform::win::readTheme();
    const bool light = platform::win::flippedLight(now.appsLight);
    const bool done = platform::win::applyTheme(light);
    qCInfo(lc).noquote() << QStringLiteral("menu action \"theme\": %1")
                                .arg(!done ? QStringLiteral("failed")
                                           : light ? QStringLiteral("light") : QStringLiteral("dark"));
}

bool ProductApp::renameDesktopItem(const QString& path, const QString& newName, QString* newPath)
{
    const bool done = platform::win::renameShellItem(path, newName, backdropOwner(), newPath);
    m_itemCache.remove(path);
    m_shellResync.start();
    return done;
}

bool ProductApp::deleteDesktopItems(const QStringList& paths, bool permanent, bool silent)
{
    using platform::win::DeleteMode;
    const DeleteMode mode = !permanent ? DeleteMode::RecycleBin
                            : silent   ? DeleteMode::PermanentSilent
                                       : DeleteMode::Permanent;
    const bool done = platform::win::deleteShellItems(paths, mode, backdropOwner());
    m_shellResync.start();
    return done;
}

bool ProductApp::moveDesktopItems(const QStringList& paths, const QPoint& delta)
{
    if (!m_shellView)
        return false;
    // Session 61 (the user: "impossible de déplacer une icône"): the drawn
    // desktop names a file C:/..., the shell C:\... -- looked up as such, the
    // move found nothing and every file icon stayed put. Translated here.
    QStringList names;
    for (const QString& path : paths) {
        const QString name = shellNameOf(path);
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    if (names.isEmpty()) {
        qCInfo(lc).noquote() << QStringLiteral("nothing to move: %1 not on the desktop")
                                    .arg(paths.join(QLatin1String(", ")));
        return false;
    }
    int moved = 0;
    if (m_shellAutoArrange) {
        // "Auto arrange": Explorer decides the places, as on its own desktop --
        // the drop only says where in the order.
        for (const QString& name : std::as_const(names)) {
            if (m_shellView->positionItem(name, m_itemPositions.value(name) + delta))
                ++moved;
        }
    } else {
        // Free icons only (a filed one is hidden there), the grid if Explorer
        // aligns to it, never under a fence unless the user allows it.
        const QSet<QString> filed = m_container ? m_container->filedPathKeys() : QSet<QString>();
        QList<QPair<QString, QPoint>> items;
        for (auto it = m_itemPositions.constBegin(); it != m_itemPositions.constEnd(); ++it) {
            if (names.contains(it.key()) || !filed.contains(ui::BackdropLayout::pathKey(it.key())))
                items.append({it.key(), it.value()});
        }
        const QList<QRect> obstacles = m_allowDesktopOverlap || !m_container
                                           ? QList<QRect>()
                                           : m_container->fenceObstacleRects();
        const QList<core::OverlapMove> moves = core::OverlapPlanner::planMove(
            items, names, delta, m_shellSnap, obstacles, desktopWorkAreas(), desktopSlot());
        for (const core::OverlapMove& move : moves) {
            if (m_shellView->positionItem(move.key, move.to)) {
                m_itemPositions.insert(move.key, move.to);
                ++moved;
            }
        }
    }
    m_shellSignature.clear();
    m_shellResync.start();
    return moved > 0;
}

void ProductApp::showDesktopDropGrid(const QStringList& paths, const QPoint& delta, const QPoint& screen,
                                     const QPoint& anchor)
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    if (!backdrop)
        return;
    if (!m_desktopDragGrid || !drawnDesktop() || m_shellAutoArrange) {
        // Auto-arrange: Explorer decides the places, no slot to show.
        backdrop->clearDropGrid();
        return;
    }
    QStringList names;
    for (const QString& path : paths) {
        const QString name = shellNameOf(path);
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    const core::OverlapSlot slot = desktopSlot();
    const QList<QRect> areas = desktopWorkAreas();
    const QSet<QString> filed = m_container->filedPathKeys();
    QList<QPair<QString, QPoint>> items;
    for (auto it = m_itemPositions.constBegin(); it != m_itemPositions.constEnd(); ++it) {
        if (names.contains(it.key()) || !filed.contains(ui::BackdropLayout::pathKey(it.key())))
            items.append({it.key(), it.value()});
    }
    // The grid only when Explorer aligns to it: free placement has none.
    const QVector<QRect> cells =
        m_shellSnap ? core::OverlapPlanner::gridSlots(items, areas, slot) : QVector<QRect>();
    QRect target;
    if (!names.isEmpty()) {
        // The icon under the press leads; the move is the drop's own plan.
        QString lead = names.first();
        for (const QString& name : std::as_const(names)) {
            if (!anchor.isNull() && slot.boxAt(m_itemPositions.value(name)).contains(anchor))
                lead = name;
        }
        QPoint to = m_itemPositions.value(lead);
        if (qAbs(delta.x()) + qAbs(delta.y()) >= 4) {
            const QList<QRect> obstacles = m_allowDesktopOverlap ? QList<QRect>() : m_container->fenceObstacleRects();
            for (const core::OverlapMove& move :
                 core::OverlapPlanner::planMove(items, names, delta, m_shellSnap, obstacles, areas, slot)) {
                if (move.key == lead)
                    to = move.to;
            }
        }
        target = slot.boxAt(to);
    } else {
        // From elsewhere: placed under the cursor (onForeignDrop), on the grid.
        const int icon = qMax(16, m_shellIconSize);
        QPoint at = screen - QPoint(icon / 2, icon / 2);
        if (m_shellSnap)
            at = core::OverlapPlanner::snapToGrid(at, core::OverlapPlanner::gridOrigin(items, areas, slot), slot);
        target = slot.boxAt(at);
    }
    backdrop->setDropGrid(cells, target);
}

void ProductApp::hideDesktopDropGrid()
{
    if (ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr)
        backdrop->clearDropGrid();
}

QString ProductApp::widgetEngineSummary() const
{
    int native = 0;
    if (m_container) {
        for (const QString& id : m_container->fenceIds()) {
            if (const ui::FenceView* view = m_container->view(id)) {
                for (const core::DesktopItem& item : view->items())
                    native += item.isWidget() ? 1 : 0;
            }
        }
    }
    return ui::widgets::engineSummary(native);
}

void ProductApp::onForeignDrop(const QPoint& point, const QStringList& stems)
{
    m_dropStems = stems;
    m_namesBeforeDrop.clear();
    if (m_shellView) {
        for (const platform::win::ShellViewItem& item : m_shellView->read().items)
            m_namesBeforeDrop.insert(item.parsingName);
    }
    m_foreignDropAt = point;
    m_foreignDropUntil = QDateTime::currentMSecsSinceEpoch() + 15000;
    m_shellResync.start();
}

void ProductApp::armNewItemRename()
{
    m_namesBeforeNew.clear();
    for (auto it = m_itemPositions.constBegin(); it != m_itemPositions.constEnd(); ++it)
        m_namesBeforeNew.insert(it.key());
    m_renameNewUntil = QDateTime::currentMSecsSinceEpoch() + 5000;
}

void ProductApp::newDesktopFolder()
{
    if (!m_shellView)
        return;
    armNewItemRename();
    if (!m_shellView->invokeBackgroundVerb(QStringLiteral("NewFolder")))
        m_renameNewUntil = 0;
    m_shellResync.start();
}

void ProductApp::onInternalDrop(const QPoint& point, const QStringList& paths, const QPoint& origin)
{
    const QPoint delta = point - origin;
    if (qAbs(delta.x()) + qAbs(delta.y()) < 4)
        return; // let go where it was picked up
    const bool moved = moveDesktopItems(paths, delta);
    qCInfo(lc).noquote() << QStringLiteral("%1 %2 icon(s) by %3,%4")
                                .arg(moved ? QLatin1String("moved") : QLatin1String("could not move"))
                                .arg(paths.size())
                                .arg(delta.x())
                                .arg(delta.y());
}

void ProductApp::dragDesktopItems(const QStringList& paths, const QPoint& origin)
{
    if (!drawnDesktop() || paths.isEmpty())
        return;
    // Session 62: the icons travel with the cursor, as on Windows' desktop.
    QPoint hotspot;
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    const QImage picture = backdrop ? backdrop->dragImage(paths, origin, &hotspot) : QImage();
    m_dragOrigin = origin; // session 80: the grid measures the move from here
    platform::win::dragShellItems(paths, origin, backdropOwner(), picture, hotspot);
    hideDesktopDropGrid(); // whatever the drag ended on
    m_shellSignature.clear();
    m_shellResync.start();
}

void ProductApp::onDesktopKey(int key, Qt::KeyboardModifiers modifiers)
{
    ui::DesktopBackdrop* backdrop = m_container ? m_container->backdrop() : nullptr;
    if (!backdrop || !drawnDesktop())
        return;
    const QStringList selection = backdrop->selection();
    const bool ctrl = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    switch (key) {
    case Qt::Key_F2:
        if (selection.size() == 1)
            startRename(selection.first());
        break;
    case Qt::Key_Delete:
        if (!selection.isEmpty())
            deleteDesktopItems(selection, shift);
        break;
    case Qt::Key_Return:
        if (!selection.isEmpty())
            m_shellView->invokeSelectionVerb(selection, QString());
        break;
    case Qt::Key_F5:
        m_shellSignature.clear();
        m_wallpaperSignature = 0;
        syncShellView();
        refreshWallpaper();
        break;
    case Qt::Key_Menu:
    case Qt::Key_F10:
        if (key == Qt::Key_F10 && !shift)
            break;
        if (!selection.isEmpty())
            onIconMenu(selection, backdrop->iconRectOf(selection.first()).center());
        else
            showDesktopMenu(backdrop->area().center());
        break;
    case Qt::Key_C:
    case Qt::Key_X:
        if (ctrl && !selection.isEmpty())
            m_shellView->invokeSelectionVerb(selection, key == Qt::Key_C ? QStringLiteral("copy")
                                                                         : QStringLiteral("cut"));
        break;
    case Qt::Key_V:
        if (ctrl)
            m_shellView->invokeBackgroundVerb(QStringLiteral("paste"));
        break;
    case Qt::Key_Z:
        if (ctrl)
            m_shellView->invokeBackgroundUndo(false);
        break;
    case Qt::Key_Y:
        if (ctrl)
            m_shellView->invokeBackgroundUndo(true);
        break;
    case Qt::Key_N:
        if (ctrl && shift)
            newDesktopFolder();
        break;
    default:
        return;
    }
    m_shellResync.start();
}

} // namespace starfish::app

// ---------------------------------------------------------------------------
// T-092 (lot 4): the right-click menu drawn by StarFish.

namespace starfish::app {

namespace {

std::vector<core::ContextMenuItem>
toContextMenuItems(const std::vector<platform::win::DesktopShellView::ShellMenuItem>& shell)
{
    std::vector<core::ContextMenuItem> out;
    out.reserve(shell.size());
    for (const auto& s : shell) {
        core::ContextMenuItem item;
        item.separator = s.separator;
        item.label = s.label;
        item.shortcut = s.shortcut;
        item.enabled = s.enabled;
        item.checked = s.checked;
        item.radio = s.radio;
        item.isDefault = s.isDefault;
        item.command = s.command;
        item.children = toContextMenuItems(s.children);
        out.push_back(std::move(item));
    }
    return out;
}

// A popup's position in Qt's coordinates from a physical screen point.
QPoint logicalFromPhysical(const QPoint& physical)
{
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect g = screen->geometry();
        const qreal dpr = screen->devicePixelRatio();
        const QRect native(g.topLeft(), g.size() * dpr);
        if (native.contains(physical))
            return g.topLeft() + (physical - g.topLeft()) / dpr;
    }
    return physical;
}

} // namespace

bool ProductApp::modernContextMenuWanted()
{
    if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows11)
        return false; // Windows 10: its own menu is the classic one
    if (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier)
        return false; // Shift+right-click: the classic menu, as on Windows 11
    return core::ContextMenuConfig::load(core::ContextMenuConfig::defaultPath()).modern;
}

std::vector<core::ContextMenuItem>
ProductApp::composeContextMenu(core::ContextMenuKind kind, const QStringList& paths,
                               const std::vector<core::ContextMenuItem>& ours,
                               const QStringList& hiddenShellEntries)
{
    if (!m_shellView)
        return {};
    const auto shell = kind == core::ContextMenuKind::Background
                           ? m_shellView->openBackgroundMenuModel(hiddenShellEntries)
                           : m_shellView->openSelectionMenuModel(paths);
    std::vector<core::ContextMenuItem> classic = toContextMenuItems(shell);
    const core::ContextMenuConfig config =
        core::ContextMenuConfig::load(core::ContextMenuConfig::defaultPath());
    // The packaged apps' entries (IExplorerCommand), which the classic menu never has.
    std::vector<core::ContextMenuItem> packaged;
    const QString desktopFolder = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    for (const platform::win::PackagedCommand& c : platform::win::packagedCommandsFor(
             paths, kind == core::ContextMenuKind::Background, desktopFolder)) {
        core::ContextMenuItem item;
        // A title Windows 11 has a place for ("Ouvrir dans le Terminal") takes it.
        const QString known = core::contextMenuKey(c.title, false);
        item.key = known.startsWith(QLatin1String("label:")) ? c.key : known;
        item.label = c.title;
        item.iconFile = c.iconFile;
        item.enabled = c.enabled;
        item.packagedIndex = c.index;
        packaged.push_back(item);
    }
    std::vector<core::ContextMenuItem> items = core::composeWindows11Menu(
        classic, kind, config, ours, tr("Show more options"), nullptr, packaged);

    if (!items.empty() && items.back().ownId == core::kMoreOptionsId)
        items.back().shortcut = tr("Shift+F10"); // what opens it from the keyboard
    // The keys a user can name in the configuration, with what they say.
    bool grew = false;
    const QString prefix = kind == core::ContextMenuKind::Background ? QStringLiteral("background ")
                                                                       : QStringLiteral("items ");
    for (const core::ContextMenuItem& item : classic) {
        if (item.separator)
            continue;
        const QString key = core::contextMenuKey(item.label, !item.children.empty());
        if (!m_seenMenuKeys.contains(prefix + key)) {
            m_seenMenuKeys.insert(prefix + key, item.label);
            grew = true;
        }
    }
    for (const core::ContextMenuItem& item : packaged) {
        if (!m_seenMenuKeys.contains(prefix + item.key)) {
            m_seenMenuKeys.insert(prefix + item.key, item.label);
            grew = true;
        }
    }
    for (const core::ContextMenuItem& item : ours) {
        if (!m_seenMenuKeys.contains(prefix + item.key)) {
            m_seenMenuKeys.insert(prefix + item.key, item.label);
            grew = true;
        }
    }
    if (grew) {
        writeSeenMenuKeys();
        if (m_settings)
            m_settings->menuEditor()->setSeenEntries(m_seenMenuKeys);
    }
    return items;
}

int ProductApp::runContextMenuEntry(const core::ContextMenuItem& item, core::ContextMenuKind kind,
                                    const QStringList& paths, const QPoint& point,
                                    const std::vector<core::ContextMenuItem>& ours,
                                    bool* renameRequested)
{
    int result = -1;
    if (item.ownId == core::kMoreOptionsId) {
        m_shellView->closeMenuModel();
        // "Show more options": the classic menu, ours included, where it was asked.
        if (kind == core::ContextMenuKind::Background) {
            QList<platform::win::DesktopShellView::MenuEntry> extra;
            for (const core::ContextMenuItem& o : ours) {
                platform::win::DesktopShellView::MenuEntry entry{o.ownId, o.label, true, false, {}};
                for (const core::ContextMenuItem& child : o.children) // session 91: "Add a widget"
                    entry.children.push_back({child.ownId, child.label, false, false, {}});
                extra.append(entry);
            }
            QStringList hidden{QStringLiteral("StarFish"), platform::win::desktopMenuLabel()};
            result = m_shellView->showBackgroundMenu(point, backdropOwner(), extra, hidden);
        } else {
            m_shellView->showSelectionMenu(paths, point, backdropOwner(), {}, renameRequested);
        }
        return result;
    }
    if (item.command >= 0) {
        if (renameRequested && item.key == QLatin1String("rename"))
            *renameRequested = true; // the shell's edit would be in the hidden list
        else
            m_shellView->invokeMenuCommand(item.command, point, backdropOwner());
        if (item.key.startsWith(QLatin1String("new/")))
            armNewItemRename();
    } else if (item.packagedIndex >= 0) {
        platform::win::invokePackagedCommand(item.packagedIndex);
    } else if (item.customIndex >= 0) {
        const core::ContextMenuConfig config =
            core::ContextMenuConfig::load(core::ContextMenuConfig::defaultPath());
        const core::ContextMenuProfile& profile = config.profile(kind);
        if (item.customIndex < profile.custom.size()) {
            const core::ContextMenuCustomEntry& entry = profile.custom.at(item.customIndex);
            if (!entry.action.isEmpty()) {
                // Session 82: an entry that runs a StarFish action, not a program.
                runMenuAction(entry.action);
            } else {
                const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
                const bool started = QProcess::startDetached(
                    entry.program, core::expandCustomArguments(entry, paths, desktop), desktop);
                qCInfo(lc).noquote() << QStringLiteral("custom menu entry \"%1\": %2 %3")
                                            .arg(entry.label, entry.program,
                                                 started ? QStringLiteral("started") : QStringLiteral("FAILED"));
            }
        }
    } else {
        result = item.ownId;
    }
    m_shellView->closeMenuModel();
    platform::win::releasePackagedCommands();
    return result;
}

int ProductApp::showModernContextMenu(core::ContextMenuKind kind, const QStringList& paths,
                                      const QPoint& point, const std::vector<core::ContextMenuItem>& ours,
                                      bool* renameRequested, const QStringList& hiddenShellEntries)
{
    const std::vector<core::ContextMenuItem> items =
        composeContextMenu(kind, paths, ours, hiddenShellEntries);
    core::ContextMenuItem picked;
    bool any = false;
    std::unique_ptr<QMenu> menu(ui::buildModernMenu(
        items, ui::ModernMenuStyle::current(), [&](const core::ContextMenuItem& item) {
            picked = item;
            any = true;
        }));
    // A popup closes on an outside click only when its thread is in the
    // foreground (as for TrackPopupMenu); keys go to it the same way.
    if (const quintptr owner = backdropOwner())
        platform::win::bringToForeground(owner);
    menu->exec(logicalFromPhysical(point));
    menu.reset();
    if (!any) {
        m_shellView->closeMenuModel();
        platform::win::releasePackagedCommands();
        return -1;
    }
    qCInfo(lc).noquote() << QStringLiteral("drawn menu: \"%1\" (%2)").arg(picked.label, picked.key);
    return runContextMenuEntry(picked, kind, paths, point, ours, renameRequested);
}

void ProductApp::writeSeenMenuKeys()
{
    const QString path = QFileInfo(core::ContextMenuConfig::defaultPath())
                             .dir()
                             .filePath(QStringLiteral("context-menu-keys.txt"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&file);
    out << "# Les entrees vues dans les menus clic droit du bureau, par cle.\n"
           "# A utiliser dans context-menu.json : \"hidden\" (masquer), \"shown\"\n"
           "# (ramener une entree que Windows 11 laisse sous \"Afficher plus d'options\"),\n"
           "# \"order\" (mettre en tete, dans cet ordre). \"background\" = le fond du\n"
           "# bureau, \"items\" = une icone. Fichier reecrit par StarFish.\n\n";
    for (auto it = m_seenMenuKeys.cbegin(); it != m_seenMenuKeys.cend(); ++it)
        out << it.key() << "    = " << it.value() << '\n';
    file.commit();
}

void ProductApp::loadSeenMenuKeys()
{
    // What earlier runs' menus showed (writeSeenMenuKeys), so the editor lists
    // this PC's entries before a menu was opened in this run.
    QFile file(QFileInfo(core::ContextMenuConfig::defaultPath())
                   .dir()
                   .filePath(QStringLiteral("context-menu-keys.txt")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const int eq = line.indexOf(QLatin1String("    = "));
        if (line.startsWith(QLatin1Char('#')) || eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        if (!m_seenMenuKeys.contains(key))
            m_seenMenuKeys.insert(key, line.mid(eq + 6));
    }
}

void ProductApp::editContextMenu()
{
    // Session 59: the editor, in the settings window; the file by hand only
    // when there is no window (a run without one).
    if (m_settings) {
        m_settings->menuEditor()->reload();
        m_settings->menuEditor()->setSeenEntries(m_seenMenuKeys);
        m_settings->showPage(ui::SettingsDialog::Menu);
        showSettings();
        return;
    }
    const QString path = core::ContextMenuConfig::defaultPath();
    if (!QFileInfo::exists(path)) {
        core::ContextMenuConfig config; // the defaults, written out to be edited
        config.save(path);
    }
    writeSeenMenuKeys();
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

} // namespace starfish::app
