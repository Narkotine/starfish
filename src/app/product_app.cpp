#include "app/product_app.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QRect>
#include <QScreen>
#include <QTimer>
#include <QUrl>

#include "core/desktop_item.hpp"
#include "core/desktop_watcher.hpp"
#include "core/icon_hiding_marker.hpp"
#include "core/layout_store.hpp"
#include "core/preferences.hpp"
#include "platform/win/desktop_icons.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/desktop_surface.hpp"
#include "platform/win/hotkey_manager.hpp"
#include "platform/win/shell_context_menu.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/fence_material.hpp"
#include "ui/fence_menu.hpp"
#include "ui/icon_grid.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tray_icon.hpp"

QLoggingCategory lc("starfish.product");

namespace starfish::app {

namespace {

// F-021 asks for Win+Space, but Windows reserves that for the language switcher
// and refuses to register it (measured, session 15). Win+Alt+Space is close
// enough to remember and is actually accepted.
constexpr int kPeekHotkeyId = 1;
constexpr Qt::KeyboardModifiers kPeekModifiers = Qt::MetaModifier | Qt::AltModifier;
constexpr Qt::Key kPeekKey = Qt::Key_Space;

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
}

bool ProductApp::start(const Options& options, QString* error)
{
    if (m_started)
        return true;

    m_options = options;

    // The safety net, first (T-046). If a previous run hid the native icon layer
    // and never came back -- crash, kill, power cut -- the desktop has been
    // without icons since, and the marker that run left is the only surviving
    // evidence of why. Repair it before anything else exists, and regardless of
    // the mode: the icons being gone is a fact about the machine, not about this
    // document or this options set.
    repairIconHidingFromPreviousRun();

    // T-088: the user's own choices, read before anything can act on them. A file
    // that cannot be read is reported and the defaults stand -- and the default
    // here is the safe one (the desktop keeps its icons).
    m_prefs = core::Preferences(m_options.preferencesPath.isEmpty()
                                    ? core::Preferences::defaultFilePath()
                                    : m_options.preferencesPath);
    {
        QString preferencesError;
        if (!m_prefs.load(&preferencesError))
            qCWarning(lc).noquote()
                << QStringLiteral("preferences not read (%1): using the defaults")
                       .arg(preferencesError);
    }

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
    if (!m_options.layoutPath.isEmpty())
        m_store->setFilePath(m_options.layoutPath);

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
    m_container->setIconProvider([icons](const core::DesktopItem& item, int sizePx) {
        return icons->iconFor(item.path, sizePx);
    });
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
    if (m_options.claimDesktop) {
        // The development path keeps the old world: the backdrop draws the icons
        // (T-039/T-056), so it is hosted -- setDesktopIconsHidden() below does that
        // first and refuses to hide anything if it cannot. The icons are read
        // **before** they are taken over, since a hidden list view has nothing to
        // describe (T-062).
        syncNativeIcons();
        setDesktopIconsHidden(true);
    } else if (m_prefs.desktopIconsOwned()) {
        // T-088: the user asked for it in the settings window, so it happens at
        // launch rather than mid-session -- and only if a fence really holds
        // something: taking the layer over for nothing would mute a desktop that
        // had nothing to gain (T-066). The desktop is re-read first, because both
        // the free set and the slots the icons are drawn at come from that read,
        // and it has to happen while the Shell is still drawing the list.
        refreshDesktopItems();
        syncNativeIcons();
    }

    // The screen arrangement changes while StarFish is running: a monitor is
    // unplugged, docked, re-arranged, or its scaling changes. Qt is the
    // authority on what exists; the fences are revalidated onto whatever is
    // really there (T-014 / F-032).
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen*) { refreshScreens(); });
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
    connect(c, &ui::FenceContainer::fencesVisibilityChanged, this,
            [onChange](bool) { onChange(); });
    connect(c, &ui::FenceContainer::peekChanged, this, [onChange](bool) { onChange(); });
    connect(c, &ui::FenceContainer::rulesChanged, this, onChange);
}

