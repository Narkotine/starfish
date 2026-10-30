#include "app/drawn_desktop_probe.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
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
#include <ole2.h>

#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include "app/product_app.hpp"
#include "core/context_menu.hpp"
#include "ui/modern_menu.hpp"
#include "core/icon_hiding_marker.hpp"
#include "platform/win/desktop_shell_view.hpp"
#include "platform/win/desktop_surface.hpp"
#include "platform/win/packaged_commands.hpp"
#include "platform/win/shell_operations.hpp"
#include "ui/backdrop_gesture.hpp"
#include "ui/backdrop_layout.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_view.hpp"
#include "ui/percent_slider.hpp"

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

bool waitUntil(int ms, const std::function<bool()>& done)
{
    for (int waited = 0; waited < ms; waited += 100) {
        if (done())
            return true;
        waitMs(100);
    }
    return done();
}

void sendMouse(QObject* target, QEvent::Type type, const QPoint& screen, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(screen), QPointF(screen),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

void sendKey(QObject* target, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(target, &event);
}

// Plays a drag onto `target` the way OLE would, without a mouse: enter, over,
// drop at `point`. `copy` holds Ctrl (copy rather than move).
DWORD playDrop(void* target, void* data, const QPoint& point, bool copy)
{
    auto* drop = static_cast<IDropTarget*>(target);
    auto* object = static_cast<IDataObject*>(data);
    if (!drop || !object)
        return DROPEFFECT_NONE;
    const DWORD keys = MK_LBUTTON | (copy ? MK_CONTROL : 0);
    const POINTL pt{point.x(), point.y()};
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    drop->DragEnter(object, keys, pt, &effect);
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    drop->DragOver(keys, pt, &effect);
    if (effect == DROPEFFECT_NONE) {
        drop->DragLeave();
        return DROPEFFECT_NONE;
    }
    drop->Drop(object, 0, pt, &effect);
    return effect;
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
                    // On Windows 11 the menu is StarFish's own (T-092), a Qt popup.
                    if (QWidget* popup = QApplication::activePopupWidget())
                        popup->close();
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

            // ---- Lot 3 (T-091), driven in this process: nothing is clicked or
            // typed on the user's desktop. Keys go to our own window or straight
            // to our objects; drops are played on our own drop target.
            {
                const QString lot3Name = QStringLiteral("StarFish probe (lot 3)");
                const QString lot3File = QDir(desktop).filePath(lot3Name + QStringLiteral(".txt"));

                // (a) F2 through the window's own keyboard path.
                backdrop->setSelection({testNative});
                backdrop->renderNow();
                ::PostMessageW(reinterpret_cast<HWND>(backdrop->surface()->hwnd()), WM_KEYDOWN,
                               VK_F2, 0);
                const bool editorOpen = waitUntil(2000, [&] {
                    return product.renameEditor() && product.renameEditor()->isVisible();
                });
                const QString shown = editorOpen ? product.renameEditor()->text() : QString();
                check(QStringLiteral("F2 opens an edit over the caption (T-091)"),
                      editorOpen && shown == platform::win::editingNameOf(testNative),
                      QStringLiteral("\"%1\"").arg(shown));
                if (editorOpen) {
                    waitMs(300);
                    const QRect label = backdrop->labelRectOf(testNative);
                    grabScreen(label.adjusted(-150, -110, 150, 40).intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/lot3_rename_edit.png"));
                    sendKey(product.renameEditor(), Qt::Key_Escape);
                }
                check(QStringLiteral("...Escape cancels it, nothing renamed"),
                      !product.renameEditor() && QFileInfo::exists(testFile), testNative);

                // (b) Return commits, through IFileOperation; Explorer's undo
                //     takes it back -- only if its undo entry is ours.
                product.startRename(testNative);
                if (QLineEdit* edit = product.renameEditor()) {
                    edit->setText(lot3Name);
                    sendKey(edit, Qt::Key_Return);
                }
                const bool renamedByShell =
                    waitUntil(3000, [&] { return QFileInfo::exists(lot3File) && !QFileInfo::exists(testFile); });
                check(QStringLiteral("...Return renames it (IFileOperation, extension kept)"),
                      renamedByShell, lot3File);
                QString undoLabel;
                for (const QString& label : product.shellView()->backgroundMenuLabels()) {
                    if ((label.startsWith(QLatin1String("Annuler")) && label.contains(QLatin1String("Renommer")))
                        || label.startsWith(QLatin1String("Undo Rename")))
                        undoLabel = label;
                }
                bool undone = false;
                if (renamedByShell && !undoLabel.isEmpty()) {
                    waitUntil(3000, [&] { return cellIndex(backdrop, QDir::toNativeSeparators(lot3File)) >= 0; });
                    product.onDesktopKey(Qt::Key_Z, Qt::ControlModifier);
                    undone = waitUntil(4000, [&] { return QFileInfo::exists(testFile); });
                }
                check(QStringLiteral("...and Ctrl+Z (Explorer's undo) takes it back"), undone,
                      undoLabel.isEmpty() ? QStringLiteral("no undo entry of ours in the menu")
                                          : undoLabel);
                if (!QFileInfo::exists(testFile) && QFileInfo::exists(lot3File))
                    QFile::rename(lot3File, testFile);
                waitUntil(4000, [&] { return cellIndex(backdrop, testNative) >= 0; });

                // (c) Ctrl+A, Escape.
                sendKey(backdrop, Qt::Key_A, Qt::ControlModifier);
                const int all = backdrop->selection().size();
                sendKey(backdrop, Qt::Key_Escape);
                check(QStringLiteral("Ctrl+A selects every icon, Escape none"),
                      all == backdrop->cells().size() && backdrop->selection().isEmpty(),
                      QStringLiteral("%1 of %2").arg(all).arg(backdrop->cells().size()));

                // (d) The selection rectangle, from a bare spot to our icon.
                const QRect icon = backdrop->iconRectOf(testNative);
                // The nearest bare point around the icon (its neighbours' captions
                // are wide): a ring search, closest first.
                QPoint from;
                for (int r = 8; r <= 120 && from.isNull(); r += 4) {
                    for (int dx = -r; dx <= r && from.isNull(); dx += 4) {
                        for (int dy : {-r, r}) {
                            const QPoint candidate = icon.center() + QPoint(dx, dy);
                            if (backdrop->area().contains(candidate)
                                && ui::BackdropGesture::iconAt(backdrop->cells(), candidate) < 0) {
                                from = candidate;
                                break;
                            }
                        }
                    }
                }
                bool marqueeSeen = false;
                if (!from.isNull()) {
                    sendMouse(backdrop, QEvent::MouseButtonPress, from, Qt::LeftButton);
                    sendMouse(backdrop, QEvent::MouseMove, (from + icon.center()) / 2, Qt::LeftButton);
                    sendMouse(backdrop, QEvent::MouseMove, icon.center(), Qt::LeftButton);
                    marqueeSeen = !backdrop->marquee().isNull();
                    backdrop->renderNow();
                    waitMs(150);
                    platform::win::DesktopShellView::captureDesktop(
                        icon.adjusted(-140, -100, 140, 100).intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/lot3_marquee.png"));
                    sendMouse(backdrop, QEvent::MouseButtonRelease, icon.center(), Qt::NoButton);
                }
                check(QStringLiteral("a selection rectangle selects the icons it touches"),
                      marqueeSeen
                          && std::any_of(backdrop->selection().begin(), backdrop->selection().end(),
                                         [&](const QString& s) {
                                             return ui::BackdropLayout::pathKey(s)
                                                    == ui::BackdropLayout::pathKey(testNative);
                                         })
                          && backdrop->marquee().isNull(),
                      QStringLiteral("from %1,%2: %3 selected")
                          .arg(from.x())
                          .arg(from.y())
                          .arg(backdrop->selection().size()));
                backdrop->setSelection({});

                // (e) The data object a drag carries.
                QStringList carried{testNative};
                for (const platform::win::ShellViewItem& item : state.items) {
                    if (item.parsingName.startsWith(QLatin1String("::"))) {
                        carried << item.parsingName;
                        break;
                    }
                }
                // Files alone travel as CF_HDROP too; with a virtual item in the
                // lot only the shell's ID list can name everything.
                void* filesOnly = platform::win::createShellDataObject({testNative});
                bool hdrop = false;
                bool idList = false;
                platform::win::shellDataObjectFormats(filesOnly, &hdrop, nullptr);
                platform::win::releaseComObject(filesOnly);
                void* data = platform::win::createShellDataObject(carried);
                platform::win::shellDataObjectFormats(data, nullptr, &idList);
                check(QStringLiteral("a drag carries the shell's own data object"),
                      data && hdrop && idList,
                      QStringLiteral("files: CF_HDROP %2; %1 item(s) with a virtual one: ID list %3")
                          .arg(carried.size())
                          .arg(hdrop ? QLatin1String("yes") : QLatin1String("no"))
                          .arg(idList ? QLatin1String("yes") : QLatin1String("no")));

                // Session 62: the drag shows the dragged icons (the shell's drag
                // image helper), not the default "document" cursor. No DoDragDrop.
                {
                    const int at = cellIndex(backdrop, testNative);
                    const QString path = at >= 0 ? backdrop->cells().at(at).item.path : testNative;
                    QPoint hotspot;
                    const QImage picture = backdrop->dragImage({path}, icon.center(), &hotspot);
                    void* withImage = platform::win::createShellDataObject({testNative});
                    const bool attached = platform::win::attachDragImage(withImage, picture, hotspot);
                    const bool carried = platform::win::hasDragImage(withImage);
                    platform::win::releaseComObject(withImage);
                    bool translucent = false;
                    for (int y = 0; y < picture.height() && !translucent; ++y)
                        for (int x = 0; x < picture.width() && !translucent; ++x)
                            translucent = qAlpha(picture.pixel(x, y)) > 0 && qAlpha(picture.pixel(x, y)) < 255;
                    picture.save(captures + QStringLiteral("/lot62_drag_image.png"));
                    check(QStringLiteral("a drag shows the dragged icon, a little translucent"),
                          !picture.isNull() && attached && carried && translucent
                              && QRect(QPoint(), picture.size()).contains(hotspot),
                          QStringLiteral("%1x%2, hotspot %3,%4, attached %5, carried %6")
                              .arg(picture.width()).arg(picture.height())
                              .arg(hotspot.x()).arg(hotspot.y())
                              .arg(attached ? QLatin1String("yes") : QLatin1String("no"))
                              .arg(carried ? QLatin1String("yes") : QLatin1String("no")));
                }

                // (f) Our icon dropped back on the desktop moves (the file does not).
                // Session 61: carried as a real drag carries it -- the *cell's*
                // path (C:/...), not the shell's name (C:\...). Given the shell's
                // name, this check passed while every real drag moved nothing.
                const int dragged = cellIndex(backdrop, testNative);
                const QString cellPath = dragged >= 0 ? backdrop->cells().at(dragged).item.path : testNative;
                const QPoint before = product.desktopItemPosition(testNative);
                void* desktopTarget = backdrop->surface() ? backdrop->surface()->customDropTarget() : nullptr;
                const QPoint origin = icon.center();
                const QPoint target = origin + QPoint(0, 2 * backdrop->physicalMetrics().cellWidth);
                platform::win::beginInternalDrag({cellPath}, origin);
                const DWORD moveEffect = playDrop(desktopTarget, data, target, false);
                platform::win::endInternalDrag();
                const bool moved = waitUntil(3000, [&] {
                    return product.desktopItemPosition(testNative) != before;
                });
                const QPoint after = product.desktopItemPosition(testNative);
                const QSize step = product.shellView()->read().spacing;
                // On the grid when Explorer aligns to it: a whole number of steps.
                const bool onGrid = !product.desktopSnapsToGrid()
                                    || (step.width() > 0 && step.height() > 0
                                        && (after.x() - before.x()) % step.width() == 0
                                        && (after.y() - before.y()) % step.height() == 0);
                check(QStringLiteral("our icon dropped on the desktop moves there (carried as C:/..., on the grid)"),
                      desktopTarget && moved && onGrid && QFileInfo::exists(testFile)
                          && moveEffect == DROPEFFECT_NONE && cellPath.contains(QLatin1Char('/')),
                      QStringLiteral("[%5] %1,%2 -> %3,%4, grid %6x%7 (%8)")
                          .arg(before.x())
                          .arg(before.y())
                          .arg(after.x())
                          .arg(after.y())
                          .arg(cellPath)
                          .arg(step.width())
                          .arg(step.height())
                          .arg(product.desktopAutoArranges() ? QLatin1String("auto-arrange")
                               : product.desktopSnapsToGrid() ? QLatin1String("snap to grid")
                                                              : QLatin1String("free")));
                // The shell keeps that place (what a restart reads back).
                product.syncShellViewForProbe();
                bool kept = false;
                for (const platform::win::ShellViewItem& it : product.shellView()->read().items) {
                    if (it.parsingName.compare(testNative, Qt::CaseInsensitive) == 0)
                        kept = it.position == after;
                }
                check(QStringLiteral("... and the shell keeps that place"), kept && moved, {});
                product.shellView()->positionItem(testNative, before);
                platform::win::releaseComObject(data);

                // (g) A file from elsewhere dropped on the desktop is copied
                //     there by the desktop folder's own drop target.
                const QString outside = QDir::temp().filePath(QStringLiteral("starfish-probe-drop"));
                QDir().mkpath(outside);
                const QString source = QDir(outside).filePath(QStringLiteral("StarFish probe drop.txt"));
                const QString landed = QDir(desktop).filePath(QStringLiteral("StarFish probe drop.txt"));
                bool copiedIn = false;
                bool underCursor = false;
                QRect where;
                // A bare spot away from the screen's centre, where the shell
                // would not put a new file by itself.
                const QPoint dropPoint = backdrop->area().center() + QPoint(-420, 260);
                if (!QFileInfo::exists(landed)) {
                    {
                        QFile file(source);
                        file.open(QIODevice::WriteOnly);
                        file.write("dropped by the StarFish probe\n");
                    }
                    void* foreign = platform::win::createShellDataObject({QDir::toNativeSeparators(source)});
                    playDrop(desktopTarget, foreign, dropPoint, true);
                    copiedIn = waitUntil(8000, [&] {
                        return QFileInfo::exists(landed)
                               && cellIndex(backdrop, QDir::toNativeSeparators(landed)) >= 0;
                    });
                    platform::win::releaseComObject(foreign);
                    // ...and is placed under the cursor (the grid may snap it).
                    underCursor = waitUntil(6000, [&] {
                        const int i = cellIndex(backdrop, QDir::toNativeSeparators(landed));
                        if (i < 0)
                            return false;
                        const QRect box = backdrop->cells().at(i).icon.united(backdrop->cells().at(i).label);
                        return box.adjusted(-100, -100, 100, 100).contains(dropPoint);
                    });
                    const int i = cellIndex(backdrop, QDir::toNativeSeparators(landed));
                    if (i >= 0)
                        where = backdrop->cells().at(i).icon;
                }
                check(QStringLiteral("a file dropped from outside lands on the Desktop"), copiedIn,
                      landed);
                check(QStringLiteral("...under the cursor"), underCursor,
                      QStringLiteral("dropped at %1,%2, icon at %3,%4")
                          .arg(dropPoint.x()).arg(dropPoint.y()).arg(where.x()).arg(where.y()));
                QFile::remove(landed);
                QDir(outside).removeRecursively();

                // (h) New folder: it appears, its name open for editing.
                QSet<QString> existing;
                for (const platform::win::ShellViewItem& item : product.shellView()->read().items)
                    existing.insert(item.parsingName);
                product.newDesktopFolder();
                const bool newEdit = waitUntil(6000, [&] {
                    return product.renameEditor() && product.renameEditor()->isVisible();
                });
                const QString created = product.renamePath();
                check(QStringLiteral("New folder opens its name for editing"),
                      newEdit && !created.isEmpty() && !existing.contains(created)
                          && QFileInfo(created).isDir(),
                      created);
                if (newEdit) {
                    waitMs(300);
                    const QRect where = backdrop->labelRectOf(created);
                    grabScreen(where.adjusted(-150, -110, 150, 40).intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/lot3_new_folder.png"));
                    sendKey(product.renameEditor(), Qt::Key_Escape);
                }
                // Ours, new and empty: the only folder the probe may delete.
                if (!created.isEmpty() && !existing.contains(created) && QFileInfo(created).isDir()
                    && QDir(created).isEmpty())
                    product.deleteDesktopItems({created}, true, true);
                check(QStringLiteral("...and deleting it (Shift+Delete path) removes it"),
                      created.isEmpty() || waitUntil(4000, [&] { return !QFileInfo::exists(created); }),
                      created);
            }

            // Lot 4 (T-092): the right-click menu drawn by StarFish. The user's
            // configuration file is put aside and given back byte for byte.
            if (index >= 0) {
                const QString configPath = core::ContextMenuConfig::defaultPath();
                const bool hadConfig = QFileInfo::exists(configPath);
                QByteArray savedConfig;
                if (hadConfig) {
                    QFile f(configPath);
                    if (f.open(QIODevice::ReadOnly))
                        savedConfig = f.readAll();
                    QFile::remove(configPath);
                }
                const QStringList known{
                    QStringLiteral("view"), QStringLiteral("sort"), QStringLiteral("refresh"),
                    QStringLiteral("undo"), QStringLiteral("redo"), QStringLiteral("new"),
                    QStringLiteral("display-settings"), QStringLiteral("personalize"),
                    QStringLiteral("terminal"), QStringLiteral("paste"), QStringLiteral("more-options"),
                    QStringLiteral("starfish:create-fence")};
                const auto keysOf = [](const std::vector<core::ContextMenuItem>& items) {
                    QStringList keys;
                    for (const core::ContextMenuItem& item : items) {
                        if (!item.separator)
                            keys << (item.topBar ? QStringLiteral("^") : QString()) + item.key;
                    }
                    return keys;
                };
                core::ContextMenuItem create;
                create.key = QStringLiteral("starfish:create-fence");
                // The product's own label, translated as the product shows it.
                create.label = QCoreApplication::translate("starfish::app::ProductApp", "Create a fence here");
                create.ownId = 1;
                const std::vector<core::ContextMenuItem> ours{create};

                std::vector<core::ContextMenuItem> bg =
                    product.composeContextMenu(core::ContextMenuKind::Background, {}, ours);
                const QStringList bgKeys = keysOf(bg);
                bool onlyWindows11 = !bgKeys.isEmpty();
                for (const QString& key : bgKeys)
                    onlyWindows11 = onlyWindows11 && known.contains(QString(key).remove(QLatin1Char('^')));
                check(QStringLiteral("the drawn background menu has Windows 11's entries (T-092)"),
                      bgKeys.contains(QStringLiteral("view")) && bgKeys.contains(QStringLiteral("sort"))
                          && bgKeys.contains(QStringLiteral("refresh")) && bgKeys.contains(QStringLiteral("new"))
                          && bgKeys.contains(QStringLiteral("starfish:create-fence"))
                          && bgKeys.last() == QLatin1String("more-options"),
                      bgKeys.join(QLatin1Char(' ')));
                const bool terminalDeclared =
                    !platform::win::packagedCommandDeclarations()
                         .filter(QStringLiteral("Directory\\Background"))
                         .isEmpty();
                check(QStringLiteral("...with the packaged apps' entries (IExplorerCommand)"),
                      !terminalDeclared || bgKeys.contains(QStringLiteral("terminal"))
                          || !bgKeys.filter(QStringLiteral("app:")).isEmpty(),
                      terminalDeclared ? QStringLiteral("a packaged app declares one") : QStringLiteral("none declared"));
                check(QStringLiteral("...and only them (extensions under Show more options)"),
                      onlyWindows11, bgKeys.join(QLatin1Char(' ')));

                // Rendered, captured, an entry run -- all without a click.
                const QPoint bare(backdrop->area().center().x(), backdrop->area().center().y() - 300);
                core::ContextMenuItem picked;
                std::unique_ptr<QMenu> bgMenu(ui::buildModernMenu(
                    bg, ui::ModernMenuStyle::current(),
                    [&](const core::ContextMenuItem& item) { picked = item; }));
                bgMenu->popup(bare);
                waitMs(500);
                grabScreen(QRect(bare - QPoint(30, 30), bgMenu->size() + QSize(60, 60)))
                    .save(captures + QStringLiteral("/lot4_menu_background.png"));
                QAction* refresh = bgMenu->findChild<QAction*>(QStringLiteral("refresh"));
                if (refresh)
                    refresh->trigger();
                bgMenu->close();
                const int ran = picked.key == QLatin1String("refresh")
                                    ? product.runContextMenuEntry(picked, core::ContextMenuKind::Background,
                                                                  {}, bare, ours, nullptr)
                                    : -2;
                check(QStringLiteral("...an entry picked runs the shell's own command"),
                      ran == -1 && !product.shellView()->lastChosenVerb().isEmpty(),
                      QStringLiteral("picked \"%1\", verb \"%2\"")
                          .arg(picked.key, product.shellView()->lastChosenVerb()));

                bg = product.composeContextMenu(core::ContextMenuKind::Background, {}, ours);
                picked = {};
                bgMenu.reset(ui::buildModernMenu(bg, ui::ModernMenuStyle::current(),
                                                 [&](const core::ContextMenuItem& item) { picked = item; }));
                if (QAction* a = bgMenu->findChild<QAction*>(QStringLiteral("starfish:create-fence")))
                    a->trigger();
                const int own = product.runContextMenuEntry(picked, core::ContextMenuKind::Background, {},
                                                            bare, ours, nullptr);
                check(QStringLiteral("...and StarFish's own entry comes back to StarFish"), own == 1,
                      QString::number(own));
                bgMenu.reset();

                // The item's menu: the top bar and the list.
                const std::vector<core::ContextMenuItem> sel =
                    product.composeContextMenu(core::ContextMenuKind::Selection, {testNative}, {});
                const QStringList selKeys = keysOf(sel);
                check(QStringLiteral("the drawn item menu: top bar Cut, Copy, Rename, Delete"),
                      selKeys.contains(QStringLiteral("^cut")) && selKeys.contains(QStringLiteral("^copy"))
                          && selKeys.contains(QStringLiteral("^rename"))
                          && selKeys.contains(QStringLiteral("^delete"))
                          && selKeys.contains(QStringLiteral("open"))
                          && selKeys.contains(QStringLiteral("properties"))
                          && selKeys.last() == QLatin1String("more-options"),
                      selKeys.join(QLatin1Char(' ')));
                picked = {};
                std::unique_ptr<QMenu> selMenu(ui::buildModernMenu(
                    sel, ui::ModernMenuStyle::current(),
                    [&](const core::ContextMenuItem& item) { picked = item; }));
                const int selIndex = cellIndex(backdrop, testNative);
                const QPoint at = selIndex >= 0 ? backdrop->cells().at(selIndex).icon.center() : bare;
                selMenu->popup(at);
                waitMs(500);
                grabScreen(QRect(at - QPoint(30, 30), selMenu->size() + QSize(60, 60)))
                    .save(captures + QStringLiteral("/lot4_menu_item.png"));
                if (auto* button = selMenu->findChild<QToolButton*>(QStringLiteral("topbar:rename")))
                    button->click();
                selMenu->close();
                bool renameAsked = false;
                product.runContextMenuEntry(picked, core::ContextMenuKind::Selection, {testNative}, at, {},
                                            &renameAsked);
                check(QStringLiteral("...its Rename button asks for our edit"),
                      picked.key == QLatin1String("rename") && renameAsked && QFileInfo::exists(testFile),
                      picked.key);
                selMenu.reset();

                // The configuration: hide, reorder, bring back, add.
                core::ContextMenuConfig config;
                config.background.hidden << QStringLiteral("refresh");
                config.background.order << QStringLiteral("personalize");
                config.background.custom << core::ContextMenuCustomEntry{
                    QStringLiteral("StarFish probe entry"), QStringLiteral("notepad.exe"), {QStringLiteral("%desktop%")}, {}};
                config.save(configPath);
                const QStringList cfgKeys =
                    keysOf(product.composeContextMenu(core::ContextMenuKind::Background, {}, ours));
                product.shellView()->closeMenuModel();
                check(QStringLiteral("the configuration hides, reorders and adds entries"),
                      !cfgKeys.contains(QStringLiteral("refresh")) && !cfgKeys.isEmpty()
                          && cfgKeys.first() == QLatin1String("personalize")
                          && cfgKeys.contains(QStringLiteral("custom:StarFish probe entry"))
                          && cfgKeys.last() == QLatin1String("more-options"),
                      cfgKeys.join(QLatin1Char(' ')));

                // "Show more options": the classic menu, closed by the probe.
                QFile::remove(configPath);
                bg = product.composeContextMenu(core::ContextMenuKind::Background, {}, ours);
                core::ContextMenuItem more;
                for (const core::ContextMenuItem& item : bg) {
                    if (item.ownId == core::kMoreOptionsId)
                        more = item;
                }
                bool classicShown = false;
                QTimer::singleShot(1000, [&] {
                    grabScreen(QRect(bare - QPoint(30, 30), QSize(640, 760)).intersected(backdrop->area()))
                        .save(captures + QStringLiteral("/lot4_more_options.png"));
                    classicShown = true;
                    ::EndMenu();
                });
                product.runContextMenuEntry(more, core::ContextMenuKind::Background, {}, bare, ours, nullptr);
                check(QStringLiteral("Show more options opens the classic menu"),
                      more.ownId == core::kMoreOptionsId && classicShown, more.label);

                QFile::remove(configPath);
                if (hadConfig) {
                    QFile f(configPath);
                    if (f.open(QIODevice::WriteOnly))
                        f.write(savedConfig);
                }
                check(QStringLiteral("...the user's menu configuration is given back"),
                      hadConfig ? QFileInfo::exists(configPath) : !QFileInfo::exists(configPath),
                      configPath);
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

            // ---- Session 63: drawing on the desktop must follow the mouse. Moves
            // are sent to the backdrop in this process (no real input), one every
            // 16 ms like a 60 Hz mouse; the backdrop reports how many frames
            // reached its window and how long after the move each one came.
            {
                ui::FenceContainer* fences = product.container();
                const QRect box = backdrop->area();
                const QSize travel(360, 240);
                QPoint start;
                for (int y = box.top() + box.height() / 4; y < box.bottom() - travel.height() && start.isNull(); y += 40) {
                    for (int x = box.left() + box.width() / 3; x < box.right() - travel.width(); x += 40) {
                        const QPoint p(x, y);
                        if (ui::BackdropGesture::iconAt(backdrop->cells(), p) < 0 && fences->fenceAt(p).isEmpty()) {
                            start = p;
                            break;
                        }
                    }
                }
                const auto send = [&](QEvent::Type type, const QPoint& at, Qt::MouseButton button,
                                      Qt::MouseButtons buttons) {
                    QMouseEvent e(type, QPointF(at), QPointF(at), button, buttons, Qt::NoModifier);
                    QCoreApplication::sendEvent(backdrop, &e);
                };
                const auto drag = [&](Qt::MouseButton button) {
                    backdrop->resetRenderStats();
                    QElapsedTimer wall;
                    wall.start();
                    send(QEvent::MouseButtonPress, start, button, button);
                    const int steps = 60;
                    for (int i = 1; i <= steps; ++i) {
                        const QPoint at = start + QPoint(travel.width() * i / steps, travel.height() * i / steps);
                        send(QEvent::MouseMove, at, Qt::NoButton, button);
                        waitMs(16);
                    }
                    waitMs(250); // whatever is still pending lands
                    if (button == Qt::LeftButton)
                        send(QEvent::MouseButtonRelease, start + QPoint(travel.width(), travel.height()),
                             button, Qt::NoButton);
                    else
                        backdrop->cancelGesture(); // no menu: the release would open one
                    const ui::DesktopBackdrop::RenderStats st = backdrop->renderStats();
                    const qint64 ms = wall.elapsed();
                    return std::make_pair(st, ms);
                };
                const auto describe = [](const ui::DesktopBackdrop::RenderStats& st, qint64 ms) {
                    const int n = qMax(1, st.updates);
                    return QStringLiteral("%1 frame(s) for 60 moves in %2 ms; build+push %3 ms avg (max %4); "
                                          "move-to-screen %5 ms avg (max %6)")
                        .arg(st.updates)
                        .arg(ms)
                        .arg(st.costUs / n / 1000.0, 0, 'f', 1)
                        .arg(st.maxCostUs / 1000.0, 0, 'f', 1)
                        .arg(st.latencyUs / n / 1000.0, 0, 'f', 1)
                        .arg(st.maxLatencyUs / 1000.0, 0, 'f', 1);
                };
                if (!start.isNull()) {
                    const auto [band, bandMs] = drag(Qt::RightButton);
                    const auto [marquee, marqueeMs] = drag(Qt::LeftButton);
                    backdrop->setSelection({});
                    waitMs(200);
                    const auto fluid = [](const ui::DesktopBackdrop::RenderStats& st) {
                        const int n = qMax(1, st.updates);
                        return st.updates >= 40 && st.latencyUs / n < 20000;
                    };
                    check(QStringLiteral("a fence drawn on the desktop follows the mouse"), fluid(band),
                          describe(band, bandMs));
                    check(QStringLiteral("...and so does the selection rectangle"), fluid(marquee),
                          describe(marquee, marqueeMs));
                } else {
                    check(QStringLiteral("a fence drawn on the desktop follows the mouse"), false,
                          QStringLiteral("no free spot to draw on"));
                }
            }

            // ---- Session 63 (T-098): a translucent fence is composed over the
            // drawn desktop -- one opaque window, the desktop's own pixels under
            // the fence colour, no second window for the icons.
            {
                ui::FenceContainer* fences = product.container();
                const QRect box = backdrop->area();
                const QRect lowerRect(box.center() - QPoint(260, 170), QSize(340, 260));
                const QRect upperRect(box.center() - QPoint(60, 60), QSize(340, 260));
                const QString lower = fences->createFence(QStringLiteral("Translucide"), lowerRect);
                const QString upper = fences->createFence(QStringLiteral("Dessus"), upperRect);
                fences->setFenceOpacity(lower, ui::PercentSlider::alphaFromPercent(50));
                fences->setFenceOpacity(upper, ui::PercentSlider::alphaFromPercent(40));
                // Icons to show (opaque, with the desktop's shadow): a portal on a
                // Windows folder -- read only, nothing on disk is touched.
                fences->addPortal(lower, QStringLiteral("C:/Windows/Web"));
                waitMs(400);
                platform::win::DesktopSurface* surface = fences->surface(lower);
                check(QStringLiteral("a translucent fence is one opaque window over the drawn desktop (T-098)"),
                      fences->composedFences() && surface && surface->uniformAlpha() == 255
                          && !fences->iconOverlay(lower) && fences->view(lower)->textShadow(),
                      QStringLiteral("composed %1, window alpha %2, icon window %3")
                          .arg(fences->composedFences() ? QLatin1String("yes") : QLatin1String("no"))
                          .arg(surface ? surface->uniformAlpha() : -1)
                          .arg(fences->iconOverlay(lower) ? QLatin1String("yes") : QLatin1String("no")));

                // The panel's pixel is the desktop's under it, blended with the
                // fence colour at 50 % -- read from StarFish's own images.
                const auto blendOk = [](const QColor& got, const QColor& under, const QColor& over, int alpha) {
                    const auto ch = [&](int u, int o) { return (u * (255 - alpha) + o * alpha) / 255; };
                    return qAbs(got.red() - ch(under.red(), over.red())) <= 8
                           && qAbs(got.green() - ch(under.green(), over.green())) <= 8
                           && qAbs(got.blue() - ch(under.blue(), over.blue())) <= 8;
                };
                const QRect lowerShown = fences->fenceRect(lower);
                const QImage lowerFrame = fences->frame(lower);
                const QImage desk = backdrop->frame();
                const QPoint inPanel(lowerFrame.width() / 4, lowerFrame.height() * 3 / 4);
                const QColor panelRgb(fences->fenceStyle(lower).background);
                const QColor got = lowerFrame.pixelColor(inPanel);
                const QColor under = desk.pixelColor(lowerShown.topLeft() + inPanel - box.topLeft());
                check(QStringLiteral("...its panel is the desktop under it at 50 %"),
                      !lowerFrame.isNull() && blendOk(got, under, panelRgb, 128),
                      QStringLiteral("pixel %1, desktop %2, panel %3")
                          .arg(got.name(), under.name(), panelRgb.name()));

                // Over another fence: the pixels under the upper fence are the
                // lower fence's, as composed.
                const QRect upperShown = fences->fenceRect(upper);
                const QImage upperFrame = fences->frame(upper);
                const QPoint overlapAt = (lowerShown & upperShown).center();
                const QPoint inUpper = overlapAt - upperShown.topLeft();
                const QColor upperGot = upperFrame.pixelColor(inUpper);
                const QColor lowerUnder = lowerFrame.pixelColor(overlapAt - lowerShown.topLeft());
                check(QStringLiteral("...and a fence over it shows that fence through it"),
                      !upperFrame.isNull() && blendOk(upperGot, lowerUnder, QColor(fences->fenceStyle(upper).background), 153),
                      QStringLiteral("pixel %1, fence under %2").arg(upperGot.name(), lowerUnder.name()));

                // Moving: no new render, one compose per fence touched.
                const int rendersBefore = fences->frameRenderCount();
                const int composesBefore = fences->composeCount();
                const qint64 usBefore = fences->composeMicros();
                QElapsedTimer wall;
                wall.start();
                const int moves = 40;
                for (int i = 1; i <= moves; ++i) {
                    fences->setFenceRect(lower, lowerRect.translated(i * 3, i * 2), false);
                    QCoreApplication::processEvents();
                }
                const qint64 moveMs = wall.elapsed();
                const int composes = fences->composeCount() - composesBefore;
                const double perCompose = (fences->composeMicros() - usBefore) / 1000.0 / qMax(1, composes);
                check(QStringLiteral("...a move re-composes, it does not re-render"),
                      fences->frameRenderCount() == rendersBefore && composes >= moves
                          && moveMs / double(moves) < 16.0,
                      QStringLiteral("%1 moves in %2 ms (%3 ms each); %4 compose(s), %5 ms avg; renders +%6")
                          .arg(moves)
                          .arg(moveMs)
                          .arg(moveMs / double(moves), 0, 'f', 1)
                          .arg(composes)
                          .arg(perCompose, 0, 'f', 2)
                          .arg(fences->frameRenderCount() - rendersBefore));
                fences->setFenceRect(lower, lowerRect, false);
                waitMs(200);

                // The picture: the drawn desktop around the fences with the
                // fences' own frames on it (StarFish's images, not the screen).
                {
                    const QRect around = (fences->fenceRect(lower) | fences->fenceRect(upper)).adjusted(-80, -60, 80, 60)
                                             .intersected(box);
                    QImage picture = backdrop->frame().copy(around.translated(-box.topLeft()));
                    QPainter painter(&picture);
                    painter.setRenderHint(QPainter::Antialiasing, true);
                    for (const QString& id : {lower, upper}) {
                        const QRect target = fences->fenceRect(id).translated(-around.topLeft());
                        const QImage f = fences->frame(id);
                        const qreal radius = fences->view(id)->cornerRadius() * f.devicePixelRatio();
                        QPainterPath shape;
                        shape.addRoundedRect(QRectF(target), radius, radius);
                        painter.save();
                        painter.setClipPath(shape);
                        painter.drawImage(target, f);
                        painter.restore();
                    }
                    painter.end();
                    picture.save(captures + QStringLiteral("/t098_translucent_fences.png"));
                }

                // The default: a fence that follows it changes with it.
                const int productDefault = fences->defaultFenceAlpha();
                fences->setFenceOpacity(lower, -1);
                fences->setDefaultFenceAlpha(ui::PercentSlider::alphaFromPercent(30));
                const int followed = fences->fenceEffectiveAlpha(lower);
                const int own = fences->fenceEffectiveAlpha(upper);
                fences->setDefaultFenceAlpha(productDefault);
                check(QStringLiteral("...the default moves the fences that follow it, not the others"),
                      fences->fenceFollowsDefaultOpacity(lower) && followed == 179 && own == 153,
                      QStringLiteral("following %1, own %2").arg(followed).arg(own));
                fences->removeFence(upper);
                fences->removeFence(lower);
                waitMs(300);
            }

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
    // Lot 3 names (ours only): the rename target and the dropped copy.
    QFile::remove(QDir(desktop).filePath(QStringLiteral("StarFish probe (lot 3).txt")));
    QFile::remove(QDir(desktop).filePath(QStringLiteral("StarFish probe drop.txt")));
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
