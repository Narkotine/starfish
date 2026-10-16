#include "platform/win/desktop_menu.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTimer>

#include <windows.h>

namespace starfish::platform::win {
namespace {

// The event the *running* product answers. Auto-reset, so one request makes one
// fence: a menu entry clicked twice makes two, which is what a user expects from
// a menu they clicked twice.
constexpr const wchar_t* kNewFenceEvent = L"Local\\StarFish.NewFence";

// The product's own single-instance guard (T-044). Asking the running instance is
// not an optimisation -- a second process is refused, so without this the entry
// would do nothing at all while StarFish runs.
constexpr const wchar_t* kInstanceMutex = L"Local\\StarFish.Desktop";

// Explorer's location for the desktop's *background* menu. A static verb: the
// entry lives in the registry, Explorer draws it, and the only thing that runs is
// the command line below.
constexpr const char* kShellKey = "HKEY_CURRENT_USER\\Software\\Classes\\DesktopBackground\\Shell";
constexpr const char* kEntryName = "StarFish";

QString entryPath()
{
    return QStringLiteral("\"%1\" --new-fence")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}

} // namespace

QString desktopMenuLabel()
{
    // Same language as the interface (F-036 / T-014b is what will translate it, if
    // the user wants: it is one string).
    return QObject::tr("StarFish: create a fence");
}

QString desktopMenuCommand()
{
    return entryPath();
}

bool desktopMenuInstalled()
{
    QSettings shell(QString::fromLatin1(kShellKey), QSettings::NativeFormat);
    for (const QString& group : shell.childGroups()) {
        if (group.compare(QString::fromLatin1(kEntryName), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool installDesktopMenu(QString* error)
{
    QSettings shell(QString::fromLatin1(kShellKey), QSettings::NativeFormat);
    shell.beginGroup(QString::fromLatin1(kEntryName));
    // The default value of the key is the label Explorer shows.
    shell.setValue(QStringLiteral("."), desktopMenuLabel());
    // The exe's own icon (id 1, embedded since T-065): no asset to ship.
    shell.setValue(QStringLiteral("Icon"),
                   QStringLiteral("\"%1\",0")
                       .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    // Above the "New / Refresh / Display settings" block: it is the entry the user
    // came for, and burying it under the shell's own items would be a choice we
    // have no business making.
    shell.setValue(QStringLiteral("Position"), QStringLiteral("Top"));
    shell.beginGroup(QStringLiteral("command"));
    shell.setValue(QStringLiteral("."), entryPath());
    shell.endGroup();
    shell.endGroup();
    shell.sync();

    if (shell.status() != QSettings::NoError) {
        if (error)
            *error = QObject::tr("could not write %1 (QSettings status %2)")
                         .arg(QString::fromLatin1(kShellKey))
                         .arg(int(shell.status()));
        return false;
    }
    return true;
}

bool uninstallDesktopMenu(QString* error)
{
    QSettings shell(QString::fromLatin1(kShellKey), QSettings::NativeFormat);
    shell.remove(QString::fromLatin1(kEntryName));
    shell.sync();
    if (shell.status() != QSettings::NoError) {
        if (error)
            *error = QObject::tr("could not remove %1\\%2")
                         .arg(QString::fromLatin1(kShellKey),
                              QString::fromLatin1(kEntryName));
        return false;
    }
    return true;
}

bool requestFenceFromRunningInstance()
{
    HANDLE instance = ::OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);
    if (!instance)
        return false; // nobody is running: the caller starts the product instead
    ::CloseHandle(instance);

    HANDLE request = ::CreateEventW(nullptr, FALSE, FALSE, kNewFenceEvent);
    if (!request)
        return false;
    const bool asked = ::SetEvent(request) != FALSE;
    ::CloseHandle(request);
    return asked;
}

struct NewFenceWatcher::Impl {
    HANDLE event = nullptr;
    QTimer timer;
};

NewFenceWatcher::NewFenceWatcher(QObject* parent) : QObject(parent), d(new Impl)
{
}

NewFenceWatcher::~NewFenceWatcher()
{
    stop();
    delete d;
}

void NewFenceWatcher::start()
{
    if (!d->event)
        d->event = ::CreateEventW(nullptr, FALSE, FALSE, kNewFenceEvent);
    if (!d->event)
        return; // no event: the entry simply does nothing, and nothing is broken

    d->timer.setInterval(250); // one poll per watchdog beat; the cost is a syscall
    connect(&d->timer, &QTimer::timeout, this, [this] {
        if (::WaitForSingleObject(d->event, 0) == WAIT_OBJECT_0)
            emit requested();
    });
    d->timer.start();
}

void NewFenceWatcher::stop()
{
    d->timer.stop();
    if (d->event) {
        ::CloseHandle(d->event);
        d->event = nullptr;
    }
}

} // namespace starfish::platform::win
