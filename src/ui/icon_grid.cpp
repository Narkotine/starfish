#include "ui/icon_grid.hpp"

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPixmap>
#include <QRect>

#include "core/desktop_item.hpp"

namespace starfish::ui {

namespace {
constexpr QRgb kIconPlaceholder = 0x37; // dim gray square when no icon resolves
const QColor kLabelText(0xE5, 0xE7, 0xEB);
const QColor kLabelTextSelected(0xFF, 0xFF, 0xFF);
const QColor kSelection(0x3B, 0x82, 0xF6, 0x66); // accent, semi-transparent
const QColor kSelectionBorder(0x3B, 0x82, 0xF6);
} // namespace

IconGrid::IconGrid() = default;

void IconGrid::setItems(QList<core::DesktopItem> items)
{
    core::sortForDisplay(items, m_sortBy, m_ascending);
    m_items = std::move(items);
    m_selected.clear();
    m_anchor = 0;
}

void IconGrid::setSort(const QString& by, bool ascending)
{
    m_sortBy = by;
    m_ascending = ascending;
    core::sortForDisplay(m_items, m_sortBy, m_ascending);
    m_selected.clear();
    m_anchor = 0;
}

void IconGrid::paint(QPainter& painter, const QRect& contentRect, IconProvider iconFor) const
{
    if (m_items.isEmpty())
        return;

    const QVector<IconCell> cells =
        IconLayout::compute(contentRect.size(), m_iconSize, m_labelHeight, m_gap, m_padding,
                            m_items.size());

    painter.save();
    // The cells are in *content-local* coordinates -- the same space hitTest()
    // consumes -- so the painter has to move to the content origin first. Without
    // this the whole grid is painted `contentRect.top()` (title bar + padding)
    // too high, and the clip below then cuts the top off the first row: reported
    // from the real desktop as "l'icône est à moitié cachée par la barre de
    // titre". The drop indicator already translated its cells, which is how the
    // inconsistency went unnoticed.
    painter.translate(contentRect.topLeft());
    painter.setClipRect(QRect(QPoint(0, 0), contentRect.size()));

    const QFont baseFont = painter.font();
    QFont labelFont = baseFont;
    labelFont.setPointSize(qMax(8, labelFont.pointSize() - 1));
    painter.setFont(labelFont);
    const QFontMetrics metrics(labelFont);

    for (int i = 0; i < m_items.size(); ++i) {
        const IconCell& cell = cells.at(i);
        const core::DesktopItem& item = m_items.at(i);
        const bool selected = m_selected.contains(i);

        if (selected) {
            QRect highlight = cell.icon.united(cell.label).adjusted(-3, -3, 3, 3);
            painter.setPen(Qt::NoPen);
            painter.setBrush(kSelection);
            painter.drawRoundedRect(highlight, 6, 6);
            painter.setPen(kSelectionBorder);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(highlight, 6, 6);
        }

        // Icon.
        const QPixmap icon = iconFor ? iconFor(item, m_iconSize) : QPixmap();
        if (icon.isNull()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(kIconPlaceholder, kIconPlaceholder, kIconPlaceholder));
            painter.drawRect(cell.icon);
        } else {
            const QPixmap scaled =
                icon.scaled(cell.icon.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter.drawPixmap(
                cell.icon.x() + (cell.icon.width() - scaled.width()) / 2,
                cell.icon.y() + (cell.icon.height() - scaled.height()) / 2, scaled);
        }

        // Label: single line, elided to the cell width.
        painter.setPen(selected ? kLabelTextSelected : kLabelText);
        painter.drawText(cell.label, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                         metrics.elidedText(item.name, Qt::ElideMiddle, cell.label.width()));
    }

    painter.restore();
    Q_UNUSED(baseFont);
}

int IconGrid::hitTest(const QPoint& contentLocalPoint, const QSize& contentSize) const
{
    const QVector<IconCell> cells =
        IconLayout::compute(contentSize, m_iconSize, m_labelHeight, m_gap, m_padding,
                            m_items.size());
    return IconLayout::hitTest(cells, contentLocalPoint);
}

void IconGrid::selectOnly(int index)
{
    m_selected.clear();
    if (index >= 0 && index < m_items.size())
        m_selected.insert(index);
    m_anchor = index;
}

void IconGrid::toggle(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    if (m_selected.contains(index))
        m_selected.remove(index);
    else
        m_selected.insert(index);
    m_anchor = index;
}

void IconGrid::selectRangeTo(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    const int from = qMin(m_anchor, index);
    const int to = qMax(m_anchor, index);
    m_selected.clear();
    for (int i = from; i <= to; ++i)
        m_selected.insert(i);
}

void IconGrid::clearSelection()
{
    m_selected.clear();
}

int IconGrid::moveItem(int from, int to)
{
    if (from < 0 || from >= m_items.size())
        return -1;
    if (to < 0 || to >= m_items.size())
        return -1;
    if (from == to)
        return to;

    // Semantics matching QList::move: the moved item ends up at index `to` in
    // the final list. `takeAt(from)` then `insert(to, item)` does exactly
    // that -- the items between shift to make room, and the moved item lands
    // at the requested slot. No shifted-target adjustment needed.
    core::DesktopItem item = m_items.takeAt(from);
    m_items.insert(to, item);

    // The moved item is the only thing selected, and the anchor follows it.
    m_anchor = to;
    m_selected.clear();
    m_selected.insert(to);
    return to;
}

int IconGrid::dropTargetIndex(const QPoint& contentLocalPoint, const QSize& contentSize) const
{
    if (m_items.isEmpty())
        return -1;
    return hitTest(contentLocalPoint, contentSize);
}

int IconGrid::appendItems(const QList<core::DesktopItem>& items)
{
    int added = 0;
    for (const core::DesktopItem& item : items) {
        if (item.path.isEmpty())
            continue;
        bool duplicate = false;
        for (const core::DesktopItem& existing : m_items) {
            if (existing.path == item.path) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        m_items.append(item);
        ++added;
    }
    if (added > 0) {
        core::sortForDisplay(m_items, m_sortBy, m_ascending);
        m_selected.clear();
        m_anchor = 0;
    }
    return added;
}

int IconGrid::removeItemsByPath(const QStringList& paths)
{
    if (paths.isEmpty() || m_items.isEmpty())
        return 0;

    QSet<QString> doomed;
    for (const QString& path : paths) {
        if (!path.isEmpty())
            doomed.insert(path.toLower());
    }
    if (doomed.isEmpty())
        return 0;

    QList<core::DesktopItem> kept;
    kept.reserve(m_items.size());
    QHash<int, int> remap; // old index -> new index, for the selection
    for (int i = 0; i < m_items.size(); ++i) {
        if (doomed.contains(m_items.at(i).path.toLower()))
            continue;
        remap.insert(i, kept.size());
        kept.append(m_items.at(i));
    }

    const int removed = m_items.size() - kept.size();
    if (removed == 0)
        return 0;

    QSet<int> selection;
    for (int index : m_selected) {
        const auto mapped = remap.constFind(index);
        if (mapped != remap.constEnd())
            selection.insert(*mapped);
    }
    m_selected = selection;
    // The anchor may have been one of the removed tiles: fall back to the first
    // cell rather than to an index that no longer exists.
    m_anchor = remap.value(m_anchor, 0);
    m_items = std::move(kept);
    return removed;
}

QString IconGrid::launchPath(const core::DesktopItem& item)
{
    // A shortcut opens its target; QDesktopServices::openUrl on the .lnk already
    // resolves to the target on Windows, but using the resolved path is explicit
    // and also covers the rare case where the link target is empty.
    return item.targetPath.isEmpty() ? item.path : item.targetPath;
}

} // namespace starfish::ui
