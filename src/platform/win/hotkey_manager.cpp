#include "platform/win/hotkey_manager.hpp"

#include <QDebug>
#include <QKeySequence>
#include <QLoggingCategory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace starfish::platform::win {

namespace {

QLoggingCategory lc("starfish.hotkey");

constexpr wchar_t kWindowClass[] = L"StarFishHotkeyWindow";

inline HWND toHwnd(quintptr handle)
{
    return reinterpret_cast<HWND>(handle);
}

inline quintptr fromHwnd(HWND hwnd)
{
    return reinterpret_cast<quintptr>(hwnd);
}

// Qt modifiers -> RegisterHotKey modifier bits.
UINT toWinModifiers(Qt::KeyboardModifiers modifiers)
{
    UINT win = 0;
    if (modifiers & Qt::ShiftModifier)
        win |= MOD_SHIFT;
    if (modifiers & Qt::ControlModifier)
        win |= MOD_CONTROL;
    if (modifiers & Qt::AltModifier)
        win |= MOD_ALT;
    if (modifiers & Qt::MetaModifier)
        win |= MOD_WIN;
    return win;
}

// Qt key -> virtual-key code. Only what a global hotkey plausibly needs: a
// letter, a digit, a function key or Space. 0 means "unsupported".
UINT toVirtualKey(Qt::Key key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Space:
        return VK_SPACE;
    case Qt::Key_Tab:
        return VK_TAB;
    case Qt::Key_Escape:
        return VK_ESCAPE;
    default:
        return 0;
    }
}

} // namespace

qintptr starfishHotkeyProc(quintptr hwnd, unsigned message, quintptr wParam, quintptr lParam)
{
    auto* manager = reinterpret_cast<HotkeyManager*>(
        ::GetWindowLongPtrW(toHwnd(hwnd), GWLP_USERDATA));

    if (message == WM_HOTKEY && manager) {
        manager->onHotkey(static_cast<unsigned>(wParam));
        return 0;
    }
    return ::DefWindowProcW(toHwnd(hwnd), message, static_cast<WPARAM>(wParam),
                            static_cast<LPARAM>(lParam));
}

HotkeyManager::HotkeyManager(QObject* parent)
    : QObject(parent)
{
}

HotkeyManager::~HotkeyManager()
{
    unregisterAll();
    if (m_hwnd && ::IsWindow(toHwnd(m_hwnd))) {
        ::SetWindowLongPtrW(toHwnd(m_hwnd), GWLP_USERDATA, 0);
        ::DestroyWindow(toHwnd(m_hwnd));
    }
    m_hwnd = 0;
}

void HotkeyManager::ensureWindow()
{
    if (m_hwnd && ::IsWindow(toHwnd(m_hwnd)))
        return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // No CS_DBLCLKS, no background brush: the window is never shown.
        wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&starfishHotkeyProc);
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            qCCritical(lc) << "RegisterClassExW failed, error" << ::GetLastError();
            return;
        }
        registered = true;
    }

    // HWND_MESSAGE: a message-only window. It is never composited, never
    // appears in the z-order and cannot interfere with the desktop placement of
    // ADR-4 -- it exists only to receive WM_HOTKEY.
    HWND hwnd = ::CreateWindowExW(0, kWindowClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        qCCritical(lc) << "CreateWindowExW(HWND_MESSAGE) failed, error" << ::GetLastError();
        return;
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_hwnd = fromHwnd(hwnd);
}

bool HotkeyManager::registerHotkey(int id, Qt::KeyboardModifiers modifiers, Qt::Key key)
{
    if (id <= 0)
        return false;

    ensureWindow();
    if (!m_hwnd)
        return false;

    const UINT vk = toVirtualKey(key);
    if (vk == 0) {
        qCWarning(lc).noquote()
            << QStringLiteral("unsupported key for a global hotkey: %1").arg(describe(modifiers, key));
        return false;
    }

    // Re-registering the same id is a no-op, so callers can be idempotent.
    if (m_registered.contains(id))
        unregisterHotkey(id);

    ::SetLastError(0);
    if (!::RegisterHotKey(toHwnd(m_hwnd), id, toWinModifiers(modifiers), vk)) {
        const DWORD error = ::GetLastError();
        qCWarning(lc).noquote()
            << QStringLiteral("RegisterHotKey(%1) refused, error %2%3")
                   .arg(describe(modifiers, key))
                   .arg(error)
                   .arg(error == ERROR_HOTKEY_ALREADY_REGISTERED
                            ? QStringLiteral(" (already owned by the system or another app)")
                            : QString());
        return false;
    }

    m_registered.insert(id);
    qCInfo(lc).noquote()
        << QStringLiteral("hotkey %1 registered for id %2").arg(describe(modifiers, key)).arg(id);
    return true;
}

void HotkeyManager::unregisterHotkey(int id)
{
    if (!m_registered.contains(id))
        return;
    if (m_hwnd && ::IsWindow(toHwnd(m_hwnd)))
        ::UnregisterHotKey(toHwnd(m_hwnd), id);
    m_registered.remove(id);
}

void HotkeyManager::unregisterAll()
{
    const QSet<int> ids = m_registered;
    for (const int id : ids)
        unregisterHotkey(id);
}

void HotkeyManager::onHotkey(unsigned id)
{
    emit triggered(static_cast<int>(id));
}

QString HotkeyManager::describe(Qt::KeyboardModifiers modifiers, Qt::Key key)
{
    // Built by hand rather than through QKeySequence(modifiers | key): "Win" is
    // what a Windows user reads on the box, where Qt would print "Meta".
    QStringList parts;
    if (modifiers & Qt::ControlModifier)
        parts << QStringLiteral("Ctrl");
    if (modifiers & Qt::ShiftModifier)
        parts << QStringLiteral("Shift");
    if (modifiers & Qt::AltModifier)
        parts << QStringLiteral("Alt");
    if (modifiers & Qt::MetaModifier)
        parts << QStringLiteral("Win");
    parts << QKeySequence(static_cast<int>(key)).toString(QKeySequence::NativeText);
    return parts.join(QLatin1Char('+'));
}

} // namespace starfish::platform::win