void ProductApp::wirePersistence()
{
    connectLifecycle([this] { onLifecycleChanged(); });

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
    // T-088: and the way back is offered whenever StarFish *may* take the desktop's
    // icons over -- the user's switch, present in the product, is not the developer
    // path, so it cannot be the only thing that carries the exit.
    m_tray->setRestoreIconsOffered(m_options.claimDesktop || m_prefs.desktopIconsOwned());
    connect(m_tray, &ui::TrayIcon::newFenceRequested, this, &ProductApp::onNewFence);
    connect(m_tray, &ui::TrayIcon::settingsRequested, this, &ProductApp::showSettings);
    connect(m_tray, &ui::TrayIcon::quitRequested, this, &ProductApp::quitRequested);
    connect(m_tray, &ui::TrayIcon::fencesVisibilityToggled, this,
            [this](bool visible) { m_container->setFencesVisible(visible); });
    connect(m_tray, &ui::TrayIcon::peekToggled, this,
            [this](bool active) { m_container->setPeekActive(active); });
    connect(m_tray, &ui::TrayIcon::desktopIconsToggled, this,
            &ProductApp::onDesktopIconsToggled);
    connect(m_tray, &ui::TrayIcon::restoreIconsRequested, this,
            &ProductApp::restoreDesktopIcons);
    // F-038 / T-057: the tray speaks for the whole desktop, so its material entry
    // applies to every fence (a fence's own menu is the per-fence one).
    connect(m_tray, &ui::TrayIcon::fenceMaterialRequested, this,
            &ProductApp::applyMaterialToAllFences);

    if (!m_tray->show())
        qCWarning(lc) << "the tray icon could not be shown";

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
    // T-088: the settings window's checkbox is the user's handle on the preference,
    // and it is the *only* thing it is allowed to change (T-059: it holds no
    // container, so it cannot touch a fence). One signal, one method, and that
    // method is the same one a probe or a hot key would call.
    connect(m_settings, &ui::SettingsDialog::desktopIconsOwnedToggled, this,
            &ProductApp::setOwnsDesktopIcons);
    m_settings->setDesktopIconsOwned(m_prefs.desktopIconsOwned());
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

    if (!m_hotkeys->registerHotkey(kPeekHotkeyId, kPeekModifiers, kPeekKey)) {
        // Reported, never swallowed: the failure of a documented combination is
        // information, not noise (session 15).
        qCWarning(lc).noquote()
            << QStringLiteral("could not register %1 for Peek (already taken?)")
                   .arg(platform::win::HotkeyManager::describe(kPeekModifiers, kPeekKey));
    }
    if (!m_hotkeys->registerHotkey(kHideHotkeyId, kHideModifiers, kHideKey)) {
        qCWarning(lc).noquote()
            << QStringLiteral("could not register %1 for quick hide (already taken?)")
                   .arg(platform::win::HotkeyManager::describe(kHideModifiers, kHideKey));
    }

    connect(m_hotkeys, &platform::win::HotkeyManager::triggered, this, [this](int id) {
        if (id == kPeekHotkeyId)
            m_container->togglePeek();
        else if (id == kHideHotkeyId)
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
    const QString id = createFenceWithDefaultPlacement();
    if (id.isEmpty())
        return;
    if (m_tray)
        m_tray->notify(tr("StarFish"), tr("Created \"%1\"").arg(m_container->fenceTitle(id)));
}

void ProductApp::onDesktopMenu(const QRect& rect, const QPoint& point)
{
    if (!m_container)
        return;

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
    if (ui::DesktopBackdrop* backdrop = m_container->backdrop()) {
        if (platform::win::DesktopSurface* surface = backdrop->surface())
            owner = reinterpret_cast<void*>(surface->hwnd());
    }

    const platform::win::ShellContextMenu::BackgroundChoice choice =
        platform::win::ShellContextMenu::showForDesktopBackground(point, owner, label);

    // Only our own entry creates anything. The shell's verbs have already run
    // inside the menu call, and a dismissed menu creates nothing at all.
    if (choice != platform::win::ShellContextMenu::BackgroundChoice::StarfishVerb)
        return;

    const QString id = m_container->createFence(nextFenceTitle(), target);
    if (id.isEmpty())
        return;
    qCInfo(lc).noquote() << QStringLiteral("created %1 at %2,%3 %4x%5 from the desktop menu")
                                .arg(id)
                                .arg(target.x())
                                .arg(target.y())
                                .arg(target.width())
                                .arg(target.height());
    if (m_tray)
        m_tray->notify(tr("StarFish"), tr("Created \"%1\"").arg(m_container->fenceTitle(id)));
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
    // F-022: the desktop's own double-click, the gesture the feature was always
    // specified with. It is the same toggle the tray and the hot key use.
    connect(backdrop, &ui::DesktopBackdrop::quickHideRequested, this,
            [this] { m_container->toggleFencesVisible(); });

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
                const int filed = m_container->fileInto(id, {m_container->itemForPath(path)});
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
                // The same resolution a fence's tile uses (F-006/T-005): a .lnk or
                // a .url opens through its target, never through the link file.
                const QString target = ui::IconGrid::launchPath(m_container->itemForPath(path));
                if (target.isEmpty())
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
}

void ProductApp::showFenceMenu(const QString& id, const QPoint& globalPos)
{
    if (!m_container || !m_container->contains(id))
        return;

    ui::FenceMenu::State state;
    state.rolledUp = m_container->fenceRolledUp(id);
    // Read from the fence every time the menu opens rather than remembered here:
    // the check mark must follow the document, and the document may have been
    // changed by the tray a moment ago.
    state.material = m_container->fenceMaterial(id);
    // T-082: what the material covers. Both come from the document -- the material
    // *is* the fence's alpha -- so the menu cannot drift from what is on screen.
    state.materialIsTranslucent =
        ui::FenceMaterial::isTranslucent(ui::FenceMaterial::alphaOf(m_container->fenceStyle(id).background));
    state.fadeIcons = m_container->fenceIconsFollowMaterial(id);
    // T-080: the fence's own look, read from the document each time the menu
    // opens -- the radius entry marked is the one the fence *is*, and the way back
    // to "no band" only exists when there is a band.
    const core::FenceStyle fenceStyle = m_container->fenceStyle(id);
    state.radius = fenceStyle.radius;
    state.hasTitleBarColor = !fenceStyle.titleBarColor.isEmpty();

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
            default:
                break;
            }
            // Exclusive where the choice is one of a set: a fence is shown in one
            // material and has one radius, and a radio mark that can mark two at
            // once would be describing a state the document cannot hold (the
            // material *is* the alpha of the fence's colour). The colours submenu
            // is commands, not a state, so it gets no group at all.
            const bool radio = entry.action != ui::FenceMenu::Action::Colours;
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
                    connect(choice, &QAction::triggered, this,
                            [this, id, what, value] { runFenceMenuCommand(id, what, value); });
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
    case ui::FenceMenu::Action::Rename: {
        bool accepted = false;
        const QString current = m_container->fenceTitle(id);
        const QString title = QInputDialog::getText(nullptr, tr("Rename fence"),
                                                    tr("Fence name:"), QLineEdit::Normal,
                                                    current, &accepted);
        const QString name = title.trimmed();
        // An empty or unchanged name changes nothing, and the name is stored
        // trimmed: it is the one piece of text the fence shows, and a title made
        // of spaces would leave a fence that looks unnamed with no way to say so.
        if (accepted && !name.isEmpty() && name != current)
            onRenameFence(id, name);
        break;
    }
    case ui::FenceMenu::Action::Remove:
        onRemoveFence(id);
        break;
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
        const QColor picked = QColorDialog::getColor(QColor(current.background), nullptr,
                                                     tr("Panel colour"),
                                                     QColorDialog::ShowAlphaChannel);
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
        const QColor picked = QColorDialog::getColor(start, nullptr, tr("Title bar colour"),
                                                     QColorDialog::ShowAlphaChannel);
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

bool ProductApp::ownsDesktopIcons() const
{
    return m_prefs.desktopIconsOwned();
}

bool ProductApp::setOwnsDesktopIcons(bool owned)
{
    if (m_prefs.desktopIconsOwned() == owned)
        return true;

    m_prefs.setDesktopIconsOwned(owned);
    QString error;
    const bool saved = m_prefs.save(&error);
    if (!saved) {
        // Said out loud, and not hidden behind a checkbox that stays ticked: this
        // run behaves as asked, the next one will not remember it. A choice that
        // silently fails to persist is worse than one that refuses to be made.
        qCWarning(lc).noquote()
            << QStringLiteral("could not write the preferences %1: %2")
                   .arg(m_prefs.filePath(), error);
        if (m_tray)
            m_tray->notify(tr("StarFish"), tr("The choice could not be saved: %1").arg(error));
    }

    if (owned) {
        // The desktop has to be re-read before the layer goes away: the icons
        // outside the fences are redrawn at the slots the Shell gave them.
        refreshDesktopItems();
        if (m_tray)
            m_tray->notify(tr("StarFish"),
                           tr("An icon a fence holds is now drawn only in its fence. The icons outside the fences no longer answer the mouse until this is turned off."));
    }

    // Turning it on takes the layer over as soon as a fence holds an icon; turning
    // it off gives the icons back immediately. Both are syncNativeIcons' job, so
    // there is exactly one place where "the icons are ours" is decided.
    syncNativeIcons();

    if (m_tray)
        m_tray->setRestoreIconsOffered(m_options.claimDesktop || m_prefs.desktopIconsOwned());
    if (m_settings)
        m_settings->setDesktopIconsOwned(m_prefs.desktopIconsOwned());
    refreshStatus();
    return saved;
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
    // ⚠️ T-066 -- and StarFish does **not** take the list over either, in the
    // product, unless the user asked for it. Hiding the desktop's icons and drawing
    // them ourselves means the icons on screen stop being the user's: they cannot be
    // clicked, dragged or right-clicked (that is T-042, unwritten). [T-066, session
    // 36: what triggered it was not a mode the user chose but the *first* icon
    // dropped into a fence -- the whole point of the application -- so the desktop
    // was taken over minutes after a normal launch, with no way back except
    // quitting. The user reported exactly that: "starfish ne masque pas les icônes
    // du bureau, affiche les fences sur le bureau normal, et que ce soit comme ça
    // dès le démarrage".]
    //
    // T-088 keeps the lesson and gives the wish a handle: the takeover happens
    // **only** behind `claimDesktop` (the development path and the probes) or behind
    // the user's own switch (the settings window's checkbox, persisted in
    // core::Preferences). It is still never a consequence of filing an icon -- the
    // switch decides, once, and the guardrail's own words are the rule: "si un jour
    // il doit revenir, ce sera une case à cocher assumée, jamais un effet de bord de
    // « ranger une icône »".
    //
    // The price stays stated rather than hidden: with the switch off, an icon a
    // fence holds is drawn **twice**, once by the desktop at its place and once as a
    // tile in the fence. Removing the duplicate is what T-042 is for; until then a
    // duplicate is a cosmetic annoyance while a dead desktop is a broken product.
    if (!m_options.claimDesktop && !m_prefs.desktopIconsOwned()) {
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
    QString error;
    QList<core::DesktopItem> items = platform::win::DesktopScanner::scan(&error);
    if (!error.isEmpty())
        qCWarning(lc).noquote() << QStringLiteral("could not read the desktop: %1").arg(error);
    // The Windows desktop sorts by name unless the user turned auto-arrange off.
    // This is only the *enumeration* order: since ADR-13 the positions come from
    // the shell itself (syncNativeIcons -> the backdrop's native rectangles), and
    // the icons on screen are the native ones, in the order the user put them.
    core::sortForDisplay(items, QStringLiteral("name"), true);
    m_container->setDesktopItems(std::move(items));
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
    if (m_settings)
        m_settings->setStatusText(statusText());
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
    // T-088: the user's own switch, described as what it is *doing* rather than as
    // what was asked for -- with no fenced icon there is nothing to draw, and the
    // line must not claim otherwise.
    if (m_prefs.desktopIconsOwned())
        parts << (m_ownsIcons ? tr("StarFish draws the desktop icons")
                              : tr("StarFish will draw the desktop icons (no fenced icon yet)"));
    if (m_container->backdrop() && m_container->backdrop()->enabled())
        parts << m_container->backdrop()->statusText();
    parts << (m_watcher && m_watcher->isWatching()
                  ? tr("watching %1 folder(s)").arg(m_watcher->folders().size())
                  : tr("not watching any folder"));

    if (m_hotkeys) {
        if (m_hotkeys->isRegistered(kPeekHotkeyId))
            parts << tr("Peek: %1").arg(
                platform::win::HotkeyManager::describe(kPeekModifiers, kPeekKey));
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

    if (m_store) {
        QString error;
        if (!m_store->saveNow(&error))
            qCWarning(lc).noquote() << QStringLiteral("last write failed: %1").arg(error);
    }

    delete m_settings;
    m_settings = nullptr;
    delete m_tray;
    m_tray = nullptr;

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

} // namespace starfish::app
