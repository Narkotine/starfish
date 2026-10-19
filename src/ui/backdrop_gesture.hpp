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
//   * **right-drag on the bare desktop** -> draw a fence there (F-001). The
//     gesture ends on the desktop's *own* context menu -- the one SysListView32
//     answered before ADR-9 hid it -- with a StarFish entry inserted at the top
//     (T-055). A plain right-click opens the same menu with nothing drawn, which
//     is exactly the desktop menu T-039 had taken away (T-054).
//   * **left double-click on the bare desktop** -> quick hide / show (F-022).
//   * **left press on one of the icons this layer draws** -> pick it up, and let
//     it go on a fence (T-042, first slice). This is the half that was missing
//     when F-040 made the takeover a user's switch: hiding the native list takes
//     away the *source* of every drag, so the icons StarFish draws have to
//     answer the mouse themselves or the mode hides the icons and offers no way
//     to file them.
//
// The right button is not a detail: it is what Fences does, it is what the user
// asked for, and it leaves the left button free for the desktop icons' own
// behaviour (the carry above, and later marquee selection).
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
// says which button and whether that point landed on one of the icons the
// backdrop draws, and gets back what to do. That is what makes the rules
// testable without a desktop, and what keeps the widget half (DesktopBackdrop)
// to plumbing.
class BackdropGesture
{
public:
    struct Metrics {
        // How far the pointer must travel before a press counts as a drag.
        // Below it, press-and-release is a click and starts nothing: the desktop
        // must not sprout a fence because the user tapped it.
        int dragThreshold = 6;
        // A drawn rectangle smaller than a fence may be is *not offered* as a
        // fence: a draw that small is a slip of the hand, not a request. The
        // menu still opens (the user right-clicked the desktop), it simply has no
        // rectangle to propose. The caller passes the container's own minimum, so
        // the two cannot drift apart.
        QSize minimumFence{160, 120};
    };

    enum class Button {
        Left,  // F-022's double-click, and later the desktop icons' own gestures
        Right, // F-001: draw a fence, then the desktop's context menu
    };

    enum class Action {
        None,             // nothing to do
        Drawing,          // the rubber band moved: repaint
        DesktopMenu,      // show the desktop's menu; `rect` is the fence drawn, or null
        Cancelled,        // a draw that was taken away
        ToggleVisibility, // F-022
        // T-042: an icon this layer draws. `point` is the cursor, in the same
        // space as the cells, so a caller can ask what is under it.
        IconDrag,     // ... is being carried: the caller paints what it paints
        IconDrop,     // ... was let go at `point`: file it wherever that is
        IconActivate, // ... was double-clicked: open it
    };

    struct Result {
        Action action = Action::None;
        // DesktopMenu only: the rectangle the user drew, in virtual-screen
        // pixels, when it is large enough to be a fence -- null when the gesture
        // was a plain right-click, or a draw too small to be one. `point` is
        // where the button came up, so a caller that has no rectangle can still
        // place something where the user aimed.
        QRect rect;
        QPoint point;
    };

    void setMetrics(const Metrics& metrics) { m_metrics = metrics; }
    const Metrics& metrics() const { return m_metrics; }

    // A button went down at `point`, which is -- or is not -- on one of the icons
    // the backdrop draws. The **left** button on an icon picks it up (T-042): the
    // caller remembers *which* icon, because this machine only has to know that
    // the gesture is not the desktop's. The right button never starts on an icon
    // -- a fence drawn from on top of one is a fence nobody asked for.
    Result press(const QPoint& point, Button button, bool onIcon);

    // The pointer moved. Reports while a draw or a carried icon is in progress.
    Result move(const QPoint& point);

    // The button came up: this is where a right-drag becomes a menu, and where a
    // carried icon is let go. A release of the *other* button is ignored rather
    // than treated as an ending -- a stray left click in the middle of a
    // right-drag must not open the menu, nor drop the icon somewhere.
    Result release(const QPoint& point, Button button);

    // A double-click. On the bare desktop the *left* one toggles the fences'
    // visibility (F-022); on one of the icons this layer draws it opens the item
    // (T-042) -- the desktop's oldest gesture, and ours to provide because there
    // is no shell list behind the icon to answer it.
    Result doubleClick(const QPoint& point, bool onIcon);

    // The gesture was taken away -- the mouse capture was stolen, the window
    // went away, the mode was turned off. Nothing is created, no menu opens, and
    // a carried icon is put back where it was.
    Result cancel();

    bool armed() const { return m_armed; }
    bool drawing() const { return m_drawing; }
    Button button() const { return m_button; }
    QPoint origin() const { return m_origin; }
    QPoint current() const { return m_current; }

