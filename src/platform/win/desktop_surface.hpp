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

    // The lowest alpha any pixel of a per-pixel frame may carry, and the reason
    // it is not zero (T-060, ADR-11). It is a property of the layered window
    // itself, so it lives with the class that owns the window.
    //
    // A per-pixel layered window is hit-tested against its own alpha: Windows
    // lets the mouse through wherever the alpha is zero (MSDN, "Layered
    // Windows"). A frame that has to be transparent almost everywhere -- the
    // desktop backdrop, or the rounded corners of a translucent fence -- would
    // therefore be a window the cursor can only find in places, or not at all.
    // One level out of 255 is invisible on any wallpaper and turns the frame
    // back into a shape the cursor can find anywhere.
    // (Measured: with an alpha-0 frame the cursor finds the *parent*; at this
    // floor it finds the layered child itself --
    // experiments/spike_layer_alpha_shape.cpp.)
    static constexpr int kInputAlphaFloor = 1;

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

    // Where the surface sits among Progman's children (T-039), re-anchored by
    // ADR-12: the desktop's wallpaper belongs to SHELLDLL_DefView, which StarFish
    // no longer hides (it hides the list view inside it), so *every* surface of
    // ours has to sit above the DefView.
    //
    //   BelowIconLayer  the normal fence position: the top of Progman's child
    //                   order, above the backdrop and above the wallpaper.
    //   AboveIconLayer  Peek (F-021). It is the same position since ADR-12 --
    //                   a fence is already above everything the desktop paints,
    //                   which is why Peek has no visible effect left (T-053).
    //                   Floating above *application* windows would need a
    //                   top-level window, a different ownership model out of v1.
    //   Backdrop        the layer StarFish draws the desktop's own icons on
    //                   (ADR-9 option A). It takes the slot just above the
    //                   DefView -- under the fences, above the wallpaper, whose
    //                   pixels keep showing through it.
    enum class Layer {
        BelowIconLayer,
        AboveIconLayer,
        Backdrop,
    };

    void setLayer(Layer layer);
    Layer layer() const { return m_layer; }

    // Compatibility with the T-009 API: Peek is exactly the AboveIconLayer role.
    void setRaisedAboveIcons(bool raised);
    bool raisedAboveIcons() const { return m_layer == Layer::AboveIconLayer; }

    // A purely decorative surface must not eat the clicks meant for whatever is
    // under it. When set, WM_NCHITTEST answers HTTRANSPARENT, so Windows hands
    // the mouse to the next window down the z-order. This is the backdrop's
    // state until it is given the empty desktop's gestures (T-041), which are
    // the only thing it can honour today. Independent of the drop target: that
    // one follows setDropHandler (a surface with nothing to do with a drop must
    // not swallow it -- T-052).
    void setClickThrough(bool clickThrough);
    bool clickThrough() const { return m_clickThrough; }

    // ADR-21: the backdrop *is* the desktop -- the native list is hidden and every
    // icon on screen is ours. Nothing is under this window to step aside for, so
    // the backdrop answers HTCLIENT everywhere, left button included (the
    // ADR-13 step-asides below are the shared-desktop mode, and stay for it).
    // ADR-21: the drawn desktop owns all input -- its clicks take the keyboard
    // focus, and its keys (F2, Delete, Ctrl+C...) are posted to the input target
    // as QKeyEvents.
    void setOwnsAllInput(bool owns) { m_ownsAllInput = owns; }
    // ADR-21 lot 3: a drop target of the caller's own (an IDropTarget*, the
    // surface takes the reference it is given), built once the window exists.
    // Takes precedence over the drop handler.
    void setDropTargetFactory(std::function<void*(quintptr hwnd)> factory)
    {
        m_dropTargetFactory = std::move(factory);
    }
    void* customDropTarget() const { return m_customDropTarget; }
    bool ownsAllInput() const { return m_ownsAllInput; }

    // The rectangles the *native* desktop icons occupy, in this window's own
    // (physical) coordinates -- client coordinates, because that is what a child
    // window's hit test works in.
    //
    // With ADR-13 StarFish no longer hides the desktop's icons: they stay where
    // the user put them and keep their behaviour, and a surface of ours sits
    // *above* them. The mouse has to keep reaching them, so the hit test steps
    // aside exactly where an icon is (see WM_NCHITTEST). Taken and replayed
    // would be a different click (T-042); stepped aside is the same click.
    // Filled by whoever just read the list view (platform/win/desktop_icons).
    void setNativeIconRects(const QVector<QRect>& rects) { m_nativeIconRects = rects; }
    const QVector<QRect>& nativeIconRects() const { return m_nativeIconRects; }

    // Frame to display. The image is copied, so the caller may release it.
    // A null image clears the surface.
    // Session 63: `frame` differs from the last one only inside `dirty`
    // (frame pixels): those rows are copied and only that rectangle repainted.
    // Same size and blit path required -- otherwise this is setFrame().
    void updateFrame(const QImage& frame, const QRect& dirty);
    void setFrame(const QImage& frame);
    // T-098: paints what is invalid now (UpdateWindow), so a moved window shows
    // its new pixels with its new place.
    void repaintNow();
    // Session 64: the fences' stacking is the container's. `raiseToTop` puts
    // this window above its siblings (a fence that is clicked); `isAbove` reads
    // the real Win32 order, for the container and the probes.
    void raiseToTop();
    bool isAbove(const DesktopSurface* other) const;
    QImage frame() const;

    // ⚠️ ADR-15 -- the uniform alpha, which is the path every fence takes.
    //
    // The per-pixel push at the bottom of this block does NOT compose on a child
    // of Progman: the call returns TRUE and the pixels stay the wallpaper.
    // Measured 2026-10-18 with six test windows side by side on the user's own
    // desktop -- the UpdateLayeredWindow one invisible, the
    // SetLayeredWindowAttributes one beside it visible, same rect, same process,
    // both layered, both children of Progman. Every fence has gone down that path
    // since T-075, so **every fence was invisible**, which is the user's report
    // verbatim ("je ne vois plus les fences ... même en matériel opaque").
    //
    // A fence therefore shows itself the way Windows has always shown a shaped
    // window: one uniform alpha for the whole window (which is exactly what a
    // material *is* -- a single-colour panel has no per-pixel alpha to express),
    // a rounded window *region* (that is what removes the pixels outside the
    // panel, so the corners are truly transparent), and the frame blitted from
    // WM_PAINT -- which ignores alpha, and no longer matters because the region
    // has already taken the corners away.
    //
    // The cost, stated rather than hidden: a region is 1-bit, so the corner edge
    // is not antialiased, and the corner pixels are no longer the window's -- a
    // click exactly on a rounded corner falls through to the desktop, where
    // ADR-11's alpha floor used to keep it. That floor existed only because the
    // per-pixel path was the one doing the rounding; it has no object left.
    bool setUniformAlpha(int alpha);
    int uniformAlpha() const { return m_uniformAlpha; }

    // The rounded panel of the window, as a region: 0 = no region (square).
    // A region lives in window coordinates, so place() re-applies it whenever the
    // window's size changes.
    void setCornerRadius(int radiusPx);
    int cornerRadius() const { return m_cornerRadius; }

    // ⚠️ T-082 -- the *other* shape a hosted surface can take: the **union of
    // these rectangles** (window coordinates, physical pixels) instead of the
    // rounded panel. A fence whose material is translucent uses it for its icon
    // overlay: the window then exists only where a tile is, so the panel's
    // uniform alpha shows the wallpaper through everywhere else while the tiles
    // themselves are painted by this opaque window sitting above it.
    //
    // Calling this selects the tile shape (setShape(TileUnion)); an empty vector
    // is then a *real* state -- a fence with no icon yet -- and means the window
    // shows nothing at all. That is deliberately not the same as "no region",
    // which Windows reads as "the whole window".
    void setRegionRects(const QVector<QRect>& rects);
    const QVector<QRect>& regionRects() const { return m_regionRects; }

    // The shape of the window's visible pixels: the rounded panel of a fence
    // (setCornerRadius) or the union of the tile rectangles above (T-082).
    enum class Shape {
        RoundedPanel,
        TileUnion,
    };
    void setShape(Shape shape);
    Shape shape() const { return m_shape; }

    // T-082: places this surface directly **below** `other` (another surface of
    // ours on the same icon host) instead of at the top of the host's child order.
    //
    // ⚠️ The direction is not a detail: `SetWindowPos(hwnd, hwndInsertAfter, ...)`
    // puts `hwnd` *below* `hwndInsertAfter`, so "directly above" cannot be asked
    // for at all -- the pair is expressed the other way round: the overlay takes
    // the top of the host's children, and the panel takes its place directly below
    // it (that is why the *panel* is the one holding this relation).
    void setStackedBelow(const DesktopSurface* other);
    const DesktopSurface* stackedBelow() const { return m_stackedBelow; }

    // Per-pixel transparency mode (T-017). DEV PATH ONLY, and known not to reach
    // the screen on a child of Progman (ADR-15). It is kept for the one surface
    // that has nothing else -- the full-screen backdrop drawn by --claim-desktop
    // and the probes -- which the product no longer hosts at all (T-071). Nothing
    // the user sees rests on it.
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

    // The same counters, as numbers, for the probes (T-050 / T-077). A surface
    // that is seen to do nothing must be distinguishable from one that is merely
    // quiet, so the *work avoided* is counted as explicitly as the work done.
    //
    //   paintCount()             WM_PAINTs handled. On the blit path (ADR-15)
    //                            this is what "the window was asked to show
    //                            itself" means, so it is the number a rest or a
    //                            move has to leave alone.
    //   layeredPushes()          successful UpdateLayeredWindow pushes. Fences no
    //                            longer push anything (ADR-15); the backdrop
    //                            still does, and it is the only surface for which
    //                            this number is not zero.
    //   mismatchedFramePushes()  pushes whose frame was not the size of the
    //                            window it went to. Must stay 0: such a push
    //                            resizes the window to the frame (T-077).
    //   staleFrameSkips()        pushes *not* made because the frame was about to
    //                            be replaced at the new size by the caller.
    //   currentPlacements()      place() calls that found the window already
    //                            where it belongs -- no SetWindowPos, no repaint.
    //                            The 2 s watchdog lands here when at rest (T-050).
    int paintCount() const { return m_paintCount; }
    int layeredPushes() const { return m_pushCount; }
    int mismatchedFramePushes() const { return m_mismatchedPushCount; }
    int staleFrameSkips() const { return m_stalePushSkips; }
    int currentPlacements() const { return m_currentPlaceCount; }

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
    // Applies m_cornerRadius as the window's region (ADR-15).
    void applyShape();

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
    // ADR-15: the material (a uniform alpha for the whole window) and the
    // rounded panel (a region). Both are properties of the window, not of the
    // frame -- the frame is blitted with alpha ignored.
    int m_uniformAlpha = 255;
    int m_cornerRadius = 0;
    // T-082: the tiles' shape (see setRegionRects), and the surface this one is
    // stacked directly above (see setStackAbove). Both are empty/null for every
    // fence panel -- they exist for the icon overlay.
    QVector<QRect> m_regionRects;
    Shape m_shape = Shape::RoundedPanel;
    // The surface this one is stacked directly below -- set on a fence's *panel*
    // (its overlay is the window above it). See setStackedBelow.
    const DesktopSurface* m_stackedBelow = nullptr;
    QSize m_shapedSize; // the size the region was built for
    // Navigation state (T-009), both honoured by place() so the watchdog cannot
    // undo them.
    bool m_visible = true;
    Layer m_layer = Layer::BelowIconLayer;
    bool m_clickThrough = false;
    bool m_ownsAllInput = false;

    QVector<QRect> m_nativeIconRects;
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
    std::function<void*(quintptr)> m_dropTargetFactory;
    void* m_customDropTarget = nullptr;
    void releaseCustomDropTarget();
    bool forwardKey(quintptr wParam);
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
    QRect m_paintRect; // session 63: the WM_PAINT update rectangle being served
    QRect m_lastUpdateRegion;

    // Layered-push / placement counters, for paintReport() and the probes
    // (T-050, T-077). See the public accessors for what each one means.
    int m_pushCount = 0;
    int m_mismatchedPushCount = 0;
    int m_stalePushSkips = 0;
    int m_currentPlaceCount = 0;
};

} // namespace starfish::platform::win
