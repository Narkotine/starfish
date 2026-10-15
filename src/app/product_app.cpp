#include "app/product_app.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRect>
#include <QScreen>
#include <QTimer>

#include "core/desktop_watcher.hpp"
#include "core/layout_store.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/desktop_surface.hpp"
#include "platform/win/hotkey_manager.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "ui/fence_container.hpp"
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

} // namespace

ProductApp::ProductApp(QObject* parent)
    : QObject(parent)
{
}

ProductApp::~ProductApp()
{
    shutdown();
}

QSize ProductApp::desktopBounds()
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->geometry().size() : QSize(1920, 1080);
}

bool ProductApp::start(const Options& options, QString* error)
{
    if (m_started)
        return true;

    m_options = options;

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
    m_container->setDesktopBounds(desktopBounds());
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

    refreshStatus();
    m_started = true;

    if (m_options.settingsWindow && m_options.showSettingsOnStart)
        showSettings();

    return true;
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

    if (m_settings)
        m_settings->refresh();

    if (m_tray) {
        m_tray->setFenceCount(m_container->fenceCount());
        m_tray->setFencesVisible(m_container->fencesVisible());
        m_tray->setPeekActive(m_container->peekActive());
        m_tray->setDesktopIconsHidden(desktopIconsHidden());
    }

    // A portal appearing or leaving changes what has to be watched (F-018). The
    // cheap comparison keeps a fence drag (60 signals a second) from re-listing
    // the desktop folders.
    if (m_watcher && m_container->portalFolders() != m_portalFolders) {
        m_portalFolders = m_container->portalFolders();
        syncWatchedFolders();
    }

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
    connect(m_tray, &ui::TrayIcon::newFenceRequested, this, &ProductApp::onNewFence);
    connect(m_tray, &ui::TrayIcon::settingsRequested, this, &ProductApp::showSettings);
    connect(m_tray, &ui::TrayIcon::quitRequested, this, &ProductApp::quitRequested);
    connect(m_tray, &ui::TrayIcon::fencesVisibilityToggled, this,
            [this](bool visible) { m_container->setFencesVisible(visible); });
    connect(m_tray, &ui::TrayIcon::peekToggled, this,
            [this](bool active) { m_container->setPeekActive(active); });
    connect(m_tray, &ui::TrayIcon::desktopIconsToggled, this,
            &ProductApp::onDesktopIconsToggled);

    if (!m_tray->show())
        qCWarning(lc) << "the tray icon could not be shown";

    m_tray->setFenceCount(m_container->fenceCount());
    m_tray->setFencesVisible(m_container->fencesVisible());
    m_tray->setPeekActive(m_container->peekActive());
    m_tray->setDesktopIconsHidden(desktopIconsHidden());
}

void ProductApp::wireSettings()
{
    if (!m_options.settingsWindow)
        return;

    m_settings = new ui::SettingsDialog(m_container, nullptr);
    connect(m_settings, &ui::SettingsDialog::newFenceRequested, this, &ProductApp::onNewFence);
    connect(m_settings, &ui::SettingsDialog::renameFenceRequested, this,
            &ProductApp::onRenameFence);
    connect(m_settings, &ui::SettingsDialog::removeFenceRequested, this,
            &ProductApp::onRemoveFence);
    connect(m_settings, &ui::SettingsDialog::rollUpToggled, this, &ProductApp::onRollUpToggled);
    connect(m_settings, &ui::SettingsDialog::desktopIconsToggled, this,
            &ProductApp::onDesktopIconsToggled);
    m_settings->setDesktopIconsHidden(desktopIconsHidden());
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
            });

    connect(m_watcher, &core::DesktopWatcher::folderChanged, this,
            [this](const QString& folder) { m_container->refreshPortalsFor(folder); });

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

    // To the right of everything, wrapping to a new row when the line would run
    // off the screen: a fence created under an existing one looks like nothing
    // happened. The container clamps the rect to the desktop anyway.
    const QSize bounds = m_container->desktopBounds();
    const QStringList ids = m_container->fenceIds();

    int x = 80;
    int y = 80;
    for (const QString& id : ids)
        x = qMax(x, m_container->fenceRect(id).right() + 24);
    if (x + kDefaultFenceWidth > bounds.width()) {
        x = 80;
        y = 80;
        for (const QString& id : ids)
            y = qMax(y, m_container->fenceRect(id).bottom() + 24);
    }

    return m_container->createFence(tr("Fence %1").arg(m_container->fenceCount() + 1),
                                    QRect(x, y, kDefaultFenceWidth, kDefaultFenceHeight));
}

void ProductApp::onNewFence()
{
    const QString id = createFenceWithDefaultPlacement();
    if (id.isEmpty())
        return;
    if (m_tray)
        m_tray->notify(tr("StarFish"), tr("Created \"%1\"").arg(m_container->fenceTitle(id)));
}

void ProductApp::onRenameFence(const QString& id, const QString& title)
{
    m_container->setFenceTitle(id, title); // emits contentChanged -> auto-save
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
        } else if (platform::win::DesktopSurface::setDesktopIconsHidden(hidden)) {
            m_iconsHidden = hidden;
        } else {
            qCWarning(lc) << "could not change the desktop icon visibility";
        }
    }

    if (m_tray)
        m_tray->setDesktopIconsHidden(m_iconsHidden);
    if (m_settings)
        m_settings->setDesktopIconsHidden(m_iconsHidden);

    refreshStatus();
}

void ProductApp::showSettings()
{
    if (!m_settings)
        return;
    m_settings->refresh();
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
    parts << tr("%1 rule(s)").arg(m_container->rules().size());
    parts << (m_iconsHidden ? tr("desktop icons hidden") : tr("desktop icons visible"));
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

    // Restore only what we hid ourselves (see setDesktopIconsHidden).
    if (m_iconsHidden) {
        platform::win::DesktopSurface::setDesktopIconsHidden(false);
        m_iconsHidden = false;
    }

    m_started = false;
}

} // namespace starfish::app
