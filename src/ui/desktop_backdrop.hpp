#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "core/desktop_item.hpp"
#include "platform/win/desktop_surface.hpp"
#include "ui/backdrop_gesture.hpp"
#include "ui/backdrop_layout.hpp"
#include "ui/icon_grid.hpp"
#include "ui/screen_layout.hpp"

class QKeyEvent;
class QMouseEvent;

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
// Since ADR-13 it does **not** draw the desktop's icons by default. The user's
// own words: "the normal mode is having the wallpaper displayed", and the icons
// belong to that desktop -- so they stay the *native* ones, where the user put
// them, with their own clicks, drags and menus. What is left of this window is
// the empty desktop's input: it stands over the wallpaper, steps aside wherever
// an icon is (DesktopSurface::setNativeIconRects) or wherever the left button is
// down (that drag is Windows' own icon selection), and answers only the *bare*
// right button -- the draw-a-fence gesture (F-001) and the desktop's own menu
// with StarFish's entry in it (T-055). setDrawFreeIcons(true) brings the old
// behaviour back, and is what the probes compare it against.
//
// It is therefore **click-through** (DesktopSurface::setClickThrough) wherever
// it has nothing to do with a click (T-042: swallowing a click it cannot honour
// would be a silent regression), and the draw-a-fence gesture plus F-022's
// double-click are the exceptions (T-041). It still registers no drop target
// (T-052), because dropping onto the desktop means *moving a file*, which is a
// different decision.
class DesktopBackdrop : public QObject
{
    Q_OBJECT

public:
    explicit DesktopBackdrop(QObject* parent = nullptr);
    ~DesktopBackdrop() override;

    DesktopBackdrop(const DesktopBackdrop&) = delete;
    DesktopBackdrop& operator=(const DesktopBackdrop&) = delete;

    // The lowest alpha any pixel of a frame may carry, and the reason it is not
    // zero (T-060). The rule and its measurement now live with the layered
    // window itself -- DesktopSurface::kInputAlphaFloor, which is also what a
    // translucent fence uses for the corners outside its rounded panel (T-057).
    // This alias keeps the name the probe and the docs already use.
    static constexpr int kInputAlphaFloor = platform::win::DesktopSurface::kInputAlphaFloor;

    void setIconProvider(IconGrid::IconProvider provider);

    // The native desktop icons, in virtual-screen physical pixels -- the space
    // WM_NCHITTEST and this class's own mouse events use. Read from the shell's
    // list view by platform/win/desktop_icons and handed over by the product.
    //
    // Two jobs, and they are the same job: the surface steps the mouse aside
    // over them (so the icons keep their own behaviour), and the gesture machine
    // reads them to know whether a right press was on an icon or on the bare
    // desktop -- which is the difference between the shell's *item* menu and its
    // *background* menu (T-055).
    void setNativeIconRects(const QVector<QRect>& rects);
    const QVector<QRect>& nativeIconRects() const { return m_nativeRects; }

    // The slot the shell gives each icon, keyed by its display name -- the same
    // read as the rectangles above, kept in the form the layout needs. It is what
    // makes the fence-less icons land where *they* are when this layer has to
    // draw them (auto-arrange on: a single icon cannot be moved aside, so the
    // whole list is taken over instead of duplicating the icons a fence holds).
    void setNativeIconBoxes(const QHash<QString, QRect>& boxes);
    const QHash<QString, QRect>& nativeIconBoxes() const { return m_nativeBoxes; }

    // Whether this layer draws the fence-less icons itself (ADR-9 option A,
    // which is what the probes measure this against) or leaves them to the
    // desktop (ADR-13, the default). Off means: paint nothing but the input
    // floor, and let the native icons show through.
    void setDrawFreeIcons(bool draw);
    bool freeIconsDrawn() const { return m_drawFreeIcons; }

    // ADR-21 -- the drawn desktop. With a wallpaper set *and* the free icons
    // drawn, the backdrop is an **opaque** surface (the blit path, ADR-15: the
    // per-pixel one does not reach the screen on a child of Progman) showing the
    // wallpaper, the icons where the shell keeps them, and the selection. It then
    // owns every click (DesktopSurface::setOwnsAllInput): the native list is
    // hidden, so nothing is under it to step aside for. Without a wallpaper the
    // old transparent layer stays -- an opaque frame with nothing to show would
    // be a black desktop.
    void setWallpaper(const QImage& wallpaper);
    const QImage& wallpaper() const { return m_wallpaper; }
    bool drawsWholeDesktop() const { return m_drawFreeIcons && !m_wallpaper.isNull(); }

