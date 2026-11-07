#include "app/readme_probe.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>

#include "core/i18n.hpp"
#include "core/system_metrics.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_view.hpp"
#include "ui/fence_widgets.hpp"
#include "ui/widget_settings_dialog.hpp"

namespace starfish::app {

namespace {

QImage wallpaper(const QSize& size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&image);
    QLinearGradient base(0, 0, size.width(), size.height());
    base.setColorAt(0.0, QColor(0x0F, 0x1E, 0x3A));
    base.setColorAt(0.55, QColor(0x2B, 0x4C, 0x7E));
    base.setColorAt(1.0, QColor(0x6D, 0x5B, 0x97));
    p.fillRect(image.rect(), base);
    QRadialGradient glow(QPointF(size.width() * 0.72, size.height() * 0.35), size.width() * 0.45);
    glow.setColorAt(0, QColor(0x8E, 0xC5, 0xFF, 0x50));
    glow.setColorAt(1, QColor(0x8E, 0xC5, 0xFF, 0));
    p.fillRect(image.rect(), glow);
    return image;
}

} // namespace

int runReadmeProbe(QApplication& app)
{
    Q_UNUSED(app);
    core::i18n::install(QStringLiteral("en")); // the README is in English
    QStringList report;
    const QString out = QStringLiteral("docs/images");
    QDir().mkpath(out);

    // Dummy files, generic names, deleted at the end (QTemporaryDir).
    QTemporaryDir temp;
    const QString root = QDir::fromNativeSeparators(temp.path());
    const auto file = [&](const QString& name) {
        const QString path = root + QLatin1Char('/') + name;
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("demo");
        return path;
    };
    const auto folder = [&](const QString& name) {
        const QString path = root + QLatin1Char('/') + name;
        QDir().mkpath(path);
        return path;
    };
    platform::win::ShellIconCache icons;
    const auto items = [&](const QStringList& paths) {
        QList<core::DesktopItem> list;
        for (const QString& path : paths) {
            core::DesktopItem item = platform::win::DesktopScanner::itemForPath(path);
            item.name = QFileInfo(path).completeBaseName().isEmpty() ? QFileInfo(path).fileName()
                                                                    : QFileInfo(path).completeBaseName();
            if (QFileInfo(path).isDir())
                item.name = QFileInfo(path).fileName();
            list << item;
        }
        return list;
    };

    const QSize canvasSize(1600, 900);
    QImage canvas = wallpaper(canvasSize);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);

    struct Spec {
        QString title;
        QRect rect;
        QColor panel;
        QColor band;
        bool tab = false;
        bool rolled = false;
        QStringList paths;
    };
    const QList<Spec> specs{
        {QStringLiteral("Work"), QRect(48, 48, 430, 250), QColor(0x1F, 0x29, 0x37, 0xE0), QColor(), false, false,
         {file(QStringLiteral("Quarterly report.txt")), file(QStringLiteral("Budget.csv")),
          file(QStringLiteral("Meeting notes.txt")), folder(QStringLiteral("Clients")),
          folder(QStringLiteral("Archive")), file(QStringLiteral("Roadmap.txt"))}},
        {QStringLiteral("Documents"), QRect(512, 48, 400, 250), QColor(0x1E, 0x3A, 0x5F, 0xD0), QColor(), true, false,
         {folder(QStringLiteral("Invoices")), folder(QStringLiteral("Letters")), file(QStringLiteral("Checklist.txt")),
          file(QStringLiteral("Travel plan.txt"))}},
        {QStringLiteral("Games"), QRect(48, 340, 430, 230), QColor(0x18, 0x18, 0x2A, 0xD8), QColor(0x7C, 0x3A, 0xED),
         false, false,
         {folder(QStringLiteral("Racing")), folder(QStringLiteral("Puzzle")), folder(QStringLiteral("Strategy")),
          folder(QStringLiteral("Saves"))}},
        {QStringLiteral("Tools (rolled up)"), QRect(512, 340, 400, 230), QColor(0x0F, 0x76, 0x6E, 0xD8), QColor(),
         false, true, {file(QStringLiteral("setup.ini")), file(QStringLiteral("tasks.bat"))}},
        {QStringLiteral("Downloads"), QRect(48, 612, 864, 240), QColor(0x11, 0x18, 0x27, 0xC8), QColor(0x25, 0x63, 0xEB),
         false, false,
         {folder(QStringLiteral("Wallpapers")), folder(QStringLiteral("Fonts")), file(QStringLiteral("Manual.txt")),
          file(QStringLiteral("Recipe.txt")), file(QStringLiteral("Packing list.txt")), folder(QStringLiteral("Music")),
          file(QStringLiteral("Changelog.txt"))}},
    };
    for (const Spec& spec : specs) {
        ui::FenceView view;
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.setIconProvider([&icons](const core::DesktopItem& item, int size) { return icons.iconFor(item.path, size, false); });
        view.setTitle(spec.title);
        view.setPanelColor(spec.panel);
        view.setCornerRadius(10);
        view.setBorder(1, QColor(0x3B, 0x82, 0xF6, 0x90));
        view.setTitleBarColor(spec.band);
        view.setTabTitle(spec.tab);
        view.setLabelsShown(true);
        view.setItems(items(spec.paths));
        QSize size = spec.rect.size();
        if (spec.rolled) {
            view.setRolledUp(true);
            size.setHeight(view.metrics().titleBarHeight);
        }
        view.resize(size);
        view.show();
        QApplication::processEvents();
        QImage shot(size, QImage::Format_ARGB32_Premultiplied);
        shot.fill(Qt::transparent);
        {
            QPainter vp(&shot);
            view.render(&vp, QPoint(), QRegion(), QWidget::DrawChildren);
        }
        view.hide();
        painter.drawImage(spec.rect.topLeft(), shot);
    }

    // Widgets on the desktop, with fake readings (no real hardware names).
    static core::MetricHistory fake[core::kMetricCount] = {core::MetricHistory(300), core::MetricHistory(300),
                                                           core::MetricHistory(300), core::MetricHistory(300)};
    const double gb = 1024.0 * 1024.0 * 1024.0;
    for (int i = 0; i < 300; ++i) {
        const double t = i / 8.0;
        core::MetricPoint cpu, ram, gpu, vram;
        cpu.percent = float(24 + 14 * std::sin(t) + 6 * std::sin(t * 2.7));
        ram.percent = float(46 + 2 * std::sin(t / 6));
        ram.totalBytes = 32 * gb;
        ram.usedBytes = ram.totalBytes * ram.percent / 100;
        gpu.percent = float(std::max(2.0, 30 + 25 * std::sin(t / 2.3)));
        vram.percent = 38;
        vram.totalBytes = 12 * gb;
        vram.usedBytes = vram.totalBytes * 0.38;
        fake[0].push(cpu); fake[1].push(ram); fake[2].push(gpu); fake[3].push(vram);
    }
    ui::widgets::setMetricsSource([](core::Metric m) { return &fake[int(m)]; }, [](bool) {});
    ui::DesktopBackdrop desk; // never attached
    desk.setDesktopWidgetCell(QSize(100, 100));
    core::DesktopItem clock = ui::widgets::create(QStringLiteral("clock"));
    clock.widgetSettings.insert(QStringLiteral("date"), true);
    core::DesktopItem cpu = ui::widgets::create(QStringLiteral("cpu"));
    cpu.widgetSettings.insert(QStringLiteral("mode"), QStringLiteral("graph"));
    core::DesktopItem ram = ui::widgets::create(QStringLiteral("ram"));
    ram.widgetSettings.insert(QStringLiteral("mode"), QStringLiteral("graph"));
    core::DesktopItem group = ui::widgets::create(QStringLiteral("system"));
    core::DesktopItem note = ui::widgets::withNoteText(
        ui::widgets::create(QStringLiteral("notes")),
        QStringLiteral("Ideas for the weekend\n- hike by the lake\n- try the new pasta place\n- tidy the desktop"));
    note.widgetSettings.insert(QStringLiteral("backdrop"), true);
    desk.setDesktopWidgets({{clock, QPoint(1340, 40)}, {cpu, QPoint(1340, 150)}, {ram, QPoint(1340, 250)},
                            {group, QPoint(1340, 360)}, {note, QPoint(1100, 520)}});
    desk.paintDesktopWidgets(painter, QRect(QPoint(0, 0), canvasSize), QRect(QPoint(0, 0), canvasSize));
    painter.end();
    canvas.convertToFormat(QImage::Format_RGB32).save(out + QStringLiteral("/hero.png"));
    report << QStringLiteral("hero.png %1x%2").arg(canvas.width()).arg(canvas.height());

    // Details: the widgets column, the fences, the settings window.
    canvas.copy(QRect(1080, 20, 500, 720)).convertToFormat(QImage::Format_RGB32).save(out + QStringLiteral("/widgets.png"));
    canvas.copy(QRect(24, 24, 912, 850)).convertToFormat(QImage::Format_RGB32).save(out + QStringLiteral("/zones.png"));
    {
        core::DesktopItem settingsOf = ui::widgets::create(QStringLiteral("system"));
        settingsOf.widgetSettings.insert(QStringLiteral("mode"), QStringLiteral("graph"));
        ui::WidgetSettingsDialog dialog(settingsOf);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QApplication::processEvents();
        dialog.grab().save(out + QStringLiteral("/widget-settings.png"));
        dialog.hide();
    }
    report << QStringLiteral("widgets.png zones.png widget-settings.png");
    QFile reportFile(QStringLiteral("build/readme-probe.txt"));
    if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text))
        reportFile.write(report.join(QLatin1Char('\n')).toUtf8() + '\n');
    return 0;
}

} // namespace starfish::app
