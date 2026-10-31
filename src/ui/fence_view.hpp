#pragma once

#include <QColor>
#include <QPointF>
#include <QStringList>
#include <QTransform>
#include <QPainterPath>
#include <QPair>
#include <QWidget>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QWheelEvent;

#include "core/desktop_item.hpp"
#include "ui/drag_image.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/fence_metrics.hpp"
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
    // T-080: the band's own colour, or an **invalid** QColor for "no band" -- the
    // fence as it has always looked, the title drawn straight on the panel. The
    // band is painted inside the panel's own rounded shape, so its top corners
    // follow the panel's rounding rather than getting a rounding of their own.
    void setTitleBarColor(const QColor& color);
    QColor titleBarColor() const { return m_titleBarColor; }
    // T-098: the title and the captions with the desktop's drop shadow, for a
    // translucent fence (the wallpaper shows behind the text).
    void setTextShadow(bool on);
    bool textShadow() const { return m_grid.textShadow(); }
    // The radius as the fence **draws** it: the stored value, bounded so it can
    // never exceed half the shorter side (a larger radius describes no shape at
    // all, and a region built from it would erase the panel). The document keeps
    // what was chosen; the geometry decides what is possible, here, in the one
    // place both the paint and the window's region ask.
    void setCornerRadius(int px);
    // T-094 (session 65): the border, 0 = none.
    void setBorder(int width, const QColor& color);
    // Session 65: a light highlight while the pointer is over the fence.
    void setHoverHighlight(bool on) { m_hoverHighlight = on; }
    // Session 69: what the hover does (the fence's own, or the default's).
    enum class HoverEffect { None, Lighter, Opacity, Colour, NoBorder, Glow };
    void setHoverEffect(HoverEffect effect) { m_hoverEffect = effect; }
    HoverEffect hoverEffect() const { return m_hoverEffect; }
    bool hovered() const { return m_hovered; }
    int cornerRadius() const;

    // Roll-up (F-020). A rolled-up fence is only its title bar: the grid is not
    // drawn and there is no content area. The widget is resized to the visible
    // rect by FenceContainer -- it never resizes itself, exactly like the
    // geometry gestures of T-007.
    void setRolledUp(bool rolledUp);
    bool rolledUp() const { return m_rolledUp; }

    // Where icons come from. Bound by the application to a ShellIconCache.
    void setIconProvider(IconGrid::IconProvider provider) { m_iconProvider = std::move(provider); }

    // Session 60: the fence's spacing (title, insets, gaps), named.
    void setMetrics(const FenceMetrics& metrics);
    const FenceMetrics& metrics() const { return m_metrics; }
    // Where the title text is drawn, widget-local: the inline rename's edit goes
    // exactly there, and a double-click on it renames instead of rolling up.
    QRect titleTextRect() const;

    // Session 60: where dropped tiles are inserted, and whether an addition is
    // sorted by name (the user's switch, off by default: the order is his).
    void setSortOnAdd(bool sort) { m_sortOnAdd = sort; }
    bool sortOnAdd() const { return m_sortOnAdd; }
    // Adds tiles before the one at `index` (-1 or past the end: at the end).
    int insertItems(const QList<core::DesktopItem>& items, int index);

    // Content area in widget-local coordinates (excludes the title bar and the
    // fence's outer padding). The grid is drawn here.
    QRect contentRect() const;

    // The width of one grid cell as the grid currently computes it: 1.5x the
    // icon, or wider when the fence's longest caption needs the room (T-078).
    // Exposed so a caller laying something out against the grid -- a probe
    // reading the painted bands, most of all -- asks the layout instead of
    // copying the formula and drifting away from it.
    int cellWidth() const { return m_grid.cellWidth(); }
    void setFixedCellWidth(int px);
    // T-076 (session 65): the title bar shown or not. Hidden, a thin band at
    // the top still moves the fence (a grip shows on hover); rolled up, the
    // title comes back -- a rolled-up fence *is* its title.
    void setTitleBarShown(bool shown);
    bool titleShown() const { return m_titleBarShown || m_rolledUp; }
    int titleBarHeight() const { return titleShown() ? m_metrics.titleBarHeight : kHiddenTitleBand; }
    static constexpr int kHiddenTitleBand = 12;
    // Session 66: the title's alignment in its bar (Qt::AlignLeft / HCenter /
    // Right), its inset (wider under a strong rounding), and the opacity the
    // icons alone are painted with (fadeIcons, composed).
    void setTitleAlignment(Qt::Alignment align);
    // Session 68: the side the title bar sits on. Left reads bottom to top,
    // right top to bottom; rolled up, the bar is always along the top.
    enum class TitleSide { Top, Bottom, Left, Right };
    void setTitleSide(TitleSide side);
    TitleSide titleSide() const;
    // Session 70: the side chosen, whatever the roll-up; the bar's own switch.
    TitleSide storedTitleSide() const { return m_titleSide; }
    bool titleBarShown() const { return m_titleBarShown; }
    int borderWidth() const { return m_borderWidth; }
    // Session 71: what paintEvent draws. Background = panel, band, border;
    // Foreground = title text, icons, labels, hints. The composed desktop puts a
    // material's grain between the two, so it lies *behind* the icons.
    enum class PaintLayers { All, Background, Foreground };
    void setPaintLayers(PaintLayers layers) { m_paintLayers = layers; }
    QRect titleBandRect() const;     // the band, widget coordinates
    QTransform bandTransform() const; // band coordinates -> widget
    int bandLength() const;
    FenceHandle handleAt(const QPoint& point) const;
    // Session 69 (T-115): the title bar as a tab as long as the title, joined
    // to the body by a rounded (concave) junction; the rest of the strip is
    // not the fence's. outlinePath() is the fence's shape for a size (logical
    // pixels): what is painted, and the window region.
    void setTabTitle(bool tab);
    bool tabTitle() const { return m_tabTitle; }
    bool tabbed() const { return m_tabTitle && titleShown(); }
    QPainterPath outlinePath(const QSizeF& size, qreal inset = 0) const;
    QTransform outlineTransform(const QSizeF& size) const;
    QPair<int, int> tabSpan(int length) const; // offset, length along the bar
    int tabPadding() const;
    Qt::Alignment titleAlignment() const { return m_titleAlign; }
    int titleInset() const;
    void setIconOpacity(qreal opacity);
    qreal iconOpacity() const { return m_iconOpacity; }

    // The item under a widget-local point, or -1.
    int itemAt(const QPoint& localPoint) const;

    // T-082: where the grid's tiles are, in *widget* coordinates -- one rectangle
    // per tile, the pixels it owns and nothing more. The container hands these to
    // the icon overlay, whose window region is their union, so the tiles stay
    // opaque while the panel around them takes the material's alpha. Empty for a
    // rolled-up fence (there is no content area) and for a fence with no tiles.
    QVector<QRect> tileRects() const;

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
    // Session 65: global selection -- cleared from outside (another fence or
    // the desktop was clicked). Repaints only when something was selected.
    void clearSelection();
    QStringList selectedPaths() const;
    // Session 65: where tiles dragged from another fence would land (-1: none);
    // and whether this fence's own drag is over another fence (no "back to
    // the desktop" hint then).
    void setExternalDropIndex(int index);
    void setDragOverOtherFence(bool over);

