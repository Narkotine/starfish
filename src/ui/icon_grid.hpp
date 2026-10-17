#pragma once

#include <functional>
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

    int iconSize() const { return m_iconSize; }
    // The width of one grid cell: wider than the icon, so a caption has room to
    // wrap instead of being cut to the width of the icon square (T-049). Derived
    // from the icon size rather than stored, so the two cannot drift apart.
    int cellWidth() const { return IconLayout::cellWidthFor(m_iconSize); }
    int gap() const { return m_gap; }
    int padding() const { return m_padding; }
    int labelHeight() const { return m_labelHeight; }

    // Draws the grid inside `contentRect` (already excludes the title bar and
    // the fence's outer padding). Caller is expected to clip to contentRect.
    void paint(QPainter& painter, const QRect& contentRect, IconProvider iconFor) const;

    // Index of the item at a content-local point, or -1.
    int hitTest(const QPoint& contentLocalPoint, const QSize& contentSize) const;

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

    // --- folder portals (T-011) ---------------------------------------------
    // Removes every tile whose path is in `paths`, compared case-insensitively
    // (Windows paths are, and a portal listing can come back with a different
    // capitalisation than the one it went out with). The selection follows the
    // tiles that stay -- it is not cleared, because a folder changing under a
    // portal is not the user selecting something else. Returns the number
    // removed.
    int removeItemsByPath(const QStringList& paths);

    // The path QDesktopServices should open for a double-click (F-005 / T-005).
    // A .lnk/.url opens its resolved target, everything else opens itself.
    static QString launchPath(const core::DesktopItem& item);

private:
    QList<core::DesktopItem> m_items;
    QString m_sortBy = QStringLiteral("name");
    bool m_ascending = true;

    int m_iconSize = 48;
    int m_gap = 8;
    int m_padding = 12;
    int m_labelHeight = 34;

    QSet<int> m_selected;
    int m_anchor = 0;
};

} // namespace starfish::ui
