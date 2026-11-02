#include "ui/icon_grid.hpp"

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRect>
#include <QStringList>

#include "core/desktop_item.hpp"

namespace starfish::ui {

namespace {
constexpr QRgb kIconPlaceholder = 0x37; // dim gray square when no icon resolves
// Captions get two lines (T-049); a third line would not fit the label band and
// would overlap the next row's icon.
constexpr int kCaptionLines = 2;
const QColor kLabelText(0xE5, 0xE7, 0xEB);
const QColor kLabelTextSelected(0xFF, 0xFF, 0xFF);
const QColor kSelection(0x3B, 0x82, 0xF6, 0x66); // accent, semi-transparent
const QColor kSelectionBorder(0x3B, 0x82, 0xF6);

// The caption font for a given base font: one point smaller, never below 8 pt.
// One function, used by setBaseFont() *and* by paint() when no font was handed
// over, so the width a caption is measured into and the width it is drawn into
// are the same number (T-078) -- the cell is derived from the text, and a
// measurement taken with another font would be a measurement of something else.
QFont captionFontFrom(const QFont& base)
{
    QFont font = base;
    font.setPointSize(qMax(8, font.pointSize() - 1));
    return font;
}
} // namespace

IconGrid::IconGrid() = default;

void IconGrid::setBaseFont(const QFont& font)
{
    m_captionFont = captionFontFrom(font);
    m_hasBaseFont = true;
}

int IconGrid::cellWidth() const
{
    // T-049's fixed cell is the floor; a fence whose longest caption does not fit
    // it gets the room it needs, up to twice the icon (T-078). Nothing else
    // changes: short names keep the exact cell they had.
    const int floorWidth = IconLayout::cellWidthFor(m_iconSize);
    if (!m_labelsShown) // session 76: no name to make room for
        return m_fixedCellWidth > 0 ? qMax(m_fixedCellWidth, m_iconSize + 8) : m_iconSize + 12;
    if (m_fixedCellWidth > 0)
        return qMax(m_fixedCellWidth, m_iconSize + 8);
    if (!m_hasBaseFont || m_items.isEmpty())
        return floorWidth;

    const QFontMetrics metrics(m_captionFont);
    QStringList captions;
    captions.reserve(m_items.size());
    for (const core::DesktopItem& item : m_items)
        captions.append(item.name.isEmpty() ? item.fileName : item.name);

    return IconLayout::cellWidthForCaptions(
        floorWidth, IconLayout::maxCellWidthFor(m_iconSize), captions, kCaptionLines,
        [&metrics](const QString& text) { return metrics.horizontalAdvance(text); });
}

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

// Session 74: a tile whose file is not found -- a plain page with a warning
// badge instead of the old dark square that said nothing.
static void paintMissingIcon(QPainter& painter, const QRect& box, bool restorable)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = box.width(), h = box.height();
    const QRectF page(box.x() + w * 0.2, box.y() + h * 0.08, w * 0.6, h * 0.8);
    const qreal fold = page.width() * 0.32;
    QPainterPath sheet;
    sheet.moveTo(page.topLeft());
    sheet.lineTo(page.right() - fold, page.top());
    sheet.lineTo(page.right(), page.top() + fold);
    sheet.lineTo(page.bottomRight());
    sheet.lineTo(page.bottomLeft());
    sheet.closeSubpath();
    painter.setPen(QPen(QColor(0x6B, 0x72, 0x80), qMax(1.0, w / 40.0)));
    painter.setBrush(QColor(0xE5, 0xE7, 0xEB, 0xD8));
    painter.drawPath(sheet);
    QPainterPath corner;
    corner.moveTo(page.right() - fold, page.top());
    corner.lineTo(page.right() - fold, page.top() + fold);
    corner.lineTo(page.right(), page.top() + fold);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(corner);
    const qreal d = w * 0.46;
    const QRectF badge(box.x() + w - d - w * 0.04, box.y() + h - d - h * 0.02, d, d);
    painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), qMax(1.0, w / 36.0)));
    painter.setBrush(restorable ? QColor(0x3B, 0x82, 0xF6) : QColor(0xF5, 0x9E, 0x0B));
    painter.drawEllipse(badge);
    if (restorable) {
        // Session 75: StarFish kept a copy -- a "put back" arrow, not a warning.
        const QRectF ring = badge.adjusted(d * 0.26, d * 0.26, -d * 0.26, -d * 0.26);
        QPen arrow(QColor(0xFF, 0xFF, 0xFF), qMax(1.2, d / 9.0), Qt::SolidLine, Qt::RoundCap);
        painter.setPen(arrow);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(ring, 90 * 16, 270 * 16);
        const QPointF tip(ring.center().x(), ring.top());
        const qreal a = d * 0.14;
        painter.drawLine(tip, tip + QPointF(-a, -a * 0.9));
        painter.drawLine(tip, tip + QPointF(-a, a * 0.9));
        painter.restore();
        return;
    }
    QFont bang = painter.font();
    bang.setBold(true);
    bang.setPixelSize(qMax(6, qRound(d * 0.72)));
    painter.setFont(bang);
    painter.setPen(QColor(0xFF, 0xFF, 0xFF));
    painter.drawText(badge, Qt::AlignCenter, QStringLiteral("!"));
    painter.restore();
}

