#include "ui/fence_view.hpp"
#include "ui/fence_widgets.hpp"
#include "ui/modern_menu.hpp"
#include "ui/zone_shape.hpp"
#include "ui/widget_resize.hpp"

#include <QKeyEvent>
#include <QMetaMethod>
#include <QtMath>
#include <cmath>
#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QTransform>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QUrl>
#include <QToolTip>
#include <QMenu>
#include <QCursor>
#include <QDir>
#include <QWheelEvent>

#if defined(_WIN32)
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/shell_context_menu.hpp"
#include <windows.h>
#endif

namespace starfish::ui {

namespace {

// Pointer feedback for the fence geometry handles (T-007). Only visible in
// windowed mode: on the hosted desktop the real window is the DesktopSurface,
// which owns the cursor.
Qt::CursorShape cursorForHandle(FenceHandle handle)
{
    switch (handle) {
    case FenceHandle::TitleBar:
        return Qt::SizeAllCursor;
    case FenceHandle::Left:
    case FenceHandle::Right:
        return Qt::SizeHorCursor;
    case FenceHandle::Top:
    case FenceHandle::Bottom:
        return Qt::SizeVerCursor;
    case FenceHandle::TopLeft:
    case FenceHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case FenceHandle::TopRight:
    case FenceHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case FenceHandle::None:
    default:
        return Qt::ArrowCursor;
    }
}

} // namespace

FenceView::FenceView(QWidget* parent)
    : QWidget(parent)
{
    // Session 79: a widget's next change (a clock's minute), aligned on it.
    m_widgetTimer.setSingleShot(true);
    m_widgetTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_widgetTimer, &QTimer::timeout, this, [this] {
        update();
        emit widgetTick();
    });
    setObjectName(QStringLiteral("starfishFenceView"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    m_grid.setIconSize(48);
    m_grid.setGap(m_metrics.cellGap);
    m_grid.setPadding(m_metrics.gridPadding);
    m_grid.setLabelHeight(m_metrics.labelHeight);
    // The grid derives its cell width from the captions it holds (T-078), and it
    // must measure them with the font it paints them with -- so it is handed the
    // widget's font here rather than reading it off the painter later, which
    // hitTest (a mouse event, no painter in sight) could not do.
    m_grid.setBaseFont(font());

    // The fence accepts files dropped from the Explorer (T-006-3). This is the
    // path used when the fence is a real Qt window; the desktop-hosted surface
    // takes the OLE IDropTarget path in DesktopSurface instead.
    setAcceptDrops(true);
    // Session 66: the resize arrows follow the pointer with no button held --
    // without tracking, Qt drops those moves and the cursor only changed on a
    // fence that had been grabbed once.
    setMouseTracking(true);
}

void FenceView::setTitle(const QString& title)
{
    m_title = title;
    update();
}

QString FenceView::title() const
{
    return m_title;
}

void FenceView::setItems(QList<core::DesktopItem> items)
{
    m_grid.setItems(std::move(items));
    update();
    emit contentChanged();
}

void FenceView::setSort(const QString& by, bool ascending)
{
    m_grid.setSort(by, ascending);
    update();
    emit contentChanged();
}

void FenceView::setPanelColor(const QColor& color)
{
    // F-038 / T-057: the alpha is kept. It used to be dropped here, because the
    // opaque GDI blit of the hosted surface ignores it -- but that is the
    // surface's mode, not the widget's business, and while the alpha was thrown
    // away here the persisted one could never reach a single pixel. The widget
    // paints what the surface will push; a fence whose material is translucent
    // lets the wallpaper read through its panel. The layer below the panel is
    // filled by FenceContainer::renderFence at the input alpha floor, which is
    // what makes the corners *outside* the rounded panel transparent (T-075) --
    // and what keeps them reachable by the mouse (ADR-11).
    m_panelColor = QColor(color.red(), color.green(), color.blue(), color.alpha());
    update();
    emit contentChanged();
}

void FenceView::setShape(const QString& shape)
{
    if (m_shape == shape)
        return;
    m_shape = shape;
    update();
    emit contentChanged();
}

void FenceView::setCornerRadius(int px)
{
    // T-080: the widget keeps the value it was *asked* for; cornerRadius() bounds
    // it by the geometry, because a radius wider than half the fence describes no
    // shape at all. It used to be clamped to 64 here, which was a guess at the
    // same limit made without knowing the size.
    m_cornerRadius = qMax(0, px);
    update();
    emit contentChanged();
}

int FenceView::cornerRadius() const
{
    // T-080: the radius never exceeds half the shorter side (T-080's own warning:
    // past that, the rounded shape is empty and the window's region would erase the
    // panel). Recomputed on every ask rather than stored, so a fence that is
    // resized smaller -- by a gesture, or by coming back on a smaller screen --
    // cannot keep a rounding its own size no longer allows.
    return qBound(0, m_cornerRadius, qMin(width(), height()) / 2);
}

void FenceView::setTitleBarColor(const QColor& color)
{
    // T-080: an invalid colour is not an error, it is the *absence* of a band --
    // the fence as it looked before this existed, the title on the panel. Storing
    // the alpha as given keeps the same contract as the panel's colour.
    m_titleBarColor = color.isValid()
        ? QColor(color.red(), color.green(), color.blue(), color.alpha())
        : QColor();
    update();
    emit contentChanged();
}

void FenceView::setRolledUp(bool rolledUp)
{
    if (m_rolledUp == rolledUp)
        return;
    m_rolledUp = rolledUp;
    // No content area is left, so a cursor left over from a grip would be
    // misleading until the pointer moves again.
    setCursor(Qt::ArrowCursor);
    update();
}

void FenceView::setRolling(bool rolling)
{
    if (m_rolling == rolling)
        return;
    m_rolling = rolling;
    update();
}

QRect FenceView::contentRect() const
{
    // A rolled-up fence is nothing but its title bar: there is no content area
    // to hit-test or paint into.
    if (m_rolledUp)
        return QRect();
    const FenceMetrics& m = m_metrics;
    // Session 68: the band may sit on any side; the icons take the rest.
    const int band = titleBarHeight() + (titleShown() ? m.titleGap : 0);
    switch (titleSide()) {
    case TitleSide::Bottom:
        return rect().adjusted(m.inset, m.inset, -m.inset, -band);
    case TitleSide::Left:
        return rect().adjusted(band, m.inset, -m.inset, -m.inset);
    case TitleSide::Right:
        return rect().adjusted(m.inset, m.inset, -band, -m.inset);
    case TitleSide::Top:
        break;
    }
    return rect().adjusted(m.inset, band, -m.inset, -m.inset);
}

FenceView::TitleSide FenceView::titleSide() const
{
    // A rolled-up fence is its title bar: a horizontal strip reads as a top
    // bar; session 73: a side bar rolls into a vertical strip and keeps its side.
    if (m_rolledUp && (m_titleSide == TitleSide::Top || m_titleSide == TitleSide::Bottom))
        return TitleSide::Top;
    return m_titleSide;
}

void FenceView::setTitleSide(TitleSide side)
{
    if (m_titleSide == side)
        return;
    m_titleSide = side;
    update();
    emit contentChanged();
}

QRect FenceView::titleBandRect() const
{
    const int band = titleBarHeight();
    switch (titleSide()) {
    case TitleSide::Bottom:
        return QRect(0, height() - band, width(), band);
    case TitleSide::Left:
        return QRect(0, 0, band, height());
    case TitleSide::Right:
        return QRect(width() - band, 0, band, height());
    case TitleSide::Top:
        break;
    }
    return QRect(0, 0, width(), band);
}

QTransform FenceView::bandTransform() const
{
    // Band coordinates: x along the bar (its length), y across it (0..band).
    // Left reads bottom to top, right top to bottom -- both face the fence.
    QTransform t;
    switch (titleSide()) {
    case TitleSide::Bottom:
        t.translate(0, height() - titleBarHeight());
        break;
    case TitleSide::Left:
        t.translate(0, height());
        t.rotate(-90);
        break;
    case TitleSide::Right:
        t.translate(width(), 0);
        t.rotate(90);
        break;
    case TitleSide::Top:
        break;
    }
    return t;
}

int FenceView::bandLength() const
{
    const TitleSide side = titleSide();
    return side == TitleSide::Left || side == TitleSide::Right ? height() : width();
}

void FenceView::setTabTitle(bool tab)
{
    if (m_tabTitle == tab)
        return;
    m_tabTitle = tab;
    update();
    emit contentChanged();
}

QTransform FenceView::outlineTransform(const QSizeF& size) const
{
    // Outline coordinates: x along the bar, y from the bar's *outer* edge
    // inwards, over the whole fence (the bottom bar is mirrored, not turned).
    QTransform t;
    switch (titleSide()) {
    case TitleSide::Bottom:
        t = QTransform(1, 0, 0, -1, 0, size.height());
        break;
    case TitleSide::Left:
        t.translate(0, size.height());
        t.rotate(-90);
        break;
    case TitleSide::Right:
        t.translate(size.width(), 0);
        t.rotate(90);
        break;
    case TitleSide::Top:
        break;
    }
    return t;
}

QPair<int, int> FenceView::tabSpan(int length) const
{
    // Session 69 (T-115): the tab is as long as the title, plus its padding.
    QFont titleFont = font();
    titleFont.setBold(true);
    const QFontMetrics fm(titleFont);
    const QString text = m_title.isEmpty() ? tr("Fence") : m_title;
    const int pad = tabPadding();
    const int w = qMin(length, fm.horizontalAdvance(text) + 2 + 2 * pad);
    int x = 0;
    if (m_titleAlign & Qt::AlignHCenter)
        x = (length - w) / 2;
    else if (m_titleAlign & Qt::AlignRight)
        x = length - w;
    return {x, w};
}

int FenceView::tabPadding() const
{
    const int rt = qMin(cornerRadius(), titleBarHeight() / 2);
    return qMax(m_metrics.titleInset, rt + 4) + qMax(0, m_borderWidth);
}

QPainterPath FenceView::outlinePath(const QSizeF& size, qreal inset) const
{
    // Session 70: one outline, offset inwards by `inset` as a true parallel
    // curve -- every straight edge moves in by `inset`, a convex corner keeps
    // its centre with radius r - inset, a concave fillet keeps its centre with
    // radius f + inset. The border is stroked on outlinePath(size, width / 2),
    // so its outer edge *is* outlinePath(size, 0), which is the window's shape:
    // the same thickness everywhere, nothing cut by the region.
    const qreal r = qBound(0, m_cornerRadius, int(qMin(size.width(), size.height()) / 2));
    auto rounded = [](QPainterPath& p, const QRectF& rect, qreal radius) {
        if (rect.width() <= 0 || rect.height() <= 0)
            return;
        const qreal rr = qBound<qreal>(0, radius, qMin(rect.width(), rect.height()) / 2);
        if (rr > 0)
            p.addRoundedRect(rect, rr, rr);
        else
            p.addRect(rect);
    };
    QPainterPath path;
    if (!tabbed()) {
        rounded(path, QRectF(QPointF(0, 0), size).adjusted(inset, inset, -inset, -inset), r - inset);
        return path;
    }
    const TitleSide side = titleSide();
    const bool across = side == TitleSide::Left || side == TitleSide::Right;
    const qreal L = across ? size.height() : size.width();
    const qreal T = across ? size.width() : size.height();
    const qreal b = qMin<qreal>(titleBarHeight(), T);
    const QPair<int, int> span = tabSpan(int(L));
    const qreal x0 = span.first;
    const qreal x1 = span.first + span.second;
    // The tab's corners and its junctions share the tab's height: the fence's
    // radius, up to half of it each (a convex corner over a concave one).
    const qreal rt = qMin<qreal>(r, b / 2);
    const qreal bodyH = T - b;
    if (bodyH <= 1) {
        // Rolled up: the tab and nothing else.
        rounded(path, QRectF(x0 + inset, inset, x1 - x0 - 2 * inset, T - 2 * inset), rt - inset);
    } else {
        const qreal rb = qMin<qreal>(r, qMin(L, bodyH) / 2);
        rounded(path, QRectF(inset, b + inset, L - 2 * inset, bodyH - 2 * inset), rb - inset);
        QPainterPath tab;
        // Down past the body's corners, so its own lower corners are inside.
        const qreal tabBottom = qMin(b + rb + rt + 1, T - inset);
        rounded(tab, QRectF(x0 + inset, inset, x1 - x0 - 2 * inset, tabBottom - inset), rt - inset);
        path = path.united(tab);
        const qreal f = rt;
        if (f >= 1) {
            auto fillet = [&](const QRectF& square, const QPointF& centre) {
                QPainterPath sq;
                sq.addRect(square);
                QPainterPath circle;
                circle.addEllipse(centre, f + inset, f + inset);
                path = path.united(sq.subtracted(circle));
            };
            if (x0 - f >= rb)
                fillet(QRectF(x0 - f, b - f, f + inset, f + inset), QPointF(x0 - f, b - f));
            if (x1 + f <= L - rb)
                fillet(QRectF(x1 - inset, b - f, f + inset, f + inset), QPointF(x1 + f, b - f));
        }
    }
    return outlineTransform(size).map(path.simplified());
}

FenceHandle FenceView::handleAt(const QPoint& point) const
{
    if (zone_shape::isButton(m_shape) && m_locked)
        return FenceHandle::None;
    const FenceHandle handle = rawHandleAt(point);
    if (!m_locked || handle == FenceHandle::None || handle == FenceHandle::TitleBar)
        return handle;
    // Session 94: a grip on the band of a locked fence is the band (roll-up
    // gestures); any other grip does not exist.
    return rect().contains(point) && titleBandRect().contains(point) ? FenceHandle::TitleBar
                                                                     : FenceHandle::None;
}

void FenceView::setLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    if (locked) {
        endWidgetResize(); // session 98
        endButtonMoveMode(); // session 107
    }
    setCursor(Qt::ArrowCursor);
    if (locked) {
        m_dragOriginIndex = -1;
        m_dragging = false;
        m_externalDropIndex = -1;
    }
    update();
    emit contentChanged();
}

FenceHandle FenceView::rawHandleAt(const QPoint& point) const
{
    if (zone_shape::isButton(m_shape)) {
        // Session 107: by default a Bouton is not movable -- the disc is
        // HTCLIENT, the cursor stays an arrow, and icons receive the click.
        // Only compact move mode turns the disc into a grab.
        if (m_locked || !m_buttonMoveMode || !m_rolledUp)
            return FenceHandle::None;
        const QRect box = rect();
        const QPoint c = box.center();
        const int r = qMin(box.width(), box.height()) / 2;
        const int dx = point.x() - c.x(), dy = point.y() - c.y();
        return (dx * dx + dy * dy <= r * r) ? FenceHandle::TitleBar : FenceHandle::None;
    }

    if (tabbed()) {
        // Session 69: the tab is the bar; the body has its grips; the rest of
        // the strip along the bar is not the fence's (the window region leaves
        // it to the desktop).
        const QPointF op = outlineTransform(QSizeF(size())).inverted().map(QPointF(point));
        const int b = titleBarHeight();
        const QPair<int, int> span = tabSpan(bandLength());
        const bool inTab = op.x() >= span.first && op.x() <= span.first + span.second;
        if (op.y() < b + (inTab ? kResizeBorder : 0)) {
            if (!inTab || !rect().contains(point))
                return FenceHandle::None;
            if (op.y() >= kResizeBorder)
                return FenceHandle::TitleBar;
            switch (titleSide()) {
            case TitleSide::Bottom: return FenceHandle::Bottom;
            case TitleSide::Left: return FenceHandle::Left;
            case TitleSide::Right: return FenceHandle::Right;
            case TitleSide::Top: break;
            }
            return FenceHandle::Top;
        }
        QRect body = rect();
        switch (titleSide()) {
        case TitleSide::Bottom: body.adjust(0, 0, 0, -b); break;
        case TitleSide::Left: body.adjust(b, 0, 0, 0); break;
        case TitleSide::Right: body.adjust(0, 0, -b, 0); break;
        case TitleSide::Top: body.adjust(0, b, 0, 0); break;
        }
        return FenceGeometry::hitTest(body, point, kResizeBorder, 0, cornerRadius());
    }
    // The resize grips first (any side), then the band wherever it lies.
    const FenceHandle handle = FenceGeometry::hitTest(rect(), point, kResizeBorder, 0, cornerRadius());
    if (handle == FenceHandle::None && rect().contains(point) && titleBandRect().contains(point))
        return FenceHandle::TitleBar;
    return handle;
}

void FenceView::setMetrics(const FenceMetrics& metrics)
{
    m_metrics = metrics;
    m_grid.setPadding(metrics.gridPadding);
    m_grid.setGap(metrics.cellGap);
    m_grid.setLabelHeight(metrics.labelHeight);
    update();
}

QRect FenceView::titleTextRect() const
{
    QFont titleFont = font();
    titleFont.setBold(true);
    const QFontMetrics fm(titleFont);
    const QString text = m_title.isEmpty() ? tr("Fence") : m_title;
    const int h = fm.height();
    if (tabbed()) {
        const QPair<int, int> span = tabSpan(bandLength());
        const int pad = tabPadding();
        const int w = qMax(0, qMin(span.second - 2 * pad, fm.horizontalAdvance(text) + 2));
        return bandTransform().mapRect(QRect(span.first + pad, (m_metrics.titleBarHeight - h) / 2, w, h));
    }
    const int inset = titleInset();
    const int available = qMax(0, bandLength() - 2 * inset);
    const int w = qMin(available, fm.horizontalAdvance(text) + 2);
    int x = inset;
    if (m_titleAlign & Qt::AlignHCenter)
        x = inset + (available - w) / 2;
    else if (m_titleAlign & Qt::AlignRight)
        x = inset + available - w;
    // In band coordinates, then mapped onto the widget (session 68).
    return bandTransform().mapRect(QRect(x, (m_metrics.titleBarHeight - h) / 2, w, h));
}

int FenceView::titleInset() const
{
    // Session 67 (the user: "décaler le titre du rayon de courbure"): the
    // title starts past the whole rounded corner and the border -- the s66
    // curve-at-the-capitals offset was not enough to the eye.
    const int border = qMax(0, m_borderWidth);
    const int r = cornerRadius();
    return qMax(m_metrics.titleInset, r > 0 ? r + border : m_metrics.titleInset + border);
}

void FenceView::setTitleAlignment(Qt::Alignment align)
{
    if (m_titleAlign == align)
        return;
    m_titleAlign = align;
    update();
    emit contentChanged();
}

void FenceView::setIconOpacity(qreal opacity)
{
    m_iconOpacity = qBound<qreal>(0.0, opacity, 1.0);
}

int FenceView::insertItems(const QList<core::DesktopItem>& items, int index)
{
    const int added = m_grid.insertItems(items, index, m_sortOnAdd);
    if (added > 0) {
        update();
        emit contentChanged();
    }
    return added;
}

int FenceView::itemAt(const QPoint& localPoint) const
{
    if (zone_shape::isButton(m_shape)) {
        if (m_rolledUp)
            return -1;
        const QVector<QRect> rects = buttonItemRects();
        for (int i = rects.size() - 1; i >= 0; --i)
            if (rects.at(i).contains(localPoint))
                return i;
        return -1;
    }
    if (!contentRect().contains(localPoint))
        return -1;
    return m_grid.hitTest(localPoint - contentRect().topLeft(), contentRect().size());
}

int FenceView::insertionIndexAt(const QPoint& localPoint, bool* afterCell) const
{
    const QRect content = contentRect();
    if (content.isEmpty())
        return -1;
    const QVector<ui::IconCell> cells = m_grid.cellsPlusOne(content.size());
    return IconLayout::insertionIndex(cells, localPoint - content.topLeft(), m_grid.flow(), afterCell);
}

QLine FenceView::insertionBar(int gap, bool afterCell) const
{
    const QRect content = contentRect();
    if (gap < 0 || content.isEmpty())
        return {};
    const QVector<ui::IconCell> cells = m_grid.cellsPlusOne(content.size());
    const QLine bar = IconLayout::insertionBar(cells, gap, m_grid.flow(), m_grid.gap(), afterCell);
    return bar.isNull() ? bar : bar.translated(content.topLeft());
}

QLine FenceView::shownInsertionBar() const
{
    if (m_dragging && !m_releaseArmed && m_dropTargetIndex >= 0)
        return insertionBar(m_dropTargetIndex, m_dropAfterCell);
    if (m_externalDropIndex >= 0 && !m_sortOnAdd)
        return insertionBar(m_externalDropIndex, m_externalAfterCell);
    return {};
}

QVector<QRect> FenceView::tileRects() const
{
    if (zone_shape::isButton(m_shape))
        return buttonItemRects();
    // The grid lays its cells out from the content origin (hitTest subtracts it
    // above), so the tile rectangles are the cells moved there.
    const QRect content = contentRect();
    QVector<QRect> rects = m_grid.cellRects(content.size());
    for (QRect& rect : rects)
        rect.translate(content.topLeft());
    return rects;
}

void FenceView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QColor titleText(0xE5, 0xE7, 0xEB);
    const QColor hintText(0x9C, 0xA3, 0xAF);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (zone_shape::isButton(m_shape)) {
        // Session 101: no classic panel / title bar -- the disc is the fence.
        paintButton(painter);
        return;
    }

