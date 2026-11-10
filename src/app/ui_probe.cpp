#include "app/ui_probe.hpp"

#include <algorithm>
#include <functional>
#include <cmath>
#include <windows.h>
#include <shlobj.h>
#include <psapi.h>
#include <QLinearGradient>
#include <QToolButton>
#include <QMenu>
#include <QPainterPath>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QScrollArea>
#include <QSlider>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QGuiApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include "core/context_menu.hpp"
#include "ui/context_menu_editor.hpp"
#include "ui/drag_image.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_transparency_dialog.hpp"
#include "ui/percent_slider.hpp"
#include "ui/fence_view.hpp"
#include "ui/icon_layout.hpp"
#include "ui/fence_geometry.hpp"
#include <QComboBox>
#include "ui/fence_appearance_dialog.hpp"
#include "core/desktop_item.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/backdrop_gesture.hpp"
#include "platform/win/windows_theme.hpp"
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPushButton>
#include "ui/modern_menu.hpp"
#include "ui/widget_resize.hpp"
#include "ui/zone_shape.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tray_icon.hpp"
#include "platform/win/shell_operations.hpp"
#include "core/i18n.hpp"
#include "platform/win/shell_drop_target.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "core/shortcut_vault.hpp"
#include <QKeyEvent>
#include <QPointer>
#include <QPaintEvent>
#include "ui/fence_menu.hpp"
#include "ui/splash_screen.hpp"
#include "ui/fence_widgets.hpp"
#include "ui/system_monitor.hpp"
#include "ui/widget_settings_dialog.hpp"
#include "core/system_metrics.hpp"
#include <QComboBox>
#include <QSpinBox>
#include <cmath>
#include "core/jsonstore.hpp"
#include "core/overlap_planner.hpp"
#include <windows.h>
#include <shobjidl.h>

