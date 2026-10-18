#pragma once

#include <algorithm>
#include <functional>

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace starfish::ui {

// Pure geometry for the icon grid of a fence (T-005). No drawing, no Win32, no
// widget -- so the math is unit-tested in tests/test_icon_grid.cpp on any
// platform. The matching paint logic lives in IconGrid.
//
// A cell stacks an icon square above a caption band:
//
//        +-----------+   -+
//        |   icon    |    | iconSize
//        +-----------+   -+
//        |  caption  |    | labelHeight (two lines at most)
//        +-----------+   -+
//        <--cellW---->      gap -> next column
//
// cellW is *wider* than the icon (T-049). A cell only as wide as its icon gives
// a caption no room whatsoever: "Epic Games Launcher" came out as "Epi...her",
// elided in the middle -- exactly what the user reported ("les noms ne
// s'affichent pas entièrement" -- the names do not show in full). The icon is
// centred inside the cell, and the caption wraps to the cell width, on two
// lines at most, elided on the right.
//
// Since T-078 the cell is not a fixed 1.5x either: `cellWidthFor` is the floor
// and `cellWidthForCaptions` gives a fence whose longest name needs more the
// room it needs (bounded by `maxCellWidthFor`). See IconGrid::cellWidth.
struct IconCell {
    QRect icon;  // square the icon pixmap is drawn into (centred inside cellW)
    QRect label; // band below the icon the caption is drawn into
};

class IconLayout
{
public:
    // How wide a cell is for a given icon size: 1.5x, i.e. 72 px for the default
    // 48 px icon. That is the room a two-line caption needs, and the same order
    // as the Windows desktop (~75 px for 48 px icons). Since T-078 this is the
    // *floor*: a fence whose longest caption needs more gets a wider cell, up to
    // `maxCellWidthFor`.
    static int cellWidthFor(int iconSize) { return iconSize + iconSize / 2; }

    // The widest a cell may get (T-078): twice the icon. Deriving the width from
    // the longest caption would otherwise let one very long name thin out the
    // whole grid, and past this point a caption is elided whatever the cell
    // does -- so the search below is bounded, and the bound is a documented
    // number rather than a constant buried in a caller.
    static int maxCellWidthFor(int iconSize) { return iconSize * 2; }

    // The character a shortened caption ends with.
    static constexpr QChar kEllipsis = QChar(u'\u2026');

    // How many columns fit given the content width. Always at least one, so a
    // fence narrower than a single cell still shows a single column. A column is
    // `cellWidth` wide -- wider than the icon since T-049.
    static int columnsForWidth(int viewportWidth, int cellWidth, int gap, int padding)
    {
        if (viewportWidth <= 0 || cellWidth <= 0)
            return 1;
        const int usable = viewportWidth - 2 * padding;
        if (usable <= 0)
            return 1;
        // (cols - 1) * gap between columns; each column is cellWidth wide.
        const int cols = (usable + gap) / (cellWidth + gap);
        return qMax(1, cols);
    }

    // One cell per item, row-major, top-to-bottom then left-to-right. Items past
    // the bottom are still computed (the caller clips / scrolls), so the vector
    // length always equals `itemCount`.
    static QVector<IconCell> compute(const QSize& viewport, int iconSize, int cellWidth,
                                     int labelHeight, int gap, int padding, int itemCount)
    {
        QVector<IconCell> cells;
        cells.reserve(qMax(0, itemCount));
        if (itemCount <= 0)
            return cells;

        const int icon = qMax(1, iconSize);
        const int cellW = qMax(icon, cellWidth);
        const int columns = columnsForWidth(viewport.width(), cellW, gap, padding);
        const int cellH = icon + labelHeight;

        for (int i = 0; i < itemCount; ++i) {
            const int col = i % columns;
            const int row = i / columns;
            const int x = padding + col * (cellW + gap);
            const int y = padding + row * (cellH + gap);

            IconCell cell;
            cell.icon = QRect(x + (cellW - icon) / 2, y, icon, icon);
            cell.label = QRect(x, y + icon, cellW, labelHeight);
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

    // --- captions (T-049) ---------------------------------------------------

    // Splits `text` into at most `maxLines` lines that each fit within `width`
    // pixels, greedily filling each line with whole words and eliding the last
    // line on the *right* when the text still does not fit -- the way Windows
    // labels a desktop icon. A single word wider than the line is cut with the
    // same ellipsis (there is no better break inside it). Eliding in the middle
    // is what turned a real name into a stub, so it is deliberately not offered.
    //
    // `widthOf` measures a candidate string (QFontMetrics::horizontalAdvance
    // when painting, a stub in the unit tests), which is what keeps this header
    // free of QtGui.
    static QStringList wrapToLines(const QString& text, int maxLines, int width,
                                   const std::function<int(const QString&)>& widthOf)
    {
        QStringList lines;
        const int maxW = qMax(1, width);
        const int lineCount = qMax(1, maxLines);
        const QStringList words = text.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.isEmpty())
            return lines;

        QString pending;
        for (int i = 0; i < words.size(); ++i) {
            const QString& word = words.at(i);
            const QString candidate =
                pending.isEmpty() ? word : pending + QLatin1Char(' ') + word;
            if (widthOf(candidate) <= maxW) {
                pending = candidate;
                continue;
            }
            if (pending.isEmpty()) {
                // A word wider than the whole line: it takes the line by itself
                // and will be elided when it is emitted.
                pending = word;
                continue;
            }
            if (lines.size() == lineCount - 1) {
                // The last line holds whatever is left of the text, elided, so
                // the cut is visible rather than the name quietly stopping.
                QString rest = pending;
                for (int j = i; j < words.size(); ++j)
                    rest += QLatin1Char(' ') + words.at(j);
                lines.append(elidedRight(rest, maxW, widthOf));
                return lines;
            }
            lines.append(elidedRight(pending, maxW, widthOf));
            pending = word;
        }
        lines.append(elidedRight(pending, maxW, widthOf));
        return lines;
    }

    // `text`, shortened on the right with an ellipsis so that it fits in
    // `width` -- or unchanged when it already does.
    static QString elidedRight(const QString& text, int width,
                               const std::function<int(const QString&)>& widthOf)
    {
        if (widthOf(text) <= width)
            return text;
        int lo = 0;
        int hi = text.size();
        while (lo < hi) {
            const int mid = (lo + hi + 1) / 2;
            if (widthOf(text.left(mid) + kEllipsis) <= width)
                lo = mid;
            else
                hi = mid - 1;
        }
        return text.left(lo).trimmed() + kEllipsis;
    }

    // The width a cell must have for *every* caption to wrap onto at most
    // `maxLines` lines without being elided (T-078) -- never below `minWidth`
    // (the fixed cell of T-049) and never above `maxWidth`.
    //
    // The user's report was "lisible pour la plupart des noms, manque encore un
    // peu pour les plus longs": a fixed cell is exactly right for the average
    // name and cuts the long ones, and the fix is to let the *longest* caption
    // of the fence decide -- there is only one cell width per grid, so one name
    // that needs room already decides for all of them.
    //
    // `fits()` asks `wrapToLines` itself, the very function the painter wraps
    // with, so the geometry cannot disagree with what is drawn. Candidates are
    // the widths of every group of consecutive words: between two of them the
    // greedy wrap makes the same decisions, so a width in between can never be
    // the first one that fits -- which is why this scans instead of searching,
    // and why the answer does not rest on `fits` being monotone.
    static int cellWidthForCaptions(int minWidth, int maxWidth, const QStringList& captions,
                                    int maxLines, const std::function<int(const QString&)>& widthOf)
    {
        const int floorWidth = qMax(1, minWidth);
        const int ceilWidth = qMax(floorWidth, maxWidth);
        const auto fits = [&](int width) {
            for (const QString& caption : captions) {
                const QStringList lines = wrapToLines(caption, maxLines, width, widthOf);
                for (const QString& line : lines) {
                    if (line.endsWith(kEllipsis))
                        return false;
                }
            }
            return true;
        };

        if (captions.isEmpty() || fits(floorWidth))
            return floorWidth;

        QVector<int> candidates;
        candidates.append(ceilWidth);
        for (const QString& caption : captions) {
            const QStringList words =
                caption.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (int first = 0; first < words.size(); ++first) {
                QString group;
                for (int last = first; last < words.size(); ++last) {
                    group = group.isEmpty() ? words.at(last)
                                            : group + QLatin1Char(' ') + words.at(last);
                    const int width = widthOf(group);
                    if (width > floorWidth && width < ceilWidth)
                        candidates.append(width);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (int width : candidates) {
            if (fits(width))
                return width;
        }
        // Not even the widest cell holds this caption (a single word longer than
        // the ceiling, say): the cell takes the ceiling and the caption is elided
        // there, exactly as it was before T-078 -- only wider.
        return ceilWidth;
    }
};

} // namespace starfish::ui