    // The selected icons (paths, as the cells carry them). Drawn with the
    // desktop's own highlight; changed by the clicks this class reads, or by the
    // owner (an icon filed into a fence leaves the selection).
    void setSelection(const QStringList& paths);
    const QStringList& selection() const { return m_selection; }

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
    // ADR-21 lot 3: the selection rectangle being drawn, or a null rect.
    QRect marquee() const { return m_marquee; }
    // Ends whatever the mouse was doing (the shell's drag loop took over).
    void cancelGesture();
    // ADR-21 lot 3: the drawn desktop's drop target (see DesktopSurface::
    // setDropTargetFactory), used whenever the opaque surface is hosted.
    void setDropTargetFactory(std::function<void*(quintptr)> factory)
    {
        m_dropFactory = std::move(factory);
    }
    // The caption rectangle of an icon (screen pixels) -- where F2 edits.
    QRect labelRectOf(const QString& path) const;
    QRect iconRectOf(const QString& path) const;
    // Session 62: the picture a drag of `paths` carries (physical pixels), the
    // icons laid out as they sit -- those within reach of `press`, as Windows
    // caps a big selection. `hotspot` = `press` inside the image.
    QImage dragImage(const QStringList& paths, const QPoint& press, QPoint* hotspot) const;

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
    // T-042 (first slice): one of the icons *this layer draws*. The backdrop says
    // which path and where, and the product decides what that means -- file it
    // into the fence under the point, or open it. Same split as everywhere else:
    // the widget signals, the owner decides.
    //
    // Only the icons we draw speak here -- both signals are emitted while
    // setDrawFreeIcons(true) is in force, and never for a native icon. The native
    // ones belong to the shell, which is exactly why the surface steps the mouse
    // aside over them instead of reporting anything.
    void iconDropRequested(const QString& path, const QPoint& point);
    void iconActivateRequested(const QString& path);
    // ADR-21: a right-click on one of the icons this layer draws. `paths` is the
    // selection it acts on (the clicked icon joined it first, as on the real
    // desktop); `point` is where the button came up, in screen pixels.
    void iconMenuRequested(const QStringList& paths, const QPoint& point);
    void selectionChanged(const QStringList& paths);
    // ADR-21 lot 3 (T-091). A key the drawn desktop does not answer itself
    // (F2, Delete, Return, Ctrl+C/X/V/Z/Y, F5, the menu key): the product maps
    // it to the shell. Ctrl+A, Escape and the arrows are answered here.
    void keyCommand(int key, Qt::KeyboardModifiers modifiers);
    // An icon drag left its threshold: the product runs the shell's drag loop
    // (SHDoDragDrop) with `paths`, picked up at `origin` (screen pixels).
    void iconDragRequested(const QStringList& paths, const QPoint& origin);

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
    void paintRubberBand(QImage& frame, const QRect& box) const;

    bool handleMouse(QMouseEvent* mouse);
    bool handleKey(QKeyEvent* key);
    // The cell whose path is `path`, or -1.
    int cellOf(const QString& path) const;
    // True when `screen` (virtual-screen pixels) is inside a native icon.
    bool overNativeIcon(const QPoint& screen) const;
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
    QVector<QRect> m_nativeRects;
    QHash<QString, QRect> m_nativeBoxes;
    bool m_drawFreeIcons = false;
    QImage m_frame;

    platform::win::DesktopSurface* m_surface = nullptr;
    QTimer m_renderTimer;
    bool m_enabled = false;

    // T-041. The gesture machine is pure (BackdropGesture); this class only feeds
    // it the mouse and acts on what it answers.
    BackdropGesture m_gesture;
    QRect m_rubberBand;
    bool m_gestures = false;
    // T-042: the path of the icon the left button went down on. The machine only
    // says "an icon, and it travelled"; *which* icon is the widget's business,
    // because the identity lives in the cells (DesktopItem), which the pure
    // machine has no reason to carry.
    QString m_pressedIcon;

    // ADR-21.
    QImage m_wallpaper;
    QStringList m_selection;
    // A left press on an icon that was already selected: the selection only
    // narrows to it on the release, so a multi-selection can still be carried.
    QString m_narrowOnRelease;
    // The right button went down on an icon: its release opens the item menu.
    bool m_rightOnIcon = false;
    // ADR-21 lot 3: the left-button selection rectangle on the bare desktop.
    bool m_marqueeArmed = false;
    QPoint m_marqueeOrigin;
    QRect m_marquee;
    QStringList m_marqueeBase;
    QPoint m_iconPressPoint;
    std::function<void*(quintptr)> m_dropFactory;
    void setSelectionInternal(const QStringList& paths);
    // Re-hosts the window when the surface mode (transparent layer / opaque
    // desktop) has to change.
    void remodel(bool wasWholeDesktop);
};

} // namespace starfish::ui
