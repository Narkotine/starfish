#include <QApplication>
#include <QTimer>
#include <QLoggingCategory>

#include "version.h"

namespace {
QLoggingCategory lc("starfish.main");
}

// Placeholder entry point. Renders nothing yet: the desktop host (WorkerW
// integration, see docs/technique.md ADR-4) lands in Phase 1/2.
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("StarFish");
    QApplication::setApplicationVersion(QStringLiteral(STARFISH_VERSION));

    qCInfo(lc) << "StarFish" << STARFISH_VERSION << "skeleton starting";

    // Keep the skeleton headless-friendly so CI can run it.
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