public:
    // Session 62: the picture a drag of tile `index` carries (device pixels at
    // `dpr`), and where the press sits in it (logical). For the ghost and the probe.
    QImage dragImageFor(int index, const QPoint& press, qreal dpr, QPoint* hotspot) const;
    bool dragGhostVisible() const;

signals:
    // Fired whenever the visual content changes (selection, items, sort) so the
    // owner can re-render the offscreen frame and push it to the surface.
    void contentChanged();
    void hoverChanged(bool hovered); // session 71: enter / leave

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

    // Session 60: a double-click on the title *text* (or the menu's Rename) asks
    // to edit it in place. The view is an image on the desktop, so the edit is
    // the product's own window, laid over titleTextRect().
    void titleEditRequested();

    // T-076: a right-click that did **not** land on a tile -- the title bar, the
    // empty part of the panel, or the whole bar of a rolled-up fence. The item
    // under the cursor keeps the Shell's own menu (T-006); everything else is
    // ours, and this is the report that says so. `globalPos` is screen
    // coordinates, the only frame a menu can be placed in.
    //
    // Nothing is built here: FenceMenu owns the content, the product owns the
    // actions (the same split as every other gesture in this widget).
    void fenceMenuRequested(const QPoint& globalPos);
    // Session 64: any press on the fence -- it comes to the front.
    void pressed();
    // Session 65: the user selected (or cleared) icons here -- the selection is
    // global, so the container clears every other fence and the desktop.
    void selectionGesture();
    // Session 65: a right-click on an icon. `paths` is the selection it acts on;
    // `localPos` is in this widget's coordinates (the container makes it
    // physical: the product shows StarFish's menu there).
    void itemMenuRequested(const QStringList& paths, const QPoint& localPos);
    // Session 65: a tile drag (`paths`, the selection) at `localPos`; the same
    // with `dropped` when the button came up outside the fence. The container
    // decides: another fence (insertion mark, move) or the desktop.
    void itemsDraggedOutside(const QStringList& paths, const QPoint& localPos, bool dropped);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
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
    FenceMetrics m_metrics;
    bool m_sortOnAdd = false;
    // Inward width of the resize grips along each edge (T-007).
    static constexpr int kResizeBorder = FenceGeometry::kResizeBorder;

    QString m_title;
    QColor m_panelColor{0x1F, 0x29, 0x37};
    // T-080: invalid until someone picks one (see setTitleBarColor).
    QColor m_titleBarColor;
    int m_borderWidth = 1;
    QColor m_borderColor = QColor(0x3B, 0x82, 0xF6);
    bool m_hoverHighlight = true;
    HoverEffect m_hoverEffect = HoverEffect::Lighter;
    bool m_hovered = false;
    bool m_titleBarShown = true;
    PaintLayers m_paintLayers = PaintLayers::All;
    Qt::Alignment m_titleAlign = Qt::AlignLeft;
    TitleSide m_titleSide = TitleSide::Top;
    bool m_tabTitle = false;
    qreal m_iconOpacity = 1.0;
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
    // Session 62: the picture of the dragged tile, under the cursor.
    DragGhost* m_ghost = nullptr;
    void updateDragGhost(bool dragging);
    // F-011: the pointer of an in-flight tile drag is outside the fence, so the
    // release will hand the tile back to the desktop. Recomputed on every move
    // (dragging back in disarms it again), which is why it is not a one-way
    // flag set when the cursor leaves.
    bool m_releaseArmed = false;
    QStringList m_dragPaths;
    int m_externalDropIndex = -1;
    bool m_dragOverOtherFence = false;

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
