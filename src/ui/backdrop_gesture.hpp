#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>

#include "ui/backdrop_layout.hpp"

namespace starfish::ui {

// The two gestures the empty desktop carries once StarFish owns the icon layer
// (T-041, docs/produit.md):
//
//   * **drag on the bare desktop** -> draw a fence of that size (F-001),
//   * **double-click on the bare desktop** -> quick hide / show (F-022).
//
// Both had always looked like they needed a global mouse hook (T-028): the bare
// desktop belongs to SysListView32, another process's window, so the press never
// reaches us. They do not. Since T-039 the backdrop is a full-screen window of
// ours that *is* the desktop while the native layer is hidden, so the gesture
// starts inside one of our own windows and there is nothing to intercept at the
// system level. T-028 is therefore only still needed for a gesture that begins
// outside our windows.
//
// Pure: no widget, no Win32, no event type. The caller turns its mouse events
// into press/move/release/doubleClick with a point in **virtual-screen pixels**
// -- the space the backdrop cells and the stored fence rects both use (T-014) --
// says whether that point landed on one of the icons the backdrop draws, and
// gets back what to do. That is what makes the rules testable without a desktop,
// and what keeps the widget half (DesktopBackdrop) to plumbing.
class BackdropGesture
{
public:
    struct Metrics {
        // How far the pointer must travel before a press counts as a drag.
        // Below it, press-and-release is a click and starts nothing: the desktop
        // must not sprout a fence because the user tapped it.
        int dragThreshold = 6;
        // A drawn rectangle smaller than a fence may be is refused rather than
        // grown to the minimum: a draw that small is a slip of the hand, not a
        // request. The caller passes the container's own minimum, so the two
        // cannot drift apart.
        QSize minimumFence{160, 120};
    };

    enum class Action {
        None,             // nothing to do
        Drawing,          // the rubber band moved: repaint
        FenceDrawn,       // a fence to create; `rect` is in virtual-screen pixels
        Cancelled,        // a draw that ended too small, or was taken away
        ToggleVisibility, // F-022
    };

    struct Result {
        Action action = Action::None;
        QRect rect; // FenceDrawn only
    };

    void setMetrics(const Metrics& metrics) { m_metrics = metrics; }
    const Metrics& metrics() const { return m_metrics; }

    // The left button went down at `point`, which is -- or is not -- on one of
    // the icons the backdrop draws. A press on an icon starts nothing: those
    // icons belong to the desktop and their behaviour is T-042's, and a fence
    // drawn from on top of an icon is a fence the user did not ask for.
    Result press(const QPoint& point, bool onIcon);

    // The pointer moved. Reports only while a draw is in progress.
    Result move(const QPoint& point);

    // The button came up: this is where a drawn fence is decided.
    Result release(const QPoint& point);

    // A double-click. On the bare desktop it toggles the fences' visibility
    // (F-022); on an icon it is the desktop's business, not ours.
    Result doubleClick(const QPoint& point, bool onIcon);

    // The gesture was taken away -- the mouse capture was stolen, the window
    // went away, the mode was turned off. Nothing is created.
    Result cancel();

    bool armed() const { return m_armed; }
    bool drawing() const { return m_drawing; }
    QPoint origin() const { return m_origin; }
    QPoint current() const { return m_current; }

    // The rubber band to paint, or a null rect when nothing is being drawn.
    QRect rubberBand() const;

    // Which of the icons the backdrop draws is under `point`, or -1. The cells
    // carry virtual-screen pixels, like `point`. A cell owns both its icon and
    // its caption band: clicking a label is clicking the icon.
    static int iconAt(const QVector<BackdropCell>& cells, const QPoint& point);

    // The rectangle two corners describe, normalised -- dragging right-to-left
    // or bottom-to-top is the same gesture. Built from x/y/width/height rather
    // than from two corners because QRect(topLeft, bottomRight) is inclusive and
    // would report a 101x101 rect for a 100x100 drag.
    static QRect normalized(const QPoint& from, const QPoint& to);

private:
    void reset();

    Metrics m_metrics;
    bool m_armed = false;
    bool m_drawing = false;
    QPoint m_origin;
    QPoint m_current;
};

inline int BackdropGesture::iconAt(const QVector<BackdropCell>& cells, const QPoint& point)
{
    for (int i = 0; i < cells.size(); ++i) {
        if (cells.at(i).icon.contains(point) || cells.at(i).label.contains(point))
            return i;
    }
    return -1;
}

inline QRect BackdropGesture::normalized(const QPoint& from, const QPoint& to)
{
    const int x = qMin(from.x(), to.x());
    const int y = qMin(from.y(), to.y());
    return QRect(x, y, qAbs(to.x() - from.x()), qAbs(to.y() - from.y()));
}

inline QRect BackdropGesture::rubberBand() const
{
    if (!m_drawing)
        return {};
    return normalized(m_origin, m_current);
}

inline void BackdropGesture::reset()
{
    m_armed = false;
    m_drawing = false;
}

inline BackdropGesture::Result BackdropGesture::press(const QPoint& point, bool onIcon)
{
    reset();
    if (onIcon)
        return {};
    m_armed = true;
    m_origin = point;
    m_current = point;
    return {};
}

inline BackdropGesture::Result BackdropGesture::move(const QPoint& point)
{
    if (!m_armed)
        return {};

    if (!m_drawing) {
        const QPoint delta = point - m_origin;
        if (qAbs(delta.x()) < m_metrics.dragThreshold && qAbs(delta.y()) < m_metrics.dragThreshold)
            return {};
        m_drawing = true;
    }

    m_current = point;
    return {Action::Drawing, {}};
}

inline BackdropGesture::Result BackdropGesture::release(const QPoint& point)
{
    if (!m_drawing) {
        // A click, not a gesture. Nothing was drawn and nothing is created.
        reset();
        return {};
    }

    m_current = point;
    const QRect drawn = normalized(m_origin, m_current);
    reset();

    if (drawn.width() < m_metrics.minimumFence.width()
        || drawn.height() < m_metrics.minimumFence.height())
        return {Action::Cancelled, drawn};

    return {Action::FenceDrawn, drawn};
}

inline BackdropGesture::Result BackdropGesture::doubleClick(const QPoint& point, bool onIcon)
{
    Q_UNUSED(point);
    // A double-click arrives as press, release, double-click, release: the two
    // presses must not leave a draw armed behind them.
    reset();
    if (onIcon)
        return {};
    return {Action::ToggleVisibility, {}};
}

inline BackdropGesture::Result BackdropGesture::cancel()
{
    const bool wasDrawing = m_drawing;
    reset();
    // Only worth a repaint when a rubber band was actually on screen.
    return wasDrawing ? Result{Action::Cancelled, {}} : Result{};
}

} // namespace starfish::ui
