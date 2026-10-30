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

#include <functional>

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
                // T-090: a new item has no position of its own; the shell gives it
                // a free slot, and that is where it is drawn -- on no other icon.
                const QRect mine = backdrop->cells().at(index).icon;
                int overlaps = 0;
                for (int i = 0; i < backdrop->cells().size(); ++i) {
                    if (i != index && backdrop->cells().at(i).icon.intersects(mine))
                        ++overlaps;
                }
                check(QStringLiteral("...in a free slot of its own, on the screen (T-090)"),
                      overlaps == 0 && backdrop->area().contains(mine),
                      QStringLiteral("at %1,%2, %3 overlap(s)")
                          .arg(mine.x())
                          .arg(mine.y())
                          .arg(overlaps));
            }

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

            // A virtual item (This PC, the Recycle Bin) is filed like any icon
            // (the user's decision, session 58), under the name the desktop gives it.
            {
                QString virtualName;
                QString virtualLabel;
                for (const platform::win::ShellViewItem& item : state.items) {
                    if (item.parsingName.startsWith(QLatin1String("::"))) {
                        virtualName = item.parsingName;
                        virtualLabel = item.displayName;
                        break;
                    }
                }
                if (!virtualName.isEmpty()) {
                    ui::FenceContainer* container = product.container();
                    const QString fence = container->createFence(
                        QStringLiteral("Virtual"),
                        QRect(backdrop->area().center() - QPoint(160, 120), QSize(320, 240)));
                    const core::DesktopItem item = container->itemForPath(virtualName);
                    const int filed = fence.isEmpty() ? 0 : container->fileInto(fence, {item});
                    waitMs(600);
                    check(QStringLiteral("a virtual item is filed and leaves the desktop"),
                          filed == 1 && cellIndex(backdrop, virtualName) < 0
                              && item.name == virtualLabel,
                          QStringLiteral("%1 \"%2\" (tile \"%3\")")
                              .arg(virtualName, virtualLabel, item.name));
                    platform::win::DesktopShellView::captureDesktop(
                        QRect(backdrop->area().center() - QPoint(300, 200), QSize(600, 400))
                            .intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/virtual_item_in_fence.png"));
                    if (!fence.isEmpty())
                        container->removeFence(fence);
                    waitMs(600);
                    check(QStringLiteral("...and comes back when its fence goes"),
                          cellIndex(backdrop, virtualName) >= 0, virtualName);
                }
            }

            // The shortcut arrow on fence tiles is a setting (session 58). A
            // shortcut of our own, filed, drawn with and without it; the user's
            // value is put back whatever happens.
            {
                const QString link = QDir(desktop).filePath(QStringLiteral("StarFish probe arrow.lnk"));
                const QString linkNative = QDir::toNativeSeparators(link);
                const bool userValue = ProductApp::fenceShortcutArrowsSetting();
                if (!QFileInfo::exists(link) && QFile::link(testFile, link)) {
                    waitMs(2000);
                    ui::FenceContainer* container = product.container();
                    const QRect where(backdrop->area().center() - QPoint(160, 120), QSize(320, 240));
                    const QString fence = container->createFence(QStringLiteral("Arrows"), where);
                    container->fileInto(fence, {container->itemForPath(linkNative)});
                    product.setFenceShortcutArrows(true);
                    waitMs(500);
                    const QImage with = platform::win::DesktopShellView::captureDesktop(where);
                    product.setFenceShortcutArrows(false);
                    waitMs(500);
                    const QImage without = platform::win::DesktopShellView::captureDesktop(where);
                    with.save(captures + QStringLiteral("/fence_arrow_on.png"));
                    without.save(captures + QStringLiteral("/fence_arrow_off.png"));
                    check(QStringLiteral("the fence-tile shortcut arrow follows its setting"),
                          !with.isNull() && with != without,
                          QStringLiteral("default on; captures fence_arrow_on/off.png"));
                    container->removeFence(fence);
                }
                product.setFenceShortcutArrows(userValue);
                QFile::remove(link);
                waitMs(1000);
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

            // ---- Lot 2 (T-090), simulated in this process: nothing is sent to
            // Explorer, nothing is clicked. What Explorer would do is done to
            // *our* windows and to the list the way StarFish itself shows it.
            const auto waitFor = [](int ms, const std::function<bool()>& done) {
                for (int waited = 0; waited < ms; waited += 250) {
                    if (done())
                        return true;
                    waitMs(250);
                }
                return done();
            };
            const auto wholeDesktop = [&] {
                const platform::win::ShellViewState now =
                    product.shellView() ? product.shellView()->read()
                                        : platform::win::ShellViewState{};
                return product.drawnDesktop() && backdrop->drawsWholeDesktop()
                       && backdrop->isAttached() && !listVisible() && marker.isPresent()
                       && now.ok && backdrop->cells().size() == now.items.size();
            };

            // 1. The shell shows its list again on its own (a rebuilt view).
            platform::win::DesktopSurface::setDesktopIconsHidden(false);
            const bool rehidden =
                waitFor(5000, [&] { return product.listReshownCount() >= 1 && wholeDesktop(); });
            check(QStringLiteral("a list the shell shows again is hidden again (T-090)"), rehidden,
                  QStringLiteral("re-hidden %1 time(s)").arg(product.listReshownCount()));

            // 2. The screens change (same arrangement here: one monitor).
            product.screensChangedForProbe();
            waitMs(800);
            check(QStringLiteral("after a screen change the desktop is drawn whole"),
                  wholeDesktop() && backdrop->wallpaper().size() == backdrop->area().size(),
                  QStringLiteral("wallpaper %1x%2, area %3x%4")
                      .arg(backdrop->wallpaper().width())
                      .arg(backdrop->wallpaper().height())
                      .arg(backdrop->area().width())
                      .arg(backdrop->area().height()));

            // 3. Explorer restarts: its new list is visible, our windows -- the
            //    desktop and a fence -- die with the old Progman. The list comes
            //    first, so the user never sees a desktop without icons.
            ui::FenceContainer* container = product.container();
            const QString survivor = container->createFence(
                QStringLiteral("Survivor"),
                QRect(backdrop->area().center() - QPoint(150, 100), QSize(300, 200)));
            waitMs(300);
            const int rehostedBefore = container->rehostedFenceCount();
            platform::win::DesktopSurface::setDesktopIconsHidden(false);
            if (platform::win::DesktopSurface* fenceSurface = container->surface(survivor))
                ::DestroyWindow(reinterpret_cast<HWND>(fenceSurface->hwnd()));
            if (platform::win::DesktopSurface* surface = backdrop->surface())
                ::DestroyWindow(reinterpret_cast<HWND>(surface->hwnd()));
            const bool dropped = waitFor(4000, [&] {
                return product.desktopLosses() >= 1 && !product.drawnDesktop() && listVisible();
            });
            check(QStringLiteral("a lost desktop window drops the drawn desktop, icons visible"),
                  dropped,
                  QStringLiteral("losses %1, icons visible %2")
                      .arg(product.desktopLosses())
                      .arg(listVisible() ? QLatin1String("yes") : QLatin1String("no")));
            const bool back = waitFor(15000, wholeDesktop);
            check(QStringLiteral("...and the desktop is drawn again once it is back"), back,
                  QStringLiteral("%1 drawn").arg(backdrop->cells().size()));
            const bool fenceBack = waitFor(5000, [&] {
                platform::win::DesktopSurface* s = container->surface(survivor);
                return s && s->isAttached() && container->rehostedFenceCount() > rehostedBefore;
            });
            check(QStringLiteral("...and a fence lost with it is hosted again"), fenceBack,
                  QStringLiteral("%1 fence(s) hosted again")
                      .arg(container->rehostedFenceCount() - rehostedBefore));
            platform::win::DesktopShellView::captureDesktop(backdrop->area())
                .save(captures + QStringLiteral("/lot2_after_simulated_restart.png"));
            container->removeFence(survivor);
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
