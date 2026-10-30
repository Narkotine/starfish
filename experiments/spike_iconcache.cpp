#include <QGuiApplication>
#include <QPixmap>
#include "platform/win/shell_icon_cache.hpp"
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    starfish::platform::win::ShellIconCache cache;
    QPixmap p = cache.iconFor(QString::fromLocal8Bit(argv[1]), 48);
    p.save(QString::fromLocal8Bit(argv[2]));
    return p.isNull();
}
