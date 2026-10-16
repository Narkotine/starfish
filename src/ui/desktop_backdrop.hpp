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
#include "ui/backdrop_layout.hpp"
#include "ui/icon_grid.hpp"
#include "ui/screen_layout.hpp"

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
// It is deliberately **click-through** (DesktopSurface::setClickThrough): it
// paints icons but owns none of their behaviour yet (renaming, cut/paste, drops
// = T-042), and swallowing a click it cannot honour would be a silent
// regression. Until then the mouse keeps reaching whatever is underneath.
class DesktopBackdrop : public QObject
{
    Q_OBJECT

public:
    explicit DesktopBackdrop(QObject* parent = nullptr);
    ~DesktopBackdrop() override;

    DesktopBackdrop(const DesktopBackdrop&) = delete;
    DesktopBackdrop& operator=(const DesktopBackdrop&) = delete;

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

private:
    void recompute();
    void onRenderTimer();
    // The primary monitor's scaling: the frame is physical pixels throughout, so
    // the logical metrics and the caption font are scaled by this.
    qreal scale() const;
    QImage buildFrame() const;

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
};

} // namespace starfish::ui
