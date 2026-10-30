#include "platform/win/autostart.hpp"

#include <QDir>
#include <QVector>

#include <windows.h>

namespace starfish::platform::win {

namespace {

const wchar_t* w(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

bool readString(const QString& key, const QString& name, QString* out)
{
    DWORD bytes = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, w(key), w(name), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       nullptr, nullptr, &bytes)
        != ERROR_SUCCESS)
        return false;
    QVector<wchar_t> buffer(int(bytes / sizeof(wchar_t)) + 1, 0);
    if (::RegGetValueW(HKEY_CURRENT_USER, w(key), w(name), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       nullptr, buffer.data(), &bytes)
        != ERROR_SUCCESS)
        return false;
    *out = QString::fromWCharArray(buffer.data());
    return true;
}

bool readBinary(const QString& key, const QString& name, QByteArray* out)
{
    DWORD bytes = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, w(key), w(name), RRF_RT_REG_BINARY, nullptr, nullptr, &bytes)
        != ERROR_SUCCESS)
        return false;
    out->resize(int(bytes));
    return ::RegGetValueW(HKEY_CURRENT_USER, w(key), w(name), RRF_RT_REG_BINARY, nullptr, out->data(), &bytes)
           == ERROR_SUCCESS;
}

bool deleteValue(const QString& key, const QString& name)
{
    const LSTATUS s = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, w(key), w(name));
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}

} // namespace

Autostart::Autostart(QString runKey, QString approvedKey, QString valueName)
    : m_runKey(std::move(runKey)), m_approvedKey(std::move(approvedKey)), m_valueName(std::move(valueName))
{
}

QString Autostart::commandFor(const QString& exePath)
{
    return QLatin1Char('"') + QDir::toNativeSeparators(QDir::cleanPath(exePath)) + QLatin1Char('"');
}

QString Autostart::exeOf(const QString& command)
{
    const QString c = command.trimmed();
    if (c.startsWith(QLatin1Char('"'))) {
        const int end = c.indexOf(QLatin1Char('"'), 1);
        return end > 0 ? c.mid(1, end - 1) : c.mid(1);
    }
    const int space = c.indexOf(QLatin1Char(' '));
    return space > 0 ? c.left(space) : c;
}

AutostartState Autostart::read() const
{
    AutostartState state;
    state.registered = readString(m_runKey, m_valueName, &state.command);
    QByteArray approved;
    if (readBinary(m_approvedKey, m_valueName, &approved) && !approved.isEmpty())
        state.disabledByUser = (quint8(approved.at(0)) & 1) != 0; // 02 on, 03 off
    return state;
}

bool Autostart::setEnabled(bool on, const QString& exePath)
{
    if (!on) {
        const bool run = deleteValue(m_runKey, m_valueName);
        deleteValue(m_approvedKey, m_valueName);
        return run;
    }
    const QString command = commandFor(exePath);
    const DWORD bytes = DWORD((command.size() + 1) * sizeof(wchar_t));
    if (::RegSetKeyValueW(HKEY_CURRENT_USER, w(m_runKey), w(m_valueName), REG_SZ, w(command), bytes)
        != ERROR_SUCCESS)
        return false;
    if (read().disabledByUser) {
        // What Task Manager writes for "Enabled".
        const BYTE enabled[12] = {0x02};
        ::RegSetKeyValueW(HKEY_CURRENT_USER, w(m_approvedKey), w(m_valueName), REG_BINARY, enabled,
                          sizeof(enabled));
    }
    return read().enabled();
}

bool Autostart::refresh(const QString& exePath)
{
    const AutostartState state = read();
    if (!state.registered)
        return false;
    const QString wanted = QDir::toNativeSeparators(QDir::cleanPath(exePath));
    if (exeOf(state.command).compare(wanted, Qt::CaseInsensitive) == 0)
        return false;
    const QString command = commandFor(exePath);
    const DWORD bytes = DWORD((command.size() + 1) * sizeof(wchar_t));
    return ::RegSetKeyValueW(HKEY_CURRENT_USER, w(m_runKey), w(m_valueName), REG_SZ, w(command), bytes)
           == ERROR_SUCCESS;
}

} // namespace starfish::platform::win
