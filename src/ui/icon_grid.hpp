#pragma once

#include <functional>
#include <QFont>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>

#include "core/desktop_item.hpp"
#include "ui/icon_layout.hpp"

class QPainter;
class QPixmap;

namespace starfish::ui {

// The icon grid of a single fence (T-005). It owns the *model* of the grid
// (which items, how they are sorted, which are selected) and knows how to draw
// it. It does not own a window: FenceView renders it inside its content area,
// and the same offscreen image is what DesktopSurface blits to the desktop.
//
// Painting is decoupled from the icon source through IconProvider, so the grid
// itself stays free of any Windows/Shel1 dependency and can be unit-tested
// with a stub provider.
class IconGrid
{
public:
    // Returns the QPixmap to draw for `item` at `sizePx`, or a null pixmap when
    // none is available (the grid then draws a placeholder). The provider is
    // typically a ShellIconCache bound by FenceView.
    using IconProvider = std::function<QPixmap(const core::DesktopItem& item, int sizePx)>;

    IconGrid();

    void setItems(QList<core::DesktopItem> items);
    const QList<core::DesktopItem>& items() const { return m_items; }

    // Sort fields match Fence::sortBy in core/jsonstore.hpp: name|type|size|date.
    void setSort(const QString& by, bool ascending);
    QString sortBy() const { return m_sortBy; }
    bool ascending() const { return m_ascending; }

    // Visual layout. Defaults mirror the schema's v1 values (docs/technique.md §5).
    void setIconSize(int px) { m_iconSize = qMax(16, px); }
    void setGap(int px) { m_gap = qMax(0, px); }
    void setPadding(int px) { m_padding = qMax(0, px); }
    void setLabelHeight(int px) { m_labelHeight = qMax(0, px); }
    // T-098: captions get the desktop's drop shadow (a translucent fence shows
    // the wallpaper behind them).
    void setTextShadow(bool on) { m_textShadow = on; }
    bool textShadow() const { return m_textShadow; }

    // The font the captions are drawn with -- and the one the cell width is
    // derived from (T-078). The view hands over its own font, so the room a
    // caption asks for and the room it is painted into are measured by the same
    // metrics, and `hitTest` keeps landing on the cells `paint` drew. A grid
    // that is never given one (a unit test, a grid with no view) keeps the fixed
    // 1.5x cell and whatever font the painter arrives with.
    void setBaseFont(const QFont& font);
    bool hasBaseFont() const { return m_hasBaseFont; }

    int iconSize() const { return m_iconSize; }
    // Session 62: the icon and caption rects of every cell (content-local).
    QVector<IconCell> cells(const QSize& contentSize) const
    {
        return IconLayout::compute(contentSize, m_iconSize, cellWidth(), m_labelHeight, m_gap, m_padding,
                                   m_items.size());
    }
    QFont captionFont() const { return m_captionFont; }
    // The width of one grid cell: wider than the icon, so a caption has room to
    // wrap instead of being cut to the width of the icon square (T-049) --
    // and, since T-078, as wide as the longest caption of this grid needs,
    // between 1.5x and 2x the icon. Derived from the items rather than stored,
    // so the two cannot drift apart.
    int cellWidth() const;
    // T-085: a width chosen by hand (0: automatic).
    void setFixedCellWidth(int px) { m_fixedCellWidth = qMax(0, px); }
    int fixedCellWidth() const { return m_fixedCellWidth; }
    int gap() const { return m_gap; }
    int padding() const { return m_padding; }
    int labelHeight() const { return m_labelHeight; }

    // Draws the grid inside `contentRect` (already excludes the title bar and
    // the fence's outer padding). Caller is expected to clip to contentRect.
    void paint(QPainter& painter, const QRect& contentRect, IconProvider iconFor) const;

    // Index of the item at a content-local point, or -1.
    int hitTest(const QPoint& contentLocalPoint, const QSize& contentSize) const;

