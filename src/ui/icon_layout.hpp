#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>

namespace starfish::ui {

// Pure geometry for the icon grid of a fence (T-005). No drawing, no Win32, no
// widget -- so the math is unit-tested in tests/test_icon_grid.cpp on any
// platform. The matching paint logic lives in IconGrid.
//
// A cell stacks an icon square above a label band:
//
//        +-----------+   -+
//        |   icon    |    | iconSize
//        +-----------+   -+
//        |  label    |    | labelHeight
//        +-----------+   -+
//        <--cellW-->      gap -> next column
//
// cellW is the icon size; the icon is centred horizontally so the label can
// wrap to the same width.
struct IconCell {
    QRect icon;  // square the icon pixmap is drawn into (centred inside cellW)
    QRect label; // band below the icon the caption is drawn into
};

class IconLayout
{
public:
    // How many columns fit given the content width. Always at least one, so a
    // fence narrower than a single cell still shows a single column.
    static int columnsForWidth(int viewportWidth, int iconSize, int gap, int padding)
    {
        if (viewportWidth <= 0 || iconSize <= 0)
            return 1;
        const int usable = viewportWidth - 2 * padding;
        if (usable <= 0)
            return 1;
        // (cols - 1) * gap between columns; each column is iconSize wide.
        const int cols = (usable + gap) / (iconSize + gap);
        return qMax(1, cols);
    }

    // One cell per item, row-major, top-to-bottom then left-to-right. Items past
    // the bottom are still computed (the caller clips / scrolls), so the vector
    // length always equals `itemCount`.
    static QVector<IconCell> compute(const QSize& viewport, int iconSize, int labelHeight,
                                     int gap, int padding, int itemCount)
    {
        QVector<IconCell> cells;
        cells.reserve(qMax(0, itemCount));
        if (itemCount <= 0)
            return cells;

        const int columns = columnsForWidth(viewport.width(), iconSize, gap, padding);
        const int cellW = iconSize;
        const int cellH = iconSize + labelHeight;

        for (int i = 0; i < itemCount; ++i) {
            const int col = i % columns;
            const int row = i / columns;
            const int x = padding + col * (cellW + gap);
            const int y = padding + row * (cellH + gap);

            IconCell cell;
            cell.icon = QRect(x + (cellW - iconSize) / 2, y, iconSize, iconSize);
            cell.label = QRect(x, y + iconSize, cellW, labelHeight);
            cells.append(cell);
        }
        return cells;
    }

    // Index of the cell containing `point` (in the same coordinate space as the
    // cells, i.e. content-local), or -1 when the point is outside every cell.
    static int hitTest(const QVector<IconCell>& cells, const QPoint& point)
    {
        for (int i = 0; i < cells.size(); ++i) {
            const QRect hit = cells.at(i).icon.united(cells.at(i).label);
            if (hit.contains(point))
                return i;
        }
        return -1;
    }
};

} // namespace starfish::ui