    // T-042: an icon of this layer is being held, and whether it has travelled
    // far enough to count as a drag. Exposed for the probes: what is on screen
    // during a carry is otherwise only observable as pixels.
    bool iconArmed() const { return m_iconArmed; }
    bool iconDragging() const { return m_iconDragging; }

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
    Button m_button = Button::Left;
    QPoint m_origin;
    QPoint m_current;
    // T-042: the left button is down on one of the icons this layer draws.
    bool m_iconArmed = false;
    bool m_iconDragging = false;
    QPoint m_iconOrigin;
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
    m_iconArmed = false;
    m_iconDragging = false;
}

inline BackdropGesture::Result BackdropGesture::press(const QPoint& point, Button button,
                                                      bool onIcon)
{
    reset();
    // T-042 (first slice): the **left** button on one of the icons this layer
    // draws picks it up. F-040 made the takeover the user's switch, and hiding
    // the native list takes away the source of every drag -- so if these icons do
    // not answer the mouse, the mode hides them and offers no way to file them.
    if (onIcon && button == Button::Left) {
        m_iconArmed = true;
        m_iconOrigin = point;
        return {};
    }
    // Only the right button draws a fence (F-001). A press on an icon never
    // starts one: the fence would come out of an icon the user was aiming at.
    if (button != Button::Right || onIcon)
        return {};
    m_armed = true;
    m_button = button;
    m_origin = point;
    m_current = point;
    return {};
}

inline BackdropGesture::Result BackdropGesture::move(const QPoint& point)
{
    if (m_iconArmed) {
        // Carried past the threshold it is a drag -- the caller paints whatever
        // it paints for one (T-087 asks for the ghost). Below it, nothing has
        // happened yet: a click on an icon is not a gesture.
        if (!m_iconDragging) {
            const QPoint delta = point - m_iconOrigin;
            if (qAbs(delta.x()) < m_metrics.dragThreshold
                && qAbs(delta.y()) < m_metrics.dragThreshold)
                return {};
            m_iconDragging = true;
        }
        m_current = point;
        return {Action::IconDrag, {}, point};
    }

    if (!m_armed)
        return {};

    if (!m_drawing) {
        const QPoint delta = point - m_origin;
        if (qAbs(delta.x()) < m_metrics.dragThreshold && qAbs(delta.y()) < m_metrics.dragThreshold)
            return {};
        m_drawing = true;
    }

    m_current = point;
    return {Action::Drawing, {}, point};
}

inline BackdropGesture::Result BackdropGesture::release(const QPoint& point, Button button)
{
    if (m_iconArmed) {
        // The other button does not end a carry: a stray right click in the
        // middle of holding an icon must not drop it somewhere.
        if (button != Button::Left)
            return {};
        const bool carried = m_iconDragging;
        m_iconArmed = false;
        m_iconDragging = false;
        // Below the threshold this was a click on an icon, and a click on an icon
        // is not a gesture yet (selection is T-042's other half): nothing to
        // report rather than something the caller has to filter out.
        return carried ? Result{Action::IconDrop, {}, point} : Result{};
    }

    if (!m_armed || button != m_button)
        return {};

    m_current = point;
    const bool wasDrawing = m_drawing;
    const QRect drawn = normalized(m_origin, m_current);
    reset();

    // A right-release always opens the desktop's menu -- that is what a
    // right-click on the desktop does, and taking it away was T-039's most
    // visible cost (T-054). What the draw adds is the rectangle the menu can
    // offer; a draw too small to be a fence simply offers none, rather than
    // being silently grown to the minimum.
    Result result;
    result.action = Action::DesktopMenu;
    result.point = point;
    if (wasDrawing && drawn.width() >= m_metrics.minimumFence.width()
        && drawn.height() >= m_metrics.minimumFence.height())
        result.rect = drawn;
    return result;
}

inline BackdropGesture::Result BackdropGesture::doubleClick(const QPoint& point, bool onIcon)
{
    // A double-click arrives as press, release, double-click, release: whatever
    // the presses armed has to be gone by the time the trailing release arrives.
    reset();
    // T-042: on one of the icons this layer draws, the double-click is the
    // desktop's oldest gesture -- open it. It is ours to provide, because the
    // icon is only pixels here: there is no shell list behind it to answer.
    if (onIcon)
        return {Action::IconActivate, {}, point};
    return {Action::ToggleVisibility, {}, {}};
}

inline BackdropGesture::Result BackdropGesture::cancel()
{
    const bool wasDrawing = m_drawing;
    reset();
    // Only worth a repaint when a rubber band was actually on screen.
    return wasDrawing ? Result{Action::Cancelled, {}, {}} : Result{};
}

} // namespace starfish::ui
