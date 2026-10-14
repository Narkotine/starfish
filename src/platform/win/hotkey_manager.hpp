#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <Qt>

namespace starfish::platform::win {

class HotkeyManager;

// The window procedure of the message-only hotkey window. Declared here only so
// the class can befriend it; it is not part of the public API.
qintptr starfishHotkeyProc(quintptr hwnd, unsigned message, quintptr wParam, quintptr lParam);

// Global hotkeys (F-021 Peek, F-022 quick hide) through RegisterHotKey, hosted
// on a message-only window owned by this class.
//
// Two things are deliberate:
//
//  * **Failures are reported, never swallowed.** Win+Space -- the combo
//    docs/produit.md gives for Peek -- is already owned by the Windows input
//    language switcher on most systems, so RegisterHotKey fails with
//    ERROR_HOTKEY_ALREADY_REGISTERED (1409). A silent failure would leave the
//    user pressing a key that does nothing; the caller gets a false back and the
//    probe prints it.
//  * **No visible window.** The host is a HWND_MESSAGE window, so registering a
//    hotkey never puts anything on screen and never touches the desktop
//    placement machinery of ADR-4.
class HotkeyManager : public QObject
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // Registers `key` with `modifiers` under `id`. Returns false when Windows
    // refuses the combination (already taken by the system or another app) or
    // when the message window could not be created. `id` must be > 0.
    bool registerHotkey(int id, Qt::KeyboardModifiers modifiers, Qt::Key key);

    void unregisterHotkey(int id);
    void unregisterAll();
    bool isRegistered(int id) const { return m_registered.contains(id); }

    // Human-readable form ("Win+Space"), for reports and error messages.
    static QString describe(Qt::KeyboardModifiers modifiers, Qt::Key key);

signals:
    // The user pressed the registered combination. `id` is the one passed to
    // registerHotkey().
    void triggered(int id);

private:
    friend qintptr starfishHotkeyProc(quintptr hwnd, unsigned message, quintptr wParam,
                                      quintptr lParam);

    void ensureWindow();
    void onHotkey(unsigned id);

    quintptr m_hwnd = 0;
    QSet<int> m_registered;
};

} // namespace starfish::platform::win
