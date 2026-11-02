#include "platform/win/startup_timeline.hpp"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QString>

#include <windows.h>

namespace starfish::platform::win::StartupTimeline {

namespace {
bool g_enabled = false;
QSet<QByteArray>& onceSeen()
{
    static QSet<QByteArray> seen;
    return seen;
}
QString logPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/StarFish");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/startup.log");
}
} // namespace

void enable()
{
    if (g_enabled)
        return;
    g_enabled = true;
    QFile file(logPath());
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
}

bool enabled()
{
    return g_enabled;
}

double now()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;
    FILETIME nowFt{};
    ::GetSystemTimePreciseAsFileTime(&nowFt);
    const auto ticks = [](const FILETIME& ft) {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return (ticks(nowFt) - ticks(created)) / 10000.0;
}

void mark(const char* what)
{
    if (!g_enabled)
        return;
    QFile file(logPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write(QStringLiteral("%1 ms  %2\n").arg(now(), 8, 'f', 1).arg(QString::fromUtf8(what)).toUtf8());
}

void markOnce(const char* what)
{
    if (!g_enabled || onceSeen().contains(QByteArray(what)))
        return;
    onceSeen().insert(QByteArray(what));
    mark(what);
}

} // namespace starfish::platform::win::StartupTimeline
