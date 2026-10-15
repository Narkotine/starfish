#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include "ui/fence_geometry.hpp"

namespace starfish::ui {

// One monitor, as the fence geometry needs to reason about it (T-014 / F-032).
//
// `geometry` is in **physical** pixels, in Windows' virtual-screen coordinates
// -- which is exactly the coordinate space of Progman's client area, and
// therefore of FenceWindow::rect. The primary monitor sits at (0,0); a monitor
// Windows has placed to the left of or above it gets negative coordinates.
//
// Qt's QScreen::geometry(), by contrast, is in *logical* pixels: the platform
// layer multiplies by devicePixelRatio before handing a screen over (see
// ProductApp::screenLayout()). Keeping the whole model physical means one
// coordinate space for the geometry, the persisted document and Win32, with
// the DPI conversion confined to the two places that need it (rendering and
// mouse routing).
struct ScreenInfo {
    QRect geometry;
    qreal devicePixelRatio = 1.0;
    QString name; // "\\.\DISPLAY1" -- what a fence persists as its monitor
    bool primary = false;
};

// What Windows calls the virtual screen, plus the two lookups the fence rules
// need: which screen a rect belongs to, and where a rect belongs when its
// screen is gone. Pure -- no QScreen, no Win32 -- so a mixed-DPI multi-monitor
// arrangement is unit-tested without owning two monitors
// (tests/test_screen_layout.cpp).
class ScreenLayout
{
public:
    // The single-screen case, which is also the v1 default: one screen with its
    // origin at (0,0) and no scaling. Every caller that only knows a size (the
    // probes, the older tests) keeps working through this.
    static ScreenLayout single(const QSize& size)
    {
        ScreenLayout layout;
        ScreenInfo screen;
        screen.geometry = QRect(QPoint(0, 0), size);
        screen.primary = true;
        layout.m_screens.append(screen);
        return layout;
    }

    void addScreen(const ScreenInfo& screen) { m_screens.append(screen); }

    bool isEmpty() const { return m_screens.isEmpty(); }
    int count() const { return m_screens.size(); }
    const QList<ScreenInfo>& screens() const { return m_screens; }

    // The bounding box of every screen: the area for which Progman's client
    // area exists. With monitors of different heights this box contains dead
    // zones -- areas no monitor draws -- which is precisely why a fence is
    // clamped to *its* screen rather than to this box.
    QRect bounds() const
    {
        QRect box;
        for (const ScreenInfo& screen : m_screens)
            box = box.isNull() ? screen.geometry : box.united(screen.geometry);
        return box;
    }

    // One rect per screen: the snapping targets. Snapping against every screen
    // (rather than against the bounding box) is what makes a fence dock to the
    // inner edge of a neighbouring monitor instead of to the far edge of the
    // virtual desktop.
    QList<QRect> frames() const
    {
        QList<QRect> rects;
        rects.reserve(m_screens.size());
        for (const ScreenInfo& screen : m_screens)
            rects.append(screen.geometry);
        return rects;
    }

    // The primary monitor when one is flagged, otherwise the first known one.
    // Null only when the layout is empty.
    const ScreenInfo* primary() const
    {
        for (const ScreenInfo& screen : m_screens) {
            if (screen.primary)
                return &screen;
        }
        return m_screens.isEmpty() ? nullptr : &m_screens.first();
    }

    // The screen a fence persisted as `name`, or null. Device names survive a
    // resolution change but not a monitor being unplugged.
    const ScreenInfo* screenNamed(const QString& name) const
    {
        if (name.isEmpty())
            return nullptr;
        for (const ScreenInfo& screen : m_screens) {
            if (screen.name == name)
                return &screen;
        }
        return nullptr;
    }

    const ScreenInfo* screenAt(const QPoint& point) const
    {
        for (const ScreenInfo& screen : m_screens) {
            if (screen.geometry.contains(point))
                return &screen;
        }
        return nullptr;
    }

    // Closest screen by distance to its rect, for a point that is in no screen
    // at all (a dead zone between monitors of different sizes, or a monitor
    // that has just been unplugged). Null only when the layout is empty.
    const ScreenInfo* nearestScreen(const QPoint& point) const
    {
        const ScreenInfo* best = nullptr;
        qint64 bestDistance = 0;
        for (const ScreenInfo& screen : m_screens) {
            const qint64 dx = axisDistance(point.x(), screen.geometry.left(),
                                           screen.geometry.right());
            const qint64 dy = axisDistance(point.y(), screen.geometry.top(),
                                           screen.geometry.bottom());
            const qint64 distance = dx * dx + dy * dy;
            if (!best || distance < bestDistance) {
                best = &screen;
                bestDistance = distance;
            }
        }
        return best;
    }

    // The screen a fence belongs to, decided by its centre: the whole fence
    // stays on one monitor, which is what keeps it reachable (a fence in a dead
    // zone between two differently sized monitors would be invisible and
    // impossible to grab).
    const ScreenInfo* screenForRect(const QRect& rect) const
    {
        if (m_screens.isEmpty())
            return nullptr;
        if (const ScreenInfo* screen = screenAt(rect.center()))
            return screen;
        return nearestScreen(rect.center());
    }

    qreal devicePixelRatioFor(const QRect& rect) const
    {
        const ScreenInfo* screen = screenForRect(rect);
        return screen ? screen->devicePixelRatio : 1.0;
    }

    // Device name to persist for a fence: derived from where the fence *is*,
    // never from where it was, so the document cannot claim a monitor the fence
    // does not sit on.
    QString nameFor(const QRect& rect) const
    {
        const ScreenInfo* screen = screenForRect(rect);
        return screen ? screen->name : QString();
    }

    // The area a fence must stay inside: the rect of its screen.
    QRect clampRectFor(const QRect& rect) const
    {
        const ScreenInfo* screen = screenForRect(rect);
        return screen ? screen->geometry : QRect();
    }

    // Where a rect stored in the document belongs on *this* layout (F-032).
    //
    // The stored rect is in virtual-screen coordinates, so the common case --
    // nothing changed -- keeps it as it is. The interesting cases are the ones
    // a laptop user hits: a monitor unplugged, a monitor re-arranged, a
    // resolution changed.
    //
    //   1. the rect is (still) on a screen -> keep it, clamped to that screen;
    //   2. otherwise, its recorded monitor exists -> put it back on that
    //      monitor, clamped (the monitor moved, the fence did not follow);
    //   3. otherwise -> on the nearest screen, then on the primary.
    //
    // A fence is never left outside every screen: it would be unreachable with
    // the mouse, which is the failure this rule exists to prevent.
    QRect restoreRect(const QRect& stored, const QString& storedMonitor) const
    {
        if (m_screens.isEmpty())
            return stored; // nothing known: do not invent a move

        const ScreenInfo* screen = screenAt(stored.center());
        if (!screen)
            screen = screenNamed(storedMonitor);
        if (!screen)
            screen = nearestScreen(stored.center());
        if (!screen)
            screen = primary();
        if (!screen)
            return stored;
        return FenceGeometry::clampIntoRect(stored, screen->geometry);
    }

private:
    // Distance from `value` to the inclusive [low, high] span, 0 when inside.
    static qint64 axisDistance(int value, int low, int high)
    {
        if (value < low)
            return low - value;
        if (value > high)
            return value - high;
        return 0;
    }

    QList<ScreenInfo> m_screens;
};

} // namespace starfish::ui
