#pragma once

#include <functional>
#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include "core/desktop_item.hpp"
#include "core/jsonstore.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/icon_grid.hpp"

namespace starfish::platform::win {
class DesktopSurface;
}

namespace starfish::ui {

class FenceView;

// One live fence (T-007): the model geometry, the widget that renders it and,
// when hosted, the raw Win32 surface that shows it on the desktop.
struct FenceWindow {
    QString id;
    QString title;
    // Persisted device name of the monitor (T-008). v1 only ever runs on the
    // primary one, so it is stored and reported but not acted upon (T-014).
    QString monitor;
    QRect rect; // desktop coordinates (Progman client == primary monitor, v1)
    core::FenceStyle style; // panel colour + radius are applied to the view
    bool rolledUp = false;  // persisted for F-020 (T-009), not acted upon yet
    FenceView* view = nullptr;
    starfish::platform::win::DesktopSurface* surface = nullptr;

    // Last frame produced by renderFence(). Kept even without a surface, so the
    // lifecycle can be exercised headless (tests, CI, --fence-demo).
    QImage frame;

    // Geometry drag bookkeeping: `dragStartRect` is captured when a gesture
    // begins, so every delta is applied to the same reference and rounding
    // cannot accumulate.
    QRect dragStartRect;
    bool dragging = false;
};

// Owns the set of fences and their lifecycle: create, move, resize, delete and
// snapping (T-007). It is the piece that makes an inter-fence gesture possible
// -- a FenceView only reports "the user is dragging my title bar / my edge",
// the container decides where that lands once the other fences are known.
//
// Two modes:
//   * headless (default) -- only FenceView + geometry, no desktop window. Used
//     by the unit tests and by --fence-demo when --host is not passed.
//   * hosted -- every fence additionally gets a DesktopSurface attached under
//     the icon layer, and the rendered frame is pushed to it. This is the real
//     product path.
class FenceContainer : public QObject
{
    Q_OBJECT

public:
    explicit FenceContainer(QObject* parent = nullptr);
    ~FenceContainer() override;

    FenceContainer(const FenceContainer&) = delete;
    FenceContainer& operator=(const FenceContainer&) = delete;

    // Icon source bound to every FenceView created afterwards.
    void setIconProvider(IconGrid::IconProvider provider);

    // Resolves a path coming back from the persisted layout into a DesktopItem
    // (T-008). The default resolver uses core::itemFromPathOnly (no Shell, no
    // Win32); the application binds DesktopScanner::itemForPath so a restored
    // tile is exactly what the Shell would draw.
    using ItemResolver = std::function<core::DesktopItem(const QString& path)>;
    void setItemResolver(ItemResolver resolver);

    // The area fences are kept inside and snapped to. In v1 this is the primary
    // monitor; multi-monitor offsets are T-014.
    void setDesktopBounds(const QSize& bounds);
    QSize desktopBounds() const { return m_bounds; }

    void setHostOnDesktop(bool enabled) { m_host = enabled; }
    bool hostsOnDesktop() const { return m_host; }

    void setSnapThreshold(int px) { m_snapThreshold = qMax(0, px); }
    int snapThreshold() const { return m_snapThreshold; }

    void setMinimumFenceSize(const QSize& size) { m_minSize = size; }
    QSize minimumFenceSize() const { return m_minSize; }

    // --- lifecycle ----------------------------------------------------------

    // Creates a fence with a fresh id and returns it. The rect is clamped to
    // the desktop bounds. When hosting is on, the surface is attached and the
    // first frame is pushed.
    QString createFence(const QString& title, const QRect& rect,
                        const QList<core::DesktopItem>& items = {});

    // Deletes a fence. The files themselves are never touched (AGENTS.md golden
    // rule): only the zone disappears. Returns false when the id is unknown.
    bool removeFence(const QString& id);

    bool contains(const QString& id) const { return find(id) != nullptr; }
    int fenceCount() const { return m_fences.size(); }
    QStringList fenceIds() const;

    QString fenceTitle(const QString& id) const;
    QRect fenceRect(const QString& id) const;
    QList<core::DesktopItem> fenceItems(const QString& id) const;
    core::FenceStyle fenceStyle(const QString& id) const;
    bool fenceRolledUp(const QString& id) const;

    void setFenceTitle(const QString& id, const QString& title);
    void setFenceStyle(const QString& id, const core::FenceStyle& style);
    void setFenceSort(const QString& id, const QString& by, bool ascending);

    // Applies a new rect. The rect is clamped to the desktop bounds and, unless
    // `snap` is false, snapped to the other fences and the screen edges. Returns
    // the rect actually applied (an unknown id returns a null rect).
    QRect setFenceRect(const QString& id, const QRect& rect, bool snap = true);

    // Access for tests / diagnostics. May be null in headless mode (surface).
    FenceView* view(const QString& id) const;
    starfish::platform::win::DesktopSurface* surface(const QString& id) const;
    QImage frame(const QString& id) const;

    // Renders the fence's widget offscreen and pushes it to its surface.
    void renderFence(const QString& id);

    // All rects except `excludeId`, for snapping.
    QList<QRect> otherRects(const QString& excludeId) const;

    // --- persistence (T-008) -------------------------------------------------
    // The live fences as a versioned document, ready for core::JsonStore. The
    // order of `fences` is the creation order.
    core::Layout toLayout() const;

    // Replaces every fence with the ones in `layout`: this is what runs at
    // startup. Fence ids, titles, rects, styles, sort and item paths are
    // restored, and the item paths go through the item resolver. Returns false
    // with an error message when the schema is unsupported -- in which case
    // nothing is touched. Files are never affected, only zones.
    bool applyLayout(const core::Layout& layout, QString* error = nullptr);

signals:
    void fenceCreated(const QString& id);
    void fenceRemoved(const QString& id);
    void fenceRectChanged(const QString& id, const QRect& rect);
    void fenceContentChanged(const QString& id);

private:
    FenceWindow* find(const QString& id);
    const FenceWindow* find(const QString& id) const;

    // Shared body of createFence and applyLayout. An empty `requestedId` means
    // "allocate a fresh one"; a restored id is kept verbatim so that the
    // document is stable across restarts.
    QString addFence(const QString& requestedId, const QString& title, const QRect& rect,
                     const core::FenceStyle& style, const QString& sortBy, bool sortAscending,
                     bool rolledUp, const QList<core::DesktopItem>& items);

    core::DesktopItem resolveItem(const QString& path) const;

    // FenceView geometry gestures -> move/resize with snapping.
    void beginGeometryDrag(const QString& id);
    void applyGeometryDrag(const QString& id, FenceHandle handle, const QPoint& delta);
    void endGeometryDrag(const QString& id);

    QString nextId();

    QList<FenceWindow> m_fences;
    IconGrid::IconProvider m_iconProvider;
    ItemResolver m_itemResolver;
    QSize m_bounds{1920, 1080};
    QSize m_minSize{160, 120};
    int m_snapThreshold = 8;
    bool m_host = false;
    int m_nextId = 1;
};

} // namespace starfish::ui