void IconGrid::paint(QPainter& painter, const QRect& contentRect, IconProvider iconFor) const
{
    if (m_items.isEmpty())
        return;

    const QVector<IconCell> cells =
        IconLayout::compute(contentRect.size(), m_iconSize, cellWidth(), labelHeight(), m_gap,
                            m_padding, m_items.size(), m_flow);

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
    // The font the cell width was derived from, when the view gave us one -- so
    // what is drawn is what was measured (T-078).
    const QFont labelFont = m_hasBaseFont ? m_captionFont : captionFontFrom(baseFont);
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
        const QPixmap icon = item.missing ? QPixmap() : (iconFor ? iconFor(item, m_iconSize) : QPixmap());
        if (item.missing) {
            paintMissingIcon(painter, cell.icon, item.restorable);
        } else if (icon.isNull()) {
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

        if (!m_labelsShown)
            continue; // session 76: names hidden (the tooltip says it)
        // Caption: at most two lines, elided on the *right* when it still does
        // not fit (T-049). Windows labels a desktop icon the same way; eliding
        // in the middle turned "Epic Games Launcher" into "Epi...her", which is
        // what the user reported. The lines are wrapped here rather than by
        // drawText so that the limit is two and the elision is on the right.
        const QPen textPen(selected ? kLabelTextSelected : kLabelText);
        painter.setPen(textPen);
        const QStringList captionLines = IconLayout::wrapToLines(
            item.name, kCaptionLines, cell.label.width(),
            [&metrics](const QString& candidate) { return metrics.horizontalAdvance(candidate); });
        int lineTop = cell.label.top();
        for (const QString& captionLine : captionLines) {
            const QRect line(cell.label.x(), lineTop, cell.label.width(), metrics.lineSpacing());
            if (m_textShadow) {
                painter.setPen(QColor(0, 0, 0, 0xA0));
                painter.drawText(line.translated(1, 1), Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                                 captionLine);
                painter.setPen(textPen);
            }
            painter.drawText(line, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine, captionLine);
            lineTop += metrics.lineSpacing();
        }
    }

    painter.restore();
}

int IconGrid::hitTest(const QPoint& contentLocalPoint, const QSize& contentSize) const
{
    const QVector<IconCell> cells =
        IconLayout::compute(contentSize, m_iconSize, cellWidth(), labelHeight(), m_gap, m_padding,
                            m_items.size(), m_flow);
    return IconLayout::hitTest(cells, contentLocalPoint);
}

QVector<QRect> IconGrid::cellRects(const QSize& contentSize) const
{
    QVector<QRect> rects;
    if (m_items.isEmpty() || contentSize.isEmpty())
        return rects;
    // The same call hitTest() and paint() make, with the same arguments: the cell
    // a tile is drawn in is the cell its region is built from.
    const QVector<IconCell> cells =
        IconLayout::compute(contentSize, m_iconSize, cellWidth(), labelHeight(), m_gap, m_padding,
                            m_items.size(), m_flow);
    const QRect area(QPoint(0, 0), contentSize);
    rects.reserve(cells.size());
    for (const IconCell& cell : cells) {
        // Rows past the bottom are computed but never drawn: a tile that is not
        // on screen has no pixels to stand for, so it is not part of the shape.
        const QRect tile = cell.icon.united(cell.label).intersected(area);
        if (!tile.isEmpty())
            rects.append(tile);
    }
    return rects;
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

    // Session 60: the user put it there -- the order is now his, and a restart
    // (setItems sorts by m_sortBy) must not put the alphabet back.
    m_sortBy = QStringLiteral("manual");
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

int IconGrid::insertItems(const QList<core::DesktopItem>& items, int index, bool sortByName)
{
    int at = index < 0 || index > m_items.size() ? m_items.size() : index;
    int added = 0;
    for (const core::DesktopItem& item : items) {
        if (item.path.isEmpty())
            continue;
        bool duplicate = false;
        for (const core::DesktopItem& existing : m_items) {
            if (existing.path.compare(item.path, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        m_items.insert(at++, item);
        ++added;
    }
    if (added == 0)
        return 0;
    if (sortByName) {
        m_sortBy = QStringLiteral("name");
        m_ascending = true;
        core::sortForDisplay(m_items, m_sortBy, m_ascending);
    } else {
        m_sortBy = QStringLiteral("manual");
    }
    m_selected.clear();
    m_anchor = 0;
    return added;
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

bool IconGrid::replaceItem(const QString& oldPath, const core::DesktopItem& item)
{
    int index = -1;
    int other = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).path.compare(oldPath, Qt::CaseInsensitive) == 0)
            index = i;
        else if (m_items.at(i).path.compare(item.path, Qt::CaseInsensitive) == 0)
            other = i;
    }
    if (index < 0 || item.path.isEmpty())
        return false;
    if (other >= 0) // already shown under its new name: one tile is enough
        return removeItemsByPath({oldPath}) > 0;
    QStringList selected;
    for (int i : std::as_const(m_selected))
        selected << m_items.at(i).path;
    if (selected.contains(oldPath, Qt::CaseInsensitive))
        selected << item.path;
    m_items[index] = item;
    if (m_sortBy != QLatin1String("manual")) {
        core::sortForDisplay(m_items, m_sortBy, m_ascending);
        m_selected.clear();
        for (int i = 0; i < m_items.size(); ++i) {
            if (selected.contains(m_items.at(i).path, Qt::CaseInsensitive))
                m_selected.insert(i);
        }
    }
    return true;
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
