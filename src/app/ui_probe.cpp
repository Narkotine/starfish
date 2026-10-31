#include "app/ui_probe.hpp"

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
