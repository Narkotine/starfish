#pragma once

#include <functional>
#include <QList>
#include <QSet>

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
