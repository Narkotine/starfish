#include "app/ui_probe.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QGuiApplication>
#include <QListWidget>
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

    // --- the tray icon and its menu --------------------------------------------
    ui::TrayIcon tray;
    const bool shown = tray.show();
    tray.setFenceCount(3);
    tray.setFenceMaterial(QStringLiteral("translucent"));
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
