#include "app/drawn_desktop_probe.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/product_app.hpp"
#include "core/icon_hiding_marker.hpp"
#include "platform/win/desktop_shell_view.hpp"
#include "platform/win/desktop_surface.hpp"
#include "ui/backdrop_layout.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_container.hpp"

namespace starfish::app {

namespace {

Q_LOGGING_CATEGORY(lc, "starfish.probe.drawn")

void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The screen as the user sees it, the open menu included (a popup is not a
// child of Progman, so PrintWindow would not see it).
QImage grabScreen(const QRect& rect)
{
    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, rect.width(), rect.height());
    HGDIOBJ previous = ::SelectObject(memory, bitmap);
    ::BitBlt(memory, 0, 0, rect.width(), rect.height(), screen, rect.x(), rect.y(), SRCCOPY);
    ::SelectObject(memory, previous);
    QImage image = QImage::fromHBITMAP(bitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return image;
}

int cellIndex(const ui::DesktopBackdrop* backdrop, const QString& path)
{
    const QString key = ui::BackdropLayout::pathKey(path);
    for (int i = 0; i < backdrop->cells().size(); ++i) {
        if (ui::BackdropLayout::pathKey(backdrop->cells().at(i).item.path) == key)
            return i;
    }
    return -1;
}

} // namespace

int runDrawnDesktopProbe(QApplication& app)
{
    Q_UNUSED(app);
    QStringList log;
    int failures = 0;
    auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
        if (!ok)
            ++failures;
        log << QStringLiteral("%1  %2  %3")
                   .arg(ok ? QLatin1String("PASS") : QLatin1String("FAIL"))
                   .arg(step, -60)
                   .arg(detail);
    };

    const QString probeDir = QStringLiteral("build/drawn_probe");
    const QString captures = QStringLiteral("build/captures");
    QDir().mkpath(probeDir);
    QDir().mkpath(captures);
    const QString layoutPath = probeDir + QStringLiteral("/layout.json");
    QFile::remove(layoutPath);

    core::IconHidingMarker marker;
    const auto listVisible = [] { return !platform::win::DesktopSurface::desktopIconsHidden(); };
    const bool visibleAtStart = listVisible();
    check(QStringLiteral("the desktop's icons are visible at the start"), visibleAtStart,
          QStringLiteral("the probe refuses to run otherwise"));

    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString testFile = QDir(desktop).filePath(QStringLiteral("StarFish probe (lot 1).txt"));
    const QString renamedFile =
        QDir(desktop).filePath(QStringLiteral("StarFish probe (lot 1, renamed).txt"));
    const QString testNative = QDir::toNativeSeparators(testFile);
    const QString renamedNative = QDir::toNativeSeparators(renamedFile);

