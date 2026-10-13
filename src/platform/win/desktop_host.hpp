#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QtGlobal>

#include "core/zorder.hpp"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace starfish::platform::win {

// Hosts StarFish windows on the Windows desktop: above the wallpaper, below
// the native icon layer. This is the C++/Qt port of the T-003 spike
// (experiments/spike_workerw.py); the reference sequence is ADR-4 in
// docs/technique.md:
//
//   1. create a frameless top-level window               (Qt::Window)
//   2. make Qt itself want WS_EX_LAYERED, then SetParent(hwnd, Progman) and
//      add WS_CHILD / drop WS_POPUP
//   3. SetWindowPos(hwnd, SHELLDLL_DefView, ...)         -> just below the icons
//
// The layered flag is MANDATORY. On recent Windows 11 builds a non-layered
// child of Progman is not composited by DWM: IsWindowVisible() is true and the
// z-order is correct, yet nothing is drawn -- a silent failure that cost two
// sessions (see ADR-4).
//
// Two Qt-specific traps, both measured in session 7 (ADR-4):
//   * Qt *removes* WS_EX_LAYERED shortly after attach() unless the widget's
//     opacity is below 1.0, and Windows then refuses to add it back to a
//     cross-process child -- silently (SetWindowLongPtr returns 0, GetLastError
//     stays 0). attach() therefore asks for a hair under full opacity and
//     verifies the flag instead of setting it.
//   * Qt no longer blits its backing store into the native window once that
//     window is a child of Progman, so the hosted window stays blank on screen.
//     The placement below is correct; the rendering path is still open (T-020).
//
// Two safety nets keep the placement honest:
//   * WM_WINDOWPOSCHANGING is rewritten on the fly so that Qt (or anything
//     else) can never stack a hosted window above the icon layer;
//   * a watchdog timer re-checks the desktop handles, because a WS_CHILD of
//     Progman no longer receives the WM_SETTINGCHANGE / WM_DISPLAYCHANGE
//     broadcasts that a top-level window would get.
class DesktopHost : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit DesktopHost(QObject* parent = nullptr);
    ~DesktopHost() override;

    // True when Progman and SHELLDLL_DefView were found (i.e. Explorer is up).
    bool isAvailable() const;

    // Reparents `window` to the desktop and places it at `rect`.
    // `window` must be a decoration-less top-level widget; it is shown as part
    // of the sequence. `rect` is in Progman client coordinates, which match
    // the primary monitor for the single-screen case (multi-screen offsetting
    // is T-014).
    bool attach(QWidget* window, const QRect& rect);

    // Moves a hosted window (keeps the z-order anchored below the icons).
    bool setGeometry(QWidget* window, const QRect& rect);

    // Returns the window to a plain top-level one.
    void detach(QWidget* window);

    // Re-applies parenting and z-order for every hosted window. Call it after
    // showing, resizing or re-creating a hosted window.
    void syncZOrder();

    // Reversible hiding of the native desktop icons (ShowWindow on DefView).
    // No registry write on the Win11 build tested (ADR-4).
    bool setIconsHidden(bool hidden);
    bool iconsHidden() const;

    // Uniform opacity of a hosted window (0-255). Per-pixel transparency --
    // needed to let the wallpaper show through a translucent fence background
    // -- is NOT supported yet: it requires UpdateLayeredWindow compositing.
    bool setWindowAlpha(QWidget* window, int alpha);

    // Compares the window's position in Progman's window chain with
    // SHELLDLL_DefView's. A correct verdict is necessary but NOT sufficient:
    // always cross-check visually (ADR-4).
    core::PlacementVerdict verifyPlacement(const QWidget* window);

    // Prints the desktop hierarchy (Progman -> DefView -> icon list) to the
    // log; used by the `--dump` diagnostic.
    static void dumpDesktopHierarchy();

signals:
    // The desktop was re-acquired (Explorer restart, resolution change). A
    // hosted window is a child of Progman and dies with it: owners must
    // re-create their windows and attach them again.
    void desktopReacquired();

    // A desktop settings change was observed (debounced).
    void desktopSettingsChanged();

protected:
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override;

private:
    struct Hosted {
        QWidget* widget = nullptr;
        quintptr hwnd = 0;
        QRect rect;
        int alpha = 255;
    };

    bool refreshDesktopHandles();
    void ensureFilterInstalled();
    void scheduleResync();
    void watchdogTick();
    bool applyPlacement(Hosted& hosted);
    int indexInDesktopChain(quintptr hwnd, int* defViewIndex) const;
    int indexOfHosted(const QWidget* window) const;

    QList<Hosted> m_hosted;
    quintptr m_iconHost = 0; // Progman on Win11; the DefView-hosting WorkerW on Win10
    quintptr m_defView = 0;
    bool m_filterInstalled = false;
    bool m_resyncing = false;
    bool m_handlesChanged = false;
    QTimer m_resyncTimer;  // debounces settings-change notifications
    QTimer m_watchdog;     // catches Explorer restarts and z-order drift
};

} // namespace starfish::platform::win