    // T-094: the border's own width and colour (0: none); session 65 hover:
    // a touch lighter, the border brighter.
    QColor border = m_borderColor;
    QColor fill = m_panelColor;
    QColor bandFill = m_titleBarColor;
    int borderWidth = m_borderWidth;
    // Session 69: the hover effect, the fence's own or the default.
    const HoverEffect hover = m_hovered && m_hoverHighlight ? m_hoverEffect : HoverEffect::None;
    auto lighter = [](const QColor& c) {
        return QColor::fromHsv(c.hsvHue(), c.hsvSaturation(), qMin(255, c.value() + 14), c.alpha());
    };
    auto opaquer = [](QColor c) {
        c.setAlpha(qMin(255, c.alpha() + 90));
        return c;
    };
    auto tinted = [this](const QColor& c) {
        const QColor a = m_borderColor;
        return QColor((c.red() * 2 + a.red()) / 3, (c.green() * 2 + a.green()) / 3, (c.blue() * 2 + a.blue()) / 3,
                      c.alpha());
    };
    switch (hover) {
    case HoverEffect::Lighter:
        border = border.lighter(140);
        fill = lighter(fill);
        if (bandFill.isValid())
            bandFill = lighter(bandFill);
        break;
    case HoverEffect::Opacity:
        fill = opaquer(fill);
        if (bandFill.isValid())
            bandFill = opaquer(bandFill);
        break;
    case HoverEffect::Colour:
        fill = tinted(fill);
        if (bandFill.isValid())
            bandFill = tinted(bandFill);
        break;
    case HoverEffect::NoBorder:
        borderWidth = 0;
        break;
    case HoverEffect::Glow:
        border = border.lighter(150);
        break;
    case HoverEffect::None:
        break;
    }
    const qreal half = qMax(0.5, m_borderWidth / 2.0);
    // Session 69: the outline -- a rounded panel, or a body with its tab.
    const QPainterPath panel = outlinePath(QSizeF(size()), half);
    // T-080, session 66: the title bar's own colour is *not* laid over the
    // panel (two translucent layers made it near opaque): the panel fills
    // below the band, the band fills the band, the border goes over both.
    if (m_paintLayers != PaintLayers::Foreground) {
    if (m_titleBarColor.isValid()) {
        const QRect bandRect = titleBandRect();
        painter.save();
        painter.setClipRegion(QRegion(rect()).subtracted(QRegion(bandRect)));
        painter.fillPath(panel, fill);
        painter.setClipRect(bandRect);
        painter.fillPath(panel, bandFill);
        painter.restore();
    } else {
        painter.fillPath(panel, fill);
    }
    if (hover == HoverEffect::Glow) {
        // An inner glow along the outline (the window region cuts anything
        // outside it): wide soft strokes, clipped to the fence.
        painter.save();
        painter.setClipPath(panel);
        painter.setBrush(Qt::NoBrush);
        for (int i = 3; i >= 1; --i) {
            QColor glow = border;
            glow.setAlpha(40);
            painter.setPen(QPen(glow, i * 6));
            painter.drawPath(panel);
        }
        painter.restore();
    }
    if (borderWidth > 0) {
        painter.setPen(QPen(border, borderWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(panel);
    }
    } // background
    if (m_paintLayers == PaintLayers::Background)
        return;

    if (titleShown()) {
        QFont titleFont = font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        const TitleSide side = titleSide();
        {
            const int inset = titleInset();
            painter.save();
            painter.setTransform(bandTransform(), true);
            QRect titleRect(inset, 0, qMax(0, bandLength() - 2 * inset), m_metrics.titleBarHeight);
            if (tabbed()) {
                const QPair<int, int> span = tabSpan(bandLength());
                const int pad = tabPadding();
                titleRect = QRect(span.first + pad, 0, qMax(0, span.second - 2 * pad), m_metrics.titleBarHeight);
            }
            const QString titleText_ = painter.fontMetrics().elidedText(
                m_title.isEmpty() ? tr("Fence") : m_title, Qt::ElideRight, titleRect.width());
            const Qt::Alignment align =
                Qt::AlignVCenter | (tabbed() ? Qt::AlignHCenter : (m_titleAlign ? m_titleAlign : Qt::AlignLeft));
            if (m_grid.textShadow()) {
                painter.setPen(QColor(0, 0, 0, 0xA0));
                painter.drawText(titleRect.translated(1, 1), align, titleText_);
            }
            painter.setPen(titleText);
            painter.drawText(titleRect, align, titleText_);
            painter.restore();
        }
    } else if (m_hovered && !m_locked) {
        // T-076: no title bar -- a small grip says where the fence is moved from.
        // Session 95: none on a locked fence, which cannot be moved.
        painter.save();
        painter.setTransform(bandTransform(), true);
        painter.setPen(QPen(QColor(255, 255, 255, 0x70), 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(bandLength() / 2.0 - 14, kHiddenTitleBand / 2.0 + 1),
                         QPointF(bandLength() / 2.0 + 14, kHiddenTitleBand / 2.0 + 1));
        painter.restore();
    }

    const QRect content = contentRect();
    // Rolled up (F-020): the widget is the title bar, nothing else is drawn.
    if (m_rolledUp || content.isEmpty())
        return;

    if (m_grid.items().isEmpty()) {
        painter.setFont(font());
        painter.setPen(hintText);
        // F-036 / T-014b: a string drawn *in* a fence is part of the interface, so
        // it goes through tr() like a menu label. It was a QStringLiteral -- the
        // one kind of user-visible text a translation catalogue cannot see.
        painter.drawText(content, Qt::AlignCenter, tr("empty fence -- no icons yet"));
    }
    // Session 100: an empty fence went on to show nothing of a drop either --
    // the bar below was never reached. It is now.
    if (!m_grid.items().isEmpty()) {

        // Session 66: "the material on the icons too" fades the icons, and only them.
        painter.setOpacity(m_iconOpacity);
        m_grid.paint(painter, content, m_iconProvider);
        painter.setOpacity(1.0);
        scheduleWidgetTick(); // session 79: the next minute (second) of a clock
        if (const int hidden = hiddenItemCount(); hidden > 0) {
            // Session 76: the fence is too small for all its icons -- say how many
            // are out of sight; a click on the badge lists them.
            const QRect badge = hiddenBadgeRect();
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(0x3B, 0x82, 0xF6), 1.0));
            painter.setBrush(QColor(0x11, 0x18, 0x27, 0xE0));
            painter.drawRoundedRect(QRectF(badge).adjusted(0.5, 0.5, -0.5, -0.5), badge.height() / 2.0, badge.height() / 2.0);
            QFont bold = font();
            bold.setBold(true);
            bold.setPixelSize(11);
            painter.setFont(bold);
            painter.setPen(QColor(0xFF, 0xFF, 0xFF));
            painter.drawText(badge, Qt::AlignCenter, QStringLiteral("+%1").arg(hidden));
            painter.restore();
        }
    }
    if (m_dragging) {
        // Either the grid is about to receive the tile, or the desktop is:
        // never both, so the two hints are mutually exclusive by construction.
        if (m_releaseArmed) {
            // Session 100 (t122u): a fence's widgets never go to the desktop --
            // no "let go here" for them.
            const bool widgetsOnly = std::all_of(m_dragPaths.cbegin(), m_dragPaths.cend(), core::isWidgetPath);
            if (!m_dragOverOtherFence && !widgetsOnly)
                paintReleaseHint(painter);
        } else {
            paintInsertionBar(m_dropTargetIndex, m_dropAfterCell, painter);
        }
    }
    // Session 65: tiles from another fence would land here -- unless the fence
    // sorts what it receives (the bar would lie then).
    if (m_externalDropIndex >= 0 && !m_sortOnAdd)
        paintInsertionBar(m_externalDropIndex, m_externalAfterCell, painter);
    // Session 98: the resize mode's outline -- the widget's exact bounds.
    if (!m_resizePath.isEmpty()) {
        const QRect r = widgetResizeRect();
        if (!r.isEmpty())
            widget_resize::paintOutline(painter, r, accentColor(), 1.0);
    }
}

int FenceView::widgetResizeIndex() const
{
    if (m_resizePath.isEmpty())
        return -1;
    const QList<core::DesktopItem>& items = m_grid.items();
    for (int i = 0; i < items.size(); ++i)
        if (items.at(i).path == m_resizePath)
            return i;
    return -1;
}

QSize FenceView::widgetResizePitch() const
{
    // From one cell to the next: the widget's own cells plus the gaps between.
    const int i = widgetResizeIndex();
    const QVector<QRect> rects = tileRects();
    if (i < 0 || i >= rects.size())
        return {};
    const QSize span = m_grid.items().at(i).span.isValid() ? m_grid.items().at(i).span : QSize(1, 1);
    const int gap = m_grid.gap();
    return QSize((rects.at(i).width() + gap) / qMax(1, span.width()),
                 (rects.at(i).height() + gap) / qMax(1, span.height()));
}

QRect FenceView::widgetResizeRect() const
{
    const int i = widgetResizeIndex();
    const QVector<QRect> rects = tileRects();
    if (i < 0 || i >= rects.size())
        return {};
    if (!m_resizeDragging)
        return rects.at(i);
    const QSize pitch = widgetResizePitch();
    const int gap = m_grid.gap();
    return QRect(rects.at(i).topLeft(), QSize(m_resizePreview.width() * pitch.width() - gap,
                                              m_resizePreview.height() * pitch.height() - gap));
}

bool FenceView::beginWidgetResize(const QString& path)
{
    if (m_locked || !core::isWidgetPath(path))
        return false;
    const QString before = m_resizePath;
    m_resizePath = path;
    if (widgetResizeIndex() < 0 || widget_resize::isLocked(m_grid.items().at(widgetResizeIndex()))) {
        m_resizePath = before; // session 100: a locked widget has no resize mode
        return false;
    }
    if (!m_resizeIdle.isSingleShot()) {
        m_resizeIdle.setSingleShot(true);
        connect(&m_resizeIdle, &QTimer::timeout, this, &FenceView::endWidgetResize);
    }
    m_resizeIdle.start(widget_resize::kIdleMs);
    m_resizeDragging = false;
    update();
    emit contentChanged();
    emit widgetResizeModeChanged(true);
    return true;
}

void FenceView::endWidgetResize()
{
    if (m_resizePath.isEmpty())
        return;
    m_resizeIdle.stop();
    m_resizePath.clear();
    m_resizeDragging = false;
    setCursor(Qt::ArrowCursor);
    update();
    emit contentChanged();
    emit widgetResizeModeChanged(false);
}

bool FenceView::beginButtonMoveMode()
{
    if (m_locked || !zone_shape::isButton(m_shape))
        return false;
    if (m_buttonMoveMode)
        return true;
    if (!m_buttonMoveIdle.isSingleShot()) {
        m_buttonMoveIdle.setSingleShot(true);
        connect(&m_buttonMoveIdle, &QTimer::timeout, this, &FenceView::endButtonMoveMode);
    }
    m_buttonMoveMode = true;
    setFocusPolicy(Qt::StrongFocus);
    setFocus(Qt::OtherFocusReason);
    setCursor(Qt::SizeAllCursor);
    m_buttonMoveIdle.start(widget_resize::kIdleMs);
    update();
    emit contentChanged();
    emit buttonMoveModeChanged(true);
    return true;
}

void FenceView::endButtonMoveMode()
{
    if (!m_buttonMoveMode)
        return;
    const bool wasDragging = m_geometryDragging;
    m_buttonMoveIdle.stop();
    m_buttonMoveMode = false;
    m_geometryDragging = false;
    m_geometryHandle = FenceHandle::None;
    setCursor(Qt::ArrowCursor);
    update();
    emit contentChanged();
    if (wasDragging)
        emit geometryDragCancelled();
    emit buttonMoveModeChanged(false);
}

void FenceView::mousePressEvent(QMouseEvent* event)
{
    emit pressed();
    if (!m_resizePath.isEmpty()) { // session 98: an edge, or the mode ends
        const int edges = event->button() == Qt::LeftButton
                              ? widget_resize::edgesAt(widgetResizeRect(), event->pos(), 6)
                              : 0;
        if (edges) {
            const int i = widgetResizeIndex();
            m_resizeEdges = edges;
            m_resizeDragging = true;
            m_resizeStart = event->pos();
            m_resizeStartSpan = m_grid.items().at(i).span.isValid() ? m_grid.items().at(i).span : QSize(1, 1);
            m_resizePreview = m_resizeStartSpan;
            m_resizeIdle.start(widget_resize::kIdleMs);
            event->accept();
            return;
        }
        endWidgetResize();
    }
    if (m_buttonMoveMode && event->button() == Qt::LeftButton) {
        // Session 107: a click on the disc starts the drag (handle below);
        // a click elsewhere ends the mode, like the widget resize mode.
        if (handleAt(event->pos()) == FenceHandle::None) {
            endButtonMoveMode();
            event->accept();
            return;
        }
        m_buttonMoveIdle.start(widget_resize::kIdleMs);
    }
    if (event->button() == Qt::LeftButton && hiddenItemCount() > 0
        && hiddenBadgeRect().contains(event->pos())) {
        showHiddenItemsMenu(); // session 76
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // Right-click selects the item under the cursor and shows the Windows
        // shell context menu for it. The single path (mousePressEvent) covers
        // both a real Qt window and a desktop-hosted surface that posts
        // QMouseEvent(RightButton) from its own WndProc: a QContextMenuEvent
        // would only fire from the platform, not from a posted event.
        const int index = itemAt(event->pos());
        // T-076: when no tile is under the cursor -- the title bar, the empty
        // part of the panel, or the bar of a rolled-up fence -- the menu is
        // **ours** (FenceMenu). The selection is left alone in that case: a
        // right-click on empty space is not a statement about the icons, and the
        // tile path below is unchanged, so an icon keeps the Shell's own menu.
        if (index >= 0) {
            // As on the desktop: the clicked icon joins the selection it acts
            // on, or becomes it.
            if (!m_grid.isSelected(index))
                m_grid.selectOnly(index);
            update();
            emit contentChanged();
            emit selectionGesture();
            if (isSignalConnected(QMetaMethod::fromSignal(&FenceView::itemMenuRequested)))
                emit itemMenuRequested(selectedPaths(), event->pos());
            else
                showContextMenuForIndex(index, event->globalPosition().toPoint());
            return;
        }
        emit fenceMenuRequested(event->globalPosition().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int index = itemAt(event->pos());
    const Qt::KeyboardModifiers mods = event->modifiers();

    // T-007: the title bar moves the fence, a border grip resizes it. Checked
    // before the icon grid so a fence gesture never disturbs the icon selection
    // -- which is also what keeps the two drags mutually exclusive.
    const FenceHandle handle =
        handleAt(event->pos());
    if (handle != FenceHandle::None && m_locked) {
        event->accept(); // session 94: the band of a locked fence never moves it
        return;
    }
    if (handle != FenceHandle::None) {
        m_geometryHandle = handle;
        m_geometryStartScreen = event->globalPosition();
        m_geometryDragging = true;
        m_dragOriginIndex = -1;
        m_dragging = false;
        updateDragGhost(false);
        m_dropTargetIndex = -1;
        m_releaseArmed = false;
        emit geometryDragStarted();
        return;
    }

    if (index < 0) {
        if (!(mods & Qt::ControlModifier))
            m_grid.clearSelection();
    } else if (mods & Qt::ControlModifier) {
        m_grid.toggle(index);
    } else if (mods & Qt::ShiftModifier) {
        m_grid.selectRangeTo(index);
    } else if (!m_grid.isSelected(index)) {
        // Session 65: a press on an icon already selected keeps the selection,
        // so a multi-selection can be dragged; the release narrows it.
        m_grid.selectOnly(index);
    }
    emit selectionGesture();

    // Arm a possible drag (T-006-2). The move enters drag mode only once the
    // cursor leaves the system threshold -- a click in place never reorders.
    m_dragOriginIndex = (!m_locked && index >= 0 && index < m_grid.items().size()) ? index : -1;
    m_dragStartPos = event->pos();
    m_dropTargetIndex = -1;
    m_dragging = false;
    updateDragGhost(false);
    m_releaseArmed = false;

    update();
    emit contentChanged();
}

void FenceView::mouseMoveEvent(QMouseEvent* event)
{
    // Fence gesture (T-007) takes priority: while the title bar or a border
    // grip is held, the pointer moves the fence, never an icon.
    if (m_resizeDragging) { // session 98: the outline follows, in whole cells
        const QSize preview = widget_resize::spanAfter(m_resizeStartSpan, widgetResizePitch(),
                                                       event->pos() - m_resizeStart, m_resizeEdges);
        m_resizeIdle.start(widget_resize::kIdleMs);
        if (preview != m_resizePreview) {
            m_resizePreview = preview;
            update();
            emit contentChanged();
        }
        return;
    }
    if (!m_resizePath.isEmpty() && !(event->buttons() & Qt::LeftButton)) {
        const int edges = widget_resize::edgesAt(widgetResizeRect(), event->pos(), 6);
        if (edges) {
            setCursor(edges == widget_resize::Corner ? Qt::SizeFDiagCursor
                      : edges == widget_resize::Right ? Qt::SizeHorCursor
                                                      : Qt::SizeVerCursor);
            m_resizeIdle.start(widget_resize::kIdleMs); // hovering a grip is a touch
            return;
        }
    }
    if (m_buttonMoveMode && !(event->buttons() & Qt::LeftButton))
        setCursor(Qt::SizeAllCursor);
    if (m_geometryDragging) {
        if (m_buttonMoveMode)
            m_buttonMoveIdle.start(widget_resize::kIdleMs);
        // Measured against the *screen*, not against this widget: the widget
        // moves with the fence, so a widget-local delta (`event->pos()`) shrinks
        // as the fence catches up with the cursor and the drag ends up covering
        // only about half the distance the cursor actually travelled, in lagging
        // steps. Screen coordinates are the one frame that does not move.
        emit geometryDragDelta(m_geometryHandle,
                               (event->globalPosition() - m_geometryStartScreen).toPoint());
        return;
    }

    if (m_dragOriginIndex < 0 || !(event->buttons() & Qt::LeftButton)) {
        // Idle: reflect the handle under the pointer so the user can see where
        // the fence can be grabbed.
        if (!(event->buttons() & Qt::LeftButton))
            setCursor(m_locked ? Qt::ArrowCursor : cursorForHandle(handleAt(event->pos())));
        if (!(event->buttons() & Qt::LeftButton))
            updateMissingTip(event->position().toPoint());
        return;
    }

    if (!m_dragging) {
        // QApplication::startDragDistance() is the platform-consistent
        // threshold (Qt::Drag is too sensitive for an icon grid where the
        // cells are small and the cursor jitters on them).
        if (QApplication::startDragDistance() >=
            (event->pos() - m_dragStartPos).manhattanLength())
            return;
        // Session 65: the selection is what moves (the dragged icon always
        // belongs to it -- see the press).
        // Session 100: a locked widget stays where it is; the rest of a
        // selection still moves.
        const core::DesktopItem& origin = m_grid.items().at(m_dragOriginIndex);
        if (origin.isWidget() && widget_resize::isLocked(origin))
            return;
        if (!m_grid.isSelected(m_dragOriginIndex))
            m_grid.selectOnly(m_dragOriginIndex);
        QStringList paths = selectedPaths();
        for (const core::DesktopItem& item : m_grid.items())
            if (item.isWidget() && widget_resize::isLocked(item))
                paths.removeAll(item.path);
        m_dragPaths = paths;
        m_dragging = true;
    }
    updateDragGhost(true);

    // Session 80: the gap it would go to, shown as a bar -- none when letting
    // go would leave a single icon where it is.
    bool after = false;
    int gap = insertionIndexAt(event->pos(), &after);
    if (m_dragPaths.size() <= 1 && (gap == m_dragOriginIndex || gap == m_dragOriginIndex + 1))
        gap = -1;
    if (gap != m_dropTargetIndex || after != m_dropAfterCell) {
        m_dropTargetIndex = gap;
        m_dropAfterCell = after;
        update();
        emit contentChanged(); // the hosted surface is an image: re-render it
    }
    // F-011: outside the fence there is no cell to drop onto, and the release
    // hands the tile back to the desktop instead of reordering the grid. The
    // hint has to follow the cursor both ways -- dragging back inside re-arms
    // the ordinary drop.
    const bool outside = !rect().contains(event->pos());
    if (outside != m_releaseArmed) {
        m_releaseArmed = outside;
        update();
    }
    emit itemsDraggedOutside(m_dragPaths, event->pos(), false);
}

void FenceView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (m_resizeDragging) { // session 98
        m_resizeDragging = false;
        m_resizeIdle.start(widget_resize::kIdleMs);
        const QString path = m_resizePath;
        const QSize span = m_resizePreview;
        update();
        emit contentChanged();
        if (span != m_resizeStartSpan)
            emit widgetResized(path, span);
        return;
    }

    if (m_geometryDragging) {
        // The container already applied every delta as it arrived; the release
        // only closes the gesture (and lets it settle, e.g. commit the final
        // snapped rect to the layout in T-008).
        m_geometryDragging = false;
        m_geometryHandle = FenceHandle::None;
        emit geometryDragFinished();
        if (m_buttonMoveMode) // session 107: a drop ends the move mode
            endButtonMoveMode();
        return;
    }
    // Session 107: a left click on the compact disc still expands it (hover
    // also does). Not in move mode -- that click is a grab, not an unroll.
    if (zone_shape::isButton(m_shape) && m_rolledUp && !m_buttonMoveMode && !m_locked
        && zone_shape::buttonContainsPoint(rect(), event->pos())) {
        emit rollUpRequested(false);
        return;
    }

    if (m_dragging) {
        const int from = m_dragOriginIndex;
        // F-011: letting go outside the fence gives the tile back to the
        // desktop. Decided *before* the reorder, because it is a different
        // gesture with a different owner: the fence stops holding the icon and
        // the desktop layer draws it again, with no restore step in between.
        if (!rect().contains(event->pos()) && from >= 0 && from < m_grid.items().size()
            && !m_grid.items().at(from).path.isEmpty()) {
            const QStringList paths = m_dragPaths.isEmpty() ? QStringList{m_grid.items().at(from).path} : m_dragPaths;
            m_dragging = false;
            updateDragGhost(false);
            m_dragOriginIndex = -1;
            m_dragStartPos = QPoint();
            m_dropTargetIndex = -1;
            m_releaseArmed = false;
            m_dragPaths.clear();
            m_dragOverOtherFence = false;
            update();
            // Session 65: the container decides -- another fence under the
            // cursor takes them, otherwise the desktop does (F-011).
            if (isSignalConnected(QMetaMethod::fromSignal(&FenceView::itemsDraggedOutside)))
                emit itemsDraggedOutside(paths, event->pos(), true);
            else
                emit itemReleaseRequested(paths);
            return;
        }
        if (m_dragPaths.size() > 1) {
            // Session 65: a multi-selection moves as a block, in its order, to
            // where the cursor is. Session 80: to the gap the bar shows -- the
            // icons that stay and sit before it say where the block goes.
            const int gap = insertionIndexAt(event->pos());
            QList<core::DesktopItem> moving;
            int at = 0;
            for (int i = 0; i < m_grid.items().size(); ++i) {
                const core::DesktopItem& item = m_grid.items().at(i);
                if (m_dragPaths.contains(item.path))
                    moving.append(item);
                else if (gap >= 0 && i < gap)
                    ++at;
            }
            if (gap < 0)
                at = -1; // nowhere to measure from: the end
            if (!moving.isEmpty()) {
                m_grid.removeItemsByPath(m_dragPaths);
                m_grid.insertItems(moving, at, false);
                m_grid.setManualOrder();
            }
            m_dragging = false;
            updateDragGhost(false);
            m_dragOriginIndex = -1;
            m_dragStartPos = QPoint();
            m_dropTargetIndex = -1;
            m_releaseArmed = false;
            m_dragPaths.clear();
            update();
            emit contentChanged();
            return;
        }
        // Session 80: the icon goes into the gap the bar showed (the nearest
        // one, in the flow order); past the last icon means the end. In the
        // final list the gaps after `from` are one lower.
        const int gap = insertionIndexAt(event->pos());
        int to = gap < 0 ? m_grid.items().size() - 1 : (gap > from ? gap - 1 : gap);
        to = qBound(0, to, qMax(0, m_grid.items().size() - 1));
        if (to >= 0 && from >= 0 && from != to)
            m_grid.moveItem(from, to);
        m_dragPaths.clear();
        m_dragging = false;
        updateDragGhost(false);
        m_dragOriginIndex = -1;
        m_dragStartPos = QPoint();
        m_dropTargetIndex = -1;
        m_releaseArmed = false;
        update();
        emit contentChanged();
        return;
    }

    // A press that never crossed the drag threshold is just a click -- the
    // selection already happened in mousePressEvent; nothing else to do.
    m_dragOriginIndex = -1;
    m_dragStartPos = QPoint();
}

void FenceView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // F-020: a double-click on the title bar rolls the fence up or down. Checked
    // before the grid so a double-click on an icon still launches it.
    if (handleAt(event->pos())
        == FenceHandle::TitleBar) {
        // Session 76: not while the Appearance dialog edits this fence.
        if (m_titleLocked) {
            event->accept();
            return;
        }
        // Session 60: on the title's text, a double-click renames in place (as a
        // file's name in Explorer); anywhere else on the band it still rolls up.
        if (!m_locked && titleShown() && titleTextRect().adjusted(-4, -4, 4, 4).contains(event->pos())) {
            emit titleEditRequested();
            return;
        }
        emit rollUpRequested(!m_rolledUp);
        return;
    }

    const int index = itemAt(event->pos());
    if (index < 0 || index >= m_grid.items().size()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (m_grid.items().at(index).isWidget()) { // session 79: nothing to open
        // Session 81: a widget whose content is text (the notes) is written in
        // place, as the title is; the others have nothing to open.
        const core::DesktopItem& widget = m_grid.items().at(index);
        if (widgets::isEditable(widget.widgetType))
            emit widgetEditRequested(widget.path);
        event->accept();
        return;
    }
    // Session 76 (the user: a browser-installed web app opened the plain browser, the
    // "Apps" Settings shortcut opened the Explorer): the *item itself* goes to
    // the shell, as Explorer's double-click does -- a shortcut keeps its
    // arguments, its working folder, its app id, its URI. Resolving the target
    // here and opening that dropped all of it.
    const core::DesktopItem& item = m_grid.items().at(index);
    if (!starfish::platform::win::openShellItem(item.path)) {
        const QString target = IconGrid::launchPath(item);
        if (!target.isEmpty() && !target.startsWith(QLatin1String("::")))
            QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    }
    emit itemActivated(index);
}

void FenceView::wheelEvent(QWheelEvent* event)
{
    // F-020: the wheel over the title bar rolls the fence up (away from the
    // user) or down (towards them). Only the title bar reacts, so scrolling over
    // the icon grid is never hijacked.
    if (handleAt(event->position().toPoint())
        != FenceHandle::TitleBar) {
        QWidget::wheelEvent(event);
        return;
    }

    const int dy = event->angleDelta().y();
    if (dy == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    const bool wantRolledUp = dy > 0;
    if (wantRolledUp != m_rolledUp)
        emit rollUpRequested(wantRolledUp);
    event->accept();
}

void FenceView::addDroppedItems(const QList<core::DesktopItem>& items)
{
    // Session 60: a drop is an addition -- at the end, sorted only when the user
    // asked for it (the switch); appendItems() stays the portals' listing.
    insertItems(items, -1);
}

int FenceView::appendItems(const QList<core::DesktopItem>& items)
{
    const int added = m_grid.appendItems(items);
    if (added > 0) {
        update();
        emit contentChanged();
    }
    return added;
}

int FenceView::replaceItems(const QStringList& previousPaths, const QList<core::DesktopItem>& items)
{
    const int removed = m_grid.removeItemsByPath(previousPaths);
    const int added = m_grid.appendItems(items);
    if (removed > 0 || added > 0) {
        update();
        emit contentChanged();
    }
    return added;
}

void FenceView::scheduleWidgetTick()
{
    if (m_rolledUp || !m_grid.hasWidgets()) {
        m_widgetTimer.stop();
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    int next = -1;
    for (const core::DesktopItem& item : m_grid.items()) {
        if (!item.isWidget())
            continue;
        const int ms = widgets::msUntilNextChange(item, now);
        if (ms >= 0 && (next < 0 || ms < next))
            next = ms;
    }
    // Sooner than what is pending (seconds just turned on), never later.
    if (next >= 0 && (!m_widgetTimer.isActive() || m_widgetTimer.remainingTime() > next))
        m_widgetTimer.start(next);
    else if (next < 0)
        m_widgetTimer.stop();
}

int FenceView::widgetTickInterval() const
{
    return m_widgetTimer.isActive() ? m_widgetTimer.remainingTime() : -1;
}

void FenceView::setIconFlow(int flow)
{
    if (m_grid.flow() == flow)
        return;
    m_grid.setFlow(flow);
    update();
    emit contentChanged();
}

void FenceView::setLabelsShown(bool shown)
{
    if (m_grid.labelsShown() == shown)
        return;
    m_grid.setLabelsShown(shown);
    update();
    emit contentChanged();
}

QStringList FenceView::hiddenItemPaths() const
{
    QStringList paths;
    // Session 86: no "+N" on a rolled-up fence, nor while it rolls (the shrinking
    // panel hides icons only for the length of the animation).
    if (m_rolledUp || m_rolling)
        return paths;
    const QRect content = contentRect();
    if (content.isEmpty())
        return paths;
    const QVector<IconCell> cells = m_grid.cells(content.size());
    const QList<core::DesktopItem>& items = m_grid.items();
    for (int i = 0; i < items.size() && i < cells.size(); ++i) {
        // An icon cut by the edge counts as out of sight: it cannot be read.
        if (!QRect(QPoint(0, 0), content.size()).contains(cells.at(i).icon))
            paths << items.at(i).path;
    }
    return paths;
}

int FenceView::hiddenItemCount() const
{
    return hiddenItemPaths().size();
}

QRect FenceView::hiddenBadgeRect() const
{
    const QRect content = contentRect();
    const int h = 18;
    const int w = 16 + QFontMetrics(font()).horizontalAdvance(QStringLiteral("+%1").arg(qMax(1, hiddenItemCount())));
    return QRect(content.right() - w - 3, content.bottom() - h - 3, w, h);
}

void FenceView::showHiddenItemsMenu()
{
    const QStringList hidden = hiddenItemPaths();
    if (hidden.isEmpty())
        return;
    QMenu menu;
    for (const core::DesktopItem& item : m_grid.items()) {
        if (!hidden.contains(item.path))
            continue;
        const QPixmap pixmap = !item.missing && !item.isWidget() && m_iconProvider ? m_iconProvider(item, 32) : QPixmap();
        QAction* action = menu.addAction(QIcon(pixmap), item.name.isEmpty() ? item.fileName : item.name);
        if (item.isWidget()) { // session 79: listed, nothing to open
            action->setEnabled(false);
            continue;
        }
        const QString path = item.path;
        connect(action, &QAction::triggered, this, [path] { starfish::platform::win::openShellItem(path); });
    }
    menu.exec(QCursor::pos());
}

bool FenceView::replaceItem(const QString& oldPath, const core::DesktopItem& item)
{
    if (!m_grid.replaceItem(oldPath, item))
        return false;
    m_tipIndex = -1;
    update();
    emit contentChanged();
    return true;
}

QRect FenceView::labelRectOf(const QString& path) const
{
    const QList<core::DesktopItem>& items = m_grid.items();
    const QRect content = contentRect();
    const QVector<IconCell> cells = m_grid.cells(content.size());
    for (int i = 0; i < items.size() && i < cells.size(); ++i) {
        if (items.at(i).path.compare(path, Qt::CaseInsensitive) == 0)
            return cells.at(i).label.translated(content.topLeft()).intersected(content);
    }
    return {};
}

QRect FenceView::itemRectOf(const QString& path) const
{
    const QList<core::DesktopItem>& items = m_grid.items();
    const QRect content = contentRect();
    if (content.isEmpty())
        return {};
    const QVector<IconCell> cells = m_grid.cells(content.size());
    for (int i = 0; i < items.size() && i < cells.size(); ++i) {
        if (items.at(i).path.compare(path, Qt::CaseInsensitive) == 0)
            return cells.at(i).icon.united(cells.at(i).label).translated(content.topLeft()).intersected(content);
    }
    return {};
}

void FenceView::updateMissingTip(const QPoint& localPoint)
{
    const int index = itemAt(localPoint);
    if (index == m_tipIndex)
        return;
    m_tipIndex = index;
    const QList<core::DesktopItem>& items = m_grid.items();
    if (index >= 0 && index < items.size() && items.at(index).missing) {
        QToolTip::showText(QCursor::pos(),
                           tr("Not found: %1").arg(QDir::toNativeSeparators(items.at(index).path))
                               + QLatin1Char('\n') + tr("Renamed, moved or deleted outside StarFish.")
                               + QLatin1Char('\n')
                               + (items.at(index).restorable ? tr("Right-click to restore the shortcut or remove it from the fence.")
                                                             : tr("Right-click to remove it from the fence.")));
    } else if (index >= 0 && index < items.size() && !m_grid.labelsShown()) {
        // Session 76: names hidden -- the name on hover.
        QToolTip::showText(QCursor::pos(), items.at(index).name.isEmpty() ? items.at(index).fileName
                                                                          : items.at(index).name);
    } else {
        QToolTip::hideText();
    }
}

int FenceView::removeItemsByPath(const QStringList& paths)
{
    // F-011: the tile is not hidden, moved or deleted -- the fence simply stops
    // listing it, and the desktop backdrop draws it again because it is no
    // longer in the occupied set (T-039). One place holds the fact, and it is
    // the fence's item list.
    const int removed = m_grid.removeItemsByPath(paths);
    if (removed > 0) {
        update();
        emit contentChanged();
    }
    return removed;
}

void FenceView::dragEnterEvent(QDragEnterEvent* event)
{
    // Qt translates the Explorer's CF_HDROP into a list of file:// URLs.
    if (!m_locked && event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void FenceView::dragMoveEvent(QDragMoveEvent* event)
{
    if (!m_locked && event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void FenceView::dropEvent(QDropEvent* event)
{
    if (m_locked || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    QList<core::DesktopItem> items;
#if defined(_WIN32)
    // Resolve each dropped path the same way the desktop scan does, so the
    // tile (display name, shortcut target, hidden flag) matches the Shell.
    items.reserve(event->mimeData()->urls().size());
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        if (local.isEmpty())
            continue;
        const core::DesktopItem item =
            starfish::platform::win::DesktopScanner::itemForPath(local);
        if (!item.path.isEmpty())
            items.append(item);
    }
#else
    // Non-Windows: keep the path so the model still gains a tile; name falls
    // back to the file name through itemForPath's equivalent on that platform.
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        if (local.isEmpty())
            continue;
        core::DesktopItem item;
        item.path = local;
        item.fileName = QFileInfo(local).fileName();
        item.name = item.fileName;
        items.append(item);
    }
#endif

    if (!items.isEmpty())
        insertItems(items, insertionIndexAt(event->position().toPoint())); // where it was let go
    event->acceptProposedAction();
}

void FenceView::paintInsertionBar(int gap, bool afterCell, QPainter& painter)
{
    // Session 80: a thin accent bar in the gutter where the icon will go,
    // across the flow, with a pale halo so it reads on any panel.
    const QLine bar = insertionBar(gap, afterCell);
    if (bar.isNull())
        return;
    const QColor accent(0x3B, 0x82, 0xF6);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(255, 255, 255, 110), 6.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(bar);
    painter.setPen(QPen(accent, 3.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(bar);
    painter.restore();
}

void FenceView::paintReleaseHint(QPainter& painter)
{
    // F-011: what letting go *now* would do. The cursor is outside the fence,
    // so the tile is about to go back to the desktop. Drawn inside the fence
    // because the fence is all we own -- and a dashed accent border is
    // deliberately *not* the drop indicator: one means "the grid takes it",
    // the other means "the desktop takes it".
    const QColor accent(0x3B, 0x82, 0xF6);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(accent, 2.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0), m_cornerRadius,
                            m_cornerRadius);
    const QRect content = contentRect();
    if (!content.isEmpty()) {
        painter.setPen(accent);
        painter.drawText(content, Qt::AlignCenter,
                         tr("release to put it back on the desktop"));
    }
    painter.restore();
}

void FenceView::showContextMenuForIndex(int index, const QPoint& globalPos)
{
#if defined(_WIN32)
    if (index < 0 || index >= m_grid.items().size())
        return;
    // Prefer the resolved target for the menu (so "Open with", "Run as
    // administrator" and friends act on what the user actually meant to
    // launch); fall back to the desktop path for plain files / folders.
    const core::DesktopItem& item = m_grid.items().at(index);
    const QString path = !item.targetPath.isEmpty() ? item.targetPath : item.path;
    if (path.isEmpty())
        return;
    starfish::platform::win::ShellContextMenu::showForPath(
        path, globalPos, reinterpret_cast<void*>(::GetForegroundWindow()));
#else
    Q_UNUSED(index);
    Q_UNUSED(globalPos);
#endif
}

} // namespace starfish::ui

namespace starfish::ui {

QImage FenceView::dragImageFor(int index, const QPoint& press, qreal dpr, QPoint* hotspot) const
{
    if (index < 0 || index >= m_grid.items().size())
        return {};
    const QRect content = contentRect();
    const QVector<IconCell> cells = m_grid.cells(content.size());
    if (index >= cells.size())
        return {};
    DragTile tile;
    tile.icon = cells.at(index).icon.translated(content.topLeft());
    tile.label = cells.at(index).label.translated(content.topLeft());
    const core::DesktopItem& item = m_grid.items().at(index);
    tile.pixmap = m_iconProvider && !item.isWidget() ? m_iconProvider(item, qRound(m_grid.iconSize() * dpr)) : QPixmap();
    tile.text = item.name;
    QPoint topLeft;
    const QImage image = DragImage::compose({tile}, m_grid.captionFont(), DragImage::kOpacity, dpr, &topLeft);
    if (hotspot)
        *hotspot = press - topLeft;
    return image;
}

bool FenceView::dragGhostVisible() const
{
    return m_ghost && m_ghost->isVisible();
}


QVector<QRect> FenceView::buttonItemRects() const
{
    if (!zone_shape::isButton(m_shape) || m_rolledUp)
        return {};
    return zone_shape::buttonItemRects(contentRect(), m_grid.items().size(), m_grid.iconSize());
}

void FenceView::paintButton(QPainter& painter)
{
    const QRect box = rect();
    const int side = qMin(box.width(), box.height());
    const QRect circle(box.center().x() - side / 2, box.center().y() - side / 2, side, side);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor fill = m_panelColor;
    if (m_hovered && m_hoverEffect == HoverEffect::Lighter)
        fill = fill.lighter(112);
    painter.setPen(QPen(m_borderColor.isValid() ? m_borderColor : QColor(0x3B, 0x82, 0xF6),
                        qMax(1, m_borderWidth)));
    painter.setBrush(fill);
    painter.drawEllipse(circle.adjusted(1, 1, -1, -1));
    if (m_rolledUp) {
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(qMax(11, box.height() / 5));
        painter.setFont(f);
        painter.setPen(QColor(0xE5, 0xE7, 0xEB));
        const QString label = m_title.isEmpty() ? tr("Button") : m_title;
        painter.drawText(box.adjusted(6, 6, -6, -6), Qt::AlignCenter | Qt::TextWordWrap, label);
        if (m_buttonMoveMode)
            widget_resize::paintCircleOutline(painter, circle.adjusted(1, 1, -1, -1),
                                              accentColor(), 1.0);
        return;
    }
    const auto geoms = zone_shape::buttonItemGeometry(contentRect(), m_grid.items().size(),
                                                      m_grid.iconSize());
    const QList<core::DesktopItem>& items = m_grid.items();
    const bool names = m_grid.labelsShown();
    QFont nameFont = font();
    nameFont.setPixelSize(qMax(10, m_grid.iconSize() / 4));
    for (int i = 0; i < geoms.size() && i < items.size(); ++i) {
        const QPixmap icon = m_iconProvider ? m_iconProvider(items.at(i), m_grid.iconSize()) : QPixmap();
        const QRect r = geoms.at(i).icon;
        if (!icon.isNull())
            painter.drawPixmap(r, icon);
        else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x60, 0x70, 0x80));
            painter.drawRoundedRect(r, 6, 6);
        }
        if (m_grid.isSelected(i)) {
            painter.setPen(QPen(QColor(0x3B, 0x82, 0xF6), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 8, 8);
        }
        if (names && !geoms.at(i).label.isEmpty()) {
            painter.setFont(nameFont);
            painter.setPen(QColor(0xE5, 0xE7, 0xEB));
            const QString caption = painter.fontMetrics().elidedText(
                items.at(i).name, Qt::ElideRight, geoms.at(i).label.width());
            painter.drawText(geoms.at(i).label, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, caption);
        }
    }
}

void FenceView::updateDragGhost(bool dragging)
{
    if (!dragging) {
        if (m_ghost) {
            m_ghost->hide();
            m_ghost->deleteLater();
            m_ghost = nullptr;
        }
        return;
    }
    if (!m_ghost) {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
        QPoint hotspot;
        const QImage image = dragImageFor(m_dragOriginIndex, m_dragStartPos, dpr, &hotspot);
        if (image.isNull())
            return;
        m_ghost = new DragGhost;
        m_ghost->setImage(image, hotspot);
        connect(this, &QObject::destroyed, m_ghost, &QObject::deleteLater);
    }
    m_ghost->follow(QCursor::pos());
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setTextShadow(bool on)
{
    if (m_grid.textShadow() == on)
        return;
    m_grid.setTextShadow(on);
    update();
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::clearSelection()
{
    endWidgetResize(); // session 98: a click elsewhere ends the resize mode
    if (m_grid.selectedIndices().isEmpty())
        return;
    m_grid.clearSelection();
    update();
    emit contentChanged();
}

QStringList FenceView::selectedPaths() const
{
    QList<int> indices = m_grid.selectedIndices().values();
    std::sort(indices.begin(), indices.end());
    QStringList paths;
    for (int i : indices) {
        if (i >= 0 && i < m_grid.items().size() && !m_grid.items().at(i).path.isEmpty())
            paths.append(m_grid.items().at(i).path);
    }
    return paths;
}

void FenceView::setExternalDropIndex(int index, bool afterCell)
{
    if (m_locked)
        index = -1; // session 94: nothing lands in a locked fence, so no bar
    if (m_externalDropIndex == index && m_externalAfterCell == afterCell)
        return;
    m_externalDropIndex = index;
    m_externalAfterCell = afterCell;
    update();
    emit contentChanged();
}

void FenceView::setDragOverOtherFence(bool over)
{
    if (m_dragOverOtherFence == over)
        return;
    m_dragOverOtherFence = over;
    update();
    emit contentChanged();
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setBorder(int width, const QColor& color)
{
    if (m_borderWidth == width && m_borderColor == color)
        return;
    m_borderWidth = qMax(0, width);
    m_borderColor = color;
    update();
}

void FenceView::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
    m_hovered = true;
    if (m_hoverHighlight || !titleShown()) {
        update();
        emit contentChanged();
    }
    emit hoverChanged(true);
}

void FenceView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && !m_resizePath.isEmpty()) { // session 98
        endWidgetResize();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_buttonMoveMode) { // session 107
        endButtonMoveMode();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F2) {
        // Session 74: rename the selected icon (the edit is the product's).
        const QStringList selected = selectedPaths();
        if (!selected.isEmpty() && !core::isWidgetPath(selected.first())) // session 79: no name to edit
            emit renameRequested(selected.first());
        event->accept();
        return;
    }
    if (event->key() != Qt::Key_Delete) {
        QWidget::keyPressEvent(event);
        return;
    }
    // Session 73: Delete sends the selection to the Recycle Bin (Shift: the
    // Shell's permanent delete, with its own confirmation). Shell folders that
    // are not files (This PC, the Recycle Bin, ...) are never deleted.
    QStringList paths;
    QStringList widgetPaths; // session 79: Delete takes a widget out (no file)
    for (const QString& path : selectedPaths()) {
        if (core::isWidgetPath(path)) {
            widgetPaths.append(path);
            continue;
        }
        if (path.isEmpty() || path.startsWith(QLatin1String("::")) || path.startsWith(QLatin1String("shell:"), Qt::CaseInsensitive))
            continue;
        paths.append(path);
    }
    if (!widgetPaths.isEmpty())
        emit itemReleaseRequested(widgetPaths);
    if (!paths.isEmpty())
        emit deleteRequested(paths, event->modifiers().testFlag(Qt::ShiftModifier));
    event->accept();
}

void FenceView::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    m_hovered = false;
    if (m_hoverHighlight || !titleShown()) {
        update();
        emit contentChanged();
    }
    emit hoverChanged(false);
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setFixedCellWidth(int px)
{
    if (m_grid.fixedCellWidth() == px)
        return;
    m_grid.setFixedCellWidth(px);
    update();
}

} // namespace starfish::ui

namespace starfish::ui {

void FenceView::setTitleBarShown(bool shown)
{
    if (m_titleBarShown == shown)
        return;
    m_titleBarShown = shown;
    update();
    emit contentChanged();
}

} // namespace starfish::ui
