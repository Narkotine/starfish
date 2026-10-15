#pragma once

#include <functional>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

namespace starfish::platform::win {

class ShellDropTarget;

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

    // The native desktop icons (ADR-3). The icon layer belongs to the desktop,
    // not to one surface, so these are static: the product hides the icons once
    // and restores them on the way out. ShowWindow on SHELLDLL_DefView is
    // reversible and writes nothing to the registry (ADR-4).
    static bool setDesktopIconsHidden(bool hidden);
    static bool desktopIconsHidden();

    // Creates the window and hosts it just below the icon layer. `rect` is in
    // Progman client coordinates -- physical pixels of Windows' virtual screen
    // (T-014), which is also what a fence's stored rect uses.
    bool attach(const QRect& rect);

    // Destroys the window.
    void detach();

    bool isAttached() const;
    quintptr hwnd() const;

    // Re-places the window, keeping it anchored below the icon layer.
    bool setGeometry(const QRect& rect);
    QRect geometry() const;

    // Quick hide (F-022): takes the surface off screen without destroying it.
    // The state is remembered so the 2 s watchdog re-place does not put a hidden
    // fence back on screen.
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }

    // Peek (F-021): raises the surface above the icon layer while active.
    // Within this window model -- a child of Progman -- HWND_TOP is the
    // frontmost position a fence can reach, i.e. above SHELLDLL_DefView.
    // Floating above *application* windows would need a top-level window, which
    // is a different ownership model and is not part of v1.
    void setRaisedAboveIcons(bool raised);
    bool raisedAboveIcons() const { return m_raisedAboveIcons; }

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

    // The scaling of the monitor this surface sits on (T-014). A surface owns a
    // raw Win32 window, whose coordinates are physical pixels, while the Qt
    // widget rendered into it works in logical pixels: mouse positions coming
    // from this window's WndProc are divided by this ratio before being posted
    // to the input target. 1.0 (the default) is the 100 % monitor, where the
    // two spaces coincide.
    void setDevicePixelRatio(qreal ratio);
    qreal devicePixelRatio() const { return m_dpr; }

    // Routes mouse input coming from this surface's own WndProc to a target
    // QObject (typically the FenceView rendered into this surface), so selection
    // and double-click work on the live desktop. Coordinates are the surface's
    // client coordinates converted to the *widget's* logical coordinates, which
    // match the fence's local coordinates because the fence is rendered at
    // (0,0) of the frame. Set to nullptr to stop routing.
    void setInputTarget(QObject* target);
    QObject* inputTarget() const;

    // Handler invoked when the Explorer drops files onto this surface (T-006-3).
    // `clientPos` is in the surface's **physical** client pixels (unlike the
    // mouse events above: this one is OLE's, not ours); paths are the dropped
    // file/folder paths. Set before attach() so the OLE drop target is
    // registered against the window.
    void setDropHandler(std::function<void(const QPoint&, const QStringList&)> handler);
    const std::function<void(const QPoint&, const QStringList&)>& dropHandler() const;

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
    // T-014: the monitor's scaling, used to convert the physical mouse
    // coordinates this window receives into the widget's logical ones.
    qreal m_dpr = 1.0;
    bool m_placed = false;
    bool m_perPixelAlpha = false;
    // Navigation state (T-009), both honoured by place() so the watchdog cannot
    // undo them.
    bool m_visible = true;
    bool m_raisedAboveIcons = false;

    QObject* m_inputTarget = nullptr;
    bool m_leftDown = false;
    bool m_rightDown = false;
    // Last position seen from this surface's own messages, so a gesture lost to
    // WM_CAPTURECHANGED can be ended where it actually was.
    QPointF m_lastPos;
    QPointF m_lastScreenPos;

    // OLE drag-and-drop (T-006-3). The drop target is created on attach() and
    // registered with the window so the Explorer can drop files onto it; it is
    // revoked and released on detach(). OleInitialize/Uninitialize are balanced
    // against a single init on this thread.
    std::function<void(const QPoint&, const QStringList&)> m_dropHandler;
    ShellDropTarget* m_dropTarget = nullptr;
    bool m_oleInit = false;

    // Builds a QMouseEvent from a surface mouse message and posts it to the
    // input target (see setInputTarget). Coordinates are client-local, which
    // match the fence's local coordinates because the fence is rendered at
    // (0,0) of the frame.
    void forwardMouse(unsigned message, quintptr lParam);

    // Ends a gesture the widget still believes is running, by posting the
    // releases it is waiting for. Called when the mouse capture is taken away
    // (WM_CAPTURECHANGED): without it, a fence whose title bar was being dragged
    // when something else grabbed the mouse would follow the cursor forever.
    void releaseStuckGesture();

    // Paint-cycle counters, for paintReport().
    int m_paintCount = 0;
    int m_eraseCount = 0;
    int m_lastClipBox = -1;
    int m_lastBlitLines = -1;
    QRect m_lastUpdateRegion;
};

} // namespace starfish::platform::win