    if (visibleAtStart && !QFileInfo::exists(testFile) && !QFileInfo::exists(renamedFile)) {
        ProductApp product;
        ProductApp::Options options;
        options.layoutPath = layoutPath;
        options.hostOnDesktop = true;
        options.tray = false;
        options.settingsWindow = false;
        options.showSettingsOnStart = false;
        options.hotkeys = false;
        options.watchDesktop = true;
        options.starterFence = false; // no fence: every item of the view is drawn
        options.claimDesktop = true;
        QString error;
        const bool started = product.start(options, &error);
        check(QStringLiteral("the product starts"), started, error);

        ui::DesktopBackdrop* backdrop =
            product.container() ? product.container()->backdrop() : nullptr;
        if (started && backdrop) {
            waitMs(500);
            check(QStringLiteral("the desktop is drawn by StarFish"), product.drawnDesktop(),
                  backdrop->statusText());
            check(QStringLiteral("the backdrop is the opaque whole-desktop surface"),
                  backdrop->drawsWholeDesktop() && backdrop->surface()
                      && !backdrop->surface()->perPixelAlpha()
                      && backdrop->surface()->ownsAllInput(),
                  QStringLiteral("wallpaper %1x%2")
                      .arg(backdrop->wallpaper().width())
                      .arg(backdrop->wallpaper().height()));
            check(QStringLiteral("the native list is hidden, the marker written (T-046)"),
                  !listVisible() && marker.isPresent(), marker.path());

            const platform::win::ShellViewState state = product.shellView()->read();
            check(QStringLiteral("every item of the shell's view is drawn"),
                  state.ok && backdrop->cells().size() == state.items.size(),
                  QStringLiteral("%1 in the view, %2 drawn")
                      .arg(state.items.size())
                      .arg(backdrop->cells().size()));
            int placed = 0;
            for (const platform::win::ShellViewItem& item : state.items) {
                const int i = cellIndex(backdrop, item.parsingName);
                if (i >= 0 && backdrop->cells().at(i).icon.x() == item.position.x())
                    ++placed;
            }
            check(QStringLiteral("each icon is where the shell keeps it"),
                  placed == state.items.size(),
                  QStringLiteral("%1 of %2 at the view's own x").arg(placed).arg(state.items.size()));

            const QImage full = platform::win::DesktopShellView::captureDesktop(backdrop->area());
            full.save(captures + QStringLiteral("/lot1_drawn_desktop.png"));

            const QStringList background = product.shellView()->backgroundMenuLabels();
            const int submenuEntries =
                static_cast<int>(std::count_if(background.begin(), background.end(),
                                               [](const QString& l) {
                                                   return l.contains(QLatin1String(" > "));
                                               }));
            check(QStringLiteral("the real background menu, submenus filled"),
                  background.size() >= 5 && submenuEntries >= 3,
                  QStringLiteral("%1 entries, %2 in submenus").arg(background.size()).arg(submenuEntries));
            for (const QString& label : background)
                log << QStringLiteral("        bg: %1").arg(label);

            // A file of our own appears...
            {
                QFile file(testFile);
                file.open(QIODevice::WriteOnly);
                file.write("StarFish probe -- deleted by the probe itself.\n");
            }
            int index = -1;
            for (int waited = 0; waited < 6000 && index < 0; waited += 250) {
                waitMs(250);
                index = cellIndex(backdrop, testNative);
            }
            check(QStringLiteral("a new file on the Desktop appears on the drawn desktop"),
                  index >= 0, testNative);

            if (index >= 0) {
                const QRect cell = backdrop->cells().at(index).icon.united(
                    backdrop->cells().at(index).label);
                const QStringList itemMenu = product.shellView()->selectionMenuLabels({testNative});
                check(QStringLiteral("the item's real shell menu"), itemMenu.size() >= 5,
                      QStringLiteral("%1 entries").arg(itemMenu.size()));
                for (const QString& label : itemMenu)
                    log << QStringLiteral("        item: %1").arg(label);

                backdrop->setSelection({testNative});
                backdrop->renderNow();
                waitMs(200);
                const QRect around = cell.adjusted(-120, -60, 120, 60).intersected(backdrop->area());
                platform::win::DesktopShellView::captureDesktop(around)
                    .save(captures + QStringLiteral("/lot1_selected_item.png"));

                // The two menus as the user sees them, closed by the probe.
                const QPoint at = cell.center();
                const QRect shot(at.x() - 40, at.y() - 40, 520, 900);
                QTimer::singleShot(1000, [&] {
                    grabScreen(shot.intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/lot1_item_menu.png"));
                    ::EndMenu();
                });
                product.shellView()->showSelectionMenu({testNative}, at, product.backdropOwner(), {});
                const QPoint bare(backdrop->area().center().x(), backdrop->area().center().y() - 300);
                const QRect bareShot(bare.x() - 40, bare.y() - 40, 640, 760);
                QTimer::singleShot(1000, [&] {
                    grabScreen(bareShot.intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/lot1_background_menu.png"));
                    ::EndMenu();
                });
                product.showDesktopMenu(bare);
                backdrop->setSelection({});

                // The point of it all (ADR-21): filed into a fence, the icon
                // leaves the desktop -- and its file stays exactly where it is.
                ui::FenceContainer* container = product.container();
                const QPoint middle = backdrop->area().center();
                const QString fence = container->createFence(
                    QStringLiteral("Probe"), QRect(middle - QPoint(160, 120), QSize(320, 240)));
                const int filed = fence.isEmpty()
                                      ? 0
                                      : container->fileInto(fence, {container->itemForPath(testNative)});
                waitMs(600);
                check(QStringLiteral("an icon filed into a fence leaves the drawn desktop"),
                      filed == 1 && cellIndex(backdrop, testNative) < 0 && QFileInfo::exists(testFile),
                      QStringLiteral("filed %1, drawn %2, file still on the Desktop: %3")
                          .arg(filed)
                          .arg(cellIndex(backdrop, testNative) >= 0 ? QLatin1String("yes")
                                                                    : QLatin1String("no"))
                          .arg(QFileInfo::exists(testFile) ? QLatin1String("yes")
                                                           : QLatin1String("no")));
                platform::win::DesktopShellView::captureDesktop(
                    QRect(middle - QPoint(400, 300), QSize(800, 600)).intersected(backdrop->area()))
                    .save(captures + QStringLiteral("/lot1_filed_in_fence.png"));
                if (!fence.isEmpty())
                    container->removeFence(fence);
                waitMs(600);
                check(QStringLiteral("...and comes back when the fence goes"),
                      cellIndex(backdrop, testNative) >= 0, testNative);
            }

            // ...is renamed...
            const bool renamed = QFile::rename(testFile, renamedFile);
            int renamedIndex = -1;
            for (int waited = 0; renamed && waited < 6000 && renamedIndex < 0; waited += 250) {
                waitMs(250);
                renamedIndex = cellIndex(backdrop, renamedNative);
            }
            check(QStringLiteral("a rename on disk follows on the drawn desktop"),
                  renamedIndex >= 0 && cellIndex(backdrop, testNative) < 0, renamedNative);

            // ...and leaves.
            QFile::remove(renamedFile);
            bool gone = false;
            for (int waited = 0; waited < 6000 && !gone; waited += 250) {
                waitMs(250);
                gone = cellIndex(backdrop, renamedNative) < 0;
            }
            check(QStringLiteral("a deletion on disk leaves the drawn desktop"), gone,
                  renamedNative);
        }

        product.shutdown();
        check(QStringLiteral("after a clean stop the native icons are back"), listVisible(),
              QString());
        check(QStringLiteral("...and the marker is gone"), !marker.isPresent(), marker.path());
    }

    // Whatever happened: our file goes, and the desktop is given back.
    QFile::remove(testFile);
    QFile::remove(renamedFile);
    if (!listVisible() && marker.isPresent())
        platform::win::DesktopSurface::setDesktopIconsHidden(false);
    QDir(probeDir).removeRecursively();
    check(QStringLiteral("the probe left the desktop as it found it"),
          listVisible() == visibleAtStart && !QFileInfo::exists(testFile)
              && !QFileInfo::exists(renamedFile),
          QString());

    const QString reportPath = QStringLiteral("build/drawn-desktop-probe.txt");
    QFile report(reportPath);
    if (report.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&report);
        out << QStringLiteral("drawn desktop probe (ADR-21): %1 failure(s)\n").arg(failures);
        for (const QString& line : log)
            out << line << '\n';
    }
    for (const QString& line : log)
        qCInfo(lc).noquote() << line;
    return failures == 0 ? 0 : 1;
}

} // namespace starfish::app
