#pragma once

#include <QColor>
#include <QPointF>
#include <QStringList>
#include <QWidget>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QWheelEvent;

#include "core/desktop_item.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/icon_grid.hpp"

namespace starfish::ui {

// A fence: a rounded panel with a title bar, hosting an icon grid of desktop
// items (T-005). It renders itself onto any QPaintDevice (so the very same
// paint path feeds both a live window and the offscreen image DesktopSurface
// blits to the desktop), and handles the local interactions -- selection and
// double-click-to-launch. The actual launch and any shell context menu happen
// outside the widget (ADR-3); FenceView only reports intent through signals.
class FenceView : public QWidget
{
    Q_OBJECT

public:
    explicit FenceView(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;

    // The items shown in the grid and how they are ordered.
    void setItems(QList<core::DesktopItem> items);
    const QList<core::DesktopItem>& items() const { return m_grid.items(); }
    void setSort(const QString& by, bool ascending);
    QString sortBy() const { return m_grid.sortBy(); }
    bool sortAscending() const { return m_grid.ascending(); }

    // Visual style of the panel (T-008 persists these through FenceStyle). The
    // colour is kept as given, alpha included (T-057): a translucent material is
    // carried by the panel's own alpha channel, and whether that alpha reaches
    // the screen is the *surface's* mode (per-pixel pushes it, the opaque GDI
    // blit ignores it). Dropping it here is what made F-038 impossible.
    void setPanelColor(const QColor& color);
    QColor panelColor() const { return m_panelColor; }
    void setCornerRadius(int px);
    int cornerRadius() const { return m_cornerRadius; }

    // Roll-up (F-020). A rolled-up fence is only its title bar: the grid is not
    // drawn and there is no content area. The widget is resized to the visible
    // rect by FenceContainer -- it never resizes itself, exactly like the
    // geometry gestures of T-007.
    void setRolledUp(bool rolledUp);
    bool rolledUp() const { return m_rolledUp; }

    // Where icons come from. Bound by the application to a ShellIconCache.
    void setIconProvider(IconGrid::IconProvider provider) { m_iconProvider = std::move(provider); }

    // Content area in widget-local coordinates (excludes the title bar and the
    // fence's outer padding). The grid is drawn here.
    QRect contentRect() const;

    // The item under a widget-local point, or -1.
    int itemAt(const QPoint& localPoint) const;

    // Accepts items dropped from the Explorer (T-006-3) and appends them to the
    // grid. `contentChanged` is emitted so the owner re-renders. The source
    // (Windows Shell resolution) is done by the caller; this method is pure.
    void addDroppedItems(const QList<core::DesktopItem>& items);

    // Same thing, with the count of tiles actually added (0 when they were all
    // duplicates). Used by the folder portals of T-011, which append a folder's
    // listing and want to know whether anything changed.
    int appendItems(const QList<core::DesktopItem>& items);

    // Folder portal refresh (T-011): withdraws the tiles a portal contributed
    // (`previousPaths`) and appends the folder's current listing, in a single
    // `contentChanged()`. The fence's own items and the selection are left
    // alone -- a folder changing under a portal is not the user doing
    // something. Returns the number of tiles added.
    int replaceItems(const QStringList& previousPaths, const QList<core::DesktopItem>& items);

    // Takes tiles out of the grid by path (F-011 / T-040), compared
    // case-insensitively like IconGrid::removeItemsByPath. Returns how many
    // went, and emits contentChanged() when it did. The widget does not decide
    // *whether* a tile may go -- that is FenceContainer's call (a tile a portal
    // mirrors is the folder's, not the fence's).
    int removeItemsByPath(const QStringList& paths);

signals:
    // Fired whenever the visual content changes (selection, items, sort) so the
    // owner can re-render the offscreen frame and push it to the surface.
    void contentChanged();

    // F-011 / T-040: the user dragged these tiles *out* of the fence and let go
    // outside it. Like every other gesture here the widget only reports the
    // intent -- giving an icon back to the desktop is nothing but no longer
    // holding it, so FenceContainer drops it from the fence's items and the
    // desktop backdrop (T-039) draws it again. No restore code anywhere, and
    // the file itself is never touched.
    void itemReleaseRequested(const QStringList& paths);

    // A double-click on `index` asked to launch it.
    void itemActivated(int index);

    // Fence geometry gestures (T-007): the title bar moves the fence, a border
    // grip resizes it. `delta` is the pointer offset since the gesture began.
    // FenceView never moves itself -- it has no idea where the other fences are,
    // so FenceContainer owns the geometry and decides where the drag lands.
    void geometryDragStarted();
    void geometryDragDelta(FenceHandle handle, const QPoint& delta);
    void geometryDragFinished();

    // A double-click or a wheel notch on the title bar asks to roll the fence up
    // or down (F-020). Like the geometry gestures, the view only reports intent:
    // FenceContainer owns the state and the resulting size.
    void rollUpRequested(bool rolledUp);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    // Explorer drag-and-drop into the fence (T-006-3). Only files/folders
    // (CF_HDROP, exposed by Qt as mimeData()->hasUrls()) are accepted; the
    // dropped paths are resolved to DesktopItems and appended.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // Pops the Windows shell context menu for the item at `index` (-1 = empty
    // area, nothing is shown). Implemented in the .cpp so the shell headers
    // stay out of the widget header. `globalPos` is screen coordinates.
    void showContextMenuForIndex(int index, const QPoint& globalPos);

    // Draws the "insert here" highlight around the cell at `index` while a
    // drag is in progress. No-op when index < 0.
    void paintDropIndicator(int index, QPainter& painter);

    // Draws what letting go *now* would do (F-011): the cursor is outside the
    // fence, so the release hands the tile back to the desktop instead of
    // reordering the grid. Drawn inside the fence on purpose -- the cursor is
    // outside it, and the fence is the only thing we own.
    void paintReleaseHint(QPainter& painter);

    static constexpr int kTitleBarHeight = FenceGeometry::kTitleBarHeight;
    static constexpr int kCornerRadius = 10;
    static constexpr int kPadding = 12;
    // Inward width of the resize grips along each edge (T-007).
    static constexpr int kResizeBorder = FenceGeometry::kResizeBorder;

    QString m_title;
    QColor m_panelColor{0x1F, 0x29, 0x37};
    int m_cornerRadius = kCornerRadius;
    bool m_rolledUp = false;
    IconGrid m_grid;
    IconGrid::IconProvider m_iconProvider;

    // Drag state (T-006-2). The press sets `m_dragOriginIndex` and
    // `m_dragStartPos`; the move enters drag mode past the system threshold,
    // updates `m_dropTargetIndex` as the cursor moves; the release performs
    // the reorder and clears the state.
    int m_dragOriginIndex = -1;
    int m_dropTargetIndex = -1;
    QPoint m_dragStartPos;
    bool m_dragging = false;
    // F-011: the pointer of an in-flight tile drag is outside the fence, so the
    // release will hand the tile back to the desktop. Recomputed on every move
    // (dragging back in disarms it again), which is why it is not a one-way
    // flag set when the cursor leaves.
    bool m_releaseArmed = false;

    // Fence geometry drag state (T-007), mutually exclusive with the icon drag
    // above: a press that lands on the title bar or a border grip never arms an
    // icon drag, and vice versa.
    FenceHandle m_geometryHandle = FenceHandle::None;
    // The gesture reference is a **screen** position, deliberately not a
    // widget-local one. The widget -- and the hosted surface that carries it --
    // *moves under the cursor* while the drag runs, so a local reference stops
    // measuring the cursor: each step reports the cursor's position relative to
    // a window that has already followed it, and the fence then only travels a
    // fraction of the displacement (measured: it converges to **half** of it,
    // and lags on the way). Screen coordinates do not move with the window.
    QPointF m_geometryStartScreen;
    bool m_geometryDragging = false;
};

} // namespace starfish::ui