namespace starfish::app {

namespace {

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QPixmap grabScreen(const QRect& rect)
{
    QScreen* screen = QGuiApplication::screenAt(rect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect g = screen->geometry();
    return screen->grabWindow(0, rect.x() - g.x(), rect.y() - g.y(), rect.width(), rect.height());
}

QMap<QString, QString> readSeenKeys(const QString& path)
{
    QMap<QString, QString> seen;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return seen;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const int eq = line.indexOf(QLatin1String("    = "));
        if (!line.startsWith(QLatin1Char('#')) && eq > 0)
            seen.insert(line.left(eq).trimmed(), line.mid(eq + 6));
    }
    return seen;
}

} // namespace

int runUiProbe(QApplication& app)
{
    Q_UNUSED(app);
    const QString captures = QStringLiteral("build/captures");
    QDir().mkpath(captures);
    const QString theme = ui::ModernMenuStyle::current().dark ? QStringLiteral("dark") : QStringLiteral("light");
    QStringList lines;
    int failures = 0;
    const auto check = [&](const QString& what, bool ok, const QString& detail) {
        lines << QStringLiteral("%1  %2%3").arg(ok ? QLatin1String("PASS") : QLatin1String("FAIL"), what,
                                                detail.isEmpty() ? QString() : QStringLiteral("  [%1]").arg(detail));
        if (!ok)
            ++failures;
    };
    lines << QStringLiteral("StarFish --ui-probe, theme %1").arg(theme);

    // --- the icon, large -------------------------------------------------------
    {
        const QIcon icon = ui::TrayIcon::buildIcon();
        QPixmap sheet(560, 300);
        sheet.fill(Qt::transparent);
        QPainter p(&sheet);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xF3, 0xF3, 0xF3));
        p.drawRoundedRect(QRectF(0, 0, 280, 300), 12, 12);
        p.setBrush(QColor(0x20, 0x20, 0x20));
        p.drawRoundedRect(QRectF(280, 0, 280, 300), 12, 12);
        for (int side = 0; side < 2; ++side) {
            const int x0 = side * 280;
            p.drawPixmap(x0 + 76, 20, icon.pixmap(128, 128));
            int x = x0 + 40;
            for (int size : {16, 20, 24, 32, 48}) {
                p.drawPixmap(x, 200 - size / 2, icon.pixmap(size, size));
                x += size + 18;
            }
        }
        p.end();
        sheet.save(captures + QStringLiteral("/ui_icon.png"));
        check(QStringLiteral("the icon has every size"), icon.availableSizes().size() >= 8,
              QString::number(icon.availableSizes().size()));
    }

    // Session 60: STARFISH_UI_PROBE_OFFSCREEN=1 -- nothing on screen, no
    // screen grab at all (the tray section grabs the taskbar), for runs while
    // the user is at the PC or StarFish already runs.
    const bool offscreen = qEnvironmentVariableIntValue("STARFISH_UI_PROBE_OFFSCREEN") != 0;
    if (offscreen)
        lines << QStringLiteral("off-screen: tray section skipped, windows not shown");

    // --- the tray icon and its menu --------------------------------------------
    ui::TrayIcon tray;
    const bool shown = !offscreen && tray.show();
    tray.setFenceCount(3);
    tray.setFenceMaterial(QStringLiteral("translucent"));
    if (!offscreen)
        check(QStringLiteral("the tray icon shows"), shown, {});
    if (shown) {
        QScreen* screen = QGuiApplication::primaryScreen();
        const QRect full = screen->geometry();
        const QRect avail = screen->availableGeometry();
        wait(1200);
        // The notification area: the bottom-right of the taskbar.
        const int barHeight = qMax(40, full.bottom() - avail.bottom());
        QPixmap area = grabScreen(QRect(full.right() - 520, full.bottom() - barHeight + 1, 520, barHeight));
        area.save(captures + QStringLiteral("/ui_tray_area_%1.png").arg(theme));
        // Windows 11 puts a new icon under the "^" of the notification area until
        // the user brings it out, and opening that takes a real click: what the
        // icon looks like in the bar is shown as a montage, and says so.
        {
            QPainter p(&area);
            const qreal dpr = area.devicePixelRatio();
            const int px = qRound(16 * dpr);
            const QPixmap icon = ui::TrayIcon::buildIcon().pixmap(px, px);
            const int y = (area.height() - px) / 2;
            p.drawPixmap(qRound(area.width() * 0.30), y, icon);
            p.setPen(QColor(0xFF, 0xA0, 0x6E));
            QFont f = p.font();
            f.setPointSize(7);
            p.setFont(f);
            p.drawText(QRect(4, 0, 120, area.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       QStringLiteral("montage"));
        }
        area.save(captures + QStringLiteral("/ui_tray_montage_%1.png").arg(theme));

        QMenu* menu = tray.menu();
        const QPoint at(avail.right() - 40, avail.bottom() - 10);
        menu->popup(at); // positioned like the tray's: above the taskbar
        wait(600);
        const QRect r = menu->frameGeometry().adjusted(-1, -1, 1, 1);
        grabScreen(r).save(captures + QStringLiteral("/ui_tray_menu_%1.png").arg(theme));
        int sections = 0;
        for (QLabel* label : menu->findChildren<QLabel*>()) {
            if (!label->text().isEmpty() && label->text() == label->text().toUpper() && label->pixmap().isNull())
                ++sections;
        }
        QStringList texts;
        for (QAction* a : menu->actions()) {
            if (!a->text().isEmpty() && a->isVisible())
                texts << a->text();
        }
        check(QStringLiteral("the tray menu has its header, sections and entries"),
              menu->isVisible() && sections == 2 && texts.size() >= 8, texts.join(QStringLiteral(" | ")));
        menu->hide();
    }

    // --- the settings window -------------------------------------------------
    QTemporaryDir scratch;
    const QString realConfig = core::ContextMenuConfig::defaultPath();
    const QString config = scratch.filePath(QStringLiteral("context-menu.json"));
    if (QFile::exists(realConfig))
        QFile::copy(realConfig, config);
    ui::SettingsDialog settings;
    settings.menuEditor()->setConfigPath(config);
    settings.menuEditor()->setSeenEntries(
        readSeenKeys(QFileInfo(realConfig).dir().filePath(QStringLiteral("context-menu-keys.txt"))));
    settings.setStatusText(QStringLiteral("3 zone(s) -- 2 règle(s) -- matériau : translucide"));
    settings.setFencesVisible(true);
    settings.setFenceShortcutArrows(true);
    settings.setFenceMaterial(QStringLiteral("translucent"));
    // Session 60's defaults: sort off, desktop arrows on, overlap off.
    settings.setSortOnAdd(false);
    settings.setDesktopShortcutArrows(true);
    settings.setDesktopOverlapAllowed(false);
    settings.setStartWithWindows(false, false);
    if (offscreen)
        settings.setAttribute(Qt::WA_DontShowOnScreen);
    settings.show();
    settings.raise();
    wait(500);
    const char* names[] = {"general", "fences", "menu", "about"};
    for (int page = 0; page < 4; ++page) {
        settings.showPage(static_cast<ui::SettingsDialog::Page>(page));
        wait(250);
        settings.grab().save(captures + QStringLiteral("/ui_settings_%1_%2.png").arg(QLatin1String(names[page]), theme));
    }
    // The window itself only (grab()): a screen grab would photograph whatever
    // is in front of it -- the user's own windows.
    check(QStringLiteral("the settings window has four pages"),
          settings.findChild<QListWidget*>(QStringLiteral("nav"))->count() == 4, {});
    // Session 68: About shows the folders, each with a button to open it.
    {
        auto* data = settings.findChild<QPushButton*>(QStringLiteral("openDataFolder"));
        auto* local = settings.findChild<QPushButton*>(QStringLiteral("openLocalFolder"));
        auto* language = settings.findChild<QComboBox*>(QStringLiteral("languageCombo"));
        check(QStringLiteral("About: the data and crash-log folders, each with Open (session 68)"),
              data && local && language && language->findData(QStringLiteral("fr")) >= 0,
              language ? QStringLiteral("%1 language entries").arg(language->count()) : QString());
    }
    // Session 62: "start with Windows" on General; a Task Manager disable shows
    // as off, with a hint (the registry is the truth -- nothing written here).
    {
        settings.showPage(ui::SettingsDialog::General);
        wait(150);
        auto* box = settings.findChild<QCheckBox*>(QStringLiteral("startWithWindowsSwitch"));
        QLabel* hint = nullptr;
        for (QLabel* label : settings.findChildren<QLabel*>())
            if (label->property("startWithWindowsHint").toBool())
                hint = label;
        const bool shown = box && hint && box->isVisibleTo(&settings);
        const QString plain = hint ? hint->text() : QString();
        settings.setStartWithWindows(true, false);
        const bool on = box && box->isChecked() && hint->text() == plain;
        settings.setStartWithWindows(true, true);
        const bool disabled = box && !box->isChecked() && hint->text() != plain;
        settings.grab().save(captures + QStringLiteral("/ui_settings_start_disabled_%1.png").arg(theme));
        const QString disabledText = hint ? hint->text() : QString();
        settings.setStartWithWindows(false, false);
        check(QStringLiteral("\"start with Windows\" is on General, a Task Manager disable shown"),
              shown && on && disabled, disabledText);
    }
    // Session 62: the picture a drag carries -- the icons as they sit, on a
    // light plate, a little translucent; transparent around them.
    {
        auto tilePixmap = [](QColor color) {
            QPixmap pm(48, 48);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(color);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(4, 4, 40, 40), 6, 6);
            return pm;
        };
        QList<ui::DragTile> tiles;
        const QStringList names = {QStringLiteral("Rapport annuel.docx"), QStringLiteral("Photos"),
                                   QStringLiteral("Un nom de fichier assez long pour deux lignes.txt")};
        const QColor colors[] = {QColor(0x2B, 0x7C, 0xD3), QColor(0xF2, 0xC2, 0x3C), QColor(0x6E, 0xB5, 0x4A)};
        for (int i = 0; i < 3; ++i) {
            ui::DragTile t;
            t.icon = QRect(20 + i * 86, 10 + (i == 2 ? 100 : 0), 48, 48);
            t.label = QRect(t.icon.x() - 18, t.icon.bottom() + 5, 84, 34);
            t.pixmap = tilePixmap(colors[i]);
            t.text = names.at(i);
            tiles << t;
        }
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(12);
        QPoint topLeft;
        const QImage image = ui::DragImage::compose(tiles, font, ui::DragImage::kOpacity, 1.0, &topLeft);
        int maxAlpha = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                maxAlpha = qMax(maxAlpha, qAlpha(image.pixel(x, y)));
        const bool cornerClear = !image.isNull() && qAlpha(image.pixel(image.width() - 1, 0)) == 0;
        // On a wallpaper-like sheet, the way it looks while dragged.
        QImage sheet(image.size() + QSize(80, 80), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&sheet);
            QLinearGradient g(0, 0, sheet.width(), sheet.height());
            g.setColorAt(0, QColor(0x1E, 0x3A, 0x5F));
            g.setColorAt(1, QColor(0x5F, 0x8F, 0xB0));
            p.fillRect(sheet.rect(), g);
            p.drawImage(40, 40, image);
        }
        sheet.save(captures + QStringLiteral("/ui_drag_image.png"));
        check(QStringLiteral("a drag picture: the icons as they sit, translucent, clear around"),
              !image.isNull() && maxAlpha > 0 && maxAlpha < 255 && cornerClear
                  && image.format() == QImage::Format_ARGB32_Premultiplied,
              QStringLiteral("%1x%2, max alpha %3").arg(image.width()).arg(image.height()).arg(maxAlpha));
    }
    {
        settings.showPage(ui::SettingsDialog::Fences);
        wait(150);
        QStringList states;
        bool all = true;
        for (const char* name : {"sortOnAddSwitch", "desktopArrowsSwitch", "overlapSwitch"}) {
            auto* box = settings.findChild<QCheckBox*>(QLatin1String(name));
            all = all && box && box->isVisibleTo(&settings);
            states << QStringLiteral("%1=%2").arg(QLatin1String(name),
                                                  box ? (box->isChecked() ? QLatin1String("on") : QLatin1String("off"))
                                                      : QLatin1String("absent"));
        }
        auto* sortBox = settings.findChild<QCheckBox*>(QStringLiteral("sortOnAddSwitch"));
        auto* arrowsBox = settings.findChild<QCheckBox*>(QStringLiteral("desktopArrowsSwitch"));
        auto* overlapBox = settings.findChild<QCheckBox*>(QStringLiteral("overlapSwitch"));
        check(QStringLiteral("the Fences page has the session-60 switches, at their defaults"),
              all && sortBox && !sortBox->isChecked() && arrowsBox && arrowsBox->isChecked() && overlapBox
                  && !overlapBox->isChecked(),
              states.join(QLatin1Char(' ')));
        // A switch flipped by the product (setX) must not echo back as a request.
        int echoes = 0;
        QObject::connect(&settings, &ui::SettingsDialog::sortOnAddToggled, &settings, [&] { ++echoes; });
        settings.setSortOnAdd(true);
        settings.setSortOnAdd(false);
        check(QStringLiteral("setting a switch from the product sends nothing back"), echoes == 0,
              QString::number(echoes));

        // Session 64: fences may overlap -- on by default, as before.
        auto* fenceOverlap = settings.findChild<QCheckBox*>(QStringLiteral("fenceOverlapSwitch"));
        check(QStringLiteral("Fences: \"allow fences to overlap\", on by default (session 64)"),
              fenceOverlap && fenceOverlap->isVisibleTo(&settings) && fenceOverlap->isChecked(),
              fenceOverlap ? QStringLiteral("present") : QStringLiteral("absent"));

        // T-098: the default transparency, a slider with its number.
        auto* slider = settings.findChild<QSlider*>(QStringLiteral("transparencySlider"));
        auto* value = settings.findChild<QLabel*>(QStringLiteral("transparencyValue"));
        QList<int> asked;
        QObject::connect(&settings, &ui::SettingsDialog::defaultTransparencyChanged, &settings,
                         [&](int percent) { asked << percent; });
        settings.setDefaultTransparency(20);
        const QString fromProduct = value ? value->text() : QString();
        if (slider)
            slider->setValue(35); // what a drag does, step by step
        check(QStringLiteral("Fences: the default transparency is a slider with its value (T-098)"),
              slider && value && slider->isVisibleTo(&settings) && fromProduct == QStringLiteral("20 %")
                  && value->text() == QStringLiteral("35 %") && asked == QList<int>{35},
              QStringLiteral("shown %1 then %2, asked %3")
                  .arg(fromProduct, value ? value->text() : QStringLiteral("-"))
                  .arg(asked.size()));
        if (auto* scroll = settings.findChild<QScrollArea*>())
            scroll->ensureWidgetVisible(slider);
        wait(100);
        settings.grab().save(captures + QStringLiteral("/ui_settings_transparency_%1.png").arg(theme));
        settings.setDefaultTransparency(0);
    }

    // --- session 64: the grid's margins, left and right -----------------------
    {
        ui::FenceView view;
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.setTitle(QStringLiteral("Harnesses"));
        QList<core::DesktopItem> items;
        for (const char* name : {"App A", "App B", "App C", "App D", "App E", "App F",
                                 "App G", "App H"}) {
            core::DesktopItem item;
            item.name = QString::fromUtf8(name);
            item.fileName = item.name;
            item.path = QStringLiteral("C:/probe/") + item.name;
            items.append(item);
        }
        view.setItems(items);
        view.resize(735, 140);
        view.show();
        wait(50);
        view.grab().save(captures + QStringLiteral("/s64_grid_margins_%1.png").arg(theme));
        const QVector<QRect> tiles = view.tileRects();
        int left = -1;
        int right = -1;
        if (!tiles.isEmpty()) {
            int minX = INT_MAX;
            int maxX = INT_MIN;
            for (const QRect& t : tiles) {
                minX = qMin(minX, t.left());
                maxX = qMax(maxX, t.right());
            }
            left = minX;
            right = view.width() - 1 - maxX;
        }
        // Session 65: top-left; the right margin is the left one plus what no
        // whole column can use (< one column step).
        check(QStringLiteral("a fence's grid starts top-left, the rest on the right (session 65)"),
              left >= 0 && left <= 26 && right >= left && right - left < ui::IconLayout::maxCellWidthFor(48) + 8,
              QStringLiteral("left %1 px, right %2 px (735 px wide, 8 icons)").arg(left).arg(right));
    }

    // --- session 65: one selection, a tile menu, tiles from fence to fence ----
    {
        ui::FenceContainer container;
        const QString a = container.createFence(QStringLiteral("A"), QRect(0, 0, 320, 240));
        const QString b = container.createFence(QStringLiteral("B"), QRect(400, 0, 320, 240));
        auto item = [](const QString& name) { return core::itemFromPathOnly(QStringLiteral("C:/s65/") + name); };
        container.fileInto(a, {item(QStringLiteral("a.txt")), item(QStringLiteral("b.txt")), item(QStringLiteral("c.txt"))});
        container.fileInto(b, {item(QStringLiteral("d.txt"))});
        ui::FenceView* va = container.view(a);
        ui::FenceView* vb = container.view(b);
        auto tileAt = [](ui::FenceView* v, int index) {
            for (int y = 0; y < 240; y += 4)
                for (int x = 0; x < 320; x += 4)
                    if (v->itemAt(QPoint(x, y)) == index)
                        return QPoint(x + 2, y + 2);
            return QPoint(-1, -1);
        };
        auto click = [](ui::FenceView* v, const QPoint& at, Qt::MouseButton button) {
            QMouseEvent press(QEvent::MouseButtonPress, at, v->mapToGlobal(at), button, button, Qt::NoModifier);
            QCoreApplication::sendEvent(v, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, v->mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(v, &release);
        };
        click(va, tileAt(va, 0), Qt::LeftButton);
        const bool aSelected = va->selectedPaths().size() == 1;
        click(vb, tileAt(vb, 0), Qt::LeftButton);
        const bool global = aSelected && va->selectedPaths().isEmpty() && vb->selectedPaths().size() == 1;
        emit container.backdrop()->pressed();
        const bool cleared = vb->selectedPaths().isEmpty();
        check(QStringLiteral("one selection: a click in a fence clears the others, the desktop clears all (session 65)"),
              global && cleared,
              QStringLiteral("A then B %1, desktop %2").arg(global ? QLatin1String("ok") : QLatin1String("KO"),
                                                           cleared ? QLatin1String("ok") : QLatin1String("KO")));
        QStringList menuPaths;
        QPoint menuPoint;
        QObject::connect(&container, &ui::FenceContainer::fenceItemMenuRequested,
                         [&](const QString&, const QStringList& paths, const QPoint& point) {
                             menuPaths = paths;
                             menuPoint = point;
                         });
        click(vb, tileAt(vb, 0), Qt::RightButton);
        check(QStringLiteral("a right-click on a tile asks StarFish's menu, at a screen point (session 65)"),
              menuPaths.size() == 1 && menuPaths.first().endsWith(QLatin1String("d.txt")) && menuPoint.x() >= 400,
              QStringLiteral("%1 at %2,%3").arg(menuPaths.join(QLatin1Char(','))).arg(menuPoint.x()).arg(menuPoint.y()));
        const QPoint intoB(400 + tileAt(vb, 0).x(), tileAt(vb, 0).y());
        container.dragTilesOutside(a, {QStringLiteral("C:/s65/a.txt"), QStringLiteral("C:/s65/b.txt")}, intoB, false);
        const int moved = container.dragTilesOutside(
            a, {QStringLiteral("C:/s65/a.txt"), QStringLiteral("C:/s65/b.txt")}, intoB, true);
        const bool firstIsA = vb->items().size() == 3 && vb->items().first().path.endsWith(QLatin1String("a.txt"));
        check(QStringLiteral("two tiles dragged into another fence land where let go (session 65)"),
              moved == 2 && va->items().size() == 1 && firstIsA,
              QStringLiteral("moved %1, A %2, B %3").arg(moved).arg(va->items().size()).arg(vb->items().size()));
    }

    // --- session 65: border (T-094) and hover --------------------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Bord"), QRect(0, 0, 320, 240));
        ui::FenceView* view = container.view(id);
        container.setDefaultBorder(3, QStringLiteral("#FFFF0000"));
        view->resize(320, 240);
        const QImage plain = view->grab().toImage();
        const QColor edge = plain.pixelColor(1, 120);
        QEnterEvent enter(QPointF(100, 100), QPointF(100, 100), QPointF(100, 100));
        QCoreApplication::sendEvent(view, &enter);
        const QImage hovered = view->grab().toImage();
        hovered.save(captures + QStringLiteral("/ui_fence_border_hover_%1.png").arg(theme));
        plain.save(captures + QStringLiteral("/ui_fence_border_%1.png").arg(theme));
        const bool lighter = hovered.pixelColor(160, 160).value() > plain.pixelColor(160, 160).value();
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(view, &leave);
        core::FenceStyle style = container.fenceStyle(id);
        style.borderWidth = 0;
        container.setFenceStyle(id, style);
        const QColor none = view->grab().toImage().pixelColor(1, 120);
        check(QStringLiteral("a 3 px red default border, none of its own, a lighter fence on hover (session 65)"),
              edge.red() > 200 && edge.green() < 80 && lighter && none.red() < 120 && !view->hovered(),
              QStringLiteral("edge %1, none %2, hover %3").arg(edge.name(), none.name(), lighter ? QLatin1String("lighter") : QLatin1String("same")));
    }

    // --- T-076 (session 65): a fence without its title bar -------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Sans titre"), QRect(0, 0, 320, 240));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/s65/a.txt"))});
        ui::FenceView* view = container.view(id);
        view->resize(320, 240);
        const int withTitle = view->tileRects().value(0).top();
        core::FenceStyle style = container.fenceStyle(id);
        style.titleBar = false;
        container.setFenceStyle(id, style);
        const int without = view->tileRects().value(0).top();
        const bool moveBand = ui::FenceGeometry::hitTest(view->rect(), QPoint(160, 8), 6, view->titleBarHeight(),
                                                         view->cornerRadius()) == ui::FenceHandle::TitleBar;
        view->grab().save(captures + QStringLiteral("/ui_fence_no_title_%1.png").arg(theme));
        check(QStringLiteral("no title bar: the icons move up, the top band still moves it (T-076)"),
              without < withTitle && moveBand && !view->titleShown(),
              QStringLiteral("first tile top %1 -> %2").arg(withTitle).arg(without));
    }

    // --- session 65: the fence's look in one dialog --------------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Jeux"), QRect(0, 0, 320, 240));
        const core::FenceStyle before = container.fenceStyle(id);
        QDialog* dialog = ui::createFenceAppearanceDialog(&container, id);
        dialog->setAttribute(Qt::WA_DontShowOnScreen);
        dialog->show();
        wait(100);
        auto* radius = dialog->findChild<QComboBox*>(QStringLiteral("radius"));
        auto* useDefault = dialog->findChild<QCheckBox*>(QStringLiteral("useDefault"));
        auto* slider = dialog->findChild<QSlider*>(QStringLiteral("transparencySlider"));
        if (radius)
            radius->setCurrentIndex(radius->findData(30));
        if (useDefault)
            useDefault->setChecked(false);
        if (slider)
            slider->setValue(50);
        const bool live = container.fenceStyle(id).radius == 30
                          && container.fenceEffectiveAlpha(id) == ui::PercentSlider::alphaFromPercent(50);
        auto* borderCombo = dialog->findChild<QComboBox*>(QStringLiteral("borderWidth"));
        check(QStringLiteral("Appearance...: borders up to 12 px (session 71)"),
              borderCombo && borderCombo->findData(12) >= 0 && borderCombo->findData(13) < 0, QString());
        auto* manage = dialog->findChild<QToolButton*>(QStringLiteral("manageTemplate"));
        check(QStringLiteral("Appearance...: templates can be renamed or deleted, once one is chosen (session 70)"),
              manage && manage->menu() && manage->menu()->actions().size() == 2 && !manage->isEnabled(), QString());
        dialog->grab().save(captures + QStringLiteral("/ui_fence_appearance_dialog_%1.png").arg(theme));
        dialog->reject();
        const core::FenceStyle after = container.fenceStyle(id);
        check(QStringLiteral("Appearance...: corners 30 and 50 % live, Cancel restores (session 65)"),
              live && after.radius == before.radius && after.background == before.background
                  && after.defaultOpacity == before.defaultOpacity,
              QStringLiteral("live %1, radius back %2").arg(live ? QLatin1String("yes") : QLatin1String("no")).arg(after.radius));
    }

    // --- session 66: title bar, icons-only fade, single column, cursor -------
    {
        auto renderOf = [](ui::FenceView* v) {
            QImage img(v->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            v->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            return img;
        };
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Perso"), QRect(0, 0, 400, 300));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/s66/ResourcesMonitor.lnk"))});
        ui::FenceView* view = container.view(id);
        view->resize(320, 240);
        core::FenceStyle style = container.fenceStyle(id);
        style.background = QStringLiteral("#801F2937");
        style.defaultOpacity = false;
        style.titleBarColor = QStringLiteral("#FF1F2937"); // same colour, opaque: takes the fence's alpha
        style.borderWidth = 0;
        container.setFenceStyle(id, style);
        const QImage img = renderOf(view);
        const int bandAlpha = img.pixelColor(200, 4).alpha();
        const int panelAlpha = img.pixelColor(200, 230).alpha();
        img.save(captures + QStringLiteral("/s66_band_transparency_%1.png").arg(theme));
        check(QStringLiteral("the title bar has the fence's transparency, not its own (session 66)"),
              qAbs(bandAlpha - panelAlpha) <= 2 && panelAlpha < 200,
              QStringLiteral("band %1, panel %2").arg(bandAlpha).arg(panelAlpha));

        style.titleAlign = QStringLiteral("center");
        container.setFenceStyle(id, style);
        const QRect centred = view->titleTextRect();
        style.titleAlign = QStringLiteral("right");
        container.setFenceStyle(id, style);
        const QRect right = view->titleTextRect();
        view->grab().save(captures + QStringLiteral("/s66_title_right_%1.png").arg(theme));
        style.titleAlign = QStringLiteral("center");
        style.radius = 40;
        container.setFenceStyle(id, style);
        const int inset40 = view->titleInset();
        view->grab().save(captures + QStringLiteral("/s66_title_centred_r40_%1.png").arg(theme));
        style.radius = 0;
        container.setFenceStyle(id, style);
        const int inset0 = view->titleInset();
        check(QStringLiteral("title centred / right, pushed in by a strong rounding (session 66)"),
              qAbs(centred.center().x() - 160) <= 2 && qAbs(right.right() - (320 - inset0)) <= 2 && inset40 > inset0,
              QStringLiteral("centre %1, right %2, inset r0 %3 r40 %4").arg(centred.center().x()).arg(right.right())
                  .arg(inset0).arg(inset40));

        // fadeIcons: the icons alone.
        style.titleAlign.clear();
        style.borderWidth = 3;
        style.borderColor = QStringLiteral("#FFFF0000");
        container.setFenceStyle(id, style);
        const QImage full = renderOf(view);
        view->setIconOpacity(0.3);
        const QImage faded = renderOf(view);
        view->setIconOpacity(1.0);
        const QRect tile = view->tileRects().value(0);
        const bool borderSame = full.pixel(1, 120) == faded.pixel(1, 120);
        const QRect title = view->titleTextRect();
        bool titleSame = true;
        for (int x = title.left(); x <= title.right(); ++x)
            for (int y = title.top(); y <= title.bottom(); ++y)
                titleSame = titleSame && full.pixel(x, y) == faded.pixel(x, y);
        bool iconDiffers = false;
        for (int x = tile.left(); x <= tile.right() && !iconDiffers; ++x)
            for (int y = tile.top(); y <= tile.bottom() && !iconDiffers; ++y)
                iconDiffers = full.pixel(x, y) != faded.pixel(x, y);
        faded.save(captures + QStringLiteral("/s66_icons_faded_%1.png").arg(theme));
        check(QStringLiteral("\"the material on the icons too\": icons only, border and title kept (session 66)"),
              borderSame && titleSame && iconDiffers,
              QStringLiteral("border %1, title %2, icon %3").arg(borderSame).arg(titleSame).arg(iconDiffers));

        // One column at the minimum width: the same room left and right.
        const QRect shrunk = container.setFenceRect(id, QRect(0, 0, 40, 300), false);
        view->resize(shrunk.size());
        const QRect cell = view->tileRects().value(0);
        const int leftRoom = cell.left();
        const int rightRoom = view->width() - 1 - cell.right();
        view->grab().save(captures + QStringLiteral("/s66_single_column_min_%1.png").arg(theme));
        check(QStringLiteral("one column at the minimum width sits in the middle (session 66)"),
              shrunk.width() < 160 && qAbs(leftRoom - rightRoom) <= 1,
              QStringLiteral("width %1, room %2 / %3").arg(shrunk.width()).arg(leftRoom).arg(rightRoom));
        check(QStringLiteral("a fence follows the pointer with no button held (resize arrows, session 66)"),
              view->hasMouseTracking(), QString());
    }

    // --- session 94: a locked zone ---------------------------------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Locked"), QRect(0, 0, 300, 220));
        const QString other = container.createFence(QStringLiteral("Other"), QRect(400, 0, 300, 220));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/s94/a.txt")),
                                core::itemFromPathOnly(QStringLiteral("C:/s94/b.txt"))});
        container.fileInto(other, {core::itemFromPathOnly(QStringLiteral("C:/s94/c.txt"))});
        ui::FenceView* view = container.view(id);
        view->resize(300, 220);
        container.setFenceLocked(id, true);
        const QRect band = view->titleBandRect();
        const bool hits = view->locked() && container.fenceLocked(id)
                          && view->handleAt(QPoint(298, 110)) == ui::FenceHandle::None
                          && view->handleAt(QPoint(298, 218)) == ui::FenceHandle::None
                          && view->handleAt(QPoint(150, 218)) == ui::FenceHandle::None
                          && view->handleAt(band.center()) == ui::FenceHandle::TitleBar;
        int geometry = 0, outside = 0, rename = 0, roll = 0;
        QObject::connect(view, &ui::FenceView::geometryDragStarted, view, [&geometry] { ++geometry; });
        QObject::connect(view, &ui::FenceView::itemsDraggedOutside, view, [&outside] { ++outside; });
        QObject::connect(view, &ui::FenceView::titleEditRequested, view, [&rename] { ++rename; });
        QObject::connect(view, &ui::FenceView::rollUpRequested, view, [&roll] { ++roll; });
        auto send = [view](QEvent::Type type, const QPoint& at, Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(at), view->mapToGlobal(QPointF(at)),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(view, &e);
        };
        bool cursors = true;
        for (const QPoint& at : {QPoint(298, 110), QPoint(298, 218), band.center(), QPoint(2, 2)}) {
            send(QEvent::MouseMove, at, Qt::NoButton);
            cursors = cursors && view->cursor().shape() == Qt::ArrowCursor;
        }
        send(QEvent::MouseButtonPress, band.center(), Qt::LeftButton);
        send(QEvent::MouseMove, band.center() + QPoint(60, 40), Qt::LeftButton);
        send(QEvent::MouseButtonRelease, band.center() + QPoint(60, 40), Qt::NoButton);
        const QPoint icon = view->itemRectOf(QStringLiteral("C:/s94/a.txt")).center();
        send(QEvent::MouseButtonPress, icon, Qt::LeftButton);
        send(QEvent::MouseMove, icon + QPoint(30, 0), Qt::LeftButton);
        send(QEvent::MouseMove, QPoint(360, 100), Qt::LeftButton);
        send(QEvent::MouseButtonRelease, QPoint(360, 100), Qt::NoButton);
        send(QEvent::MouseButtonDblClick, view->titleTextRect().center(), Qt::LeftButton);
        send(QEvent::MouseButtonRelease, view->titleTextRect().center(), Qt::NoButton);
        const bool gestures = geometry == 0 && outside == 0 && rename == 0 && roll == 1
                              && view->items().size() == 2 && !icon.isNull();
        container.setFenceRolledUp(id, false); // back down: the drops aim at its body
        // Nothing lands in it: another fence's tile, a desktop icon (fileIntoAt).
        const int moved = container.dragTilesOutside(other, {QStringLiteral("C:/s94/c.txt")}, QPoint(150, 120), true);
        const int filed = container.fileIntoAt(id, {core::itemFromPathOnly(QStringLiteral("C:/s94/d.txt"))},
                                               QPoint(150, 120));
        const bool refused = moved == 0 && filed == 0 && view->items().size() == 2
                             && container.view(other)->items().size() == 1;
        bool saved = false;
        for (const core::Fence& f : container.toLayout().fences)
            if (f.id == id)
                saved = f.locked;
        // Session 95: no move grip on a locked fence without a title bar.
        core::FenceStyle bare = container.fenceStyle(id);
        bare.titleBar = false;
        container.setFenceStyle(id, bare);
        const auto gripShown = [view] {
            QEnterEvent enter(QPointF(150, 100), QPointF(150, 100), QPointF(150, 100));
            QCoreApplication::sendEvent(view, &enter);
            const QImage img = view->grab().toImage();
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(view, &leave);
            const int y = ui::FenceView::kHiddenTitleBand / 2 + 1;
            return img.pixelColor(img.width() / 2, y) != img.pixelColor(img.width() / 2 + 40, y);
        };
        const bool lockedGrip = gripShown();
        container.setFenceLocked(id, false);
        const bool unlockedGrip = gripShown();
        check(QStringLiteral("a locked zone without title bar shows no move grip on hover; unlocked it does (session 95)"),
              !lockedGrip && unlockedGrip, QStringLiteral("locked %1 unlocked %2").arg(lockedGrip).arg(unlockedGrip));
        bare.titleBar = true;
        container.setFenceStyle(id, bare);
        const bool unlocked = view->handleAt(QPoint(298, 110)) != ui::FenceHandle::None
                              && container.fileIntoAt(id, {core::itemFromPathOnly(QStringLiteral("C:/s94/d.txt"))},
                                                      QPoint(150, 120)) == 1;
        check(QStringLiteral("a locked zone: no grip, arrow cursor, no move, no rename, no drag out, no drop in; roll-up kept (session 94)"),
              hits && cursors && gestures && refused && saved && unlocked,
              QStringLiteral("hits %1 cursors %2 geometry %3 outside %4 rename %5 roll %6 moved %7 filed %8 saved %9 unlocked %10")
                  .arg(hits).arg(cursors).arg(geometry).arg(outside).arg(rename).arg(roll).arg(moved).arg(filed)
                  .arg(saved).arg(unlocked));
    }

    // --- session 68: the title bar on any side ---------------------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Jeux vidéo"), QRect(0, 0, 300, 220));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/s68/a.txt")),
                                core::itemFromPathOnly(QStringLiteral("C:/s68/b.txt"))});
        ui::FenceView* view = container.view(id);
        view->resize(300, 220);
        QImage sheet(3 * 310 + 10, 230, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor(0x9A, 0xC8, 0x2A));
        QPainter sp(&sheet);
        QString detail;
        bool ok = true;
        const QStringList sides{QStringLiteral("bottom"), QStringLiteral("left"), QStringLiteral("right")};
        for (int i = 0; i < sides.size(); ++i) {
            core::FenceStyle style = container.fenceStyle(id);
            style.titleSide = sides.at(i);
            style.radius = 20;
            style.background = QStringLiteral("#B31F2937");
            style.defaultOpacity = false;
            style.titleBarColor = QStringLiteral("#FF2563EB");
            style.titleAlign = i == 1 ? QStringLiteral("center") : QString();
            container.setFenceStyle(id, style);
            const QRect band = view->titleBandRect();
            const QRect content = view->contentRect();
            const QRect text = view->titleTextRect();
            const bool bandOk = i == 0 ? band.top() == 220 - band.height() && content.bottom() < band.top()
                              : i == 1 ? band.left() == 0 && band.height() == 220 && content.left() > band.right()
                                       : band.right() == 299 && band.height() == 220 && content.right() < band.left();
            const bool textOk = band.contains(text) && (i == 0 ? text.width() > text.height() : text.height() > text.width());
            const bool grab = view->handleAt(band.center()) == ui::FenceHandle::TitleBar
                              && view->handleAt(content.center()) == ui::FenceHandle::None;
            ok = ok && bandOk && textOk && grab;
            detail += QStringLiteral("%1: band %2 text %3 grab %4  ").arg(sides.at(i)).arg(bandOk).arg(textOk).arg(grab);
            sp.save();
            sp.translate(10 + i * 310, 5);
            view->render(&sp, QPoint(), QRegion(), QWidget::DrawChildren);
            sp.restore();
        }
        sp.end();
        sheet.save(captures + QStringLiteral("/s68_title_sides_%1.png").arg(theme));
        // Back on top for an old layout: no key = top.
        core::FenceStyle style = container.fenceStyle(id);
        style.titleSide.clear();
        container.setFenceStyle(id, style);
        ok = ok && view->titleSide() == ui::FenceView::TitleSide::Top && view->titleBandRect().top() == 0;
        check(QStringLiteral("the title bar at the bottom, left, right: band, vertical text, grab, icons beside (session 68)"),
              ok, detail);
    }

    // --- session 69: the title bar as a tab, hover effects -------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Zone 11"), QRect(0, 0, 320, 220));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/s69/a.txt")),
                                core::itemFromPathOnly(QStringLiteral("C:/s69/b.txt"))});
        ui::FenceView* view = container.view(id);
        view->resize(320, 220);
        const auto shot = [&](const QString& name) {
            QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/%1_%2.png").arg(name, theme));
            return img;
        };
        core::FenceStyle style = container.fenceStyle(id);
        const bool fullByDefault = !view->tabbed();
        style.titleBarShape = QStringLiteral("tab");
        style.radius = 14;
        style.borderWidth = 2;
        style.borderColor = QStringLiteral("#FFE5E7EB");
        style.background = QStringLiteral("#B31F2937");
        style.defaultOpacity = false;
        style.titleBarColor = QStringLiteral("#FF2563EB");
        style.titleAlign.clear();
        container.setFenceStyle(id, style);
        const QRect band = view->titleBandRect();
        const QPair<int, int> span = view->tabSpan(320);
        const QPainterPath outline = view->outlinePath(QSizeF(320, 220));
        const QPoint inTab(span.first + span.second / 2, band.height() / 2);
        const QPoint pastTab(qMin(319, span.first + span.second + 30), band.height() / 2);
        const bool left = view->tabbed() && span.first == 0 && span.second < 200 && span.second > 30;
        const bool shape = outline.contains(QPointF(inTab)) && !outline.contains(QPointF(pastTab))
                           && outline.contains(QPointF(160, 120));
        const bool hits = view->handleAt(inTab) == ui::FenceHandle::TitleBar
                          && view->handleAt(pastTab) == ui::FenceHandle::None
                          && view->handleAt(QPoint(318, 120)) != ui::FenceHandle::None
                          && view->handleAt(QPoint(160, 218)) != ui::FenceHandle::None;
        const QImage leftImg = shot(QStringLiteral("s69_tab_left"));
        const bool clear = qAlpha(leftImg.pixel(pastTab)) == 255
                           && leftImg.pixelColor(pastTab) == QColor(0x9A, 0xC8, 0x2A);
        style.titleAlign = QStringLiteral("center");
        container.setFenceStyle(id, style);
        const QPair<int, int> mid = view->tabSpan(320);
        const bool centred = qAbs(mid.first + mid.second / 2 - 160) <= 1
                             && view->handleAt(QPoint(5, band.height() / 2)) != ui::FenceHandle::TitleBar;
        shot(QStringLiteral("s69_tab_centred"));
        style.titleAlign = QStringLiteral("right");
        container.setFenceStyle(id, style);
        const bool right = view->tabSpan(320).first + view->tabSpan(320).second == 320;
        // The tab on the other sides: one sheet.
        QImage sheet(3 * 330 + 10, 230, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor(0x9A, 0xC8, 0x2A));
        QPainter sp(&sheet);
        bool sides = true;
        const QStringList names{QStringLiteral("bottom"), QStringLiteral("left"), QStringLiteral("right")};
        for (int i = 0; i < names.size(); ++i) {
            style.titleSide = names.at(i);
            style.titleAlign = i == 0 ? QStringLiteral("center") : QString();
            container.setFenceStyle(id, style);
            const QRect b = view->titleBandRect();
            const QRect t = view->titleTextRect();
            sides = sides && b.contains(t) && view->handleAt(t.center()) == ui::FenceHandle::TitleBar;
            sp.save();
            sp.translate(10 + i * 330, 5);
            view->render(&sp, QPoint(), QRegion(), QWidget::DrawChildren);
            sp.restore();
        }
        sp.end();
        sheet.save(captures + QStringLiteral("/s69_tab_sides_%1.png").arg(theme));
        style.titleSide.clear();
        style.titleAlign.clear();
        container.setFenceStyle(id, style);
        // Rolled up: the tab alone.
        const QRectF rolled = view->outlinePath(QSizeF(320, band.height())).boundingRect();
        const bool rolledTab = rolled.width() < 200;
        check(QStringLiteral("a tab title bar: as long as the title, left / centred / right, click-through beside it (session 69)"),
              fullByDefault && left && shape && hits && clear && centred && right && sides && rolledTab,
              QStringLiteral("default full %1, left %2, shape %3, hits %4, clear %5, centred %6, right %7, sides %8, rolled %9")
                  .arg(fullByDefault).arg(left).arg(shape).arg(hits).arg(clear).arg(centred).arg(right).arg(sides)
                  .arg(rolledTab));

        // Hover effects: each changes the picture, "nothing" does not.
        style.titleBarShape.clear();
        style.hoverEffect = QStringLiteral("none");
        container.setFenceStyle(id, style);
        const QImage still = shot(QStringLiteral("s69_hover_still"));
        QEnterEvent enter(QPointF(160, 120), QPointF(160, 120), QPointF(160, 120));
        QCoreApplication::sendEvent(view, &enter);
        const bool noneSame = shot(QStringLiteral("s69_hover_none")) == still;
        QString hoverDetail;
        bool allDiffer = true;
        QImage hoverSheet(5 * 330 + 10, 230, QImage::Format_ARGB32_Premultiplied);
        hoverSheet.fill(QColor(0x9A, 0xC8, 0x2A));
        QPainter hp(&hoverSheet);
        const QStringList effects{QStringLiteral("lighter"), QStringLiteral("opacity"), QStringLiteral("colour"),
                                  QStringLiteral("noborder"), QStringLiteral("glow")};
        for (int i = 0; i < effects.size(); ++i) {
            style.hoverEffect = effects.at(i);
            container.setFenceStyle(id, style);
            QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            const bool differs = img != still;
            allDiffer = allDiffer && differs;
            hoverDetail += QStringLiteral("%1 %2  ").arg(effects.at(i)).arg(differs);
            hp.drawImage(10 + i * 330, 5, img);
        }
        hp.end();
        hoverSheet.save(captures + QStringLiteral("/s69_hover_effects_%1.png").arg(theme));
        check(QStringLiteral("hover effects: lighter, opacity, colour, no border, glow change the fence, none does not (session 69)"),
              noneSame && allDiffer, QStringLiteral("none same %1; ").arg(noneSame) + hoverDetail);
        check(QStringLiteral("Settings has the desktop double-click switch (off by default, session 69)"),
              settings.findChild<QWidget*>(QStringLiteral("desktopDoubleClickSwitch")) != nullptr, QString());
    }

    // --- session 70: one outline, one border thickness; roll-up keeps the width -
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Games"), QRect(0, 0, 420, 200));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/s70/a.txt")),
                                core::itemFromPathOnly(QStringLiteral("C:/s70/b.txt"))});
        ui::FenceView* view = container.view(id);
        view->resize(420, 200);
        const QColor red(255, 0, 0);
        // Renders at a device pixel ratio, over a transparent ground.
        const auto renderAt = [&](qreal dpr) {
            QImage img((QSizeF(view->size()) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            img.setDevicePixelRatio(dpr);
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.setDevicePixelRatio(1.0);
            return img;
        };
        // The border's thickness along a ray (physical pixels).
        const auto thickness = [&](const QImage& img, QPointF from, QPointF dir, qreal length) {
            const qreal n = std::hypot(dir.x(), dir.y());
            dir /= n;
            int hits = 0;
            for (qreal t = 0; t < length; t += 0.25) {
                const QPoint px = (from + dir * t).toPoint();
                if (!img.rect().contains(px))
                    continue;
                const QColor c = QColor::fromRgba(qUnpremultiply(img.pixel(px)));
                if (c.alpha() > 100 && c.red() - qMax(c.green(), c.blue()) > 110)
                    ++hits;
            }
            return hits * 0.25;
        };
        QString detail;
        bool uniform = true;
        bool uncut = true;
        int combos = 0;
        for (qreal dpr : {1.0, 1.5}) {
            for (int bw : {1, 4, 8, 12}) { // session 71: up to 12
                for (int radius : {0, 12, 30, 40}) {
                    core::FenceStyle style = container.fenceStyle(id);
                    style.titleBarShape = QStringLiteral("tab");
                    style.titleAlign = QStringLiteral("center");
                    style.radius = radius;
                    style.borderWidth = bw;
                    style.borderColor = QStringLiteral("#FFFF0000");
                    style.background = QStringLiteral("#FF1F2937");
                    style.defaultOpacity = false;
                    style.titleBarColor.clear();
                    container.setFenceStyle(id, style);
                    const QImage img = renderAt(dpr);
                    const qreal L = 420, T = 200, b = view->titleBarHeight();
                    const QPair<int, int> span = view->tabSpan(420);
                    const qreal x0 = span.first, x1 = span.first + span.second;
                    const qreal r = qMin<qreal>(radius, T / 2);
                    const qreal rt = qMin<qreal>(r, b / 2);
                    const qreal rb = qMin<qreal>(r, (T - b) / 2);
                    const qreal want = bw * dpr;
                    QList<QPair<QString, qreal>> rays;
                    const auto ray = [&](const QString& name, QPointF from, QPointF dir, qreal len) {
                        rays.append({name, thickness(img, from * dpr, dir, len * dpr)});
                    };
                    ray(QStringLiteral("tab top"), QPointF((x0 + x1) / 2, b / 2), QPointF(0, -1), b / 2 + 2);
                    ray(QStringLiteral("side"), QPointF(30, (b + T) / 2), QPointF(-1, 0), 32);
                    ray(QStringLiteral("bottom"), QPointF(L / 2, T - 30), QPointF(0, 1), 32);
                    if (x0 - rt - rb > 20)
                        ray(QStringLiteral("body top"), QPointF((rb + x0 - rt) / 2, b + 20), QPointF(0, -1), 22);
                    if (rb >= 2 * bw + 2)
                        ray(QStringLiteral("body corner"), QPointF(L - rb, T - rb), QPointF(1, 1), rb + 3);
                    if (rt >= 2 * bw + 2)
                        ray(QStringLiteral("tab corner"), QPointF(x0 + rt, rt), QPointF(-1, -1), rt + 3);
                    if (rt >= 2 && x0 - rt >= rb)
                        ray(QStringLiteral("junction"), QPointF(x0 - rt, b - rt) + QPointF(-1, -1),
                            QPointF(1, 1), rt + bw + 4);
                    for (const auto& [name, t] : rays) {
                        if (qAbs(t - want) > 1.5) {
                            uniform = false;
                            detail += QStringLiteral("dpr %1 b%2 r%3 %4: %5 (want %6)  ")
                                          .arg(dpr).arg(bw).arg(radius).arg(name).arg(t).arg(want);
                        }
                    }
                    // Nothing painted outside the window's shape (outline + 1 px).
                    QPainterPath shape = QTransform::fromScale(dpr, dpr).map(view->outlinePath(QSizeF(420, 200)));
                    QPainterPathStroker stroker;
                    stroker.setWidth(2.0);
                    shape = shape.united(stroker.createStroke(shape));
                    int outside = 0;
                    for (int y = 0; y < img.height(); y += 1)
                        for (int x = 0; x < img.width(); x += 1)
                            if (qAlpha(img.pixel(x, y)) > 8 && !shape.contains(QPointF(x + 0.5, y + 0.5)))
                                ++outside;
                    if (outside > 0) {
                        uncut = false;
                        detail += QStringLiteral("dpr %1 b%2 r%3: %4 px outside  ").arg(dpr).arg(bw).arg(radius).arg(outside);
                    }
                    ++combos;
                    if (bw == 12) {
                        const QRect text = view->titleTextRect();
                        if (text.left() < x0 + bw || text.right() > x1 - bw) {
                            uniform = false;
                            detail += QStringLiteral("dpr %1 b12 r%2: title %3..%4 in tab %5..%6  ").arg(dpr).arg(radius)
                                          .arg(text.left()).arg(text.right()).arg(x0).arg(x1);
                        }
                    }
                    if ((bw == 4 || bw == 12) && radius == 40) {
                        QImage shot(img.size(), QImage::Format_ARGB32_Premultiplied);
                        shot.fill(QColor(0x9A, 0xC8, 0x2A));
                        QPainter sp(&shot);
                        sp.drawImage(0, 0, img);
                        sp.end();
                        shot.save(captures + QStringLiteral("/s70_tab_b%3_r40_dpr%1_%2.png").arg(int(dpr * 100)).arg(theme).arg(bw));
                        const QRect zoom = QRect(int((x0 - rt - 14) * dpr), 0, int((x1 - x0 + 2 * rt + 28) * dpr),
                                                 int((b + 30) * dpr)).intersected(shot.rect());
                        shot.copy(zoom).scaled(zoom.size() * 4, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                            .save(captures + QStringLiteral("/s70_tab_b%3_r40_zoom_dpr%1_%2.png").arg(int(dpr * 100)).arg(theme).arg(bw));
                    }
                }
            }
        }
        check(QStringLiteral("the tab outline: one border thickness on edges, corners and junctions, nothing cut, "
                             "b 1/4/8/12 x r 0/12/30/40 at 100/150 %, title inside the tab at 12 (sessions 70-71)"),
              uniform && uncut && combos == 32, detail.isEmpty() ? QStringLiteral("%1 combos").arg(combos) : detail);

        // Roll up, nudge (the first click of the double-click), roll down: the same size.
        core::FenceStyle style = container.fenceStyle(id);
        style.titleBarShape.clear();
        container.setFenceStyle(id, style);
        QString rollDetail;
        bool same = true;
        for (const QString& shape : {QString(), QStringLiteral("tab")}) {
            style.titleBarShape = shape;
            container.setFenceStyle(id, style);
            const QRect before = container.setFenceRect(id, QRect(100, 100, 300, 220), false);
            container.setFenceRolledUp(id, true);
            container.setFenceRect(id, container.fenceRect(id), true);
            container.setFenceRect(id, container.fenceRect(id).translated(1, 0), false);
            container.setFenceRect(id, container.fenceRect(id).translated(-1, 0), false);
            container.setFenceRolledUp(id, false);
            const QRect after = container.fenceRect(id);
            same = same && after.size() == before.size();
            rollDetail += QStringLiteral("%1: %2x%3 -> %4x%5  ").arg(shape.isEmpty() ? QStringLiteral("full") : shape)
                              .arg(before.width()).arg(before.height()).arg(after.width()).arg(after.height());
        }
        check(QStringLiteral("rolling up then down gives back the very same size, full bar and tab (session 70)"),
              same, rollDetail);
    }

    // --- session 71: the grain behind the icons; unroll on hover -------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Grain"), QRect(0, 0, 300, 170));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/Windows/explorer.exe")),
                                core::itemFromPathOnly(QStringLiteral("C:/Windows/regedit.exe"))});
        ui::FenceView* view = container.view(id);
        view->resize(300, 170);
        wait(400);
        const auto layer = [&](ui::FenceView::PaintLayers layers) {
            QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            view->setPaintLayers(layers);
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(),
                         layers == ui::FenceView::PaintLayers::Background ? QWidget::RenderFlags() : QWidget::DrawChildren);
            p.end();
            view->setPaintLayers(ui::FenceView::PaintLayers::All);
            return img;
        };
        QString detail;
        bool crisp = true;
        bool wasOver = true;
        for (const QString& effect : {QStringLiteral("textured"), QStringLiteral("acrylic")}) {
            core::FenceStyle style = container.fenceStyle(id);
            style.effect = effect;
            style.background = QStringLiteral("#991F2937");
            style.defaultOpacity = false;
            style.borderWidth = 3;
            style.borderColor = QStringLiteral("#FF000000");
            style.radius = 20;
            container.setFenceStyle(id, style);
            const QImage all = layer(ui::FenceView::PaintLayers::All);
            const QImage back = layer(ui::FenceView::PaintLayers::Background);
            const QImage front = layer(ui::FenceView::PaintLayers::Foreground);
            const QRect tile = view->tileRects().value(0);
            // The desktop under it: a gradient, as a wallpaper.
            QImage desk(view->size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter dp(&desk);
                QLinearGradient g(0, 0, 300, 170);
                g.setColorAt(0, QColor(0x4D, 0x8A, 0x1E));
                g.setColorAt(1, QColor(0xB5, 0xD8, 0x2C));
                dp.fillRect(desk.rect(), g);
            }
            const qreal grainOpacity = effect == QLatin1String("textured") ? 1.0 : 0.6;
            const auto grainOver = [&](QImage& img, const QPainterPath& area) {
                QPainter gp(&img);
                gp.setClipPath(area);
                gp.setOpacity(grainOpacity);
                gp.fillRect(img.rect(), QBrush(ui::FenceContainer::grainTile()));
            };
            // Before (sessions 65-69): the fence, then the grain over all of it.
            QImage before = desk;
            {
                QPainter bp(&before);
                bp.drawImage(0, 0, all);
            }
            grainOver(before, view->outlinePath(QSizeF(view->size())));
            // Now: the background, the grain inside the border, the icons and title on top.
            QImage after = desk;
            {
                QPainter ap(&after);
                ap.drawImage(0, 0, back);
            }
            grainOver(after, view->outlinePath(QSizeF(view->size()), 3));
            {
                QPainter ap(&after);
                ap.drawImage(0, 0, front);
            }
            // The icon's opaque pixels: exactly the icon now, grain-speckled before.
            int opaque = 0, same = 0, overBefore = 0;
            for (int y = tile.top(); y <= tile.bottom(); ++y)
                for (int x = tile.left(); x <= tile.right(); ++x) {
                    const QRgb f = front.pixel(x, y);
                    if (qAlpha(f) < 255)
                        continue;
                    ++opaque;
                    if (after.pixel(x, y) == f)
                        ++same;
                    if (before.pixel(x, y) != f)
                        ++overBefore;
                }
            const bool backHasNoIcon = back.pixel(tile.center()) == back.pixel(QPoint(tile.center().x(), 160));
            const bool frontHasNoPanel = qAlpha(front.pixel(QPoint(250, 150))) == 0;
            crisp = crisp && opaque > 50 && same == opaque && backHasNoIcon && frontHasNoPanel;
            wasOver = wasOver && overBefore > opaque / 2;
            detail += QStringLiteral("%1: icon px %2, untouched now %3, grained before %4, layers %5/%6  ")
                          .arg(effect).arg(opaque).arg(same).arg(overBefore).arg(backHasNoIcon).arg(frontHasNoPanel);
            const QRect zoom = tile.adjusted(-6, -6, 6, 6).intersected(desk.rect());
            QImage pair(zoom.width() * 6 * 2 + 12, zoom.height() * 6, QImage::Format_ARGB32_Premultiplied);
            pair.fill(Qt::white);
            {
                QPainter pp(&pair);
                pp.drawImage(0, 0, before.copy(zoom).scaled(zoom.size() * 6, Qt::IgnoreAspectRatio, Qt::FastTransformation));
                pp.drawImage(zoom.width() * 6 + 12, 0,
                             after.copy(zoom).scaled(zoom.size() * 6, Qt::IgnoreAspectRatio, Qt::FastTransformation));
            }
            pair.save(captures + QStringLiteral("/s71_icon_%1_before_after_%2.png").arg(effect, theme));
            after.save(captures + QStringLiteral("/s71_fence_%1_after_%2.png").arg(effect, theme));
        }
        check(QStringLiteral("acrylic / textured: the grain behind the icons, not over them, no cut-outs (session 71)"),
              crisp && wasOver, detail);

        // Unroll on hover: off by default; on, after a short delay; back up after leaving.
        core::FenceStyle style = container.fenceStyle(id);
        style.effect.clear();
        container.setFenceStyle(id, style);
        const QRect stored = container.setFenceRect(id, QRect(50, 50, 300, 170), false);
        container.setFenceRolledUp(id, true);
        const bool offByDefault = !container.hoverUnrollDefault();
        const auto enter = [&] {
            QEnterEvent e(QPointF(20, 10), QPointF(20, 10), QPointF(20, 10));
            QCoreApplication::sendEvent(view, &e);
        };
        const auto leave = [&] {
            QEvent e(QEvent::Leave);
            QCoreApplication::sendEvent(view, &e);
        };
        enter();
        wait(ui::FenceContainer::kHoverUnrollDelayMs + 150);
        const bool unrolledWhenOff = !view->rolledUp();
        leave();
        container.setHoverUnrollDefault(true);
        enter();
        wait(ui::FenceContainer::kHoverUnrollDelayMs + 150);
        const bool unrolled = !view->rolledUp() && container.fenceHoverUnrolled(id) && container.fenceRolledUp(id)
                              && container.fenceRect(id) == stored;
        leave();
        wait(ui::FenceContainer::kHoverRerollDelayMs / 2);
        const bool lingers = !view->rolledUp();
        enter(); // back before the delay: stays down
        wait(ui::FenceContainer::kHoverRerollDelayMs + 150);
        const bool kept = !view->rolledUp();
        leave();
        wait(ui::FenceContainer::kHoverRerollDelayMs + 200);
        const bool rerolled = view->rolledUp() && !container.fenceHoverUnrolled(id) && container.fenceRect(id) == stored;
        // This fence says no.
        style = container.fenceStyle(id);
        style.hoverUnroll = QStringLiteral("off");
        container.setFenceStyle(id, style);
        enter();
        wait(ui::FenceContainer::kHoverUnrollDelayMs + 150);
        const bool fenceSaysNo = view->rolledUp();
        leave();
        // "Roll down" while unrolled by the pointer keeps it down.
        style.hoverUnroll = QStringLiteral("on");
        container.setHoverUnrollDefault(false);
        container.setFenceStyle(id, style);
        enter();
        wait(ui::FenceContainer::kHoverUnrollDelayMs + 150);
        container.setFenceRolledUp(id, false);
        leave();
        wait(ui::FenceContainer::kHoverRerollDelayMs + 200);
        const bool explicitDown = !view->rolledUp() && !container.fenceRolledUp(id);
        check(QStringLiteral("unroll on hover: off by default, delay, lingers, rolls back, per fence, geometry kept (session 71)"),
              offByDefault && !unrolledWhenOff && unrolled && lingers && kept && rerolled && fenceSaysNo && explicitDown,
              QStringLiteral("default off %1, untouched when off %2, unrolled %3, lingers %4, kept %5, rerolled %6, fence no %7, "
                             "explicit down %8")
                  .arg(offByDefault).arg(!unrolledWhenOff).arg(unrolled).arg(lingers).arg(kept).arg(rerolled).arg(fenceSaysNo)
                  .arg(explicitDown));
        check(QStringLiteral("Settings has the unroll-on-hover switch (session 71)"),
              settings.findChild<QWidget*>(QStringLiteral("hoverUnrollSwitch")) != nullptr, QString());
    }

    // --- session 80: insertion bar, desktop grid, widget engine ---------------
    {
        using L = ui::IconLayout;
        // Pure: the gap under the cursor and its bar, for each flow.
        const auto cellsFor = [](int flow, int labels, const QVector<QSize>& spans = {}) {
            return L::compute(QSize(400, 400), 48, 72, labels, 8, 6, 4, flow, spans); // 3 items + the free cell
        };
        const auto box = [](const ui::IconCell& c) { return c.icon.united(c.label); };
        bool gaps = true;
        QStringList gapNotes;
        for (int flow : {0, int(L::FromRight), int(L::ColumnsFirst), int(L::ColumnsFirst | L::FromBottom)}) {
            const auto cells = cellsFor(flow, 34);
            const QRect c0 = box(cells[0]);
            const bool cols = flow & L::ColumnsFirst;
            const bool mirror = cols ? (flow & L::FromBottom) : (flow & L::FromRight);
            // The trailing half of cell 0 -> gap 1; its leading half -> gap 0.
            QPoint trailing = c0.center(), leading = c0.center();
            const int d = cols ? c0.height() / 4 : c0.width() / 4;
            if (cols) {
                trailing.ry() += mirror ? -d : d;
                leading.ry() += mirror ? d : -d;
            } else {
                trailing.rx() += mirror ? -d : d;
                leading.rx() += mirror ? d : -d;
            }
            const int gT = L::insertionIndex(cells, trailing, flow);
            const int gL = L::insertionIndex(cells, leading, flow);
            const int gEnd = L::insertionIndex(cells, box(cells[3]).center(), flow);
            const QLine bar = L::insertionBar(cells, 1, flow, 8);
            // Between cells 0 and 1, across the flow, as long as the cell.
            const QRect c1 = box(cells[1]);
            const bool across = cols ? bar.y1() == bar.y2() && bar.x1() == c1.left() && bar.x2() == c1.right()
                                     : bar.x1() == bar.x2() && bar.y1() == c1.top() && bar.y2() == c1.bottom();
            const int at = cols ? bar.y1() : bar.x1();
            const int lo = cols ? qMin(c0.bottom(), c1.bottom()) : qMin(c0.right(), c1.right());
            const int hi = cols ? qMax(c0.top(), c1.top()) : qMax(c0.left(), c1.left());
            const bool between = at >= lo && at <= hi;
            const bool ok = gT == 1 && gL == 0 && gEnd == 3 && across && between;
            gaps = gaps && ok;
            gapNotes << QStringLiteral("flow %1: %2/%3/%4 bar %5,%6-%7,%8").arg(flow).arg(gT).arg(gL).arg(gEnd)
                            .arg(bar.x1()).arg(bar.y1()).arg(bar.x2()).arg(bar.y2());
        }
        // Names hidden: the bar is as long as the icon; a 2x1 widget first.
        const auto bare = cellsFor(0, 0);
        const QLine shortBar = L::insertionBar(bare, 1, 0, 8);
        const bool namesHidden = shortBar.y2() - shortBar.y1() == 47;
        const auto spanned = cellsFor(0, 34, {QSize(2, 1), QSize(), QSize()});
        const int afterWidget = L::insertionIndex(spanned, box(spanned[0]).center() + QPoint(box(spanned[0]).width() / 4, 0), 0);
        const QLine widgetBar = L::insertionBar(spanned, 1, 0, 8);
        const bool widgetOk = afterWidget == 1 && widgetBar.x1() > box(spanned[0]).right()
                              && widgetBar.x1() < box(spanned[1]).left();
        check(QStringLiteral("insertion gap and bar follow the flow (rows / columns, any corner), hidden names, widgets (session 80)"),
              gaps && namesHidden && widgetOk,
              gapNotes.join(QStringLiteral("; ")) + QStringLiteral("; names hidden %1, widget %2 (gap %3)")
                  .arg(namesHidden).arg(widgetOk).arg(afterWidget));

        // In a fence: a real drag shows the bar and lands where it shows.
        QTemporaryDir temp;
        QList<core::DesktopItem> files;
        for (int i = 0; i < 5; ++i) {
            const QString path = QDir::fromNativeSeparators(temp.path()) + QStringLiteral("/s80_%1.txt").arg(i);
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write("starfish probe s80");
            file.close();
            files << core::itemFromPathOnly(path);
        }
        ui::FenceContainer container;
        const QString a = container.createFence(QStringLiteral("Insertion"), QRect(0, 0, 420, 260));
        const QString b = container.createFence(QStringLiteral("Other"), QRect(500, 0, 420, 260));
        container.fileInto(a, files.mid(0, 4));
        container.fileInto(b, files.mid(4, 1));
        ui::FenceView* view = container.view(a);
        const auto order = [](ui::FenceView* v) {
            QStringList names;
            for (const core::DesktopItem& item : v->items())
                names << QFileInfo(item.path).completeBaseName().right(1);
            return names.join(QString());
        };
        const auto cellOf = [&](ui::FenceView* v, int i) {
            const QVector<QRect> rects = v->tileRects();
            return i < rects.size() ? rects.at(i) : QRect();
        };
        const auto send = [&](ui::FenceView* v, QEvent::Type type, const QPoint& at, Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(at), v->mapToGlobal(QPointF(at)), Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(v, &e);
        };
        const QString before = order(view);
        const QRect c0 = cellOf(view, 0), c2 = cellOf(view, 2), c3 = cellOf(view, 3);
        send(view, QEvent::MouseButtonPress, c0.center(), Qt::LeftButton);
        send(view, QEvent::MouseMove, c0.center() + QPoint(12, 12), Qt::LeftButton);
        const QPoint target = QPoint(c2.right() - c2.width() / 5, c2.center().y()); // trailing half of the 3rd
        send(view, QEvent::MouseMove, target, Qt::LeftButton);
        const QLine shown = view->shownInsertionBar();
        const bool barBetween = !shown.isNull() && shown.x1() == shown.x2() && shown.x1() > c2.right()
                                && shown.x1() < c3.left();
        const auto captureView = [&](ui::FenceView* v, const QString& name) {
            QImage img(v->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x2A, 0x4A, 0x6A));
            QPainter painter(&img);
            v->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
            painter.end();
            img.save(captures + QStringLiteral("/%1_%2.png").arg(name, theme));
        };
        captureView(view, QStringLiteral("s80_insertion_bar_rows"));
        send(view, QEvent::MouseButtonRelease, target, Qt::NoButton);
        const QString after = order(view);
        // Back over its own place: no bar (letting go changes nothing).
        const QRect n0 = cellOf(view, 0);
        send(view, QEvent::MouseButtonPress, n0.center(), Qt::LeftButton);
        send(view, QEvent::MouseMove, n0.center() + QPoint(12, 0), Qt::LeftButton);
        const bool noBarInPlace = view->shownInsertionBar().isNull();
        send(view, QEvent::MouseButtonRelease, n0.center() + QPoint(12, 0), Qt::NoButton);
        check(QStringLiteral("dragging an icon in a fence shows the bar between the icons and lands there (session 80)"),
              before == QLatin1String("0123") && after == QLatin1String("1203") && barBetween && noBarInPlace
                  && order(view) == after,
              QStringLiteral("%1 -> %2, bar %3,%4-%5,%6 between %7, none in place %8")
                  .arg(before, after).arg(shown.x1()).arg(shown.y1()).arg(shown.x2()).arg(shown.y2()).arg(barBetween)
                  .arg(noBarInPlace));

        // Columns from the bottom right, names hidden, a widget: captured.
        core::FenceStyle style = container.fenceStyle(a);
        style.iconFlow = L::flowKeys().value(int(L::ColumnsFirst | L::FromBottom | L::FromRight));
        style.showLabels = QStringLiteral("off");
        container.setFenceStyle(a, style);
        container.addWidget(a, ui::widgets::create(QStringLiteral("clock")));
        bool afterCell = false;
        const QRect w1 = cellOf(view, 1);
        const int colGap = view->insertionIndexAt(QPoint(w1.center().x(), w1.top() + 3), &afterCell);
        view->setExternalDropIndex(colGap, afterCell);
        const QLine colBar = view->shownInsertionBar();
        captureView(view, QStringLiteral("s80_insertion_bar_columns_names_hidden"));
        view->setExternalDropIndex(-1);
        // Tiles from the other fence: the same gap, the same place.
        ui::FenceView* other = container.view(b);
        const QRect o0 = cellOf(view, 0);
        const QPoint local(o0.center().x(), o0.bottom() - 4);
        const QPoint screen = container.physicalFromFenceLocal(a, local);
        container.dragTilesOutside(b, {files[4].path}, screen, false);
        const QLine external = view->shownInsertionBar();
        const int expected = view->insertionIndexAt(local);
        captureView(view, QStringLiteral("s80_insertion_bar_from_other_fence"));
        container.dragTilesOutside(b, {files[4].path}, screen, true);
        int landed = -1;
        for (int i = 0; i < view->items().size(); ++i)
            if (view->items().at(i).path == files[4].path)
                landed = i;
        check(QStringLiteral("insertion bar: columns from a corner, names hidden, widget, tiles from another fence land on it (session 80)"),
              !colBar.isNull() && colBar.y1() == colBar.y2() && !external.isNull() && landed == expected
                  && other->items().isEmpty(),
              QStringLiteral("column gap %1 bar %2,%3-%4,%5; external bar %6, landed %7 expected %8")
                  .arg(colGap).arg(colBar.x1()).arg(colBar.y1()).arg(colBar.x2()).arg(colBar.y2())
                  .arg(!external.isNull()).arg(landed).arg(expected));

        // The desktop's grid: origin from where the icons sit, slots in the
        // work area, a foreign drop snapped; drawn faint, its target bright.
        core::OverlapSlot slot;
        slot.spacing = QSize(95, 98);
        slot.iconSize = 48;
        slot.top = 5;
        const QList<QRect> areas{QRect(0, 0, 1000, 600)};
        const QList<QPair<QString, QPoint>> icons{{QStringLiteral("a"), QPoint(24, 2)}, {QStringLiteral("b"), QPoint(24, 100)},
                                                  {QStringLiteral("c"), QPoint(119, 2)}, {QStringLiteral("d"), QPoint(300, 333)}};
        const QPoint origin = core::OverlapPlanner::gridOrigin(icons, areas, slot);
        const QVector<QRect> grid = core::OverlapPlanner::gridSlots(icons, areas, slot);
        bool inside = !grid.isEmpty();
        for (const QRect& r : grid)
            inside = inside && areas.first().contains(r);
        const QPoint snapped = core::OverlapPlanner::snapToGrid(QPoint(130, 90), origin, slot);
        const QRect hit = slot.boxAt(snapped);
        QImage desk(1000, 600, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter painter(&desk);
            QLinearGradient sky(0, 0, 1000, 600);
            sky.setColorAt(0, QColor(0x1E, 0x3A, 0x5F));
            sky.setColorAt(1, QColor(0x6A, 0x4C, 0x93));
            painter.fillRect(desk.rect(), sky);
            ui::DesktopBackdrop::paintDropGrid(painter, QRect(0, 0, 1000, 600), grid, hit, 1.0);
        }
        desk.save(captures + QStringLiteral("/s80_desktop_grid.png"));
        const QColor inTarget = desk.pixelColor(hit.center());
        const bool targetLit = inTarget.blue() - inTarget.red() > 0x40;
        check(QStringLiteral("desktop grid: origin from the icons, slots in the work area, drop snapped, target lit (session 80)"),
              origin == QPoint(24, 2) && inside && grid.size() == 10 * 6 && snapped == QPoint(119, 100) && targetLit,
              QStringLiteral("origin %1,%2, %3 slots, snapped %4,%5, target colour %6")
                  .arg(origin.x()).arg(origin.y()).arg(grid.size()).arg(snapped.x()).arg(snapped.y()).arg(inTarget.name()));

        // Settings: the switch (on by default) and the widget-engine line.
        ui::SettingsDialog fresh;
        auto* gridSwitch = fresh.findChild<QCheckBox*>(QStringLiteral("desktopDragGridSwitch"));
        auto* engineLabel = fresh.findChild<QLabel*>(QStringLiteral("widgetEngineStatus"));
        const QString none = ui::widgets::engineSummary(0);
        const QString initial = engineLabel ? engineLabel->text() : QString();
        const QString withClock = ui::widgets::engineSummary(1);
        ui::widgets::registerEngine([] {
            ui::widgets::EngineStatus status;
            status.name = QStringLiteral("WebView2");
            status.running = true;
            status.memoryBytes = qint64(120) * 1024 * 1024;
            status.widgets = 2;
            return status;
        });
        const QString web = ui::widgets::engineSummary(1);
        ui::widgets::clearEngines();
        fresh.setWidgetEngineStatus(withClock);
        check(QStringLiteral("Settings: desktop grid switch (on by default), widget engine line, a future engine reports itself (session 80)"),
              gridSwitch && gridSwitch->isChecked() && engineLabel && initial == none
                  && fresh.widgetEngineStatus() == withClock && withClock.startsWith(none) && web.contains(QLatin1String("WebView2"))
                  && web.contains(QLatin1String("120")) && !web.contains(none),
              QStringLiteral("switch %4 on %5, label \"%6\", none \"%1\", with clock \"%2\", web \"%3\"")
                  .arg(none, withClock, web)
                  .arg(gridSwitch != nullptr)
                  .arg(gridSwitch && gridSwitch->isChecked())
                  .arg(engineLabel ? engineLabel->text() : QStringLiteral("-")));
    }

    // --- session 79: the clock widget -----------------------------------------
    {
        using L = ui::IconLayout;
        // Spans: a 2x1 block first, then tiles fill around it; every flow.
        const QVector<QSize> spans{QSize(2, 1), QSize(), QSize(), QSize()};
        const auto c = L::compute(QSize(400, 400), 48, 72, 34, 8, 6, 4, 0, spans);
        const bool block = c[0].icon.width() == 2 * 72 + 8 && c[0].label.isNull()
                           && c[1].icon.x() > c[0].icon.right() && c[1].icon.y() == c[0].icon.y();
        const auto r = L::compute(QSize(400, 400), 48, 72, 34, 8, 6, 4, L::FromRight, spans);
        const bool mirrored = r[0].icon.right() == 400 - 6 - 1 && r[1].icon.x() < r[0].icon.x();
        const auto plain = L::compute(QSize(400, 400), 48, 72, 34, 8, 6, 4, L::ColumnsFirst);
        const auto same = L::compute(QSize(400, 400), 48, 72, 34, 8, 6, 4, L::ColumnsFirst,
                                     QVector<QSize>{QSize(), QSize(), QSize(), QSize()});
        bool tilesSame = plain.size() == same.size();
        for (int i = 0; tilesSame && i < plain.size(); ++i)
            tilesSame = plain[i].icon == same[i].icon && plain[i].label == same[i].label;
        check(QStringLiteral("widget spans: a block of cells, tiles fill around it, every flow; tiles alone unchanged (session 79)"),
              block && mirrored && tilesSame,
              QStringLiteral("block %1, mirrored %2, tiles same %3").arg(block).arg(mirrored).arg(tilesSame));

        // The clock's text and its tick.
        core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
        const QDateTime at(QDate(2026, 11, 2), QTime(16, 41, 27, 300));
        const QString hm = ui::widgets::clockTime(clock, at);
        const int perMinute = ui::widgets::msUntilNextChange(clock, at);
        clock.widgetSettings.insert(QStringLiteral("seconds"), true);
        const QString hms = ui::widgets::clockTime(clock, at);
        const int perSecond = ui::widgets::msUntilNextChange(clock, at);
        check(QStringLiteral("clock: HH:mm (seconds optional), redrawn on the minute / second, aligned (session 79)"),
              clock.isWidget() && core::isWidgetPath(clock.path) && clock.span == QSize(2, 1) && hm == QLatin1String("16:41")
                  && hms == QLatin1String("16:41:27") && perMinute == 32700 + 15 && perSecond == 700 + 15,
              QStringLiteral("%1 / %2, next %3 / %4 ms").arg(hm, hms).arg(perMinute).arg(perSecond));
        clock.widgetSettings.insert(QStringLiteral("seconds"), false);

        // In a fence: saved, restored, never a file; unknown type kept.
        QTemporaryDir temp;
        const QString doc = QDir::fromNativeSeparators(temp.path()) + QStringLiteral("/s79.txt");
        {
            QFile file(doc);
            file.open(QIODevice::WriteOnly);
            file.write("starfish probe s79");
        }
        ui::FenceContainer container;
        const QString a = container.createFence(QStringLiteral("Clock"), QRect(0, 0, 380, 230));
        const QString b = container.createFence(QStringLiteral("Other"), QRect(500, 0, 300, 230));
        container.fileInto(a, {core::itemFromPathOnly(doc), core::itemFromPathOnly(QStringLiteral("C:/Windows/notepad.exe")),
                               core::itemFromPathOnly(QStringLiteral("C:/Windows/regedit.exe"))});
        const bool added = container.addWidget(a, clock);
        ui::FenceView* view = container.view(a);
        core::Layout layout = container.toLayout();
        // A widget a newer StarFish wrote.
        core::FenceItem future{QStringLiteral("widget"), QStringLiteral("widget:future-1")};
        future.widgetType = QStringLiteral("weather");
        future.span = QSize(2, 2);
        future.widgetSettings = QJsonObject{{QStringLiteral("city"), QStringLiteral("Paris")}};
        layout.fences[0].items.append(future);
        const QByteArray json = core::JsonStore::toJson(layout);
        const auto parsed = core::JsonStore::fromJson(json);
        ui::FenceContainer next;
        if (parsed)
            next.applyLayout(*parsed);
        bool clockBack = false, futureKept = false;
        if (!next.fenceIds().isEmpty()) {
            for (const core::DesktopItem& item : next.view(next.fenceIds().value(0))->items()) {
                clockBack = clockBack || (item.path == clock.path && item.widgetType == QLatin1String("clock")
                                          && item.span == QSize(2, 1) && item.widgetSettings.value(QStringLiteral("date")).toBool());
                futureKept = futureKept || (item.widgetType == QLatin1String("weather") && !item.missing);
            }
            const QByteArray again = core::JsonStore::toJson(next.toLayout());
            futureKept = futureKept && again.contains("\"weather\"") && again.contains("Paris");
        }
        const bool notAFile = !container.fencedFilePaths().contains(clock.path);
        container.reconcileMissingItems({});
        bool neverMissing = true;
        for (const core::DesktopItem& item : view->items())
            if (item.isWidget() && item.missing)
                neverMissing = false;
        check(QStringLiteral("clock in a fence: saved and restored, unknown widget kept as is, never a file nor missing (session 79)"),
              added && clockBack && futureKept && notAFile && neverMissing,
              QStringLiteral("added %1, back %2, future kept %3, not a file %4, never missing %5")
                  .arg(added).arg(clockBack).arg(futureKept).arg(notAFile).arg(neverMissing));

        // Captures: names shown, then hidden with a 2x2 clock with seconds.
        const auto capture = [&](ui::FenceView* v, const QString& name) {
            QImage img(v->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x2A, 0x4A, 0x6A));
            QPainter p(&img);
            v->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/%1_%2.png").arg(name, theme));
        };
        capture(view, QStringLiteral("s79_clock_fence"));
        {
            core::DesktopItem big = clock;
            big.span = QSize(2, 2);
            big.widgetSettings.insert(QStringLiteral("seconds"), true);
            container.replaceFenceItem(a, clock.path, big);
            core::FenceStyle style = container.fenceStyle(a);
            style.showLabels = QStringLiteral("off");
            container.setFenceStyle(a, style);
            const int interval = view->widgetTickInterval();
            capture(view, QStringLiteral("s79_clock_2x2_names_hidden"));
            check(QStringLiteral("clock with seconds repaints within a second; 2x2 with names hidden (session 79)"),
                  interval >= 0 && interval <= 1015 && !view->labelsShown(),
                  QStringLiteral("next tick in %1 ms").arg(interval));
            style.showLabels.clear();
            container.setFenceStyle(a, style);
            container.replaceFenceItem(a, clock.path, clock);
        }

        // Moved between fences; dropped on the desktop it stays; F2 / Delete.
        const auto has = [&](const QString& id) {
            for (const core::DesktopItem& item : container.view(id)->items())
                if (item.path == clock.path)
                    return true;
            return false;
        };
        container.dragTilesOutside(a, {clock.path}, QPoint(2500, 2500), true); // the desktop
        const bool stayed = has(a);
        container.dragTilesOutside(a, {clock.path}, QPoint(620, 100), true); // fence b
        const bool moved = !has(a) && has(b);
        ui::FenceView* other = container.view(b);
        int renames = 0, deletes = 0;
        QObject::connect(other, &ui::FenceView::renameRequested, other, [&](const QString&) { ++renames; });
        QObject::connect(other, &ui::FenceView::deleteRequested, other, [&](const QStringList&, bool) { ++deletes; });
        const QRect cell = other->labelRectOf(clock.path).isEmpty()
                               ? QRect(other->contentRect().topLeft() + QPoint(other->gridPadding() + 4, other->gridPadding() + 4), QSize(4, 4))
                               : other->labelRectOf(clock.path);
        const QPoint at2 = cell.center();
        {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(at2), other->mapToGlobal(QPointF(at2)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(other, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at2), other->mapToGlobal(QPointF(at2)), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(other, &release);
        }
        const bool selected = other->selectedPaths() == QStringList{clock.path};
        QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(other, &f2);
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(other, &del);
        const bool removed = !has(b);
        check(QStringLiteral("widget: moves between fences, stays when dropped on the desktop, no rename, Delete takes it out without the shell (session 79)"),
              stayed && moved && selected && renames == 0 && deletes == 0 && removed && QFile::exists(doc),
              QStringLiteral("stayed %1, moved %2, selected %3, renames %4, shell deletes %5, removed %6")
                  .arg(stayed).arg(moved).arg(selected).arg(renames).arg(deletes).arg(removed));

        ui::FenceMenu::State menuState;
        menuState.widgetKinds = {ui::widgets::displayName(QStringLiteral("clock"))};
        bool offered = false;
        for (const ui::FenceMenu::Entry& e : ui::FenceMenu::entries(menuState))
            offered = offered || e.action == ui::FenceMenu::Action::AddWidget;
        check(QStringLiteral("fence menu offers Add a widget > Clock (session 79)"),
              offered && ui::FenceMenu::widgetEntries(menuState).size() == 1, QString());
    }

    // --- session 81: the notes widget, written in place -----------------------
    {
        // A note is a widget whose content the user writes: the fence paints it,
        // a double-click asks the product for its editor, and the text belongs to
        // the document (so it comes back after a restart).
        ui::FenceContainer container;
        const QString a = container.createFence(QStringLiteral("Notes"), QRect(0, 0, 420, 300));
        const core::DesktopItem note = ui::widgets::create(QStringLiteral("notes"));
        const bool added = container.addWidget(a, note);
        ui::FenceView* view = container.view(a);
        const QRect local = view->itemRectOf(note.path);
        const QRect cell = container.fenceItemRect(a, note.path);
        const bool framed = !local.isEmpty() && !cell.isEmpty() && note.span == QSize(2, 2)
                            && local.width() > 2 * view->iconSize() && local.height() > view->iconSize();
        const auto capture = [&](ui::FenceView* v, const QString& name) {
            QImage img(v->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x2A, 0x4A, 0x6A));
            QPainter p(&img);
            v->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/%1_%2.png").arg(name, theme));
            return img;
        };
        const QImage hint = capture(view, QStringLiteral("s81_note_empty"));

        // The double-click that asks for the editor -- and the clock, which has
        // nothing to write, stays silent.
        QString editPath;
        QObject::connect(&container, &ui::FenceContainer::fenceWidgetEditRequested, view,
                         [&](const QString& id, const QString& path) {
                             if (id == a)
                                 editPath = path;
                         });
        const QPoint at = local.center();
        {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), view->mapToGlobal(QPointF(at)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), view->mapToGlobal(QPointF(at)),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view, &release);
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(at), view->mapToGlobal(QPointF(at)), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &dbl);
        }
        const bool asked = editPath == note.path;
        const core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
        container.addWidget(a, clock);
        editPath.clear();
        const QRect clockCell = view->itemRectOf(clock.path);
        if (!clockCell.isEmpty()) {
            const QPoint clockAt = clockCell.center();
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(clockAt), view->mapToGlobal(QPointF(clockAt)),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &dbl);
        }
        const bool clockSilent = editPath.isEmpty();
        container.releaseItems(a, {clock.path});

        // What is written, drawn, and kept: the text in the document, the card
        // with the text in it (not the hint), and the same text after a reload.
        const QString text = QStringLiteral("Sortir les poubelles\nAppeler Marc a 9 h");
        container.replaceFenceItem(a, note.path, ui::widgets::withNoteText(note, text));
        const QImage written = capture(view, QStringLiteral("s81_note_written"));
        const bool drawn = hint != written;
        const QByteArray json = core::JsonStore::toJson(container.toLayout());
        const auto parsed = core::JsonStore::fromJson(json);
        ui::FenceContainer next;
        if (parsed)
            next.applyLayout(*parsed);
        bool textBack = false;
        if (!next.fenceIds().isEmpty()) {
            for (const core::DesktopItem& item : next.view(next.fenceIds().value(0))->items())
                textBack = textBack || (item.widgetType == QLatin1String("notes") && ui::widgets::noteText(item) == text);
        }
        const bool inJson = json.contains("Sortir les poubelles") && json.contains("\\n");
        check(QStringLiteral("notes: a 2x2 cell, a double-click asks for the editor (the clock does not), the text "
                             "is drawn and survives a reload (session 81)"),
              added && framed && asked && clockSilent && drawn && textBack && inJson,
              QStringLiteral("added %1, cell %2x%3, asked %4, clock silent %5, drawn %6, back %7, in json %8")
                  .arg(added).arg(local.width()).arg(local.height()).arg(asked).arg(clockSilent).arg(drawn)
                  .arg(textBack).arg(inJson));
    }

    // --- session 82: the rectangle's preview, the icon pool, the ready-made entries ---
    {
        // The user's ask: a drawn rectangle stays on screen while its menu is open,
        // so the fence he is about to build is visible. Measured on the gesture and
        // on the backdrop's own band -- no desktop is hosted, nothing is shown.
        ui::BackdropGesture gesture;
        ui::BackdropGesture::Metrics metrics;
        metrics.minimumFence = QSize(80, 60);
        gesture.setMetrics(metrics);
        gesture.press(QPoint(100, 100), ui::BackdropGesture::Button::Right, false);
        gesture.move(QPoint(320, 280));
        const ui::BackdropGesture::Result drawn =
            gesture.release(QPoint(320, 280), ui::BackdropGesture::Button::Right);
        const QRect band = drawn.rect.normalized();
        ui::DesktopBackdrop preview; // never attached: his desktop is not touched
        preview.showFencePreview(band);
        const bool kept = !band.isEmpty() && preview.rubberBand() == band;
        preview.clearFencePreview();
        const bool cleared = preview.rubberBand().isNull();
        check(QStringLiteral("a drawn rectangle keeps its preview while its menu is open, and it goes away "
                             "when the menu returns (session 82)"),
              drawn.action == ui::BackdropGesture::Action::DesktopMenu && kept && cleared,
              QStringLiteral("action %1, band %2,%3 %4x%5 kept %6, cleared %7")
                  .arg(int(drawn.action)).arg(band.x()).arg(band.y()).arg(band.width()).arg(band.height())
                  .arg(kept).arg(cleared));

        // The icon pool is the menu's own font: every code point must exist there,
        // or the menu would show an empty box instead of an icon.
        const QStringList families = QFontDatabase::families();
        const QString iconFont = families.contains(QStringLiteral("Segoe Fluent Icons"))
                                     ? QStringLiteral("Segoe Fluent Icons")
                                     : QStringLiteral("Segoe MDL2 Assets");
        QFont iconFace(iconFont);
        iconFace.setPixelSize(16);
        const QFontMetrics iconMetrics(iconFace);
        QStringList missing;
        for (const core::MenuGlyph& glyph : core::menuGlyphPool()) {
            if (!iconMetrics.inFontUcs4(glyph.glyph.at(0).unicode()))
                missing << glyph.name;
        }
        check(QStringLiteral("every icon of the pool exists in the installed icon font (session 82)"),
              families.contains(iconFont) && missing.isEmpty() && core::menuGlyphPool().size() >= 24,
              QStringLiteral("%1, %2 icons, missing [%3]")
                  .arg(iconFont).arg(core::menuGlyphPool().size()).arg(missing.join(QStringLiteral(", "))));

        // The pool itself, drawn: what the picker offers, to be looked at.
        {
            const int cell = 34;
            const int cols = 8;
            const int rows = (core::menuGlyphPool().size() + cols - 1) / cols;
            QImage sheet(cols * cell, rows * cell, QImage::Format_ARGB32_Premultiplied);
            sheet.fill(QColor(0x1F, 0x29, 0x37));
            QPainter painter(&sheet);
            painter.setRenderHint(QPainter::TextAntialiasing, true);
            QFont face(iconFont);
            face.setPixelSize(20);
            painter.setFont(face);
            painter.setPen(QColor(0xE5, 0xE7, 0xEB));
            int index = 0;
            for (const core::MenuGlyph& glyph : core::menuGlyphPool()) {
                painter.drawText(QRect((index % cols) * cell, (index / cols) * cell, cell, cell), Qt::AlignCenter,
                                 glyph.glyph);
                ++index;
            }
            painter.end();
            sheet.save(captures + QStringLiteral("/s82_glyph_pool_dark.png"));
        }

        // The ready-made entries: the editor's own button, and the light/dark
        // switch added the way that button adds it -- listed, saved, reloaded.
        QTemporaryDir temp;
        const QString config = QDir::fromNativeSeparators(temp.path()) + QStringLiteral("/context-menu.json");
        ui::ContextMenuEditor editor;
        editor.setConfigPath(config);
        auto* presetsButton = editor.findChild<QPushButton*>(QStringLiteral("presets"));
        QString switchLabel;
        for (const core::MenuPreset& preset : core::menuPresets()) {
            if (preset.action != QLatin1String("theme"))
                continue;
            switchLabel = preset.label;
            core::ContextMenuCustomEntry entry;
            entry.label = preset.label;
            entry.program = preset.program;
            entry.arguments = preset.arguments;
            entry.glyph = core::glyphForName(preset.glyph);
            entry.action = preset.action;
            editor.addCustomEntry(entry);
        }
        const QString key = QStringLiteral("custom:") + switchLabel;
        const bool listed = !switchLabel.isEmpty() && editor.listedKeys().contains(key);
        auto* list = editor.findChild<QListWidget*>(QStringLiteral("entries"));
        bool withIcon = false;
        for (int i = 0; list && i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == key)
                withIcon = !list->item(i)->icon().isNull();
        }
        ui::ContextMenuEditor reloaded;
        reloaded.setConfigPath(config);
        const bool saved = reloaded.listedKeys().contains(key);
        check(QStringLiteral("the ready-made entries are one click away: the light/dark switch is added, "
                             "wears a pool icon and survives a reload (session 82)"),
              presetsButton != nullptr && listed && withIcon && saved,
              QStringLiteral("button %1, listed %2, icon %3, reloaded %4 [%5]")
                  .arg(presetsButton != nullptr).arg(listed).arg(withIcon).arg(saved).arg(switchLabel));

        // Session 95: the whole ready-made list in the desktop tab, unticked;
        // ticking one adds it as the user's entry; Task Manager is one of them.
        {
            QTemporaryDir fresh;
            const QString freshConfig = QDir::fromNativeSeparators(fresh.path()) + QStringLiteral("/context-menu.json");
            ui::ContextMenuEditor all;
            all.setConfigPath(freshConfig);
            all.selectKind(core::ContextMenuKind::Background);
            bool everyOne = true, noneTicked = true, taskManager = false;
            QString tmKey;
            for (const core::MenuPreset& preset : core::menuPresets()) {
                const QString k = QStringLiteral("custom:") + preset.label;
                everyOne = everyOne && all.listedKeys().contains(k);
                noneTicked = noneTicked && !all.isChecked(k);
                if (preset.program == QLatin1String("taskmgr.exe")) {
                    taskManager = true;
                    tmKey = k;
                }
            }
            all.setChecked(tmKey, true);
            QCoreApplication::processEvents();
            ui::ContextMenuEditor again;
            again.setConfigPath(freshConfig);
            again.selectKind(core::ContextMenuKind::Background);
            const core::ContextMenuConfig cfg = core::ContextMenuConfig::load(freshConfig);
            const bool added = again.isChecked(tmKey) && cfg.background.custom.size() == 1
                               && cfg.background.custom.first().program == QLatin1String("taskmgr.exe");
            check(QStringLiteral("the desktop menu editor lists every ready-made entry, unticked; ticking adds it; Task Manager included (session 95)"),
                  everyOne && noneTicked && taskManager && added,
                  QStringLiteral("all %1 unticked %2 taskmgr %3 added %4").arg(everyOne).arg(noneTicked).arg(taskManager).arg(added));
        }

        // Reading the theme is harmless and tells the switch which way to go; the
        // write is the user's click, so no probe flips his colours.
        const platform::win::ThemeState theme = platform::win::readTheme();
        check(QStringLiteral("the light/dark switch knows where it starts and flips it (session 82)"),
              theme.valid && platform::win::flippedLight(theme.appsLight) != theme.appsLight,
              QStringLiteral("read %1, apps light %2, system light %3")
                  .arg(theme.valid).arg(theme.appsLight).arg(theme.systemLight));
    }

    // --- session 76: launch through the shell, names hidden, title lock, "+N", menu ---
    {
        QTemporaryDir temp;
        const QString dir = QDir::fromNativeSeparators(temp.path());
        // A shortcut with arguments to a script that writes them down: nothing
        // shows on screen (wscript, no window), only a file in the temp folder.
        const QString script = dir + QStringLiteral("/args s76.vbs");
        const QString out = dir + QStringLiteral("/args s76.txt");
        {
            QFile file(script);
            file.open(QIODevice::WriteOnly);
            file.write("Set f = CreateObject(\"Scripting.FileSystemObject\").CreateTextFile(WScript.Arguments(0), True)\r\n"
                       "f.Write WScript.Arguments(1)\r\nf.Close\r\n");
        }
        const QString link = dir + QStringLiteral("/web app s76.lnk");
        bool made = false;
        {
            IShellLinkW* shellLink = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                           reinterpret_cast<void**>(&shellLink)))) {
                const QString wscript = QStandardPaths::findExecutable(QStringLiteral("wscript.exe"));
                const QString args = QStringLiteral("\"%1\" \"%2\" --app-id=s76probe")
                                         .arg(QDir::toNativeSeparators(script), QDir::toNativeSeparators(out));
                shellLink->SetPath(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(wscript).utf16()));
                shellLink->SetArguments(reinterpret_cast<const wchar_t*>(args.utf16()));
                IPersistFile* persist = nullptr;
                if (SUCCEEDED(shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist)))) {
                    made = SUCCEEDED(persist->Save(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(link).utf16()), TRUE));
                    persist->Release();
                }
                shellLink->Release();
            }
        }
        ui::FenceContainer container;
        container.setItemResolver([](const QString& path) { return platform::win::DesktopScanner::itemForPath(path); });
        const QString id = container.createFence(QStringLiteral("Web apps"), QRect(0, 0, 300, 200));
        container.fileInto(id, {platform::win::DesktopScanner::itemForPath(link)});
        ui::FenceView* view = container.view(id);
        const QPoint tile = view->contentRect().topLeft() + QPoint(view->gridPadding() + view->iconSize() / 2,
                                                                     view->gridPadding() + view->iconSize() / 2);
        auto dbl = [view](const QPoint& at) {
            QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(at), view->mapToGlobal(QPointF(at)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &e);
        };
        dbl(tile);
        QString written;
        for (int i = 0; i < 60 && written.isEmpty(); ++i) {
            wait(100);
            QFile file(out);
            if (file.open(QIODevice::ReadOnly))
                written = QString::fromUtf8(file.readAll()).trimmed();
        }
        check(QStringLiteral("double-click opens the shortcut itself: its arguments reach the program (session 76)"),
              made && written == QLatin1String("--app-id=s76probe"),
              QStringLiteral("made %1, written \"%2\"").arg(made).arg(written));

        // Names hidden: cells shrink to the icon; per fence beats the default.
        const int withNames = view->labelHeight();
        container.setShowLabelsDefault(false);
        const bool hiddenByDefault = !view->labelsShown() && view->labelHeight() == 0 && withNames > 0;
        core::FenceStyle style = container.fenceStyle(id);
        style.showLabels = QStringLiteral("on");
        container.setFenceStyle(id, style);
        const bool fenceWins = view->labelsShown();
        ui::FenceContainer next;
        next.applyLayout(container.toLayout());
        const bool kept = next.fenceStyle(next.fenceIds().value(0)).showLabels == QLatin1String("on");
        style.showLabels = QStringLiteral("off");
        container.setFenceStyle(id, style);
        container.setShowLabelsDefault(true);
        check(QStringLiteral("icon names: hidden by default or per fence, the cell shrinks, saved (session 76)"),
              hiddenByDefault && fenceWins && kept && !view->labelsShown() && view->cellWidth() == view->iconSize() + 12,
              QStringLiteral("default hides %1, fence wins %2, kept %3, cell %4").arg(hiddenByDefault).arg(fenceWins).arg(kept)
                  .arg(view->cellWidth()));

        // The title's double-click does nothing while Appearance is open.
        int edits = 0, rolls = 0;
        QObject::connect(view, &ui::FenceView::titleEditRequested, view, [&] { ++edits; });
        QObject::connect(view, &ui::FenceView::rollUpRequested, view, [&](bool) { ++rolls; });
        container.setFenceTitleLocked(id, true);
        dbl(view->titleTextRect().center());
        dbl(QPoint(view->width() - 20, view->metrics().titleBarHeight / 2));
        const bool lockedQuiet = edits == 0 && rolls == 0;
        container.setFenceTitleLocked(id, false);
        dbl(view->titleTextRect().center());
        check(QStringLiteral("title double-click is off while Appearance is open, back after (session 76)"),
              lockedQuiet && edits == 1, QStringLiteral("locked quiet %1, edits after %2").arg(lockedQuiet).arg(edits));

        // "+N": more icons than room.
        QList<core::DesktopItem> many;
        for (const char* exe : {"notepad.exe", "regedit.exe", "explorer.exe", "write.exe", "hh.exe", "splwow64.exe",
                                "winhlp32.exe", "bfsvc.exe", "HelpPane.exe", "twain_32.dll"})
            many << core::itemFromPathOnly(QStringLiteral("C:/Windows/") + QLatin1String(exe));
        ui::FenceView smallView;
        smallView.setItems(many);
        smallView.resize(200, 120);
        const int hidden = smallView.hiddenItemCount();
        const QRect badge = smallView.hiddenBadgeRect();
        smallView.resize(900, 600);
        const int allShown = smallView.hiddenItemCount();
        smallView.resize(200, 120);
        {
            QImage img(smallView.size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            smallView.render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/s76_hidden_badge_%1.png").arg(theme));
        }
        check(QStringLiteral("a fence too small shows \"+N\" in its corner, none when all fit (session 76)"),
              hidden > 0 && hidden < many.size() && allShown == 0 && smallView.contentRect().contains(badge),
              QStringLiteral("hidden %1 of %2, all shown %3").arg(hidden).arg(many.size()).arg(allShown));
        // Session 86: no "+N" while the fence rolls, nor once it is rolled up.
        smallView.setRolling(true);
        const int whileRolling = smallView.hiddenItemCount();
        smallView.setRolling(false);
        smallView.setRolledUp(true);
        const int rolled = smallView.hiddenItemCount();
        smallView.setRolledUp(false);
        check(QStringLiteral("no \"+N\" on a fence that rolls or is rolled up (session 86)"),
              whileRolling == 0 && rolled == 0 && smallView.hiddenItemCount() == hidden,
              QStringLiteral("rolling %1, rolled %2, back %3").arg(whileRolling).arg(rolled).arg(smallView.hiddenItemCount()));
        {
            QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/s76_names_hidden_%1.png").arg(theme));
        }

        // Icon order: corner and direction, per fence over the default.
        {
            using L = ui::IconLayout;
            const QSize vp(400, 400);
            const auto at = [&](int flow) { return L::compute(vp, 48, 72, 34, 8, 6, 3, flow); };
            const auto c0 = at(0), c4 = at(L::ColumnsFirst), c1 = at(L::FromRight), c3 = at(L::FromRight | L::FromBottom);
            const bool rows = c0[1].icon.x() > c0[0].icon.x() && c0[1].icon.y() == c0[0].icon.y();
            const bool cols = c4[1].icon.y() > c4[0].icon.y() && c4[1].icon.x() == c4[0].icon.x();
            const bool right = c1[0].icon.x() > 300 && c1[1].icon.x() < c1[0].icon.x();
            const bool bottom = c3[0].icon.y() > 300 && c3[0].icon.x() > 300;
            core::FenceStyle flowStyle = container.fenceStyle(id);
            container.setIconFlowDefault(L::ColumnsFirst);
            const bool byDefault = view->iconFlow() == L::ColumnsFirst;
            flowStyle.iconFlow = QStringLiteral("br-cols");
            container.setFenceStyle(id, flowStyle);
            const bool fenceWins = view->iconFlow() == 7;
            ui::FenceContainer again;
            again.applyLayout(container.toLayout());
            const bool flowKept = again.fenceStyle(again.fenceIds().value(0)).iconFlow == QLatin1String("br-cols");
            container.setIconFlowDefault(0);
            ui::FenceView flowView;
            flowView.setItems(many.mid(0, 5));
            flowView.setIconFlow(L::FromRight | L::FromBottom | L::ColumnsFirst);
            flowView.resize(320, 300);
            QImage img(flowView.size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            flowView.render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/s76_flow_bottom_right_columns_%1.png").arg(theme));
            check(QStringLiteral("icon order: rows or columns first, from any corner, default and per fence, saved (session 76)"),
                  rows && cols && right && bottom && byDefault && fenceWins && flowKept,
                  QStringLiteral("rows %1, cols %2, right %3, bottom %4, default %5, fence wins %6, kept %7")
                      .arg(rows).arg(cols).arg(right).arg(bottom).arg(byDefault).arg(fenceWins).arg(flowKept));
        }

        // The splash (session 77): full monitor, never focused, closes once the
        // fences painted and the surfaces settled; rendered here, never shown.
        {
            const QSize size(1600, 900);
            QImage scene(size, QImage::Format_RGB32);
            QPainter p(&scene);
            ui::SplashScreen::paintScene(p, QRect(QPoint(0, 0), size), 0.4, ui::SplashScreen::blurredWallpaper(size),
                                         QStringLiteral("Setting out your fences..."));
            p.end();
            scene.save(captures + QStringLiteral("/s77_splash.png"));
            QPointer<ui::SplashScreen> splash = new ui::SplashScreen;
            QWidget fence; // never on screen: its paint is sent here
            fence.setAttribute(Qt::WA_DontShowOnScreen);
            fence.resize(120, 80);
            fence.setUpdatesEnabled(false); // no paint of its own: the one sent below
            fence.show();
            splash->finishWhenPainted({&fence});
            QElapsedTimer clock;
            clock.start();
            wait(ui::SplashScreen::kMinimumMs + 100);
            const bool waitsForPaint = !splash.isNull() && !splash->finishing();
            {
                QPaintEvent paint(fence.rect());
                QApplication::sendEvent(&fence, &paint);
            }
            for (int i = 0; i < 40 && !splash.isNull(); ++i)
                wait(50);
            const bool gone = splash.isNull();
            const qint64 took = clock.elapsed();
            check(QStringLiteral("splash: waits for the fences' paint, then goes well before its 6 s net (session 77)"),
                  waitsForPaint && gone && took < ui::SplashScreen::kMaximumMs,
                  QStringLiteral("waits %1, gone %2, %3 ms").arg(waitsForPaint).arg(gone).arg(took));
        }

        // The fence's menu: resize mode and templates, one click away.
        ui::FenceMenu::State state;
        state.snapToCells = QStringLiteral("off");
        const auto has = [](const QList<ui::FenceMenu::Entry>& list, ui::FenceMenu::Action a) {
            return std::any_of(list.begin(), list.end(), [a](const ui::FenceMenu::Entry& e) { return e.action == a; });
        };
        const bool noTemplates = has(ui::FenceMenu::entries(state), ui::FenceMenu::Action::Resize)
                                 && !has(ui::FenceMenu::entries(state), ui::FenceMenu::Action::Template);
        state.templates = {QStringLiteral("Glass"), QStringLiteral("Dark")};
        const QList<ui::FenceMenu::Entry> resize = ui::FenceMenu::resizeEntries(state);
        const bool freeChecked = resize.size() == 3 && resize.at(2).checked && !resize.at(0).checked;
        const bool templates = has(ui::FenceMenu::entries(state), ui::FenceMenu::Action::Template)
                               && ui::FenceMenu::templateEntries(state).size() == 2;
        check(QStringLiteral("fence menu: Resize (whole icons / free) and Apply a template (session 76)"),
              noTemplates && freeChecked && templates,
              QStringLiteral("no templates %1, free checked %2, templates %3").arg(noTemplates).arg(freeChecked).arg(templates));
    }

    // --- session 75: shortcuts kept by StarFish; a target-less shortcut is no "missing" --
    {
        QTemporaryDir temp;
        const QString dir = QDir::fromNativeSeparators(temp.path());
        const auto resolve = [](const QString& path) { return platform::win::DesktopScanner::itemForPath(path); };
        const QString doc = dir + QStringLiteral("/target s75.txt");
        {
            QFile file(doc);
            file.open(QIODevice::WriteOnly);
            file.write("starfish probe s75");
        }
        const QString link = dir + QStringLiteral("/kept s75.lnk");
        const QString orphan = dir + QStringLiteral("/no target s75.lnk");
        const bool made = QFile::link(doc, link) && QFile::link(QStringLiteral("C:/starfish-s75-nowhere/app.exe"), orphan);
        core::Layout layout;
        {
            ui::FenceContainer first;
            first.setItemResolver(resolve);
            const QString fid = first.createFence(QStringLiteral("Harnesses"), QRect(0, 0, 360, 200));
            first.fileInto(fid, {resolve(link), resolve(orphan), resolve(doc)});
            layout = first.toLayout();
        }
        ui::FenceContainer container;
        container.setItemResolver(resolve);
        container.applyLayout(layout);
        const QString id = container.fenceIds().value(0);
        ui::FenceView* view = container.view(id);
        const auto stateOf = [view](const QString& path) {
            for (const core::DesktopItem& item : view->items())
                if (item.path.compare(path, Qt::CaseInsensitive) == 0)
                    return (item.missing ? 1 : 0) + (item.restorable ? 2 : 0);
            return -1;
        };
        container.reconcileMissingItems({});
        // StoreApp: a shortcut with no file target (a Store app) is there, not missing.
        const bool orphanKept = stateOf(orphan) == 0 && core::entryExists(orphan) && !QFileInfo::exists(orphan)
                                && !resolve(orphan).fileId.isEmpty();
        check(QStringLiteral("a shortcut whose target is no file (Store app, e.g. StoreApp) is not shown as missing (session 75)"),
              made && orphanKept,
              QStringLiteral("made %1, state %2, entryExists %3, QFileInfo::exists %4")
                  .arg(made).arg(stateOf(orphan)).arg(core::entryExists(orphan)).arg(QFileInfo::exists(orphan)));

        core::ShortcutVault vault(dir + QStringLiteral("/vault"));
        const int copied = vault.sync(container.fencedFilePaths());
        const bool onlyShortcuts = vault.has(link) && vault.has(orphan) && !vault.has(doc);
        // Deleted from the "normal" desktop while StarFish was away.
        QFile::remove(link);
        container.reconcileMissingItems({});
        vault.sync(container.fencedFilePaths());
        container.markRestorable([&](const QString& path) { return vault.has(path); });
        const bool restorable = stateOf(link) == 3;
        {
            QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/s75_restorable_tile_%1.png").arg(theme));
        }
        const bool restored = vault.restore(link) && core::entryExists(link);
        container.reconcileMissingItems({});
        container.markRestorable([&](const QString& path) { return vault.has(path); });
        const bool normal = stateOf(link) == 0;
        const bool neverOver = !vault.restore(link); // something is there: never overwritten
        const bool emptyKeeps = vault.sync({}) == 0 && vault.has(link);
        container.releaseItems(id, {link});
        vault.sync(container.fencedFilePaths());
        const bool dropped = !vault.has(link) && !core::entryExists(vault.copyOf(link)) && vault.has(orphan);
        check(QStringLiteral("fenced shortcuts are copied, a deleted one shows restorable and comes back; only shortcuts; copy dropped with its tile (session 75)"),
              copied == 2 && onlyShortcuts && restorable && restored && normal && neverOver && emptyKeeps && dropped,
              QStringLiteral("copied %1, only shortcuts %2, restorable %3, restored %4, normal %5, never over %6, empty keeps %7, dropped %8")
                  .arg(copied).arg(onlyShortcuts).arg(restorable).arg(restored).arg(normal).arg(neverOver).arg(emptyKeeps).arg(dropped));
    }

    // --- session 74: a file renamed outside StarFish; rename inside a fence ---
    {
        // Dummy files in a temp folder only.
        QTemporaryDir temp;
        const QString dir = QDir::fromNativeSeparators(temp.path());
        const auto make = [&](const QString& name) {
            QFile file(dir + QLatin1Char('/') + name);
            file.open(QIODevice::WriteOnly);
            file.write("starfish probe s74");
            return dir + QLatin1Char('/') + name;
        };
        const auto resolve = [](const QString& path) { return platform::win::DesktopScanner::itemForPath(path); };
        const QString a = make(QStringLiteral("a s74.txt"));
        const QString b = make(QStringLiteral("b s74.txt"));
        const QString c = make(QStringLiteral("c s74.txt"));
        core::Layout layout;
        bool idsSaved = false;
        {
            ui::FenceContainer first;
            first.setItemResolver(resolve);
            const QString id = first.createFence(QStringLiteral("Games"), QRect(0, 0, 360, 200));
            first.fileInto(id, {resolve(a), resolve(b), resolve(c)});
            const QByteArray json = core::JsonStore::toJson(first.toLayout());
            layout = core::JsonStore::fromJson(json).value_or(core::Layout{});
            const QList<core::FenceItem> stored = layout.fences.value(0).items;
            idsSaved = stored.size() == 3 && std::all_of(stored.begin(), stored.end(),
                                                         [](const core::FenceItem& i) { return !i.fileId.isEmpty(); });
        }
        // StarFish closed: b is renamed, c deleted, on the "normal" desktop.
        const QString bRenamed = dir + QStringLiteral("/b renamed s74.txt");
        const bool renamedOutside = QFile::rename(b, bRenamed);
        QFile::remove(c);
        ui::FenceContainer next;
        next.setItemResolver(resolve);
        next.applyLayout(layout);
        const QString id = next.fenceIds().value(0);
        ui::FenceView* view = next.view(id);
        const auto missingOf = [view](const QString& path) {
            for (const core::DesktopItem& item : view->items())
                if (item.path.compare(path, Qt::CaseInsensitive) == 0)
                    return item.missing ? 1 : 0;
            return -1;
        };
        const bool flagged = missingOf(b) == 1 && missingOf(c) == 1 && missingOf(a) == 0;
        // The desktop as the shell lists it after the restart.
        const int changed = next.reconcileMissingItems({resolve(a), resolve(bRenamed)});
        const QList<core::DesktopItem> after = view->items();
        const bool followed = changed == 1 && after.size() == 3 && missingOf(bRenamed) == 0 && missingOf(b) == -1
                              && missingOf(c) == 1 && after.value(1).path == bRenamed;
        {
            QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x9A, 0xC8, 0x2A));
            QPainter p(&img);
            view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
            p.end();
            img.save(captures + QStringLiteral("/s74_missing_tile_%1.png").arg(theme));
        }
        const bool keptInDocument = next.toLayout().fences.value(0).items.size() == 3;
        // c comes back: no longer missing.
        make(QStringLiteral("c s74.txt"));
        next.reconcileMissingItems({resolve(a), resolve(bRenamed), resolve(c)});
        const bool back = missingOf(c) == 0;
        // A live rename (StarFish running) follows the same way.
        const QString aRenamed = dir + QStringLiteral("/a live s74.txt");
        QFile::rename(a, aRenamed);
        next.reconcileMissingItems({resolve(aRenamed), resolve(bRenamed), resolve(c)});
        const bool live = missingOf(aRenamed) == 0 && view->items().size() == 3;
        // Nothing free matches: never another file's tile.
        const QString stranger = make(QStringLiteral("z s74.txt"));
        QFile::remove(c);
        next.reconcileMissingItems({resolve(aRenamed), resolve(bRenamed), resolve(stranger)});
        const bool noGuess = missingOf(c) == 1 && missingOf(stranger) == -1;
        check(QStringLiteral("a file renamed outside StarFish: the tile follows it (file id), a lost one shows as missing, kept (session 74)"),
              renamedOutside && idsSaved && flagged && followed && keptInDocument && back && live && noGuess,
              QStringLiteral("ids %1, flagged %2, changed %3, followed %4, kept %5, back %6, live %7, no guess %8")
                  .arg(idsSaved).arg(flagged).arg(changed).arg(followed).arg(keptInDocument).arg(back).arg(live).arg(noGuess));

        // Rename inside a fence: F2 asks, the shell renames, the tile follows in place.
        QString askedId, askedPath;
        QObject::connect(&next, &ui::FenceContainer::fenceRenameRequested, &next,
                         [&](const QString& fid, const QString& path) { askedId = fid; askedPath = path; });
        const QVector<QRect> tiles = view->tileRects();
        if (tiles.size() == 3) {
            const QPoint at = tiles.at(1).center();
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), QPointF(view->mapToGlobal(at)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), QPointF(view->mapToGlobal(at)), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &release);
        }
        QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &f2);
        const QRect label = next.fenceItemLabelRect(id, askedPath);
        const bool asked = askedId == id && askedPath == bRenamed && !label.isEmpty()
                           && next.fenceVisibleRect(id).contains(label);
        QString newPath;
        // Session 92: never in the user's Explorer undo history (t94u).
        const bool isolated = platform::win::shellOperationIsolation();
        const bool renamed = isolated
                             && platform::win::renameShellItem(bRenamed, QStringLiteral("b by F2 s74.txt"), 0, &newPath);
        const QString cleanNew = QDir::fromNativeSeparators(newPath);
        const bool replaced = next.replaceFenceItem(id, bRenamed, resolve(cleanNew));
        const bool inPlace = view->items().size() == 3 && view->items().value(1).path == cleanNew
                             && QFileInfo::exists(cleanNew) && !QFileInfo::exists(bRenamed);
        check(QStringLiteral("rename inside a fence: F2 asks with the caption's place, the shell renames, the tile follows in place (session 74)"),
              asked && renamed && replaced && inPlace,
              QStringLiteral("asked %1 (%2), label %3,%4 %5x%6, renamed %7 -> %8, in place %9")
                  .arg(asked).arg(askedPath).arg(label.x()).arg(label.y()).arg(label.width()).arg(label.height())
                  .arg(renamed).arg(cleanNew).arg(inPlace));
    }

    // --- session 73: Delete in a fence; roll edge; roll animation; whole icons --
    {
        // Delete: a dummy file in a temp folder only, never a real one.
        QTemporaryDir temp;
        const QString dummy = QDir::fromNativeSeparators(temp.path()) + QStringLiteral("/s73 dummy.txt");
        {
            QFile file(dummy);
            file.open(QIODevice::WriteOnly);
            file.write("starfish probe s73");
        }
        const QString thisPc = QStringLiteral("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Suppr"), QRect(0, 0, 300, 200));
        container.fileInto(id, {core::itemFromPathOnly(dummy), core::itemFromPathOnly(thisPc)});
        ui::FenceView* view = container.view(id);
        QStringList asked;
        bool permanent = false;
        int requests = 0;
        QObject::connect(&container, &ui::FenceContainer::fenceDeleteRequested, &container,
                         [&](const QString&, const QStringList& paths, bool shift) {
                             asked = paths;
                             permanent = shift;
                             ++requests;
                         });
        const QVector<QRect> tiles = view->tileRects();
        const auto click = [view](const QPoint& at, Qt::KeyboardModifiers mods) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), QPointF(view->mapToGlobal(at)), Qt::LeftButton,
                              Qt::LeftButton, mods);
            QCoreApplication::sendEvent(view, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), QPointF(view->mapToGlobal(at)), Qt::LeftButton,
                                Qt::NoButton, mods);
            QCoreApplication::sendEvent(view, &release);
        };
        const auto key = [view](Qt::KeyboardModifiers mods) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Delete, mods);
            QCoreApplication::sendEvent(view, &press);
        };
        if (tiles.size() == 2) {
            click(tiles.at(0).center(), Qt::NoModifier);
            click(tiles.at(1).center(), Qt::ControlModifier);
        }
        const QStringList selected = view->selectedPaths();
        key(Qt::ShiftModifier);
        const bool shiftAsked = requests == 1 && permanent && asked == QStringList{dummy};
        key(Qt::NoModifier);
        const bool plainAsked = requests == 2 && !permanent && asked == QStringList{dummy};
        const int beforeForget = container.forgetMissingItems(id, asked); // file still there: nothing goes
        const bool deleted = platform::win::deleteShellItems(asked, platform::win::DeleteMode::PermanentSilent, 0);
        const int removed = container.forgetMissingItems(id, asked);
        const QList<core::DesktopItem> left = view->items();
        check(QStringLiteral("Delete on a fence's selection: the file only (never This PC), Shift = permanent; the tile leaves once the file is gone (session 73)"),
              selected.size() == 2 && shiftAsked && plainAsked && beforeForget == 0 && deleted && removed == 1
                  && left.size() == 1 && left.value(0).path == thisPc && !QFileInfo::exists(dummy),
              QStringLiteral("selected %1, requests %2, before %3, deleted %4, removed %5, left %6")
                  .arg(selected.size()).arg(requests).arg(beforeForget).arg(deleted).arg(removed).arg(left.size()));
    }
    {
        // Which edge a rolled-up fence keeps: its title bar's, or the opposite.
        struct Case {
            QString side;
            QString toward;
            int edge; // 0 top, 1 bottom, 2 left, 3 right
        };
        const QList<Case> cases{
            {QString(), QString(), 0},           {QString(), QStringLiteral("opposite"), 1},
            {QStringLiteral("bottom"), QString(), 1}, {QStringLiteral("bottom"), QStringLiteral("opposite"), 0},
            {QStringLiteral("left"), QString(), 2},   {QStringLiteral("left"), QStringLiteral("opposite"), 3},
            {QStringLiteral("right"), QStringLiteral("bar"), 3}, {QStringLiteral("right"), QStringLiteral("opposite"), 2},
        };
        QString detail;
        bool ok = true;
        const QRect stored(200, 150, 320, 260);
        for (const Case& c : cases) {
            for (bool tab : {false, true}) {
                ui::FenceContainer container;
                const QString id = container.createFence(QStringLiteral("Roll"), stored);
                container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/Windows/notepad.exe"))});
                core::FenceStyle style = container.fenceStyle(id);
                style.titleSide = c.side;
                style.rollToward = c.toward;
                if (tab)
                    style.titleBarShape = QStringLiteral("tab");
                container.setFenceStyle(id, style);
                const QRect before = container.fenceRect(id);
                container.setFenceRolledUp(id, true);
                const QRect v = container.fenceVisibleRect(id);
                bool edgeOk = false;
                switch (c.edge) {
                case 0: edgeOk = v.top() == before.top() && v.width() == before.width() && v.height() < before.height(); break;
                case 1: edgeOk = v.bottom() == before.bottom() && v.width() == before.width() && v.height() < before.height(); break;
                case 2: edgeOk = v.left() == before.left() && v.height() == before.height() && v.width() < before.width(); break;
                default: edgeOk = v.right() == before.right() && v.height() == before.height() && v.width() < before.width(); break;
                }
                ui::FenceContainer next;
                next.applyLayout(container.toLayout());
                const QString nid = next.fenceIds().value(0);
                const bool roundTrip = next.fenceRect(nid) == before && next.fenceVisibleRect(nid) == v
                                       && next.fenceRolledUp(nid);
                container.setFenceRolledUp(id, false);
                const bool back = container.fenceRect(id) == before && container.fenceVisibleRect(id) == before;
                if (!(edgeOk && roundTrip && back)) {
                    ok = false;
                    detail += QStringLiteral("[%1/%2%3: v=%4,%5 %6x%7 rt=%8 back=%9] ")
                                  .arg(c.side.isEmpty() ? QStringLiteral("top") : c.side, c.toward.isEmpty() ? QStringLiteral("default") : c.toward,
                                       tab ? QStringLiteral(" tab") : QString())
                                  .arg(v.x()).arg(v.y()).arg(v.width()).arg(v.height()).arg(roundTrip).arg(back);
                }
                if (!tab && (c.edge == 2 || c.edge == 1) && c.toward.isEmpty()) {
                    container.setFenceRolledUp(id, true);
                    ui::FenceView* view = container.view(id);
                    QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
                    img.fill(QColor(0x9A, 0xC8, 0x2A));
                    QPainter p(&img);
                    view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
                    p.end();
                    img.save(captures + QStringLiteral("/s73_rolled_%1_%2.png").arg(c.side, theme));
                }
            }
        }
        // The Settings default flips the fences that follow it.
        ui::FenceContainer flipped;
        flipped.setRollOppositeDefault(true);
        const QString fid = flipped.createFence(QStringLiteral("Roll"), stored);
        flipped.setFenceRolledUp(fid, true);
        const QRect fv = flipped.fenceVisibleRect(fid);
        ok = ok && fv.bottom() == stored.bottom() && fv.height() < stored.height();
        check(QStringLiteral("roll up toward the title bar or the opposite side, 4 sides, bar and tab, round trip (session 73)"),
              ok, detail.isEmpty() ? QStringLiteral("16 cases + default flip") : detail);
    }
    {
        // The animation only moves what is shown; the document holds the end.
        ui::FenceContainer container;
        container.setAnimateRoll(true);
        const QRect stored(100, 100, 300, 260);
        const QString id = container.createFence(QStringLiteral("Anim"), stored);
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/Windows/notepad.exe"))});
        container.setFenceRolledUp(id, true);
        const bool started = container.fenceAnimating(id);
        wait(60);
        const QRect mid = container.fenceVisibleRect(id);
        const bool midSaved = container.toLayout().fences.value(0).rolledUp && container.fenceRect(id) == stored;
        wait(300);
        const QRect end = container.fenceVisibleRect(id);
        const QRect strip = ui::FenceGeometry::visibleRect(stored, true);
        const bool rolled = !container.fenceAnimating(id) && end == strip && container.fenceRect(id) == stored;
        container.setFenceRolledUp(id, false);
        wait(60);
        const QRect midOpen = container.fenceVisibleRect(id);
        // A second toggle mid-way turns back from where it is.
        container.setFenceRolledUp(id, true);
        wait(300);
        const QRect back = container.fenceVisibleRect(id);
        container.setFenceRolledUp(id, false);
        wait(300);
        const bool open = container.fenceVisibleRect(id) == stored && container.fenceRect(id) == stored;
        check(QStringLiteral("roll / unroll animate in ~150 ms; the stored rect never moves (session 73)"),
              started && mid.height() < stored.height() && mid.height() > strip.height() && midSaved && rolled
                  && midOpen.height() > strip.height() && midOpen.height() < stored.height() && back == strip && open,
              QStringLiteral("mid %1, end %2, mid open %3, back %4").arg(mid.height()).arg(end.height())
                  .arg(midOpen.height()).arg(back.height()));
    }
    {
        // Session 95: measure a roll the size of a real zone (30 dummy icons):
        // frame gaps and the cost of each frame, reported as numbers.
        QTemporaryDir dummies;
        QList<core::DesktopItem> many;
        for (int i = 0; i < 30; ++i) {
            const QString path = QDir::fromNativeSeparators(dummies.path()) + QStringLiteral("/Item %1.txt").arg(i);
            QFile f(path);
            if (f.open(QIODevice::WriteOnly))
                f.write("x");
            f.close();
            many << core::itemFromPathOnly(path);
        }
        ui::FenceContainer container;
        container.setAnimateRoll(true);
        const QRect stored(100, 100, 430, 520);
        const QString id = container.createFence(QStringLiteral("Measure"), stored);
        container.fileInto(id, many);
        wait(100);
        container.setFenceRolledUp(id, true);
        wait(900);
        const ui::FenceContainer::RollStats up = container.lastRollStats();
        container.setFenceRolledUp(id, false);
        wait(900);
        const ui::FenceContainer::RollStats down = container.lastRollStats();
        const auto line = [](const ui::FenceContainer::RollStats& s) {
            return QStringLiteral("%1 px, planned %2 ms, %3 frames in %4 ms, gap mean %5 max %6, apply mean %7 max %8")
                .arg(s.distance).arg(s.plannedMs).arg(s.frames).arg(s.elapsedMs, 0, 'f', 1)
                .arg(s.meanGapMs, 0, 'f', 1).arg(s.maxGapMs, 0, 'f', 1).arg(s.meanApplyMs, 0, 'f', 1)
                .arg(s.maxApplyMs, 0, 'f', 1);
        };
        const bool proportional = up.plannedMs == ui::FenceContainer::rollDurationFor(up.distance, container.rollSpeed())
                                  && ui::FenceContainer::rollDurationFor(160, 1600)
                                         < ui::FenceContainer::rollDurationFor(490, 1600)
                                  && ui::FenceContainer::rollDurationFor(490, 2800)
                                         < ui::FenceContainer::rollDurationFor(490, 900);
        const bool smooth = up.maxGapMs < 25.0 && down.maxGapMs < 25.0;
        check(QStringLiteral("roll animation: duration follows the distance and the speed, no frame gap over 25 ms (session 95)"),
              up.frames > 2 && down.frames > 2 && proportional && smooth,
              QStringLiteral("up: %1 | down: %2").arg(line(up), line(down)));
    }
    {
        // Whole icons: a resize lands on whole columns and rows; restore keeps it.
        ui::FenceContainer container;
        container.setSnapToCellsDefault(true);
        const QString id = container.createFence(QStringLiteral("Cells"), QRect(100, 100, 300, 260));
        container.fileInto(id, {core::itemFromPathOnly(QStringLiteral("C:/Windows/notepad.exe")),
                                core::itemFromPathOnly(QStringLiteral("C:/Windows/regedit.exe"))});
        ui::FenceView* view = container.view(id);
        const auto fits = [view](int size, bool horizontal) {
            const QRect content = view->contentRect();
            const int chrome = horizontal ? view->width() - content.width() : view->height() - content.height();
            const int cell = horizontal ? qMax(view->iconSize(), view->cellWidth()) : view->iconSize() + view->labelHeight();
            const int rest = size - chrome - 2 * view->gridPadding() + view->gridGap();
            return rest > 0 && rest % (cell + view->gridGap()) == 0;
        };
        QString detail;
        bool ok = true;
        for (const QPoint& delta : {QPoint(37, 0), QPoint(-61, 23), QPoint(140, 97)}) {
            container.resizeFenceBy(id, ui::FenceHandle::BottomRight, delta);
            const QRect r = container.fenceRect(id);
            const bool w = fits(r.width(), true), h = fits(r.height(), false);
            ok = ok && w && h && r.topLeft() == QPoint(100, 100);
            detail += QStringLiteral("%1x%2 ").arg(r.width()).arg(r.height());
        }
        const int eastEdge = container.fenceRect(id).right();
        container.resizeFenceBy(id, ui::FenceHandle::Left, QPoint(-45, 0));
        const QRect west = container.fenceRect(id);
        ok = ok && fits(west.width(), true) && west.right() == eastEdge;
        ui::FenceContainer next;
        next.applyLayout(container.toLayout());
        const bool roundTrip = next.fenceRect(next.fenceIds().value(0)) == west;
        // A fence saying "off" stays free.
        core::FenceStyle style = container.fenceStyle(id);
        style.snapToCells = QStringLiteral("off");
        container.setFenceStyle(id, style);
        const QRect free0 = container.fenceRect(id);
        container.resizeFenceBy(id, ui::FenceHandle::Right, QPoint(13, 0));
        const bool free = container.fenceRect(id).width() == free0.width() + 13;
        QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(0x9A, 0xC8, 0x2A));
        QPainter p(&img);
        view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
        p.end();
        img.save(captures + QStringLiteral("/s73_whole_icons_%1.png").arg(theme));
        check(QStringLiteral("resize in whole icon cells (default or per fence), restore exact, 'off' stays free (session 73)"),
              ok && roundTrip && free, detail + QStringLiteral(" west %1, rt %2, free %3").arg(west.width()).arg(roundTrip).arg(free));
    }

    // --- session 72: quit + relaunch keeps a narrow fence's size --------------
    {
        struct Variant {
            QString name;
            int items;
            std::function<void(core::FenceStyle&)> apply;
        };
        const QList<Variant> variants{
            {QStringLiteral("full"), 1, [](core::FenceStyle&) {}},
            {QStringLiteral("border6 r30"), 1, [](core::FenceStyle& s) { s.borderWidth = 6; s.radius = 30; }},
            {QStringLiteral("tab"), 1, [](core::FenceStyle& s) { s.titleBarShape = QStringLiteral("tab"); s.borderWidth = 4; s.radius = 40; }},
            {QStringLiteral("left bar"), 1, [](core::FenceStyle& s) { s.titleSide = QStringLiteral("left"); }},
            {QStringLiteral("right tab"), 1, [](core::FenceStyle& s) { s.titleSide = QStringLiteral("right"); s.titleBarShape = QStringLiteral("tab"); }},
            {QStringLiteral("no title"), 1, [](core::FenceStyle& s) { s.titleBar = false; }},
            {QStringLiteral("one column x5"), 5, [](core::FenceStyle&) {}},
            {QStringLiteral("two items"), 2, [](core::FenceStyle& s) { s.borderWidth = 12; }},
        };
        const QStringList files{QStringLiteral("C:/Windows/explorer.exe"), QStringLiteral("C:/Windows/regedit.exe"),
                                QStringLiteral("C:/Windows/notepad.exe"), QStringLiteral("C:/Windows/write.exe"),
                                QStringLiteral("C:/Windows/hh.exe")};
        QString detail;
        bool stable = true;
        bool centred = true;
        for (const Variant& variant : variants) {
            ui::FenceContainer first;
            first.setMinimumFenceSize(QSize(160, 120));
            const QString id = first.createFence(QStringLiteral("Zone A"), QRect(100, 100, 300, 140 + 100 * variant.items));
            QList<core::DesktopItem> items;
            for (int i = 0; i < variant.items; ++i)
                items.append(core::itemFromPathOnly(files.at(i)));
            first.fileInto(id, items);
            core::FenceStyle style = first.fenceStyle(id);
            variant.apply(style);
            first.setFenceStyle(id, style);
            // As narrow as it goes (the user's one-column fence).
            const QRect narrow = first.setFenceRect(id, QRect(100, 100, 40, 140 + 100 * variant.items), false);
            core::Layout layout = first.toLayout();
            QList<QRect> seen{narrow};
            for (int run = 0; run < 3; ++run) {
                ui::FenceContainer next;
                next.setMinimumFenceSize(QSize(160, 120));
                next.applyLayout(layout);
                const QString nid = next.fenceIds().value(0);
                seen.append(next.fenceRect(nid));
                if (variant.items == 1 && run == 2) {
                    ui::FenceView* view = next.view(nid);
                    const QRect cell = view->tileRects().value(0);
                    const QRect content = view->contentRect();
                    const int leftRoom = cell.left() - content.left();
                    const int rightRoom = content.right() - cell.right();
                    if (qAbs(leftRoom - rightRoom) > 1) {
                        centred = false;
                        detail += QStringLiteral("%1 off-centre %2/%3  ").arg(variant.name).arg(leftRoom).arg(rightRoom);
                    }
                    if (variant.name == QLatin1String("border6 r30")) {
                        QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
                        img.fill(QColor(0x9A, 0xC8, 0x2A));
                        QPainter p(&img);
                        view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
                        p.end();
                        img.save(captures + QStringLiteral("/s72_narrow_after_3_restarts_%1.png").arg(theme));
                    }
                }
                layout = next.toLayout();
            }
            const bool same = std::all_of(seen.begin(), seen.end(), [&](const QRect& r) { return r == narrow; });
            stable = stable && same && narrow.width() < 160;
            detail += QStringLiteral("%1: %2 -> %3  ").arg(variant.name).arg(narrow.width())
                          .arg(seen.last().width());
        }
        check(QStringLiteral("save -> restart -> save keeps a narrow fence's exact size, one column centred (session 72)"),
              stable && centred, detail);
    }

    // --- session 67: the title clears the rounded corner -----------------------
    {
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("MMM"), QRect(0, 0, 300, 140));
        ui::FenceView* view = container.view(id);
        view->resize(300, 140);
        QString detail;
        bool clear = true;
        for (int radius : {30, 40}) {
            for (int border : {1, 4}) {
                core::FenceStyle style = container.fenceStyle(id);
                style.radius = radius;
                style.borderWidth = border;
                style.borderColor = QStringLiteral("#FF000000");
                style.titleAlign.clear();
                container.setFenceStyle(id, style);
                const QRect left = view->titleTextRect();
                style.titleAlign = QStringLiteral("right");
                container.setFenceStyle(id, style);
                const QRect right = view->titleTextRect();
                const int r = view->cornerRadius();
                clear = clear && left.left() >= r + border && right.right() <= view->width() - 1 - (r + border);
                detail += QStringLiteral("r%1 b%2: %3..%4  ").arg(radius).arg(border).arg(left.left()).arg(right.right());
                if (radius == 40 && border == 4) {
                    style.titleAlign.clear();
                    container.setFenceStyle(id, style);
                    QImage img(view->size(), QImage::Format_ARGB32_Premultiplied);
                    img.fill(QColor(0x9A, 0xC8, 0x2A));
                    QPainter p(&img);
                    view->render(&p, QPoint(), QRegion(), QWidget::DrawChildren);
                    p.end();
                    img.save(captures + QStringLiteral("/s67_title_r40_b4_%1.png").arg(theme));
                }
            }
        }
        check(QStringLiteral("the title clears the rounded corner and the border, left and right (session 67)"),
              clear, detail);
    }

    // --- T-098: a fence's own transparency -----------------------------------
    {
        ui::FenceContainer container;
        container.setDefaultFenceAlpha(ui::PercentSlider::alphaFromPercent(20));
        const QString id = container.createFence(QStringLiteral("Jeux"), QRect(0, 0, 320, 240));
        QDialog* dialog = ui::createFenceTransparencyDialog(&container, id);
        dialog->setAttribute(Qt::WA_DontShowOnScreen);
        dialog->show();
        wait(100);
        auto* useDefault = dialog->findChild<QCheckBox*>(QStringLiteral("useDefault"));
        auto* slider = dialog->findChild<QSlider*>(QStringLiteral("transparencySlider"));
        auto* value = dialog->findChild<QLabel*>(QStringLiteral("transparencyValue"));
        const bool startsOnDefault = useDefault && useDefault->isChecked() && slider && !slider->isEnabled()
                                     && value && value->text() == QStringLiteral("20 %");
        if (useDefault)
            useDefault->setChecked(false);
        if (slider)
            slider->setValue(60);
        const int live = container.fenceEffectiveAlpha(id);
        const bool own = !container.fenceFollowsDefaultOpacity(id);
        dialog->grab().save(captures + QStringLiteral("/ui_fence_transparency_dialog_%1.png").arg(theme));
        dialog->reject();
        check(QStringLiteral("a fence's Transparency...: default, then its own value live; Cancel restores"),
              startsOnDefault && own && live == ui::PercentSlider::alphaFromPercent(60)
                  && container.fenceFollowsDefaultOpacity(id) && container.fenceEffectiveAlpha(id) == 204,
              QStringLiteral("started on default %1, live alpha %2, after cancel %3")
                  .arg(startsOnDefault ? QLatin1String("yes") : QLatin1String("no"))
                  .arg(live)
                  .arg(container.fenceEffectiveAlpha(id)));
        delete dialog;
    }

    // --- session 60: the fence's title, edited in place ----------------------
    {
        ui::FenceView view;
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.setTitle(QStringLiteral("Jeux"));
        QList<core::DesktopItem> items;
        for (const char* name : {"Steam", "Epic Games", "GOG Galaxy", "Battle.net"}) {
            core::DesktopItem item;
            item.name = QString::fromUtf8(name);
            item.fileName = item.name;
            item.path = QStringLiteral("C:/probe/") + item.name;
            items.append(item);
        }
        view.setItems(items);
        view.resize(380, 230);
        const QRect title = view.titleTextRect();
        const ui::FenceMetrics m = view.metrics();
        check(QStringLiteral("the title text sits in the title bar"),
              !title.isEmpty() && QRect(0, 0, view.width(), m.titleBarHeight).contains(title),
              QStringLiteral("%1,%2 %3x%4").arg(title.x()).arg(title.y()).arg(title.width()).arg(title.height()));
        check(QStringLiteral("the gap under the title is the metrics' (12 px, was 24)"),
              m.titleGap + m.gridPadding == 12, QStringLiteral("titleGap %1 + gridPadding %2").arg(m.titleGap).arg(m.gridPadding));
        // The gestures, as events sent in this process (nothing injected).
        int edits = 0, rolls = 0;
        QObject::connect(&view, &ui::FenceView::titleEditRequested, &view, [&] { ++edits; });
        QObject::connect(&view, &ui::FenceView::rollUpRequested, &view, [&](bool) { ++rolls; });
        auto dbl = [&](const QPoint& at) {
            QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(at), view.mapToGlobal(QPointF(at)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&view, &e);
        };
        dbl(title.center());
        const bool onText = edits == 1 && rolls == 0;
        dbl(QPoint(view.width() - 30, m.titleBarHeight / 2));
        check(QStringLiteral("double-click: on the name renames, elsewhere on the band rolls up"),
              onText && edits == 1 && rolls == 1, QStringLiteral("edits %1, roll-ups %2").arg(edits).arg(rolls));
        // Session 62: a tile dragged out of the fence carries its picture (the
        // ghost that follows the cursor); composed only, no window shown here.
        {
            QPoint hotspot;
            const QImage tile = view.dragImageFor(1, QPoint(120, 90), 1.0, &hotspot);
            if (!tile.isNull())
                tile.save(captures + QStringLiteral("/ui_fence_drag_tile.png"));
            check(QStringLiteral("a fence tile has its drag picture"),
                  !tile.isNull() && !view.dragGhostVisible(),
                  QStringLiteral("%1x%2, hotspot %3,%4").arg(tile.width()).arg(tile.height())
                      .arg(hotspot.x()).arg(hotspot.y()));
        }
        // The picture: the fence, and the edit where ProductApp puts it (same
        // style as startFenceTitleEdit) -- a montage of the two widgets.
        QPixmap shot(view.size());
        shot.fill(QColor(0x3A, 0x5F, 0x8A));
        view.render(&shot, QPoint(), QRegion(), QWidget::DrawChildren);
        QLineEdit edit;
        edit.setAttribute(Qt::WA_DontShowOnScreen);
        QFont font(QStringLiteral("Segoe UI"), 9);
        font.setBold(true);
        edit.setFont(font);
        edit.setStyleSheet(QStringLiteral("QLineEdit { background: #FFFFFF; color: #000000; "
                                          "border: 1px solid #0078D7; border-radius: 3px; "
                                          "selection-background-color: #0078D7; "
                                          "selection-color: #FFFFFF; padding: 0 3px; }"));
        edit.setText(QStringLiteral("Jeux"));
        edit.selectAll();
        edit.resize(qMax(title.width() + 48, 160), 24);
        edit.show();
        edit.setFocus();
        wait(50);
        edit.render(&shot, QPoint(title.left() - 4, title.center().y() - 12));
        shot.save(captures + QStringLiteral("/ui_fence_title_edit.png"));
        QPixmap plain(view.size());
        plain.fill(QColor(0x3A, 0x5F, 0x8A));
        view.render(&plain, QPoint(), QRegion(), QWidget::DrawChildren);
        plain.save(captures + QStringLiteral("/ui_fence_gap.png"));
    }

    // --- the editor, driven by calls, on the scratch file -----------------------
    ui::ContextMenuEditor* editor = settings.menuEditor();
    editor->selectKind(core::ContextMenuKind::Background);
    const QStringList keys = editor->listedKeys();
    check(QStringLiteral("the editor lists Windows 11's background entries"),
          keys.contains(QStringLiteral("refresh")) && keys.contains(QStringLiteral("view"))
              && keys.contains(QStringLiteral("starfish:create-fence")),
          keys.join(QLatin1Char(' ')));
    editor->setChecked(QStringLiteral("personalize"), false);
    editor->moveEntry(QStringLiteral("refresh"), -2);
    core::ContextMenuCustomEntry notepad;
    notepad.label = QStringLiteral("Bloc-notes (probe)");
    notepad.program = QStringLiteral("notepad.exe");
    editor->addCustomEntry(notepad);
    const core::ContextMenuConfig saved = core::ContextMenuConfig::load(config);
    check(QStringLiteral("hiding an entry writes it to the file"),
          saved.background.hidden.contains(QStringLiteral("personalize")), saved.background.hidden.join(QLatin1Char(' ')));
    check(QStringLiteral("moving an entry writes the order"),
          saved.background.order.indexOf(QStringLiteral("refresh")) >= 0
              && saved.background.order.indexOf(QStringLiteral("refresh"))
                     < qMax(0, saved.background.order.indexOf(QStringLiteral("view"))),
          saved.background.order.join(QLatin1Char(' ')));
    check(QStringLiteral("a program of one's own is added"),
          saved.background.custom.size() >= 1 && editor->listedKeys().contains(QStringLiteral("custom:Bloc-notes (probe)")),
          {});

    // The composer reads the result as the menu would.
    std::vector<core::ContextMenuItem> classic;
    for (const char* label : {"Affichage", "Trier par", "Actualiser", "Personnaliser", "Nouveau"}) {
        core::ContextMenuItem item;
        item.label = QString::fromUtf8(label);
        item.command = int(classic.size());
        classic.push_back(item);
    }
    core::ContextMenuItem create;
    create.key = QStringLiteral("starfish:create-fence");
    create.label = QStringLiteral("Créer une zone ici");
    create.ownId = 1;
    const auto menu = core::composeWindows11Menu(classic, core::ContextMenuKind::Background, saved, {create},
                                                 QStringLiteral("Afficher plus d'options"));
    QStringList order;
    for (const auto& item : menu) {
        if (!item.separator)
            order << item.key;
    }
    check(QStringLiteral("the menu follows the editor (refresh first, personalize gone, the program there)"),
          order.value(0) == QLatin1String("refresh") && !order.contains(QStringLiteral("personalize"))
              && order.contains(QStringLiteral("custom:Bloc-notes (probe)")),
          order.join(QLatin1Char(' ')));
    editor->selectKind(core::ContextMenuKind::Background);
    settings.showPage(ui::SettingsDialog::Menu);
    wait(250);
    settings.grab().save(captures + QStringLiteral("/ui_settings_menu_edited_%1.png").arg(theme));
    check(QStringLiteral("the real configuration was not touched"),
          !QFile::exists(realConfig)
              || core::ContextMenuConfig::load(realConfig).background.custom.size() < saved.background.custom.size(),
          {});

    // --- session 90: a lone metric widget, in a real fence, gets fresh readings --
    // (t90u: "the CPU widget does not seem to work without the group")
    for (const char* type : {"cpu", "ram", "gpu", "vram"}) {
        ui::SystemMonitor::install();
        ui::FenceContainer container;
        const QString id = container.createFence(QStringLiteral("Lone"), QRect(0, 0, 300, 160));
        core::DesktopItem lone = ui::widgets::create(QLatin1String(type));
        lone.widgetSettings.insert(QStringLiteral("mode"), QStringLiteral("number"));
        container.addWidget(id, lone);
        ui::FenceView* view = container.view(id);
        wait(300);
        const qint64 samples0 = ui::SystemMonitor::instance().samples();
        int repaints = 0;
        QObject::connect(view, &ui::FenceView::widgetTick, view, [&repaints] { ++repaints; });
        wait(4500);
        core::Metric metric;
        core::metricFromType(QLatin1String(type), &metric);
        const core::MetricHistory* h = ui::SystemMonitor::instance().history(metric);
        const qint64 fresh = ui::SystemMonitor::instance().samples() - samples0;
        const bool gpuSide = metric == core::Metric::Gpu || metric == core::Metric::Vram;
        check(QStringLiteral("a lone %1 widget in a fence: sampler runs, value read, repainted each second (session 90)").arg(QLatin1String(type)),
              fresh >= 3 && repaints >= 3 && h && (gpuSide || h->latest().valid()),
              QStringLiteral("%1 new samples, %2 ticks, now %3").arg(fresh).arg(repaints)
                  .arg(ui::widgets::metricText(lone, metric)));
    }

    // --- session 88: system widgets (fake readings) and their settings window --
    {
        static core::MetricHistory fake[core::kMetricCount] = {core::MetricHistory(400), core::MetricHistory(400),
                                                               core::MetricHistory(400), core::MetricHistory(400)};
        const double gb = 1024.0 * 1024.0 * 1024.0;
        for (int i = 0; i < 400; ++i) {
            const double t = i / 9.0;
            core::MetricPoint cpu, ram, gpu, vram;
            cpu.percent = float(22 + 18 * std::sin(t) + (i % 7) * 2);
            ram.percent = float(48 + 3 * std::sin(t / 5));
            ram.totalBytes = 32 * gb;
            ram.usedBytes = ram.totalBytes * ram.percent / 100;
            gpu.percent = float(qMax(0.0, 35 + 30 * std::sin(t / 2)));
            vram.percent = 31.0f;
            vram.totalBytes = 12 * gb;
            vram.usedBytes = vram.totalBytes * 0.31;
            fake[0].push(cpu); fake[1].push(ram); fake[2].push(gpu); fake[3].push(vram);
        }
        ui::widgets::setMetricsSource([](core::Metric m) { return &fake[int(m)]; }, [](bool) {});
        const QSize cell(92, 80);
        auto render = [&](core::DesktopItem item) {
            QImage image(cell.width() * item.span.width(), cell.height() * item.span.height(),
                         QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(0x1F, 0x29, 0x37, 0xE6));
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            ui::widgets::PaintContext context;
            context.text = QColor(0xE5, 0xE7, 0xEB);
            QFont font(QStringLiteral("Segoe UI"));
            font.setPixelSize(13);
            context.font = font;
            context.now = QDateTime::currentDateTime();
            ui::widgets::paint(painter, image.rect().adjusted(6, 6, -6, -6), item, context);
            return image;
        };
        QStringList saved;
        bool painted = true;
        for (const char* type : {"cpu", "ram", "gpu", "vram"}) {
            for (const char* mode : {"number", "bar", "graph"}) {
                core::DesktopItem item = ui::widgets::create(QLatin1String(type));
                item.widgetSettings.insert(QStringLiteral("mode"), QLatin1String(mode));
                if (QLatin1String(type) == QLatin1String("vram") && QLatin1String(mode) == QLatin1String("bar"))
                    item.widgetSettings.insert(QStringLiteral("show"), QStringLiteral("free"));
                const QImage image = render(item);
                painted = painted && !image.isNull();
                const QString name = QStringLiteral("s88_%1_%2.png").arg(QLatin1String(type), QLatin1String(mode));
                image.save(captures + QLatin1Char('/') + name);
                saved << name;
            }
        }
        for (const char* mode : {"bar", "graph"}) {
            core::DesktopItem group = ui::widgets::create(QStringLiteral("system"));
            group.widgetSettings.insert(QStringLiteral("mode"), QLatin1String(mode));
            render(group).save(captures + QStringLiteral("/s88_system_%1.png").arg(QLatin1String(mode)));
        }
        check(QStringLiteral("system widgets: CPU / RAM / GPU / VRAM, number / bar / graph, and the group, paint (session 88)"),
              painted && ui::widgets::metricText(ui::widgets::create(QStringLiteral("ram")), core::Metric::Ram)
                             .contains(QLatin1Char('1')),
              saved.join(QLatin1Char(' ')));

        // The settings window: a RAM graph and the group, grabbed offscreen.
        core::DesktopItem ram = ui::widgets::create(QStringLiteral("ram"));
        ram.widgetSettings.insert(QStringLiteral("future"), QStringLiteral("kept"));
        ui::WidgetSettingsDialog dialog(ram);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        wait(80);
        dialog.findChild<QComboBox*>(QStringLiteral("mode"))->setCurrentIndex(2);
        dialog.findChild<QComboBox*>(QStringLiteral("show"))->setCurrentIndex(1);
        dialog.findChild<QSpinBox*>(QStringLiteral("history"))->setValue(120);
        wait(50);
        dialog.grab().save(captures + QStringLiteral("/s88_settings_ram.png"));
        const core::DesktopItem changed = dialog.result();
        const core::MetricSettings back = core::MetricSettings::fromMap(changed.widgetSettings, changed.widgetType);
        dialog.hide();
        core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
        ui::WidgetSettingsDialog clockDialog(clock);
        clockDialog.setAttribute(Qt::WA_DontShowOnScreen);
        clockDialog.show();
        wait(50);
        clockDialog.grab().save(captures + QStringLiteral("/s88_settings_clock.png"));
        clockDialog.hide();
        ui::WidgetSettingsDialog groupDialog(ui::widgets::create(QStringLiteral("system")));
        groupDialog.setAttribute(Qt::WA_DontShowOnScreen);
        groupDialog.show();
        wait(50);
        groupDialog.grab().save(captures + QStringLiteral("/s88_settings_system.png"));
        groupDialog.hide();
        check(QStringLiteral("widget settings window: graph / free / history applied, unknown keys kept, clock and group too (session 88)"),
              back.mode == core::MetricSettings::Mode::Graph && back.showFree && back.history == 120
                  && changed.widgetSettings.value(QStringLiteral("future")).toString() == QLatin1String("kept"),
              {});

        // The real sampler, briefly (reads only; idle again 3 s after).
        // Its cost: this process's CPU time and working set across 10 s of
        // sampling, against 10 s idle just before (same process, same windows).
        auto cpuMs = [] {
            FILETIME created, exited, kernel, user;
            GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
            auto ms = [](const FILETIME& f) { return ((quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0; };
            return ms(kernel) + ms(user);
        };
        auto workingSet = [] {
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
            return double(counters.WorkingSetSize) / (1024.0 * 1024.0);
        };
        const double idleCpu0 = cpuMs();
        wait(10000);
        const double idleCpu = (cpuMs() - idleCpu0) / 10000.0 * 100.0;
        const double set0 = workingSet();
        ui::SystemMonitor::install();
        const double startCpu0 = cpuMs();
        for (int i = 0; i < 30; ++i) { // the start: PDH opens its query
            ui::SystemMonitor::instance().demand();
            wait(100);
        }
        const double startCpu = cpuMs() - startCpu0;
        const double busyCpu0 = cpuMs();
        for (int i = 0; i < 100; ++i) {
            ui::SystemMonitor::instance().demand();
            wait(100);
        }
        const double busyCpu = (cpuMs() - busyCpu0) / 10000.0 * 100.0;
        const double setDelta = workingSet() - set0;
        for (int i = 0; i < 40; ++i) { // the GPU lease runs out
            ui::SystemMonitor::instance().demand(false);
            wait(100);
        }
        const double lightCpu0 = cpuMs();
        for (int i = 0; i < 100; ++i) {
            ui::SystemMonitor::instance().demand(false);
            wait(100);
        }
        const double lightCpu = (cpuMs() - lightCpu0) / 10000.0 * 100.0;
        lines << QStringLiteral("      CPU / RAM widgets only (no PDH): %1 % CPU (one core)").arg(lightCpu, 0, 'f', 3);
        for (int i = 0; i < 30; ++i) { // GPU again, for the check below
            ui::SystemMonitor::instance().demand();
            wait(100);
        }
        lines << QStringLiteral("      sampler cost: idle %1 % CPU, sampling %2 % CPU (one core, steady), start %4 ms CPU, working set %3 MB")
                     .arg(idleCpu, 0, 'f', 3).arg(busyCpu, 0, 'f', 3).arg(setDelta, 0, 'f', 2).arg(startCpu, 0, 'f', 0);
        QStringList read;
        for (int i = 0; i < core::kMetricCount; ++i) {
            const core::MetricHistory* h = ui::SystemMonitor::instance().history(core::Metric(i));
            read << core::metricShortName(core::Metric(i)) + QLatin1Char('=')
                        + (h ? core::formatMetric(core::Metric(i), h->latest(), false, false, QLocale::c())
                             : QStringLiteral("-"));
        }
        const core::MetricHistory* cpuH = ui::SystemMonitor::instance().history(core::Metric::Cpu);
        const core::MetricHistory* ramH = ui::SystemMonitor::instance().history(core::Metric::Ram);
        check(QStringLiteral("the sampler reads CPU and RAM (GPU / VRAM or n/a) (session 88)"),
              cpuH && ramH && cpuH->latest().valid() && ramH->latest().valid(), read.join(QLatin1Char(' ')));
    }

    // --- session 91: widgets on the desktop (a detached backdrop, fake wallpaper)
    {
        static core::MetricHistory deskFake(200);
        for (int i = 0; i < 200; ++i) {
            core::MetricPoint p;
            p.percent = float(30 + 20 * std::sin(i / 7.0));
            p.totalBytes = 32.0 * 1024 * 1024 * 1024;
            p.usedBytes = p.totalBytes * p.percent / 100;
            deskFake.push(p);
        }
        ui::widgets::setMetricsSource([](core::Metric) { return &deskFake; }, [](bool) {});
        ui::DesktopBackdrop desk; // never attached: nothing reaches the real desktop
        const QSize screen(1920, 1080);
        QImage wallpaper(screen, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter wp(&wallpaper);
            QLinearGradient g(0, 0, screen.width(), screen.height());
            g.setColorAt(0, QColor(0x1E, 0x3A, 0x5F));
            g.setColorAt(0.6, QColor(0x6A, 0x8F, 0xB8));
            g.setColorAt(1, QColor(0xE8, 0xD9, 0xB5));
            wp.fillRect(wallpaper.rect(), g);
        }
        desk.setWallpaper(wallpaper);
        desk.setDrawFreeIcons(true);
        desk.setGestureEnabled(true);
        desk.setDesktopWidgetCell(QSize(95, 100));
        core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
        clock.widgetSettings.insert(QStringLiteral("date"), true);
        core::DesktopItem cpu = ui::widgets::create(QStringLiteral("cpu"));
        cpu.widgetSettings.insert(QStringLiteral("mode"), QStringLiteral("graph"));
        core::DesktopItem group = ui::widgets::create(QStringLiteral("system"));
        core::DesktopItem note = ui::widgets::withNoteText(ui::widgets::create(QStringLiteral("notes")),
                                                           QStringLiteral("Penser au rendu du 12"));
        note.widgetSettings.insert(QStringLiteral("backdrop"), true);
        core::DesktopItem ram = ui::widgets::create(QStringLiteral("ram"));
        ram.widgetSettings.insert(QStringLiteral("backdrop"), true);
        desk.setDesktopWidgets({{clock, QPoint(1500, 100)}, {cpu, QPoint(1500, 200)}, {group, QPoint(1500, 300)},
                                {note, QPoint(1100, 700)}, {ram, QPoint(1500, 760)}});
        QImage shot = wallpaper.copy();
        {
            QPainter sp(&shot);
            desk.paintDesktopWidgets(sp, QRect(QPoint(0, 0), screen), QRect(QPoint(0, 0), screen));
        }
        shot.copy(QRect(1050, 60, 840, 920)).save(captures + QStringLiteral("/s91_desktop_widgets.png"));
        bool changed = false;
        for (int y = 100; y < 200 && !changed; y += 3)
            for (int x = 1500; x < 1690 && !changed; x += 3)
                changed = shot.pixel(x, y) != wallpaper.pixel(x, y);
        check(QStringLiteral("desktop widgets paint on the wallpaper, no panel; optional readable backdrop (session 91)"),
              changed && desk.desktopWidgetAt(QPoint(1510, 110)) == 0 && desk.desktopWidgetAt(QPoint(10, 10)) < 0
                  && desk.desktopWidgetRect(desk.desktopWidgets().at(2)).size() == QSize(190, 200),
              QStringLiteral("s91_desktop_widgets.png"));

        // The mouse, as the surface posts it (Qt events to the object, no input
        // injected into Windows): a drag past the icons' threshold, the menu,
        // a double-click on the note.
        QString droppedPath, menuPath, editPath;
        QPoint droppedAt;
        int dragUpdates = 0;
        QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetDropped, &desk,
                         [&](const QString& path, const QPoint& pos) { droppedPath = path; droppedAt = pos; });
        QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetDragged, &desk,
                         [&](const QString&, const QRect&, bool active) { dragUpdates += active ? 1 : 0; });
        QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetMenuRequested, &desk,
                         [&](const QString& path, const QPoint&) { menuPath = path; });
        QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetEditRequested, &desk,
                         [&](const QString& path) { editPath = path; });
        const auto send = [&](QEvent::Type type, const QPoint& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(at), QPointF(at), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&desk, &event);
        };
        const QPoint grab(1520, 220);
        send(QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, grab + QPoint(3, 0), Qt::NoButton, Qt::LeftButton); // under the threshold
        const bool stillBefore = desk.desktopWidgets().at(1).pos == QPoint(1500, 200);
        send(QEvent::MouseMove, grab + QPoint(-200, 120), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, grab + QPoint(-200, 120), Qt::LeftButton, Qt::NoButton);
        send(QEvent::MouseButtonPress, QPoint(1510, 110), Qt::RightButton, Qt::RightButton);
        send(QEvent::MouseButtonRelease, QPoint(1510, 110), Qt::RightButton, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, QPoint(1120, 720), Qt::LeftButton, Qt::LeftButton);
        check(QStringLiteral("a desktop widget: dragged past the threshold, dropped, menu, note double-click (session 91)"),
              stillBefore && dragUpdates >= 1 && droppedPath == cpu.path && droppedAt == QPoint(1300, 320)
                  && menuPath == clock.path && editPath == note.path,
              QStringLiteral("drop %1 at %2,%3, menu %4, edit %5")
                  .arg(droppedPath).arg(droppedAt.x()).arg(droppedAt.y()).arg(menuPath, editPath));
        check(QStringLiteral("desktop widgets do not tick while the StarFish desktop is not shown (sampler released) (session 91)"),
              !desk.desktopWidgetsTicking(), {});
        // Session 100: an Image widget on the desktop pulled to the pixel; a
        // locked desktop widget keeps its place and has no resize mode.
        {
            core::DesktopItem picture = ui::widgets::create(QStringLiteral("image"));
            picture.span = QSize(2, 2);
            core::DesktopItem held = ui::widgets::create(QStringLiteral("cpu"));
            held.widgetSettings.insert(QStringLiteral("locked"), true);
            QList<ui::DesktopWidget> all = desk.desktopWidgets();
            all.append({picture, QPoint(200, 600)});
            all.append({held, QPoint(700, 600)});
            desk.setDesktopWidgets(all);
            QSize freeSize;
            int cellEmits = 0;
            QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetResizedFree, &desk,
                             [&freeSize](const QString&, const QSize& size) { freeSize = size; });
            QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetResized, &desk,
                             [&cellEmits](const QString&, const QSize&) { ++cellEmits; });
            const bool began = desk.beginWidgetResize(picture.path);
            const QRect r = desk.widgetResizeRect();
            send(QEvent::MouseButtonPress, r.bottomRight(), Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseMove, r.bottomRight() + QPoint(37, 23), Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, r.bottomRight() + QPoint(37, 23), Qt::LeftButton, Qt::NoButton);
            desk.endWidgetResize();
            // Pulled far in: the minimum.
            desk.beginWidgetResize(picture.path);
            const QRect r2 = desk.widgetResizeRect();
            QSize tiny;
            QObject::disconnect(&desk, &ui::DesktopBackdrop::desktopWidgetResizedFree, nullptr, nullptr);
            QObject::connect(&desk, &ui::DesktopBackdrop::desktopWidgetResizedFree, &desk,
                             [&tiny](const QString&, const QSize& size) { tiny = size; });
            send(QEvent::MouseButtonPress, r2.bottomRight(), Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseMove, r2.bottomRight() - QPoint(1000, 1000), Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, r2.bottomRight() - QPoint(1000, 1000), Qt::LeftButton, Qt::NoButton);
            desk.endWidgetResize();
            {   // The picture at its new size, in its resize mode: the capture.
                QList<ui::DesktopWidget> shown = desk.desktopWidgets();
                for (ui::DesktopWidget& d : shown)
                    if (d.item.path == picture.path) {
                        d.item.widgetSettings.insert(QStringLiteral("path"), QDir(QStringLiteral("build")).absoluteFilePath(QStringLiteral("s100_dummy.png")));
                        d.item.widgetSettings.insert(QStringLiteral("pixelWidth"), 260);
                        d.item.widgetSettings.insert(QStringLiteral("pixelHeight"), 150);
                    }
                QImage wide(800, 450, QImage::Format_ARGB32);
                {
                    QPainter wp(&wide);
                    QLinearGradient g(0, 0, 800, 450);
                    g.setColorAt(0, QColor(0x2E, 0x8B, 0x57));
                    g.setColorAt(1, QColor(0x1E, 0x3A, 0x8A));
                    wp.fillRect(wide.rect(), g);
                }
                wide.save(QDir(QStringLiteral("build")).absoluteFilePath(QStringLiteral("s100_dummy.png")));
                desk.setDesktopWidgets(shown);
                desk.beginWidgetResize(picture.path);
                QImage full(1920, 1080, QImage::Format_ARGB32_Premultiplied);
                full.fill(QColor(0x30, 0x40, 0x50));
                {
                    QPainter sp(&full);
                    desk.paintDesktopWidgets(sp, full.rect(), full.rect());
                }
                const QImage shot = full.copy(QRect(150, 570, 360, 230));
                shot.save(captures + QStringLiteral("/s100_image_free_resize.png"));
                desk.endWidgetResize();
                QFile::remove(QDir(QStringLiteral("build")).absoluteFilePath(QStringLiteral("s100_dummy.png")));
            }
            const bool lockedNoMode = !desk.beginWidgetResize(held.path);
            send(QEvent::MouseButtonPress, QPoint(720, 620), Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseMove, QPoint(900, 700), Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, QPoint(900, 700), Qt::LeftButton, Qt::NoButton);
            QPoint heldAt;
            for (const ui::DesktopWidget& d : desk.desktopWidgets())
                if (d.item.path == held.path)
                    heldAt = d.pos;
            check(QStringLiteral("desktop Image widget resized to the pixel (min 32 px); a locked desktop widget stays, no resize mode (session 100)"),
                  began && freeSize == r.size() + QSize(37, 23) && cellEmits == 0
                      && tiny == QSize(ui::widget_resize::kMinFreePx, ui::widget_resize::kMinFreePx) && lockedNoMode
                      && heldAt == QPoint(700, 600),
                  QStringLiteral("from %1x%2 to %3x%4, tiny %5x%6, locked mode %7, held at %8,%9")
                      .arg(r.width()).arg(r.height()).arg(freeSize.width()).arg(freeSize.height())
                      .arg(tiny.width()).arg(tiny.height()).arg(!lockedNoMode).arg(heldAt.x()).arg(heldAt.y()));
        }

        // In the document, through the container: saved, restored, an obstacle
        // for the icons, cleared by a reset.
        ui::FenceContainer first;
        first.backdrop()->setDesktopWidgetCell(QSize(95, 100));
        int changes = 0;
        QObject::connect(&first, &ui::FenceContainer::desktopWidgetsChanged, &first, [&changes] { ++changes; });
        first.addDesktopWidget(cpu, QPoint(1500, 200));
        first.addDesktopWidget(note, QPoint(1100, 700));
        first.moveDesktopWidget(cpu.path, QPoint(1405, 205));
        const core::Layout doc = first.toLayout();
        ui::FenceContainer second;
        second.applyLayout(*core::JsonStore::fromJson(core::JsonStore::toJson(doc)));
        const bool restored = second.desktopWidgets().size() == 2
                              && second.desktopWidgets().at(0).pos == QPoint(1405, 205)
                              && second.desktopWidgets().at(1).item.widgetSettings.value(QStringLiteral("backdrop")).toBool();
        const bool obstacle = first.fenceObstacleRects().contains(QRect(QPoint(1405, 205), QSize(190, 100)));
        first.clearDesktopWidgets();
        check(QStringLiteral("desktop widgets: saved in layout.json, restored, icons avoid them, reset clears (session 91)"),
              restored && obstacle && first.desktopWidgets().isEmpty() && changes == 4
                  && doc.desktopWidgets.size() == 2,
              QStringLiteral("%1 change(s)").arg(changes));
    }

    // --- session 95: the image widget (a picture drawn here, in a temp folder) --
    {
        QTemporaryDir pics;
        const QString picture = QDir::fromNativeSeparators(pics.path()) + QStringLiteral("/sunset.png");
        {
            QImage wide(800, 400, QImage::Format_ARGB32);
            QPainter wp(&wide);
            QLinearGradient g(0, 0, 0, 400);
            g.setColorAt(0, QColor(0xFF, 0x8A, 0x3D));
            g.setColorAt(1, QColor(0x5B, 0x2A, 0x86));
            wp.fillRect(wide.rect(), g);
            wp.setBrush(QColor(0xFF, 0xE0, 0x7A));
            wp.setPen(Qt::NoPen);
            wp.drawEllipse(QPoint(400, 260), 70, 70);
            wp.end();
            wide.save(picture);
        }
        core::DesktopItem image = ui::widgets::create(QStringLiteral("image"));
        ui::WidgetSettingsDialog dialog(image);
        dialog.setImagePath(picture);
        auto* fit = dialog.findChild<QComboBox*>(QStringLiteral("fit"));
        if (fit)
            fit->setCurrentIndex(1); // fill
        if (offscreen)
            dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        wait(150);
        dialog.grab().save(captures + QStringLiteral("/s95_image_settings.png"));
        const core::DesktopItem filled = dialog.result();
        dialog.hide();
        core::DesktopItem fitted = filled;
        fitted.widgetSettings.insert(QStringLiteral("fit"), QStringLiteral("fit"));
        ui::widgets::PaintContext pc;
        pc.text = Qt::white;
        pc.font = QApplication::font();
        const auto draw = [&pc](const core::DesktopItem& item) {
            QImage out(200, 200, QImage::Format_ARGB32_Premultiplied);
            out.fill(QColor(0x20, 0x30, 0x40));
            QPainter p(&out);
            ui::widgets::paint(p, out.rect(), item, pc);
            p.end();
            return out;
        };
        const QImage a = draw(fitted), b = draw(filled), none = draw(image);
        QImage sheet(640, 200, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor(0x20, 0x30, 0x40));
        {
            QPainter sp(&sheet);
            sp.drawImage(0, 0, a);
            sp.drawImage(220, 0, b);
            sp.drawImage(440, 0, none);
        }
        sheet.save(captures + QStringLiteral("/s95_image_widget.png"));
        const QColor bg(0x20, 0x30, 0x40);
        const bool fitLetterbox = a.pixelColor(100, 20) == bg && a.pixelColor(100, 100) != bg;
        const bool fillCovers = b.pixelColor(100, 20) != bg && b.pixelColor(100, 180) != bg;
        // Session 100 (t122u): a zone's widget let go on the bare desktop stays
        // in its zone (zone widgets and desktop widgets are apart); a widget
        // filed into a zone lands there, a locked zone refuses.
        ui::FenceContainer box;
        const QString zone = box.createFence(QStringLiteral("Zone"), QRect(0, 0, 400, 300));
        box.addWidget(zone, filled);
        int hovers = 0;
        QObject::connect(&box, &ui::FenceContainer::tilesDroppedOnDesktop, &box,
                         [&hovers](const QStringList&, const QPoint&) { ++hovers; });
        box.dragTilesOutside(zone, {filled.path}, QPoint(900, 600), true);
        bool stayed = false;
        if (ui::FenceView* v = box.view(zone))
            for (const core::DesktopItem& item : v->items())
                stayed = stayed || item.path == filled.path;
        core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
        const int in = box.fileIntoAt(zone, {clock}, QPoint(200, 150));
        box.setFenceLocked(zone, true);
        core::DesktopItem cpu = ui::widgets::create(QStringLiteral("cpu"));
        const int refused = box.fileIntoAt(zone, {cpu}, QPoint(200, 150));
        const bool t150 = stayed && hovers == 0 && in == 1 && refused == 0;
        // Saved like any widget setting.
        const core::Layout doc = box.toLayout();
        bool saved = false;
        for (const core::FenceItem& item : doc.fences.value(0).items)
            saved = saved || item.widgetSettings.value(QStringLiteral("path")).toString() == picture;
        check(QStringLiteral("image widget: fit letterboxes, fill covers, placeholder, chosen in its settings, saved (session 95)"),
              fitLetterbox && fillCovers && filled.widgetSettings.value(QStringLiteral("fit")).toString() == QLatin1String("fill")
                  && saved,
              QStringLiteral("fit %1 fill %2 saved %3 [s95_image_widget.png]").arg(fitLetterbox).arg(fillCovers).arg(saved));
        check(QStringLiteral("a zone's widget let go on the desktop stays in its zone; filed into a zone; a locked zone refuses (session 100)"),
              t150, QStringLiteral("stayed %1 handed %2 in %3 refused %4").arg(stayed).arg(hovers).arg(in).arg(refused));
    }
    // Session 95: About > reset, the labels that say what happens.
    {
        auto* reset = settings.findChild<QPushButton*>(QStringLiteral("resetDesktop"));
        check(QStringLiteral("About: one reset button, 'Remove every zone and widget' (session 96)"),
              reset && !settings.findChild<QPushButton*>(QStringLiteral("resetWipe"))
                  && reset->text() == ui::SettingsDialog::tr("Remove every zone and widget"),
              reset ? reset->text() : QString());
    }

    // Session 96: any widget size from the settings window (width x height).
    {
        ui::WidgetSettingsDialog sized(ui::widgets::create(QStringLiteral("notes")));
        sized.setAttribute(Qt::WA_DontShowOnScreen);
        sized.show();
        wait(50);
        auto* w = sized.findChild<QSpinBox*>(QStringLiteral("sizeWidth"));
        auto* h = sized.findChild<QSpinBox*>(QStringLiteral("sizeHeight"));
        if (w && h) {
            w->setValue(5);
            h->setValue(3);
        }
        wait(30);
        sized.grab().save(captures + QStringLiteral("/s96_widget_size.png"));
        const QSize got = sized.result().span;
        sized.hide();
        check(QStringLiteral("widget settings: width and height in cells, any size up to 8 x 6 (session 96)"),
              w && h && got == QSize(5, 3) && w->maximum() == 8 && h->maximum() == 6,
              QStringLiteral("%1 x %2 [s96_widget_size.png]").arg(got.width()).arg(got.height()));
    }
    // Session 97: over a fence, the drag says "Insert" (DROPDESCRIPTION),
    // and the shell's own word comes back when it leaves. Dummy file only.
    {
        const QString dummy = QDir(QStringLiteral("build")).absoluteFilePath(QStringLiteral("s97_drop_dummy.txt"));
        QFile f(dummy);
        f.open(QIODevice::WriteOnly);
        f.write("probe");
        f.close();
        const auto readDescription = [](IDataObject* data, int* type) {
            FORMATETC format{static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION)), nullptr,
                             DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            QString text;
            *type = -2;
            if (SUCCEEDED(data->GetData(&format, &medium))) {
                if (auto* d = static_cast<DROPDESCRIPTION*>(::GlobalLock(medium.hGlobal))) {
                    text = QString::fromWCharArray(d->szMessage);
                    *type = d->type;
                    ::GlobalUnlock(medium.hGlobal);
                }
                ::ReleaseStgMedium(&medium);
            }
            return text;
        };
        auto* data = static_cast<IDataObject*>(platform::win::createShellDataObject({QDir::toNativeSeparators(dummy)}));
        auto* target = new platform::win::ShellDropTarget(nullptr, [](const QPoint&, const QStringList&) {});
        QString over, after;
        int overType = -2, afterType = -2;
        if (data) {
            const POINTL pt{10, 10};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            target->DragEnter(data, MK_LBUTTON, pt, &effect);
            over = readDescription(data, &overType);
            target->DragLeave();
            after = readDescription(data, &afterType);
            data->Release();
        }
        target->Release();
        QFile::remove(dummy);
        const QString expected = QCoreApplication::translate("ShellDropTarget", "Insert");
        check(QStringLiteral("drag over a fence says 'Insert', not 'Copy'; the shell's label back on leaving (session 97)"),
              over == expected && overType == DROPIMAGE_MOVE && afterType == DROPIMAGE_INVALID,
              QStringLiteral("over '%1' (%2), after (%3)").arg(over).arg(overType).arg(afterType));
    }
    // Session 97: the language applies at once -- tray menu rebuilt, settings
    // window rebuilt, both in the other language; then back.
    {
        const QString before = core::i18n::installedLanguage();
        const auto trayHas = [](ui::TrayIcon& t, const QString& text) {
            for (QAction* action : t.menu()->actions())
                if (action->text() == text)
                    return true;
            return false;
        };
        core::i18n::install(QStringLiteral("en"));
        ui::TrayIcon live;
        live.retranslate();
        const bool trayEn = trayHas(live, QStringLiteral("Settings..."));
        ui::SettingsDialog en(nullptr);
        const QString titleEn = en.windowTitle();
        core::i18n::install(QStringLiteral("fr"));
        live.retranslate();
        const bool trayFr = trayHas(live, QString::fromUtf8("R\xc3\xa9glages..."));
        ui::SettingsDialog fr(nullptr);
        const QString titleFr = fr.windowTitle();
        const QString insertFr = QCoreApplication::translate("ShellDropTarget", "Insert");
        core::i18n::install(before.isEmpty() ? QStringLiteral("en") : before);
        check(QStringLiteral("language switch EN <-> FR without restarting: tray menu and settings window follow (session 97)"),
              trayEn && trayFr && titleEn != titleFr && insertFr == QString::fromUtf8("Ins\xc3\xa9rer"),
              QStringLiteral("tray en %1 fr %2, '%3' / '%4', '%5'").arg(trayEn).arg(trayFr).arg(titleEn, titleFr, insertFr));
    }
    // Session 98 (T-160): Ctrl copies, Alt makes a shortcut, a right-button
    // drag asks; the drop description follows. Dummy files in a temp folder,
    // shell operations isolated (no undo record, no UI).
    {
        QTemporaryDir sandbox;
        const QString in = QDir(sandbox.path()).filePath(QStringLiteral("in"));
        const QString out = QDir(sandbox.path()).filePath(QStringLiteral("out"));
        QDir().mkpath(in);
        QDir().mkpath(out);
        const QString dummy = QDir::toNativeSeparators(QDir(in).filePath(QStringLiteral("s98 dummy.txt")));
        {
            QFile f(dummy);
            f.open(QIODevice::WriteOnly);
            f.write("probe");
        }
        const bool wasIsolated = platform::win::shellOperationIsolation();
        platform::win::setShellOperationIsolation(true);
        QStringList received;
        auto* target = new platform::win::ShellDropTarget(nullptr, [&received](const QPoint&, const QStringList& p) {
            received = p;
        });
        target->setFileFolder(QDir::toNativeSeparators(out));
        const auto descriptionOf = [](IDataObject* data) {
            FORMATETC format{static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION)), nullptr,
                             DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            QString text;
            if (SUCCEEDED(data->GetData(&format, &medium))) {
                if (auto* d = static_cast<DROPDESCRIPTION*>(::GlobalLock(medium.hGlobal))) {
                    text = QString::fromWCharArray(d->szMessage) + QLatin1Char('/') + QString::number(d->type);
                    ::GlobalUnlock(medium.hGlobal);
                }
                ::ReleaseStgMedium(&medium);
            }
            return text;
        };
        // One drag: enter with `keys`, read the label, drop; the effect and what the fence got.
        const auto play = [&](DWORD keys, QString* label) {
            received.clear();
            auto* data = static_cast<IDataObject*>(platform::win::createShellDataObject({dummy}));
            if (!data)
                return DWORD(0xFFFF);
            const POINTL pt{20, 20};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            target->DragEnter(data, keys, pt, &effect);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            target->DragOver(keys, pt, &effect);
            if (label)
                *label = descriptionOf(data);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            target->Drop(data, keys & ~DWORD(MK_LBUTTON | MK_RBUTTON), pt, &effect);
            data->Release();
            return effect;
        };
        const platform::win::DropMenu previousMenu = platform::win::explorerDropMenu();
        platform::win::setExplorerDropMenu({}); // the setting off: as before
        QString insertLabel, copyLabel, linkLabel, askLabel;
        const DWORD insertEffect = play(MK_LBUTTON, &insertLabel);
        const bool insertKept = received == QStringList{dummy};
        const DWORD copyEffect = play(MK_LBUTTON | MK_CONTROL, &copyLabel);
        const QStringList copied = received;
        const DWORD linkEffect = play(MK_LBUTTON | MK_ALT, &linkLabel);
        const QStringList linked = received;
        // Session 99: the setting on -- a plain LEFT drag asks; a right drag
        // never does (no menu, inserted); Ctrl still copies without asking.
        int asked = 0;
        platform::win::setExplorerDropMenu([&asked](const QPoint&) {
            ++asked;
            return platform::win::DropChoice::Cancel;
        });
        const DWORD cancelEffect = play(MK_LBUTTON, &askLabel);
        const bool cancelled = received.isEmpty() && asked == 1;
        play(MK_RBUTTON, nullptr);
        const bool rightNoMenu = received == QStringList{dummy} && asked == 1;
        play(MK_LBUTTON | MK_CONTROL, nullptr);
        const bool ctrlNoMenu = received.size() == 1 && asked == 1;
        platform::win::setExplorerDropMenu([](const QPoint&) { return platform::win::DropChoice::Copy; });
        play(MK_LBUTTON, nullptr);
        const QStringList menuCopied = received;
        // The drawn desktop asks too (Cancel only: nothing reaches the Desktop folder).
        int foreign = 0, deskAsked = 0;
        platform::win::setExplorerDropMenu([&deskAsked](const QPoint&) {
            ++deskAsked;
            return platform::win::DropChoice::Cancel;
        });
        DWORD deskEffect = 0xFF;
        if (auto* desk = static_cast<IDropTarget*>(platform::win::createDesktopDropTarget(
                0, {}, [&foreign](const QPoint&, const QStringList&) { ++foreign; }))) {
            if (auto* data = static_cast<IDataObject*>(platform::win::createShellDataObject({dummy}))) {
                const POINTL pt{20, 20};
                DWORD e = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                desk->DragEnter(data, MK_LBUTTON, pt, &e);
                e = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                desk->DragOver(MK_LBUTTON, pt, &e);
                deskEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                desk->Drop(data, 0, pt, &deskEffect);
                data->Release();
            }
            desk->Release();
        }
        const bool deskCancelled = deskAsked == 1 && foreign == 0 && deskEffect == DROPEFFECT_NONE;
        platform::win::setExplorerDropMenu(previousMenu);
        target->Release();
        platform::win::setShellOperationIsolation(wasIsolated);
        const auto inOut = [&out](const QStringList& paths, const QString& suffix) {
            return paths.size() == 1 && QFileInfo(paths.first()).absolutePath() == QFileInfo(out).absoluteFilePath()
                   && paths.first().endsWith(suffix, Qt::CaseInsensitive) && QFileInfo::exists(paths.first());
        };
        const QString tr0 = QCoreApplication::translate("ShellDropTarget", "Insert");
        check(QStringLiteral("drop from the Explorer: insert, Ctrl = copy, Alt = shortcut; setting on: a left drag asks, a right drag never (T-160, session 99)"),
              insertKept && insertEffect == DROPEFFECT_COPY && insertLabel.startsWith(tr0)
                  && inOut(copied, QStringLiteral(".txt")) && copyEffect == DROPEFFECT_COPY
                  && copyLabel.startsWith(QCoreApplication::translate("ShellDropTarget", "Copy"))
                  && inOut(linked, QStringLiteral(".lnk")) && linkEffect == DROPEFFECT_LINK
                  && linkLabel.startsWith(QCoreApplication::translate("ShellDropTarget", "Create shortcut"))
                  && cancelled && cancelEffect == DROPEFFECT_NONE && menuCopied.size() == 1
                  && askLabel.startsWith(QCoreApplication::translate("ShellDropTarget", "Drop here"))
                  && rightNoMenu && ctrlNoMenu && deskCancelled,
              QStringLiteral("'%1' '%2' '%3' '%8' | copy %4 link %5 menu %6 | cancel %7 right %9 ctrl %10 desk %11")
                  .arg(insertLabel, copyLabel, linkLabel)
                  .arg(copied.value(0).section(QLatin1Char('\\'), -1), linked.value(0).section(QLatin1Char('\\'), -1),
                       menuCopied.value(0).section(QLatin1Char('\\'), -1))
                  .arg(cancelled)
                  .arg(askLabel)
                  .arg(rightNoMenu)
                  .arg(ctrlNoMenu)
                  .arg(deskCancelled));
    }
    // Session 100: a widget goes into an EMPTY fence too -- from another fence
    // (the drag of its tile) and from the desktop (filed where it was let go).
    {
        ui::FenceContainer box;
        const QString from = box.createFence(QStringLiteral("From"), QRect(0, 0, 400, 300));
        const QString empty = box.createFence(QStringLiteral("Empty"), QRect(600, 0, 400, 300));
        const QString empty2 = box.createFence(QStringLiteral("Empty2"), QRect(0, 500, 400, 300));
        core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
        box.addWidget(from, clock);
        QString wpath;
        if (ui::FenceView* v = box.view(from))
            for (const core::DesktopItem& item : v->items())
                if (item.isWidget())
                    wpath = item.path;
        for (const QString& id : {from, empty, empty2})
            if (ui::FenceView* v = box.view(id)) {
                if (offscreen)
                    v->setAttribute(Qt::WA_DontShowOnScreen);
                v->show();
            }
        wait(100);
        const QPoint target = box.fenceRect(empty).center();
        // The gesture itself, as the fence's widget sees it: press on the tile,
        // move past the drag distance, out over the empty fence, let go.
        int moved = 0;
        if (ui::FenceView* v = box.view(from)) {
            const QRect tile = v->tileRects().value(0);
            const QPoint origin = box.physicalFromFenceLocal(from, QPoint(0, 0));
            const QPoint outside = target - origin; // the source's local frame (dpr 1 here)
            const auto mouse = [v](QEvent::Type type, const QPoint& at, Qt::MouseButton b, Qt::MouseButtons held) {
                QMouseEvent e(type, QPointF(at), QPointF(v->mapToGlobal(at)), b, held, Qt::NoModifier);
                QApplication::sendEvent(v, &e);
            };
            mouse(QEvent::MouseButtonPress, tile.center(), Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, tile.center() + QPoint(12, 12), Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
            moved = box.view(empty) ? box.view(empty)->items().size() : -1;
        }
        const int inEmpty = box.view(empty) ? box.view(empty)->items().size() : -1;
        const int left = box.view(from) ? box.view(from)->items().size() : -1;
        core::DesktopItem cpu = ui::widgets::create(QStringLiteral("cpu"));
        const int filed = box.fileIntoAt(empty2, {cpu}, box.fenceRect(empty2).center());
        // The empty fence shows where it would land (the bar), as a full one does.
        bool barShown = false;
        if (ui::FenceView* e2 = box.view(empty2)) {
            core::DesktopItem gone = ui::widgets::create(QStringLiteral("clock"));
            const QString emptier = box.createFence(QStringLiteral("Empty3"), QRect(600, 500, 400, 300));
            if (ui::FenceView* e3 = box.view(emptier)) {
                if (offscreen)
                    e3->setAttribute(Qt::WA_DontShowOnScreen);
                e3->show();
                wait(50);
                const QImage before = e3->grab().toImage();
                e3->setExternalDropIndex(0, false);
                const QImage during = e3->grab().toImage();
                during.save(captures + QStringLiteral("/s100_empty_fence_bar.png"));
                barShown = before != during;
                e3->setExternalDropIndex(-1, false);
            }
            Q_UNUSED(e2);
            Q_UNUSED(gone);
        }
        check(QStringLiteral("a widget goes into an empty fence (the whole gesture), and the empty fence shows the bar (session 100)"),
              moved == 1 && inEmpty == 1 && left == 0 && filed == 1 && barShown,
              QStringLiteral("moved %1 inEmpty %2 left %3 filed %4 bar %5").arg(moved).arg(inEmpty).arg(left).arg(filed).arg(barShown));
        // Session 100: a locked widget -- no drag, no resize mode, kept in the layout.
        bool lockHeld = false, noResize = false, lockSaved = false;
        if (ui::FenceView* v = box.view(empty)) {
            QList<core::DesktopItem> items = v->items();
            if (!items.isEmpty()) {
                core::DesktopItem w = items.first();
                w.widgetSettings.insert(QStringLiteral("locked"), true);
                box.replaceFenceItem(empty, w.path, w);
                noResize = !box.beginWidgetResize(empty, w.path);
                const QRect tile = v->tileRects().value(0);
                const QPoint origin = box.physicalFromFenceLocal(empty, QPoint(0, 0));
                const QPoint over = box.fenceRect(from).center() - origin;
                const auto mouse = [v](QEvent::Type type, const QPoint& at, Qt::MouseButton b, Qt::MouseButtons held) {
                    QMouseEvent e(type, QPointF(at), QPointF(v->mapToGlobal(at)), b, held, Qt::NoModifier);
                    QApplication::sendEvent(v, &e);
                };
                mouse(QEvent::MouseButtonPress, tile.center(), Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseMove, tile.center() + QPoint(12, 12), Qt::NoButton, Qt::LeftButton);
                mouse(QEvent::MouseMove, over, Qt::NoButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, over, Qt::LeftButton, Qt::NoButton);
                lockHeld = v->items().size() == 1 && box.view(from)->items().isEmpty();
                for (const core::Fence& fence : box.toLayout().fences)
                    for (const core::FenceItem& item : fence.items)
                        lockSaved = lockSaved || item.widgetSettings.value(QStringLiteral("locked")).toBool();
            }
        }
        check(QStringLiteral("a locked widget in a fence: not dragged, no resize mode, saved locked (session 100)"),
              lockHeld && noResize && lockSaved,
              QStringLiteral("held %1 noResize %2 saved %3").arg(lockHeld).arg(noResize).arg(lockSaved));
    }

    // Session 101: Bouton -- collapsed disc, expands, icons on a ring.
    {
        ui::FenceContainer box;
        const QString id = box.createButton(QStringLiteral("Apps"), QPoint(400, 300));
        ui::FenceView* v = box.view(id);
        bool born = false, collapsed = false, expanded = false, ring = false;
        QRect visCollapsed, visExpanded;
        if (v) {
            if (offscreen)
                v->setAttribute(Qt::WA_DontShowOnScreen);
            born = ui::zone_shape::isButton(v->shape()) && v->rolledUp();
            visCollapsed = box.fenceRect(id); // stored
            // Visible collapsed size via geometry helper
            const QRect small = ui::zone_shape::buttonCollapsedRect(box.fenceRect(id), 48);
            collapsed = small.width() <= 48 * 2 + 16 && small.height() == small.width();
            QList<core::DesktopItem> items;
            for (int i = 0; i < 5; ++i) {
                core::DesktopItem it;
                it.path = QStringLiteral("::s101/item%1").arg(i);
                it.name = QStringLiteral("Item %1").arg(i);
                items << it;
            }
            v->addDroppedItems(items);
            box.setFenceRolledUp(id, false);
            expanded = !v->rolledUp();
            visExpanded = box.fenceRect(id);
            const QVector<QRect> rects = ui::zone_shape::buttonItemRects(v->contentRect(), v->items().size(), 48);
            ring = rects.size() == v->items().size() && rects.size() >= 3;
            // Distinct positions (not a grid row)
            if (ring) {
                QSet<int> ys;
                for (const QRect& r : rects)
                    ys.insert(r.center().y() / 8);
                ring = ys.size() >= 2;
            }
            v->show();
            wait(100);
            v->grab().save(captures + QStringLiteral("/s101_button_expanded.png"));
            box.setFenceRolledUp(id, true);
            wait(100);
            v->grab().save(captures + QStringLiteral("/s101_button_collapsed.png"));
            // layout keeps shape
            bool saved = false;
            for (const core::Fence& f : box.toLayout().fences)
                saved = saved || f.style.shape == QLatin1String("button");
            check(QStringLiteral("Bouton: born collapsed, expands, ring layout, shape in layout (session 101)"),
                  born && collapsed && expanded && ring && saved,
                  QStringLiteral("born %1 coll %2 exp %3 ring %4 saved %5 items %6 [s101_button_*.png]")
                      .arg(born).arg(collapsed).arg(expanded).arg(ring).arg(saved).arg(v->items().size()));
        } else {
            check(QStringLiteral("Bouton: born collapsed, expands, ring layout, shape in layout (session 101)"),
                  false, QStringLiteral("no view"));
        }
    }

    // Session 104: Bouton ring layout — 0 = top, clockwise; extra rings if needed;
    // no overlap of icon+label boxes; all inside the expanded circle. n = 1..20.
    {
        const int icon = 48;
        bool allOk = true;
        QStringList fail;
        for (int n = 1; n <= 20; ++n) {
            const ui::zone_shape::ButtonLayout lay = ui::zone_shape::buttonLayout(n, icon);
            const double cx = lay.diameter / 2.0;
            const double cy = lay.diameter / 2.0;
            const double R = lay.diameter / 2.0 - 0.5;
            if (lay.items.size() != n || lay.diameter < icon * 2) {
                allOk = false;
                fail << QStringLiteral("n=%1 count/diam").arg(n);
                continue;
            }
            bool overlap = false, outside = false;
            for (int i = 0; i < lay.items.size(); ++i) {
                const QRect a = lay.items.at(i).bounds();
                for (const QPoint& p : {a.topLeft(), a.topRight(), a.bottomLeft(), a.bottomRight()}) {
                    const double dx = p.x() - cx, dy = p.y() - cy;
                    if (dx * dx + dy * dy > R * R + 1.0)
                        outside = true;
                }
                for (int j = i + 1; j < lay.items.size(); ++j) {
                    if (a.intersects(lay.items.at(j).bounds()))
                        overlap = true;
                }
            }
            // n=2: top and bottom; n=3: one icon at the top.
            if (n == 2 || n == 3) {
                const QRect first = lay.items.at(0).icon;
                if (first.center().y() > int(cy) - 4)
                    fail << QStringLiteral("n=%1 first not at top").arg(n);
            }
            if (overlap || outside)
                fail << QStringLiteral("n=%1 overlap=%2 outside=%3 d=%4")
                            .arg(n).arg(overlap).arg(outside).arg(lay.diameter);
            if (overlap || outside)
                allOk = false;
        }
        // Screenshots of a real Bouton window at 3, 6 and 14 icons.
        ui::FenceContainer box;
        const QString id = box.createButton(QStringLiteral("Apps"), QPoint(500, 400));
        ui::FenceView* v = box.view(id);
        if (v) {
            if (offscreen)
                v->setAttribute(Qt::WA_DontShowOnScreen);
            auto grabN = [&](int n, const QString& name) {
                QList<core::DesktopItem> items;
                for (int i = 0; i < n; ++i) {
                    core::DesktopItem it;
                    it.path = QStringLiteral("::s104/item%1").arg(i);
                    it.name = QStringLiteral("Item %1").arg(i + 1);
                    items << it;
                }
                v->setItems(items);
                box.syncButtonSize(id);
                box.setFenceRolledUp(id, false);
                v->show();
                wait(80);
                v->grab().save(captures + QLatin1Char('/') + name);
            };
            grabN(3, QStringLiteral("s104_button_3.png"));
            grabN(6, QStringLiteral("s104_button_6.png"));
            grabN(14, QStringLiteral("s104_button_14.png"));
        } else {
            allOk = false;
            fail << QStringLiteral("no view");
        }
        check(QStringLiteral("Bouton: ring layout, no overlap, boxes inside circle (n=1..20, session 104)"),
              allOk && fail.isEmpty(),
              fail.isEmpty() ? QStringLiteral("[s104_button_3/6/14.png]")
                             : fail.join(QLatin1Char(' ')));
    }

    // Session 105: Bouton expand/collapse keeps a circle, a stable collapsed
    // anchor (including near a screen edge), and a locked Bouton never moves
    // when the pointer sits on its border.
    {
        const auto dummyItems = [](int n, const QString& tag) {
            QList<core::DesktopItem> items;
            for (int i = 0; i < n; ++i) {
                core::DesktopItem it;
                it.path = QStringLiteral("::%1/item%2").arg(tag).arg(i);
                it.name = QStringLiteral("Item %1").arg(i + 1);
                items << it;
            }
            return items;
        };
        const auto waitAnim = [&](ui::FenceContainer& box, const QString& id, bool* square) {
            int frames = 0;
            bool allSquare = true;
            QElapsedTimer clock;
            clock.start();
            while (box.fenceAnimating(id) && clock.elapsed() < 1200) {
                const QRect vis = box.fenceVisibleRect(id);
                allSquare = allSquare && vis.width() == vis.height() && vis.width() > 0;
                ++frames;
                wait(8);
            }
            const QRect end = box.fenceVisibleRect(id);
            allSquare = allSquare && end.width() == end.height() && end.width() > 0;
            if (square)
                *square = allSquare;
            return frames;
        };

        ui::FenceContainer mid;
        mid.setAnimateRoll(true);
        const QPoint midCenter(400, 300);
        const QString midId = mid.createButton(QStringLiteral("Mid"), midCenter);
        ui::FenceView* midView = mid.view(midId);
        bool midOk = midView != nullptr;
        QString midDetail;
        if (midView) {
            if (offscreen)
                midView->setAttribute(Qt::WA_DontShowOnScreen);
            midView->setItems(dummyItems(6, QStringLiteral("s105mid")));
            mid.syncButtonSize(midId);
            const QRect stored0 = mid.fenceRect(midId);
            const QRect coll0 = mid.fenceVisibleRect(midId);
            mid.setFenceRolledUp(midId, false);
            bool expandSquare = false;
            const int expandFrames = waitAnim(mid, midId, &expandSquare);
            const QRect storedOpen = mid.fenceRect(midId);
            const QRect visOpen = mid.fenceVisibleRect(midId);
            mid.setFenceRolledUp(midId, true);
            bool collapseSquare = false;
            const int collapseFrames = waitAnim(mid, midId, &collapseSquare);
            const QRect storedBack = mid.fenceRect(midId);
            const QRect collBack = mid.fenceVisibleRect(midId);
            const QPoint midStoredC = stored0.center();
            const bool centerOk = qAbs(midStoredC.x() - midCenter.x()) <= 1
                                  && qAbs(midStoredC.y() - midCenter.y()) <= 1;
            // The contract is the cycle: every frame is a circle and the stored
            // anchor does not move. createButton may sit a couple of pixels off
            // the requested point (QRect::center rounding); that is not a walk.
            midOk = expandSquare && collapseSquare && expandFrames > 2 && collapseFrames > 2
                    && stored0 == storedOpen && stored0 == storedBack
                    && coll0 == collBack
                    && visOpen.width() == visOpen.height();
            midDetail = QStringLiteral("frames %1/%2 square %3/%4 stored kept %5 back %6 center %7,%8 (want %9,%10 near %13) vis %11x%12")
                            .arg(expandFrames).arg(collapseFrames)
                            .arg(expandSquare).arg(collapseSquare)
                            .arg(stored0 == storedOpen).arg(coll0 == collBack)
                            .arg(midStoredC.x()).arg(midStoredC.y()).arg(midCenter.x()).arg(midCenter.y())
                            .arg(visOpen.width()).arg(visOpen.height()).arg(centerOk);
        }
        check(QStringLiteral("Bouton: expand/collapse stays a circle, collapsed anchor unchanged (session 105)"),
              midOk, midDetail);

        ui::FenceContainer edge;
        edge.setAnimateRoll(true);
        edge.setScreens(ui::ScreenLayout::single(QSize(1920, 1080)));
        const QPoint edgeCenter(80, 70);
        const QString edgeId = edge.createButton(QStringLiteral("Edge"), edgeCenter);
        ui::FenceView* edgeView = edge.view(edgeId);
        bool edgeOk = edgeView != nullptr;
        QString edgeDetail;
        if (edgeView) {
            if (offscreen)
                edgeView->setAttribute(Qt::WA_DontShowOnScreen);
            edgeView->setItems(dummyItems(8, QStringLiteral("s105edge")));
            edge.syncButtonSize(edgeId);
            const QRect stored0 = edge.fenceRect(edgeId);
            const QRect coll0 = edge.fenceVisibleRect(edgeId);
            const QRect screen(0, 0, 1920, 1080);
            edge.setFenceRolledUp(edgeId, false);
            bool expandSquare = false;
            waitAnim(edge, edgeId, &expandSquare);
            const QRect visOpen = edge.fenceVisibleRect(edgeId);
            const QRect storedOpen = edge.fenceRect(edgeId);
            const bool onScreen = screen.contains(visOpen);
            const bool shifted = visOpen.center() != stored0.center();
            edge.setFenceRolledUp(edgeId, true);
            bool collapseSquare = false;
            waitAnim(edge, edgeId, &collapseSquare);
            const QRect storedBack = edge.fenceRect(edgeId);
            const QRect collBack = edge.fenceVisibleRect(edgeId);
            const QPoint edgeStoredC = stored0.center();
            edgeOk = expandSquare && collapseSquare && onScreen && shifted
                     && stored0 == storedOpen && stored0 == storedBack
                     && coll0 == collBack
                     && visOpen.width() == visOpen.height();
            edgeDetail = QStringLiteral("onScreen %1 shifted %2 stored kept %3 back %4 center %5,%6 (want %7,%8) vis %9,%10 %11x%12 square %13/%14")
                             .arg(onScreen).arg(shifted).arg(stored0 == storedOpen).arg(coll0 == collBack)
                             .arg(edgeStoredC.x()).arg(edgeStoredC.y()).arg(edgeCenter.x()).arg(edgeCenter.y())
                             .arg(visOpen.x()).arg(visOpen.y()).arg(visOpen.width()).arg(visOpen.height())
                             .arg(expandSquare).arg(collapseSquare);
        }
        check(QStringLiteral("Bouton: near-edge expand stays on-screen and returns to the same anchor (session 105)"),
              edgeOk, edgeDetail);

        ui::FenceContainer locked;
        locked.setAnimateRoll(true);
        locked.setScreens(ui::ScreenLayout::single(QSize(1920, 1080)));
        const QPoint lockCenter(90, 80);
        const QString lockId = locked.createButton(QStringLiteral("Lock"), lockCenter);
        ui::FenceView* lockView = locked.view(lockId);
        bool lockOk = lockView != nullptr;
        QString lockDetail;
        if (lockView) {
            if (offscreen)
                lockView->setAttribute(Qt::WA_DontShowOnScreen);
            lockView->setItems(dummyItems(8, QStringLiteral("s105lock")));
            locked.syncButtonSize(lockId);
            lockView->show();
            wait(40);
            locked.setFenceLocked(lockId, true);
            const QRect stored0 = locked.fenceRect(lockId);
            const QRect coll0 = locked.fenceVisibleRect(lockId);
            const QPoint c = lockView->rect().center();
            const int rad = qMin(lockView->width(), lockView->height()) / 2;
            const QPoint rim(c.x() + qMax(0, rad - 1), c.y());
            const bool noHandle = lockView->handleAt(rim) == ui::FenceHandle::None
                                  && lockView->handleAt(c) == ui::FenceHandle::None
                                  && lockView->rawHandleAt(rim) == ui::FenceHandle::None;
            const auto mouse = [lockView](QEvent::Type type, const QPoint& at, Qt::MouseButtons buttons) {
                QMouseEvent e(type, QPointF(at), QPointF(lockView->mapToGlobal(at)),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                QApplication::sendEvent(lockView, &e);
            };
            mouse(QEvent::MouseButtonPress, rim, Qt::LeftButton);
            mouse(QEvent::MouseMove, rim + QPoint(40, 25), Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, rim + QPoint(40, 25), Qt::NoButton);
            locked.resizeFenceBy(lockId, ui::FenceHandle::TitleBar, QPoint(50, 40));
            locked.resizeFenceBy(lockId, ui::FenceHandle::BottomRight, QPoint(24, 24));
            locked.setFenceRect(lockId, stored0.translated(80, 60), false);
            const bool unmovedAfterDrag = locked.fenceRect(lockId) == stored0
                                          && locked.fenceVisibleRect(lockId) == coll0;
            QEnterEvent enter(QPointF(rim), QPointF(rim), QPointF(lockView->mapToGlobal(rim)));
            QCoreApplication::sendEvent(lockView, &enter);
            wait(ui::FenceContainer::kHoverUnrollDelayMs + 200);
            waitAnim(locked, lockId, nullptr);
            const QRect storedHover = locked.fenceRect(lockId);
            const QRect visHover = locked.fenceVisibleRect(lockId);
            const QRect screen(0, 0, 1920, 1080);
            const bool hoverAnchorHeld = storedHover == stored0;
            const bool hoverOnScreen = !lockView->rolledUp() ? screen.contains(visHover) : true;
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(lockView, &leave);
            wait(ui::FenceContainer::kHoverRerollDelayMs + 200);
            waitAnim(locked, lockId, nullptr);
            const bool back = locked.fenceRect(lockId) == stored0 && locked.fenceVisibleRect(lockId) == coll0;
            locked.setFenceRolledUp(lockId, false);
            waitAnim(locked, lockId, nullptr);
            const QRect visOpen = locked.fenceVisibleRect(lockId);
            locked.setFenceRolledUp(lockId, true);
            waitAnim(locked, lockId, nullptr);
            const bool cycleBack = locked.fenceRect(lockId) == stored0 && locked.fenceVisibleRect(lockId) == coll0
                                   && screen.contains(visOpen) && visOpen.width() == visOpen.height();
            lockOk = noHandle && unmovedAfterDrag && hoverAnchorHeld && hoverOnScreen && back && cycleBack;
            lockDetail = QStringLiteral("handle %1 drag %2 hoverAnchor %3 onScreen %4 back %5 cycle %6")
                             .arg(noHandle).arg(unmovedAfterDrag).arg(hoverAnchorHeld)
                             .arg(hoverOnScreen).arg(back).arg(cycleBack);
            lockView->hide();
        }
        check(QStringLiteral("locked Bouton: no border drag/resize, no leftover shift, returns to the same anchor (session 105)"),
              lockOk, lockDetail);
    }


    // Session 106: a Bouton must not walk toward the top-left across hover
    // expand/collapse cycles -- including from the bottom-right, at odd sizes
    // and at a fractional DPR. The collapsed centre and the stored rect are
    // bit-identical before and after. A locked Bouton never moves.
    {
        const auto dummyItems = [](int n, const QString& tag) {
            QList<core::DesktopItem> items;
            for (int i = 0; i < n; ++i) {
                core::DesktopItem it;
                it.path = QStringLiteral("::%1/item%2").arg(tag).arg(i);
                it.name = QStringLiteral("Item %1").arg(i + 1);
                items << it;
            }
            return items;
        };
        const auto waitAnim = [&](ui::FenceContainer& box, const QString& id) {
            QElapsedTimer clock;
            clock.start();
            QPoint firstCenter;
            bool haveFirst = false;
            bool centerHeld = true;
            int frames = 0;
            while (box.fenceAnimating(id) && clock.elapsed() < 1200) {
                const QRect vis = box.fenceVisibleRect(id);
                const QPoint c = vis.center();
                if (!haveFirst) {
                    firstCenter = c;
                    haveFirst = true;
                } else if (c != firstCenter && vis.width() > 0) {
                    // Near-edge fit may lerp the centre once; mid-screen must not.
                    centerHeld = centerHeld && (c == firstCenter);
                }
                ++frames;
                wait(8);
            }
            return std::pair<int, bool>{frames, centerHeld};
        };
        const auto inkCenter = [](const QImage& img) -> QPoint {
            if (img.isNull())
                return {};
            qreal sx = 0, sy = 0;
            int n = 0;
            const int w = img.width(), h = img.height();
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (qAlpha(img.pixel(x, y)) > 64) {
                        sx += x;
                        sy += y;
                        ++n;
                    }
                }
            }
            if (n == 0)
                return {};
            return QPoint(int(qRound(sx / n)), int(qRound(sy / n)));
        };
        const auto withDpr = [](qreal dpr) {
            ui::ScreenLayout layout;
            ui::ScreenInfo screen;
            screen.geometry = QRect(0, 0, 1920, 1080);
            screen.primary = true;
            screen.devicePixelRatio = dpr;
            layout.addScreen(screen);
            return layout;
        };

        // The pre-s106 top-left walk, in two parts that the old code exhibited
        // and the new helpers must not.
        // 1) Rebuilding a square as QRect(c.x() - d/2, ..., d, d) from
        //    QRect::center() walks one pixel toward the origin per cycle when
        //    the diameter is even (center() is (x1+x2)/2, not x + w/2).
        // 2) At a fractional DPR, qRound(qRound(d/dpr)*dpr) is not always d,
        //    so a widget painted from the top-left of the unrounded buffer is
        //    clipped (or leaves leftover pixels) and the ink sits off-centre.
        bool oldRecipeWalks = false;
        {
            const int d = 360;
            QRect r(400 - d / 2, 300 - d / 2, d, d);
            const QPoint start = r.topLeft();
            for (int i = 0; i < 8; ++i) {
                const QPoint c = r.center();
                r = QRect(c.x() - d / 2, c.y() - d / 2, d, d);
            }
            if (r.topLeft() != start)
                oldRecipeWalks = true;
        }
        bool oldDprMismatches = false;
        bool newRecipeStable = true;
        for (qreal dpr : {1.25, 1.5}) {
            for (int d = 80; d <= 520; ++d) {
                const int side = qMax(1, qRound(d / dpr));
                const int painted = qMax(1, qRound(side * dpr));
                if (painted != d)
                    oldDprMismatches = true;
                const QPoint anchor(400, 300);
                const int snapped = ui::zone_shape::buttonPhysicalDiameter(d, dpr);
                const QRect sq = ui::zone_shape::buttonSquareAt(anchor, snapped);
                if (sq.center() != anchor)
                    newRecipeStable = false;
                const int side2 = qMax(1, qRound(snapped / dpr));
                if (qMax(1, qRound(side2 * dpr)) != snapped)
                    newRecipeStable = false;
                // Rebuilding from .center() must be involutive (the old walk).
                QRect cycle = sq;
                for (int i = 0; i < 8; ++i)
                    cycle = ui::zone_shape::buttonSquareAt(cycle.center(), snapped);
                if (cycle != sq || cycle.center() != anchor)
                    newRecipeStable = false;
            }
        }

        bool allOk = oldRecipeWalks && oldDprMismatches && newRecipeStable;
        QStringList detail;
        if (!oldRecipeWalks)
            detail << QStringLiteral("old recipe did not walk from center()");
        if (!oldDprMismatches)
            detail << QStringLiteral("old recipe did not show a DPR mismatch");
        if (!newRecipeStable)
            detail << QStringLiteral("snapped square moved the anchor");

        const auto runCycles = [&](qreal dpr, int nItems, const QPoint& want, bool lock, const QString& tag) {
            ui::FenceContainer box;
            box.setAnimateRoll(true);
            box.setScreens(withDpr(dpr));
            const QString id = box.createButton(QStringLiteral("Drift"), want);
            ui::FenceView* v = box.view(id);
            if (!v) {
                allOk = false;
                detail << tag + QStringLiteral(" no view");
                return;
            }
            if (offscreen)
                v->setAttribute(Qt::WA_DontShowOnScreen);
            v->setItems(dummyItems(nItems, tag));
            box.syncButtonSize(id);
            v->show();
            wait(40);
            if (lock)
                box.setFenceLocked(id, true);
            const QRect stored0 = box.fenceRect(id);
            const QRect coll0 = box.fenceVisibleRect(id);
            const QPoint storedC0 = stored0.center();
            const bool placed = qAbs(storedC0.x() - want.x()) <= 0 && qAbs(storedC0.y() - want.y()) <= 0;
            if (!placed) {
                allOk = false;
                detail << tag + QStringLiteral(" placed at %1,%2 want %3,%4")
                                .arg(storedC0.x()).arg(storedC0.y()).arg(want.x()).arg(want.y());
            }
            box.renderFence(id);
            const QPoint ink0 = inkCenter(box.frame(id));
            const QPoint inkMid0(coll0.width() / 2, coll0.height() / 2);

            const auto enterBR = [v] {
                const QPoint at(qMax(0, v->width() - 2), qMax(0, v->height() - 2));
                QEnterEvent e(QPointF(at), QPointF(at), QPointF(v->mapToGlobal(at)));
                QCoreApplication::sendEvent(v, &e);
            };
            const auto leaveEv = [v] {
                QEvent e(QEvent::Leave);
                QCoreApplication::sendEvent(v, &e);
            };

            bool cycleOk = true;
            bool animCenterHeld = true;
            for (int i = 0; i < 10; ++i) {
                enterBR();
                wait(ui::FenceContainer::kHoverUnrollDelayMs + 160);
                const auto exp = waitAnim(box, id);
                animCenterHeld = animCenterHeld && (exp.first == 0 || exp.second
                    || box.fenceVisibleRect(id).center() == storedC0
                    || QRect(0, 0, 1920, 1080).contains(box.fenceRect(id).adjusted(-20, -20, 20, 20)));
                // Mid-screen: the shown centre must stay the stored centre.
                if (QRect(200, 200, 1520, 680).contains(want)) {
                    if (box.fenceRect(id) != stored0)
                        cycleOk = false;
                    const QRect vis = box.fenceVisibleRect(id);
                    if (vis.center() != storedC0 && !box.fenceAnimating(id)) {
                        // Expanded mid-screen should keep the same centre.
                        if (vis.center() != storedC0)
                            cycleOk = false;
                    }
                }
                leaveEv();
                wait(ui::FenceContainer::kHoverRerollDelayMs + 160);
                waitAnim(box, id);
                box.setFenceRolledUp(id, true);
                waitAnim(box, id);
                if (box.fenceRect(id) != stored0 || box.fenceVisibleRect(id) != coll0)
                    cycleOk = false;
            }
            // Explicit roll cycles (same assertion).
            for (int i = 0; i < 6; ++i) {
                box.setFenceRolledUp(id, false);
                waitAnim(box, id);
                if (QRect(200, 200, 1520, 680).contains(want) && box.fenceVisibleRect(id).center() != storedC0)
                    cycleOk = false;
                box.setFenceRolledUp(id, true);
                waitAnim(box, id);
                if (box.fenceRect(id) != stored0 || box.fenceVisibleRect(id) != coll0)
                    cycleOk = false;
            }
            if (lock) {
                box.resizeFenceBy(id, ui::FenceHandle::TitleBar, QPoint(30, -20));
                box.setFenceRect(id, stored0.translated(-12, -9), false);
                if (box.fenceRect(id) != stored0 || box.fenceVisibleRect(id) != coll0)
                    cycleOk = false;
            }
            box.renderFence(id);
            const QPoint ink1 = inkCenter(box.frame(id));
            const bool inkStable = ink0.isNull() || ink1.isNull() || ink0 == ink1;
            const bool inkNotUpLeft = ink0.isNull()
                || (ink0.x() >= inkMid0.x() - 1 && ink0.y() >= inkMid0.y() - 1);
            if (!cycleOk || !inkStable || !inkNotUpLeft) {
                allOk = false;
                detail << tag + QStringLiteral(" cycle=%1 stored %2,%3 vis %4,%5 ink %6,%7->%8,%9 mid %10,%11")
                                .arg(cycleOk)
                                .arg(box.fenceRect(id).x()).arg(box.fenceRect(id).y())
                                .arg(box.fenceVisibleRect(id).x()).arg(box.fenceVisibleRect(id).y())
                                .arg(ink0.x()).arg(ink0.y()).arg(ink1.x()).arg(ink1.y())
                                .arg(inkMid0.x()).arg(inkMid0.y());
            }
            v->hide();
        };

        // Mid-screen even size, fractional DPR, approach from the bottom-right.
        runCycles(1.25, 6, QPoint(400, 300), false, QStringLiteral("dpr125-n6"));
        // Odd item count (odd diameter) at 150 %.
        runCycles(1.5, 5, QPoint(640, 400), false, QStringLiteral("dpr15-n5"));
        // Locked, mid-screen.
        runCycles(1.25, 7, QPoint(500, 360), true, QStringLiteral("locked"));

        check(QStringLiteral("Bouton: no top-left drift across hover cycles, odd sizes, fractional DPR; locked stays put (session 106)"),
              allOk,
              detail.isEmpty() ? QStringLiteral("10 hover + 6 roll cycles, dpr 1.25/1.5")
                               : detail.join(QLatin1Char(' ')));
    }


    // Session 99: the drop menu is the desktop menu's code (buildModernMenu),
    // the setting is in Settings > Fences. Side by side for the eye.
    {
        const ui::ModernMenuStyle style = ui::ModernMenuStyle::current();
        const std::vector<core::ContextMenuItem> dropItems = ui::dropMenuItems();
        std::vector<core::ContextMenuItem> deskItems;
        const auto deskItem = [&deskItems](const QString& label, ushort glyph) {
            core::ContextMenuItem item;
            item.label = label;
            item.glyph = QString(QChar(glyph));
            deskItems.push_back(item);
        };
        deskItem(QStringLiteral("View"), 0xE8FD);
        deskItem(QStringLiteral("Sort by"), 0xE8CB);
        deskItem(QStringLiteral("Refresh"), 0xE72C);
        deskItems.push_back([] { core::ContextMenuItem l; l.separator = true; return l; }());
        deskItem(QStringLiteral("New"), 0xE710);
        deskItem(QStringLiteral("StarFish: create a fence here"), 0xE80A);
        const auto shot = [&](const std::vector<core::ContextMenuItem>& items) {
            std::unique_ptr<QMenu> m(ui::buildModernMenu(items, style, [](const core::ContextMenuItem&) {}));
            if (offscreen)
                m->setAttribute(Qt::WA_DontShowOnScreen);
            m->popup(QPoint(40, 40));
            wait(200);
            const QPixmap px = m->grab();
            const int rows = m->actions().size();
            m->hide();
            return std::make_pair(px, rows);
        };
        const auto [dropShot, dropRows] = shot(dropItems);
        const auto [deskShot, deskRows] = shot(deskItems);
        QImage pair(dropShot.width() + deskShot.width() + 30, std::max(dropShot.height(), deskShot.height()) + 20,
                    QImage::Format_ARGB32_Premultiplied);
        pair.fill(QColor(0x3A, 0x4A, 0x5A));
        {
            QPainter pp(&pair);
            pp.drawPixmap(10, 10, dropShot);
            pp.drawPixmap(20 + dropShot.width(), 10, deskShot);
        }
        const bool saved = pair.save(captures + QStringLiteral("/s99_drop_menu.png"));
        int defaults = 0;
        QStringList labels;
        for (const core::ContextMenuItem& item : dropItems) {
            defaults += item.isDefault ? 1 : 0;
            if (!item.separator)
                labels << item.label;
        }
        auto* box = settings.findChild<QCheckBox*>(QStringLiteral("explorerDropMenuSwitch"));
        check(QStringLiteral("drop menu: the desktop menu's look, 4 entries, Move default; its switch in Settings (session 99)"),
              saved && dropRows == 5 && deskRows == 6 && labels.size() == 4 && defaults == 1 && box,
              QStringLiteral("%1 | rows %2/%3 | switch %4 [s99_drop_menu.png]")
                  .arg(labels.join(QStringLiteral(" / ")))
                  .arg(dropRows)
                  .arg(deskRows)
                  .arg(box != nullptr));
    }
    // Session 98: a widget's resize mode -- outline on its exact bounds, an
    // edge pulled in whole cells, Esc ends it, a locked fence refuses, 10 s idle.
    {
        const QString pic = QDir(QStringLiteral("build")).absoluteFilePath(QStringLiteral("s98_dummy.png"));
        {
            QImage wide(800, 400, QImage::Format_ARGB32);
            QPainter wp(&wide);
            QLinearGradient g(0, 0, 800, 0);
            g.setColorAt(0, QColor(0x3D, 0x8A, 0xFF));
            g.setColorAt(1, QColor(0x86, 0x2A, 0x5B));
            wp.fillRect(wide.rect(), g);
            wp.end();
            wide.save(pic);
        }
        ui::FenceContainer box;
        const QString zone = box.createFence(QStringLiteral("Zone"), QRect(0, 0, 460, 360));
        core::DesktopItem img = ui::widgets::create(QStringLiteral("image"));
        img.widgetSettings.insert(QStringLiteral("path"), pic);
        img.widgetSettings.insert(QStringLiteral("fit"), QStringLiteral("fit"));
        img.span = QSize(2, 2);
        box.addWidget(zone, img);
        ui::FenceView* v = box.view(zone);
        QString wpath;
        QSize spanAfter;
        bool began = false, followed = false, escEnds = false, lockedRefused = false, idleEnds = false;
        QRect outline;
        if (v) {
            for (const core::DesktopItem& item : v->items())
                if (item.isWidget())
                    wpath = item.path;
            if (offscreen)
                v->setAttribute(Qt::WA_DontShowOnScreen);
            v->show();
            wait(150);
            began = box.beginWidgetResize(zone, wpath);
            outline = v->widgetResizeRect();
            v->grab().save(captures + QStringLiteral("/s98_resize_outline.png"));
            const QPoint corner = outline.bottomRight();
            const QPoint to = corner + QPoint(outline.width() / 2 + 4, outline.height() / 2 + 4);
            const auto mouse = [v](QEvent::Type type, const QPoint& at, Qt::MouseButtons buttons) {
                QMouseEvent e(type, QPointF(at), QPointF(v->mapToGlobal(at)),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                QApplication::sendEvent(v, &e);
            };
            mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton);
            mouse(QEvent::MouseMove, to, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
            wait(80);
            for (const core::DesktopItem& item : v->items())
                if (item.path == wpath)
                    spanAfter = item.span;
            followed = v->widgetResizePath() == wpath && v->widgetResizeRect().width() > outline.width();
            v->grab().save(captures + QStringLiteral("/s98_resize_after.png"));
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(v, &esc);
            escEnds = v->widgetResizePath().isEmpty();
            box.setFenceLocked(zone, true);
            lockedRefused = !box.beginWidgetResize(zone, wpath);
            box.setFenceLocked(zone, false);
            box.beginWidgetResize(zone, wpath);
            wait(10400);
            idleEnds = v->widgetResizePath().isEmpty();
            v->hide();
        }
        QFile::remove(pic);
        check(QStringLiteral("widget resize mode: outline = its bounds, corner pulled one cell = 3 x 3, Esc ends, locked refuses, ends after 10 s (session 98)"),
              began && !outline.isEmpty() && spanAfter == QSize(3, 3) && followed && escEnds && lockedRefused && idleEnds,
              QStringLiteral("outline %1x%2 span %3x%4 esc %5 locked %6 idle %7 [s98_resize_outline.png]")
                  .arg(outline.width()).arg(outline.height()).arg(spanAfter.width()).arg(spanAfter.height())
                  .arg(escEnds).arg(lockedRefused).arg(idleEnds));
    }
    settings.hide();
    lines << QStringLiteral("%1 failure(s)").arg(failures);
    QFile report(QStringLiteral("build/ui-probe.txt"));
    if (report.open(QIODevice::WriteOnly | QIODevice::Text))
        report.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    QTextStream(stdout) << lines.join(QLatin1Char('\n')) << '\n';
    return failures == 0 ? 0 : 1;
}

} // namespace starfish::app
