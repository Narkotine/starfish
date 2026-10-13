#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QPixmap>
#include <QRect>
#include <QTimer>
#include <QWindow>

#include "core/desktop_item.hpp"
#include "core/zorder.hpp"
#include "platform/win/desktop_host.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "ui/fence_view.hpp"
#include "version.h"

#if defined(_WIN32)
#include <cstdio>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

QLoggingCategory lc("starfish.main");

#if defined(_WIN32)
// True when the caller already gave us a usable stdout/stderr (a pipe, a
// file): the redirection must then be preserved.
bool hasRedirectedOutput()
{
    const HANDLE handles[2] = {::GetStdHandle(STD_OUTPUT_HANDLE),
                               ::GetStdHandle(STD_ERROR_HANDLE)};
    for (const HANDLE handle : handles) {
        if (!handle || handle == INVALID_HANDLE_VALUE)
            continue;
        const DWORD type = ::GetFileType(handle);
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK)
            return true;
    }
    return false;
}

// The executable is a GUI app (WIN32 in CMake) and therefore has no console of
// its own: borrow the one it was launched from, otherwise --dump and the log
// output would go nowhere.
void attachParentConsole()
{
    // Never hijack a redirection: freopen on CONOUT$ would silently discard a
    // pipe or a file the caller asked for (e.g. `starfish --dump > log.txt`).
    if (hasRedirectedOutput()) {
        // Qt decides on its own whether stderr is usable and, when it thinks it
        // is not, sends qCInfo/qCWarning to the debugger (OutputDebugString)
        // instead -- which is exactly how CLI diagnostics get lost. We have just
        // established that the caller gave us a real sink, so tell Qt about it.
        qputenv("QT_ASSUME_STDERR_HAS_CONSOLE", "1");
        return;
    }
    if (!::AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE* stream = nullptr;
#if defined(_MSC_VER)
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#else
    stream = freopen("CONOUT$", "w", stdout);
    stream = freopen("CONOUT$", "w", stderr);
#endif
    (void)stream;
}
#endif

// One line per desktop entry, for the --list-desktop diagnostic (T-004).
QString describeDesktopItem(const starfish::core::DesktopItem& item)
{
    const QString kind = item.isDir    ? QStringLiteral("dir ")
                         : item.isLink ? QStringLiteral("link")
                                       : QStringLiteral("file");
    QString line = QStringLiteral("%1  %2  %3")
                       .arg(kind, -4)
                       .arg(item.isHidden ? QStringLiteral("hidden") : QStringLiteral("------"))
                       .arg(item.name);
    if (item.sizeBytes >= 0)
        line += QStringLiteral("  %1 B").arg(item.sizeBytes);
    if (item.fromPublicDesktop)
        line += QStringLiteral("  [public desktop]");
    if (!item.targetPath.isEmpty())
        line += QStringLiteral("  -> %1").arg(item.targetPath);
    return line;
}

// File-system-safe stem for --icon-dump output files.
QString dumpStem(const QString& name)
{
    QString stem = name;
    for (QChar& ch : stem) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_'))
            ch = QLatin1Char('_');
    }
    return stem.isEmpty() ? QStringLiteral("item") : stem;
}

} // namespace

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    attachParentConsole();
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StarFish"));
    QApplication::setApplicationVersion(QStringLiteral(STARFISH_VERSION));

    // qCInfo is not guaranteed to be on by default for custom categories.
    QLoggingCategory::setFilterRules(QStringLiteral("starfish.*=true"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("StarFish -- open-source desktop fences for Windows."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption dumpOption(
        QStringLiteral("dump"),
        QStringLiteral("Print the desktop window hierarchy (Progman/DefView) and exit."));
    const QCommandLineOption listDesktopOption(
        QStringLiteral("list-desktop"),
        QStringLiteral("List the desktop folder contents as the Shell sees them and exit."));
    const QCommandLineOption iconsOption(
        QStringLiteral("icons"),
        QStringLiteral("With --list-desktop: also resolve the Shell icon of every entry."));
    const QCommandLineOption iconSizeOption(
        QStringLiteral("icon-size"),
        QStringLiteral("With --list-desktop --icons: requested icon edge length in pixels."),
        QStringLiteral("px"), QStringLiteral("48"));
    const QCommandLineOption iconDumpOption(
        QStringLiteral("icon-dump"),
        QStringLiteral("With --list-desktop --icons: write every resolved icon as a PNG into "
                       "this directory (a non-null pixmap is not proof that it is drawn)."),
        QStringLiteral("dir"));
    const QCommandLineOption demoOption(
        QStringLiteral("host-demo"),
        QStringLiteral("Host an empty fence on the desktop (Phase 1 placement check)."));
    const QCommandLineOption secondsOption(
        QStringList{QStringLiteral("s"), QStringLiteral("seconds")},
        QStringLiteral("How long the demo fence stays up, in seconds."),
        QStringLiteral("seconds"), QStringLiteral("10"));
    const QCommandLineOption hideIconsOption(
        QStringLiteral("hide-icons"),
        QStringLiteral("Hide the native desktop icons while the demo runs (restored on exit)."));

    parser.addOption(dumpOption);
    parser.addOption(listDesktopOption);
    parser.addOption(iconsOption);
    parser.addOption(iconSizeOption);
    parser.addOption(iconDumpOption);
    parser.addOption(demoOption);
    parser.addOption(secondsOption);
    parser.addOption(hideIconsOption);
    parser.process(app);

    if (parser.isSet(dumpOption)) {
        starfish::platform::win::DesktopHost::dumpDesktopHierarchy();
        return 0;
    }

    if (parser.isSet(listDesktopOption)) {
        const QStringList folders = starfish::platform::win::DesktopScanner::desktopFolders();
        qCInfo(lc).noquote() << QStringLiteral("desktop folders: %1")
                                    .arg(folders.isEmpty() ? QStringLiteral("(none resolved)")
                                                           : folders.join(QStringLiteral(" | ")));

        QString error;
        const QList<starfish::core::DesktopItem> items =
            starfish::platform::win::DesktopScanner::scan(&error);
        if (!error.isEmpty())
            qCWarning(lc).noquote() << error;

        const bool withIcons = parser.isSet(iconsOption);
        bool sizeOk = false;
        const int iconSize = parser.value(iconSizeOption).toInt(&sizeOk);
        starfish::platform::win::ShellIconCache icons;

        QString dumpDir;
        if (parser.isSet(iconDumpOption)) {
            dumpDir = parser.value(iconDumpOption);
            if (!QDir().mkpath(dumpDir)) {
                qCWarning(lc).noquote() << QStringLiteral("could not create %1").arg(dumpDir);
                dumpDir.clear();
            }
        }

        qCInfo(lc).noquote() << QStringLiteral("%1 desktop entries").arg(items.size());
        int index = 0;
        for (const starfish::core::DesktopItem& item : items) {
            QString line = describeDesktopItem(item);
            if (withIcons) {
                const QPixmap icon =
                    icons.iconFor(item.path, sizeOk && iconSize > 0 ? iconSize : 48);
                line += icon.isNull() ? QStringLiteral("  [no icon]")
                                      : QStringLiteral("  [icon %1x%2]")
                                            .arg(icon.width())
                                            .arg(icon.height());
                if (!dumpDir.isEmpty() && !icon.isNull()) {
                    const QString file = QStringLiteral("%1/%2-%3.png")
                                             .arg(dumpDir)
                                             .arg(index, 3, 10, QLatin1Char('0'))
                                             .arg(dumpStem(item.name));
                    if (!icon.save(file))
                        qCWarning(lc).noquote() << QStringLiteral("could not write %1").arg(file);
                }
            }
            qCInfo(lc).noquote() << line;
            ++index;
        }
        if (withIcons) {
            qCInfo(lc).noquote() << QStringLiteral("%1 icons cached")
                                        .arg(icons.cachedCount());
        }
        return 0;
    }

    if (parser.isSet(demoOption)) {
        bool parsed = false;
        const int seconds = parser.value(secondsOption).toInt(&parsed);
        const int durationMs = (parsed && seconds > 0 ? seconds : 10) * 1000;

        auto* host = new starfish::platform::win::DesktopHost(&app);
        auto* fence = new starfish::ui::FenceView;
        fence->setTitle(QStringLiteral("StarFish (Phase 1)"));
        fence->setMinimumSize(160, 120);

        const QRect geometry(80, 80, 480, 320);
        if (!host->attach(fence, geometry)) {
            qCCritical(lc) << "could not host the fence on the desktop";
            delete fence;
            return 1;
        }

        // Qt's own show/resize may have re-stacked the window: re-apply the
        // placement, then report where we ended up.
        host->syncZOrder();

        if (parser.isSet(hideIconsOption))
            host->setIconsHidden(true);

        const auto verdict = host->verifyPlacement(fence);
        qCInfo(lc).noquote() << QStringLiteral("placement verdict: %1")
                                    .arg(QString::fromLatin1(
                                        starfish::core::placementVerdictText(verdict)));
        qCInfo(lc) << "a correct z-order alone proves nothing (ADR-4): confirm visually that "
                      "the panel sits above the wallpaper and under the desktop icons.";

        // A hosted window is a child of Progman and dies with it: re-attach
        // after an Explorer restart, on the next event-loop turn.
        QObject::connect(
            host, &starfish::platform::win::DesktopHost::desktopReacquired, &app,
            [host, fence, geometry] {
                qCWarning(lc) << "desktop re-acquired, re-attaching the fence";
                QTimer::singleShot(0, host, [host, fence, geometry] {
                    if (host->attach(fence, geometry))
                        host->syncZOrder();
                });
            });

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [host, fence] {
            if (host->iconsHidden())
                host->setIconsHidden(false); // never leave the user without icons
            delete fence;
        });

        QTimer::singleShot(durationMs, &app, &QCoreApplication::quit);
        return app.exec();
    }

    qCInfo(lc) << "StarFish" << STARFISH_VERSION
               << "skeleton starting (no desktop integration; use --host-demo)";

    // Keep the skeleton headless-friendly so CI can run it.
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
