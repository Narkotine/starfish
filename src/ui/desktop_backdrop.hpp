#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include "core/desktop_item.hpp"
#include "ui/backdrop_gesture.hpp"
#include "ui/backdrop_layout.hpp"
#include "ui/icon_grid.hpp"
#include "ui/screen_layout.hpp"

class QMouseEvent;

namespace starfish::platform::win {
class DesktopSurface;
}

namespace starfish::ui {

// The desktop layer StarFish draws itself when it owns the icons (T-039).
//
// WHY THIS EXISTS
// ---------------
// A fence is a child of Progman placed under SHELLDLL_DefView (ADR-4), so at the
// centre of a hosted fence the topmost *visible* window is SHELLDLL_DefView and
// then SysListView32: the click goes to the desktop icon list, never to the
// fence. Measured in session 25, and the reason Fences' own model has no answer
// for "put the icons where I want": the native icon layer has to go. The
// decision (session 28, ADR-9 option A) is that StarFish then draws *every*
// icon -- the ones in a fence and the ones on the bare desktop -- which is also
// what makes "zones above the icons" possible without duplicating anything.
//
// WHAT IT IS NOT
// --------------
// It is not a second document. The icons it draws are **computed** every time
// (all desktop entries minus the ones a fence holds, BackdropLayout), never
// stored: ADR-7 stays intact, the schema does not move, and removing a tile or
// a fence puts the icon back on the desktop with no restore code at all
// (F-007/F-011 -> T-040).
//
// It is **click-through** (DesktopSurface::setClickThrough) as long as it has
// nothing to do with a click: it paints icons but owns none of their behaviour
// yet (renaming, cut/paste, drops = T-042), and swallowing a click it cannot
// honour would be a silent regression. The empty desktop's own gestures are the
// exception, and they are what turns the click-through off (T-041): a drag on
// bare desktop draws a fence (F-001) and a double-click hides or shows them all
// (F-022). Both are honoured here, so the backdrop stops being a decoration and
// becomes the desktop's input surface -- but only where it has an answer. It
// still registers no drop target (T-052), because dropping onto the desktop
// means *moving a file*, which is a different decision.
class DesktopBackdrop : public QObject
{
    Q_OBJECT

public:
    explicit DesktopBackdrop(QObject* parent = nullptr);
    ~DesktopBackdrop() override;

    DesktopBackdrop(const DesktopBackdrop&) = delete;
    DesktopBackdrop& operator=(const DesktopBackdrop&) = delete;

    // The lowest alpha any pixel of a frame may carry, and the reason it is not
    // zero (T-060).
    //
    // A per-pixel layered window is hit-tested against its own alpha: Windows
    // lets the mouse through wherever the alpha is zero (MSDN, "Layered
    // Windows"). This window stands in for a desktop whose whole point is that
    // the wallpaper shows through, so its frame used to be *fully* transparent --
    // and a fully transparent frame is a window the cursor can never find. The
    // empty desktop's gestures (F-001/T-055, F-022) then existed only in the
    // probes, which post their events straight at this object instead of letting
    // Windows deliver them. Measured: with an alpha-0 frame the cursor finds the
    // *parent*; at this floor it finds the layered child itself
    // (experiments/spike_layer_alpha_shape.cpp).
    //
    // One level out of 255 is invisible on any wallpaper, and it turns the frame
    // from a shape into a rectangle. Whether the backdrop *answers* a click is a
    // separate decision and it has not moved: while the gestures are off the
    // surface replies HTTRANSPARENT from WM_NCHITTEST (setClickThrough). A window
    // the cursor can find and that chooses to step aside is not the same thing as
    // a window the cursor cannot find at all.
    static constexpr int kInputAlphaFloor = 1;

    void setIconProvider(IconGrid::IconProvider provider);

    // The monitors, so the backdrop covers exactly the virtual screen and picks
    // up the primary monitor's scaling.
    void setScreens(const ScreenLayout& layout);
    const ScreenLayout& screens() const { return m_screens; }

    // Logical metrics; the physical ones are derived from the primary monitor's
    // device pixel ratio (the backdrop is rendered 1:1 in physical pixels, which
    // is what Progman's client space is -- T-014).
    void setMetrics(const BackdropLayout::Metrics& metrics) { m_metrics = metrics; }
    const BackdropLayout::Metrics& metrics() const { return m_metrics; }
    BackdropLayout::Metrics physicalMetrics() const;

    // The whole desktop, as the Shell presents it. Hidden entries are dropped
    // here: the Shell does not draw them either, and the backdrop has to look
    // like the desktop it replaces.
    void setDesktopItems(QList<core::DesktopItem> items);
    const QList<core::DesktopItem>& desktopItems() const { return m_desktop; }