    // Where the tiles are, in content-local coordinates: one rectangle per item
    // -- the icon square united with its caption band -- clipped to
    // `contentSize`. Exactly the pixels a tile owns, and the very cells `paint`
    // and `hitTest` use, so the three cannot disagree (T-082). The container
    // turns these into its icon overlay's window region, which is what keeps a
    // translucent fence's tiles opaque.
    QVector<QRect> cellRects(const QSize& contentSize) const;

    // --- selection ---------------------------------------------------------
    void selectOnly(int index);
    void toggle(int index);
    void selectRangeTo(int index); // extend from the anchor (Shift)
    void clearSelection();
    QSet<int> selectedIndices() const { return m_selected; }
    bool isSelected(int index) const { return m_selected.contains(index); }

    // --- reorder (T-006-2, intra-fence) -------------------------------------
    // Move a single item from `from` to the slot previously occupied by `to`.
    // Items between shift to accommodate. Out-of-range indexes are a no-op
    // (returns -1); from == to is a no-op that returns `to`. The selection
    // follows the moved item, and the anchor is set to its new position so
    // a subsequent Shift-select behaves naturally.
    // Returns the new index of the moved item, or -1 when nothing moved.
    int moveItem(int from, int to);

    // Index that would receive a drop at `contentLocalPoint` (the cell the
    // pointer is hovering over). Returns -1 when the point is in empty space
    // below the last row -- the caller decides what "drop at the end" means
    // for its UX (in FenceView, that's the bottom-right cell).
    int dropTargetIndex(const QPoint& contentLocalPoint, const QSize& contentSize) const;

    // --- drop (T-006-3, from Explorer) ---------------------------------------
    // Appends items coming from a drag-and-drop, de-duplicating by path, then
    // re-sorts to the current order and clears the selection (a drop is a fresh
    // batch, not a continuation of the current selection). Returns the number
    // of items actually added (0 when all were duplicates or the list empty).
    int appendItems(const QList<core::DesktopItem>& items);

    // Session 60: an addition by the user (a drop, a filing). Inserted before
    // `index` (-1: at the end) and **not** sorted, and the grid's order becomes
    // "manual" so it survives a restart -- unless `sortByName`, the user's
    // switch, which files the new tiles alphabetically. Duplicates are skipped.
    int insertItems(const QList<core::DesktopItem>& items, int index, bool sortByName);

    // --- folder portals (T-011) ---------------------------------------------
    // Removes every tile whose path is in `paths`, compared case-insensitively
    // (Windows paths are, and a portal listing can come back with a different
    // capitalisation than the one it went out with). The selection follows the
    // tiles that stay -- it is not cleared, because a folder changing under a
    // portal is not the user selecting something else. Returns the number
    // removed.
    int removeItemsByPath(const QStringList& paths);
    // Session 74: the tile of `oldPath` becomes `item`, in place (a rename);
    // false when there is no such tile.
    bool replaceItem(const QString& oldPath, const core::DesktopItem& item);
    // Session 65: the user placed icons by hand -- the order is now his.
    void setManualOrder() { m_sortBy = QStringLiteral("manual"); }

    // The path QDesktopServices should open for a double-click (F-005 / T-005).
    // A .lnk/.url opens its resolved target, everything else opens itself.
    static QString launchPath(const core::DesktopItem& item);

private:
    QList<core::DesktopItem> m_items;
    QString m_sortBy = QStringLiteral("name");
    int m_fixedCellWidth = 0;
    bool m_ascending = true;

    int m_iconSize = 48;
    int m_gap = 8;
    int m_padding = 12;
    int m_labelHeight = 34;
    bool m_textShadow = false;

    // The caption font, one point below the view's own (T-049). Only meaningful
    // once setBaseFont() has been called: m_hasBaseFont tells the two apart, so a
    // grid that was never given a font measures nothing and keeps the fixed cell.
    QFont m_captionFont;
    bool m_hasBaseFont = false;

    QSet<int> m_selected;
    int m_anchor = 0;
};

} // namespace starfish::ui
