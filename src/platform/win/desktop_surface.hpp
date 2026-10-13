#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QtGlobal>

namespace starfish::platform::win {

class DesktopSurface;

// The window procedure of a hosted surface. Declared here only so the class can
// befriend it; it is not part of the public API.
qintptr starfishSurfaceProc(quintptr hwnd, unsigned message, quintptr wParam, quintptr lParam);

// A raw Win32 window hosted on the desktop -- above the wallpaper, below the
// native icon layer -- that displays a Qt-rendered frame.
//
// WHY NOT A QWIDGET (measured, session 8 / T-020)
// ----------------------------------------------
// A Qt window reparented under Progman stops being composited by DWM. Qt keeps
// working on its side: the widget is visible, mapped and exposed, it receives
// paint events, and `QWidget::grab()` returns the expected 480x320 image. But
// nothing reaches the screen, and neither does a manual GDI blit issued from
// *inside* the window's own WM_PAINT through a subclassed WndProc -- with a
// full 480x320 update region and StretchDIBits reporting 320 scan lines
// written, while `PrintWindow` reads back a black surface.
//
// The same rectangle, hosted with the same sequence by a plain Win32 window
// (the T-003 spike, experiments/spike_workerw.py) is visible and covers 96.6%
// of its area. So the placement technique is sound and the Qt window is what
// breaks: the hosted surface therefore owns its WndProc and paints through the
// normal WM_PAINT cycle, while Qt only ever renders offscreen.
//
// Owning the WndProc also removes both Qt traps documented in ADR-4: nothing
// can drop WS_EX_LAYERED behind our back, and nothing can re-stack the window
// above the icon layer.
class DesktopSurface : public QObject
{
    Q_OBJECT

public:
    explicit DesktopSurface(QObject* parent = nullptr);
    ~DesktopSurface() override;

    DesktopSurface(const DesktopSurface&) = delete;
    DesktopSurface& operator=(const DesktopSurface&) = delete;

    // True when Progman and SHELLDLL_DefView are found (i.e. Explorer is up).
    static bool isDesktopAvailable();

    // Creates the window and hosts it just below the icon layer. `rect` is in
    // Progman client coordinates, which match the primary monitor on a single
    // screen (multi-screen offsetting is T-014).
    bool attach(const QRect& rect);

    // Destroys the window.
    void detach();

    bool isAttached() const;
    quintptr hwnd() const;

    // Re-places the window, keeping it anchored below the icon layer.
    bool setGeometry(const QRect& rect);
    QRect geometry() const;

    // Frame to display. The image is copied, so the caller may release it.
    // A null image clears the surface.
    void setFrame(const QImage& frame);
    QImage frame() const;

    // Per-pixel transparency mode (T-017). When enabled, frames are pushed
    // with UpdateLayeredWindow (premultiplied alpha honoured, the wallpaper
    // shows through translucent pixels) instead of being blitted from
    // WM_PAINT with StretchDIBits (which ignores alpha). Measured in session 7
    // to fail with ERROR_INVALID_PARAMETER on a child window *created by Qt*;
    // this class owns its window, so the call has a chance. Returns false if
    // the first push failed.
    bool setPerPixelAlpha(bool enabled);
    bool perPixelAlpha() const;

    // Re-applies the placement and z-order. Call it after the desktop changed
    // size or after something was stacked above the icon layer.
    void syncZOrder();

    // Routes mouse input coming from this surface's own WndProc to a target
    // QObject (typically the FenceView rendered into this surface), so selection
    // and double-click work on the live desktop. Coordinates are the surface's
    // client coordinates, which match the fence's local coordinates because the
    // fence is rendered at (0,0) of the frame. Set to nullptr to stop routing.
    void setInputTarget(QObject* target);
    QObject* inputTarget() const;

    // One-line report of what the paint cycle actually did: how many WM_PAINTs
    // were handled, the update region, the clip box and the number of scan
    // lines written. A paint that reports success while writing nothing is the
    // signature this project keeps running into, so it has to be observable.
    QString paintReport() const;

signals:
    // The desktop window was destroyed (Explorer restart) and the surface is
    // gone: the owner has to attach() again.
    void desktopLost();

private:
    friend qintptr starfishSurfaceProc(quintptr hwnd, unsigned message, quintptr wParam,
                                       quintptr lParam);

    bool ensureWindowClass();
    bool place();
    bool refreshDesktopHandles();
    void watchdogTick();

    // Returns true when the message was fully handled.
    bool handleMessage(quintptr hwnd, unsigned message, quintptr wParam, quintptr lParam,
                       qintptr* result);
    void paintInto(quintptr dc);

    // Pushes m_frame through UpdateLayeredWindow (per-pixel alpha path).
    bool pushLayeredFrame();

    quintptr m_hwnd = 0;
    quintptr m_iconHost = 0; // Progman on Win11; the DefView-hosting WorkerW on Win10
    quintptr m_defView = 0;
    QRect m_rect;
    QImage m_frame;
    QTimer m_watchdog;
    bool m_placed = false;
    bool m_perPixelAlpha = false;

    QObject* m_inputTarget = nullptr;
    bool m_leftDown = false;
    bool m_rightDown = false;

    // Builds a QMouseEvent from a surface mouse message and posts it to the
    // input target (see setInputTarget). Coordinates are client-local, which
    // match the fence's local coordinates because the fence is rendered at
    // (0,0) of the frame.
    void forwardMouse(unsigned message, quintptr lParam);

    // Paint-cycle counters, for paintReport().
    int m_paintCount = 0;
    int m_eraseCount = 0;
    int m_lastClipBox = -1;
    int m_lastBlitLines = -1;
    QRect m_lastUpdateRegion;
};

} // namespace starfish::platform::win
