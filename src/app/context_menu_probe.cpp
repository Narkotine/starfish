#include "app/context_menu_probe.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include "platform/win/desktop_shell_view.hpp"

namespace starfish::app {

namespace {

void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void dump(QStringList& out, const std::vector<platform::win::DesktopShellView::ShellMenuItem>& items,
          const QString& indent)
{
    for (const auto& item : items) {
        if (item.separator) {
            out << indent + QStringLiteral("----");
            continue;
        }
        out << indent
                   + QStringLiteral("[%1] \"%2\" verb=\"%3\" cmd=%4%5%6%7")
                         .arg(item.children.empty() ? QStringLiteral(" ") : QStringLiteral(">"),
                              item.label, item.verb)
                         .arg(item.command)
                         .arg(item.enabled ? QString() : QStringLiteral(" disabled"),
                              item.checked ? QStringLiteral(" checked") : QString(),
                              item.shortcut.isEmpty() ? QString() : QStringLiteral(" tab=") + item.shortcut);
        dump(out, item.children, indent + QStringLiteral("    "));
    }
}

} // namespace

int runContextMenuProbe(QApplication& app)
{
    Q_UNUSED(app);
    QStringList log;
    platform::win::DesktopShellView view;
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString testFile = QDir(desktop).filePath(QStringLiteral("StarFish probe menu.txt"));
    {
        QFile f(testFile);
        if (f.open(QIODevice::WriteOnly))
            f.write("StarFish context menu probe\n");
    }
    const QString native = QDir::toNativeSeparators(testFile);
    for (int i = 0; i < 50; ++i) {
        bool seen = false;
        for (const auto& item : view.read().items)
            seen = seen || item.parsingName.compare(native, Qt::CaseInsensitive) == 0;
        if (seen)
            break;
        waitMs(100);
    }
    for (const auto& item : view.read().items) {
        if (item.parsingName.startsWith(QLatin1String("::")))
            log << QStringLiteral("virtual: %1 = %2").arg(item.parsingName, item.displayName);
    }
    log << QStringLiteral("== background");
    dump(log, view.openBackgroundMenuModel(), QString());
    const QStringList targets{native, QStringLiteral("::{645FF040-5081-101B-9F08-00AA002F954E}"),
                              QStringLiteral("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")};
    for (const QString& t : targets) {
        log << QStringLiteral("== %1 (select: %2)").arg(t).arg(view.select({t}));
        dump(log, view.openSelectionMenuModel({t}), QString());
    }
    // a shortcut and a folder from the desktop, whatever they are
    for (const auto& item : view.read().items) {
        if (item.parsingName.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)) {
            log << QStringLiteral("== (a shortcut)");
            dump(log, view.openSelectionMenuModel({item.parsingName}), QString());
            break;
        }
    }
    view.closeMenuModel();
    view.select({});
    QFile::remove(testFile);

    QFile report(QStringLiteral("build/context-menu-probe.txt"));
    if (report.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&report);
        for (const QString& line : log)
            out << line << '\n';
    }
    return 0;
}

} // namespace starfish::app