    // Every path a fence holds, as BackdropLayout::pathKey() spells it. The
    // container recomputes this on every change; nothing about it is persisted.
    void setOccupiedPaths(const QSet<QString>& keys);
    const QSet<QString>& occupiedPaths() const { return m_occupied; }

    // The backdrop only exists while the native icon layer is hidden. Turning it
    // off detaches the surface (and nothing else).
    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled; }

    // The empty desktop's gestures (T-041 / F-001, F-022). Turning them on makes
    // the surface stop being click-through and routes its mouse here; turning
    // them off puts it back to decorative. They are only meaningful while the
    // backdrop stands in for the desktop, which is why the product turns them on
    // together with setEnabled(true).
    void setGestureEnabled(bool enabled);
    bool gestureEnabled() const { return m_gestures; }
    // The drag threshold and the smallest fence a draw may produce. The caller
    // passes the container's own minimum so the gesture and the container cannot
    // disagree about what a fence is.
    void setGestureMetrics(const BackdropGesture::Metrics& metrics) { m_gesture.setMetrics(metrics); }
    const BackdropGesture::Metrics& gestureMetrics() const { return m_gesture.metrics(); }

    // The rubber band currently being drawn, or a null rect. Exposed for the
    // probes: what is on screen is otherwise only observable as pixels.
    QRect rubberBand() const { return m_rubberBand; }

    // Creates the window and pushes the first frame. Returns false when there is
    // no desktop to host it on, or -- and this is the interesting case -- when
    // the per-pixel push did not take: an opaque full-screen window would hide
    // the wallpaper, so the caller must not hide the native icons on the
    // strength of a backdrop that cannot be transparent. Same discipline as the
    // crash marker (T-046): if the safe thing cannot be done, refuse to do the
    // risky one.
    bool attach();
    void detach();
    bool isAttached() const;

    // Recomputes the free set and schedules a render (coalesced: a fence drag
    // emits ~60 changes a second and each render uploads a full-screen frame).
    void refresh();
    // Recomputes and renders now.
    void renderNow();

    QRect area() const;
    int freeCount() const { return m_free.size(); }
    const QList<core::DesktopItem>& freeItems() const { return m_free; }
    const QVector<BackdropCell>& cells() const { return m_cells; }
    QImage frame() const { return m_frame; }

    platform::win::DesktopSurface* surface() const { return m_surface; }

    // One line for the settings window and the probes.
    QString statusText() const;

signals:
    // A frame was produced: how many icons the desktop shows now.
    void rendered(int freeCount);
    // The hosted window went away (Explorer restarted). The owner re-attaches on
    // the next refresh.
    void desktopLost();
    // F-001 + T-055: the user right-clicked (or right-dragged) the bare desktop.
    // `rect` is the fence the drag described, in virtual-screen pixels -- the
    // space a stored fence rect uses -- or a null rect when the gesture was a
    // plain right-click or a draw too small to be a fence. `point` is where the
    // button came up, in the same space, so the caller can place something where
    // the user aimed. The backdrop signals; it neither creates anything nor opens
    // any menu (the product owns both, as everywhere else).
    void desktopMenuRequested(const QRect& rect, const QPoint& point);
    // F-022: the user double-clicked the bare desktop.
    void quickHideRequested();

protected:
    // The surface posts its mouse events here (DesktopSurface::setInputTarget),
    // so this is where a gesture is read. A plain QObject has no mouse handling
    // of its own -- these are exactly the events Qt would otherwise ignore.
    bool event(QEvent* event) override;

private:
    void recompute();
    void onRenderTimer();
    // The primary monitor's scaling: the frame is physical pixels throughout, so
    // the logical metrics and the caption font are scaled by this.
    qreal scale() const;
    QImage buildFrame() const;

    bool handleMouse(QMouseEvent* mouse);
    bool applyGesture(const BackdropGesture::Result& result);
    // Points the hosted window at the gestures or back at nothing, depending on
    // m_gestures. Called on attach and whenever the switch flips.
    void applyGestureModeToSurface();

    ScreenLayout m_screens = ScreenLayout::single(QSize(1920, 1080));
    BackdropLayout::Metrics m_metrics;
    IconGrid::IconProvider m_iconProvider;

    QList<core::DesktopItem> m_desktop;
    QSet<QString> m_occupied;
    QList<core::DesktopItem> m_free;
    QVector<BackdropCell> m_cells;
    QImage m_frame;

    platform::win::DesktopSurface* m_surface = nullptr;
    QTimer m_renderTimer;
    bool m_enabled = false;

    // T-041. The gesture machine is pure (BackdropGesture); this class only feeds
    // it the mouse and acts on what it answers.
    BackdropGesture m_gesture;
    QRect m_rubberBand;
    bool m_gestures = false;
};

} // namespace starfish::ui
