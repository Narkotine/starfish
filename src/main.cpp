#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QLoggingCategory>
#include <QRect>
#include <QTimer>

#include "core/zorder.hpp"
#include "platform/win/desktop_host.hpp"
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
// The executable is a GUI app (WIN32 in CMake) and therefore has no console of
// its own: borrow the one it was launched from, otherwise --dump and the log
// output would go nowhere.
void attachParentConsole()
{
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
    parser.addOption(demoOption);
    parser.addOption(secondsOption);
    parser.addOption(hideIconsOption);
    parser.process(app);

    if (parser.isSet(dumpOption)) {
        starfish::platform::win::DesktopHost::dumpDesktopHierarchy();
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
