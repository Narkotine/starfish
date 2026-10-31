#include "app/ui_probe.hpp"

#include <algorithm>
#include <functional>
#include <cmath>
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
#include "ui/modern_menu.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tray_icon.hpp"
#include "platform/win/shell_operations.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "core/shortcut_vault.hpp"
#include <QKeyEvent>

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
        const bool renamed = platform::win::renameShellItem(bRenamed, QStringLiteral("b by F2 s74.txt"), 0, &newPath);
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

    settings.hide();
    lines << QStringLiteral("%1 failure(s)").arg(failures);
    QFile report(QStringLiteral("build/ui-probe.txt"));
    if (report.open(QIODevice::WriteOnly | QIODevice::Text))
        report.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    QTextStream(stdout) << lines.join(QLatin1Char('\n')) << '\n';
    return failures == 0 ? 0 : 1;
}

} // namespace starfish::app
