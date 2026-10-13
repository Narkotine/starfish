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

    // Re-applies the placement and z-order. Call it after the desktop changed
    // size or after something was stacked above the icon layer.
    void syncZOrder();

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

    quintptr m_hwnd = 0;
    quintptr m_iconHost = 0; // Progman on Win11; the DefView-hosting WorkerW on Win10
    quintptr m_defView = 0;
    QRect m_rect;
    QImage m_frame;
    QTimer m_watchdog;
    bool m_placed = false;

    // Paint-cycle counters, for paintReport().
    int m_paintCount = 0;
    int m_eraseCount = 0;
    int m_lastClipBox = -1;
    int m_lastBlitLines = -1;
    QRect m_lastUpdateRegion;
};

} // namespace starfish::platform::win
