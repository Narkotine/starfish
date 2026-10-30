#pragma once

#include <QHash>
#include <QList>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

#include "core/desktop_item.hpp"

namespace starfish::ui {

// One icon of the desktop layer StarFish draws itself (T-039, ADR-9 option A).
//
// This is the *fence-less* half of the desktop: the entries no fence holds. A
// cell stacks an icon square above a caption band, exactly like IconCell, but
// the cell is wider than the icon so a caption has room -- the width a fence
// grid lacks today (T-049).
struct BackdropCell {
    core::DesktopItem item;
    QRect icon;  // square the icon pixmap is drawn into (centred inside the cell)
    QRect label; // band below the icon the caption is drawn into
};

// Pure geometry and set algebra for the desktop backdrop (T-039). No Win32, no
// widget, no I/O -- so the two rules that matter are unit-tested on any
// platform:
//
//   * **what is free**: the desktop entries no fence holds. The set is
//     *computed* from the fences every time, never stored: ADR-7 stays intact,
//     the document schema does not move, and F-007/F-011 (removing a fence or a
//     tile puts the icon back on the desktop) follow from it with no restore
//     code at all.
//   * **where it lands**: column-major, like the Windows desktop -- fill a
//     column top to bottom, then start the next one. A fence grid fills rows
//     (IconLayout), and reusing that here would look wrong next to a real
//     desktop.
class BackdropLayout
{
public:
    struct Metrics {
        int iconSize = 48;
        int cellWidth = 72; // wider than the icon, so a caption has room
        int labelHeight = 34;
        int gap = 6;
        int margin = 8;
    };

    // Key for "the same file", insensitive to case and to the separator (Windows
    // paths are case-insensitive, and a fence can hold the path a Shell lookup
    // returned while the scanner returned the other spelling). Only ever used
    // for comparison -- never written anywhere, so it cannot corrupt a
    // byte-for-byte round trip.
    static QString pathKey(const QString& path)
    {
        QString key = path;
        key.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (key.endsWith(QLatin1Char('/')))
            key.chop(1);
        return key.toLower();
    }

    static QSet<QString> keysOf(const QList<core::DesktopItem>& items)
    {
        QSet<QString> keys;
        keys.reserve(items.size());
        for (const core::DesktopItem& item : items)
            keys.insert(pathKey(item.path));
        return keys;
    }

    // The entries `desktop` that no fence holds, in the order they came in (the
    // caller has already sorted them). An entry with an empty path is dropped:
    // nothing can be compared, and it could never be claimed by a fence.
    static QList<core::DesktopItem> freeItems(const QList<core::DesktopItem>& desktop,
                                              const QSet<QString>& occupied)
    {
        QList<core::DesktopItem> free;
        free.reserve(desktop.size());
        for (const core::DesktopItem& item : desktop) {
            if (item.path.isEmpty())
                continue;
            if (occupied.contains(pathKey(item.path)))
                continue;
            free.append(item);
        }
        return free;
    }

    // How many cells fit vertically in `height`. Always at least one, so a
    // desktop shorter than a single cell still shows a single row rather than
    // nothing.
    static int rowsForHeight(int height, const Metrics& metrics)
    {
        const int iconSize = qMax(1, metrics.iconSize);
        const int labelHeight = qMax(0, metrics.labelHeight);
        const int gap = qMax(0, metrics.gap);
        const int margin = qMax(0, metrics.margin);
        const int usable = height - 2 * margin;
        if (usable <= 0)
            return 1;
        return qMax(1, (usable + gap) / (iconSize + labelHeight + gap));
    }

    // One cell per item, column-major inside `area`. Items past the right edge
    // are still computed (the caller clips), so the vector length always equals
    // `items.size()`.
    //
    // `boxes`, when given, is what the *shell* says about an icon: the slot it
    // occupies on the desktop, keyed by the item's display name. An item it names
    // is laid out there instead of in the flow -- which is what lets StarFish draw
    // a desktop that looks like the user's own when it has to draw it at all
    // (ADR-13: auto-arrange on, so a single icon cannot be moved out of the way).
    // A named box wins over the flow; anything unnamed still flows, so a caller
    // that knows about two icons out of eighty still gets a full desktop.
    static QVector<BackdropCell> compute(const QRect& area, const Metrics& metrics,
                                         const QList<core::DesktopItem>& items,
                                         const QHash<QString, QRect>& boxes = {})
    {
        QVector<BackdropCell> cells;
        cells.reserve(items.size());
        if (items.isEmpty())
            return cells;

        const int iconSize = qMax(1, metrics.iconSize);
        const int cellWidth = qMax(iconSize, metrics.cellWidth);
        const int labelHeight = qMax(0, metrics.labelHeight);
        const int gap = qMax(0, metrics.gap);
        const int margin = qMax(0, metrics.margin);
        const int rowPitch = iconSize + labelHeight + gap;
        const int columnPitch = cellWidth + gap;
        const int rows = rowsForHeight(area.height(), metrics);

        // Where the shell put each icon, for the ones it named. Looked up by the
        // display name (the scanner and the read both ask the shell, so the two
        // spell a name the same way), then by the on-disk name.
        const auto boxFor = [&boxes](const core::DesktopItem& item) -> QRect {
            if (boxes.isEmpty())
                return {};
            // ADR-21: the drawn desktop keys the shell's slots by path -- a name
            // is not unique (a user and a Public shortcut can share one).
            const auto byPath = boxes.constFind(pathKey(item.path));
            if (byPath != boxes.constEnd())
                return byPath.value();
            const auto byName = boxes.constFind(item.name);
            if (byName != boxes.constEnd())
                return byName.value();
            const auto byFile = boxes.constFind(item.fileName);
            if (byFile != boxes.constEnd())
                return byFile.value();
            return {};
        };

        for (int i = 0; i < items.size(); ++i) {
            BackdropCell cell;
            cell.item = items.at(i);

            const QRect box = boxFor(cell.item);
            if (box.isValid() && !box.isEmpty()) {
                // The icon sits at the top of the slot the shell reported, centred
                // in it; the caption goes under it, exactly as the flow does it.
                cell.icon = QRect(box.x() + (box.width() - iconSize) / 2, box.y(), iconSize,
                                  iconSize);
                cell.label = QRect(box.x(), box.y() + iconSize, box.width(), labelHeight);
                cells.append(cell);
                continue;
            }

            const int column = i / rows;
            const int row = i % rows;
            const int x = area.x() + margin + column * columnPitch;
            const int y = area.y() + margin + row * rowPitch;

            cell.icon = QRect(x + (cellWidth - iconSize) / 2, y, iconSize, iconSize);
            cell.label = QRect(x, y + iconSize, cellWidth, labelHeight);
            cells.append(cell);
        }
        return cells;
    }
};

} // namespace starfish::ui
