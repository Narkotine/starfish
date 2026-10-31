#include "ui/desktop_backdrop.hpp"

#include <QPainterPath>
#include "ui/drag_image.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QElapsedTimer>
#include <QRegion>
#include <QtMath>
#include <QLoggingCategory>

#include "platform/win/desktop_surface.hpp"

Q_LOGGING_CATEGORY(lcBackdrop, "starfish.backdrop")

namespace starfish::ui {

namespace {

// How long a burst of changes is coalesced before the full-screen frame is
// rebuilt and pushed. A fence drag emits ~60 fenceRectChanged a second, and each
// render uploads the whole virtual screen through UpdateLayeredWindow.
constexpr int kRenderCoalesceMs = 120;

// The lowest alpha any pixel of the backdrop's frame may carry lives in the
// header (DesktopBackdrop::kInputAlphaFloor) -- it is one half of a measurement
// the probe repeats, not a rendering detail. See buildFrame().

const QColor kLabelText(0xF2, 0xF4, 0xF8);
const QColor kLabelShadow(0x00, 0x00, 0x00, 0xB4);
constexpr QRgb kIconPlaceholder = 0x37;

// The rectangle being drawn (T-041). Deliberately not the panel colour: this is
// a preview of a *gesture*, not a fence yet, and it has to read as one while it
// is dragged over whatever wallpaper is behind it.
const QColor kRubberBandPen(0x3B, 0x82, 0xF6);

// Draws the cells into an image whose origin is `area.topLeft()`. The cells
// carry virtual-screen coordinates (a monitor left of the primary one is
// negative), so the painter is translated once instead of every cell.
// `metrics` are the *physical* ones -- the frame is physical pixels throughout.
void paintCells(QPainter& painter, const QPoint& origin, const QVector<BackdropCell>& cells,
                const IconGrid::IconProvider& iconFor, const BackdropLayout::Metrics& metrics,
                qreal scale)
{
    if (cells.isEmpty())
        return;

    painter.save();
    painter.translate(-origin);

    QFont labelFont;
    labelFont.setPixelSize(qMax(9, qRound(12.0 * scale)));
    painter.setFont(labelFont);
    const QFontMetrics metricsForFont(labelFont);

    for (const BackdropCell& cell : cells) {
        const QPixmap icon = iconFor ? iconFor(cell.item, metrics.iconSize) : QPixmap();
        if (icon.isNull()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(kIconPlaceholder, kIconPlaceholder, kIconPlaceholder));
            painter.drawRect(cell.icon);
        } else {
            const QPixmap scaled =
                icon.scaled(cell.icon.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter.drawPixmap(cell.icon.x() + (cell.icon.width() - scaled.width()) / 2,
                               cell.icon.y() + (cell.icon.height() - scaled.height()) / 2, scaled);
        }

        // A caption has to stay readable on any wallpaper, so it gets the same
        // drop shadow the Windows desktop uses rather than a background plate.
        const QString text =
            metricsForFont.elidedText(cell.item.name, Qt::ElideMiddle, cell.label.width());
        painter.setPen(kLabelShadow);
        painter.drawText(cell.label.translated(1, 1),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine, text);
        painter.setPen(kLabelText);
        painter.drawText(cell.label, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine, text);
    }

    painter.restore();
}

// ADR-21 -- the desktop's own look, for the drawn desktop. Measured on the
// user's Windows 11 (build\captures\crop_icons.png): white Segoe UI captions,
// centred, up to two lines with a soft dark shadow, and a light-blue
// translucent rectangle with a lighter border around a selected icon.
const QColor kDesktopText(0xFF, 0xFF, 0xFF);
const QColor kDesktopShadow(0x00, 0x00, 0x00, 0xA0);
const QColor kSelectionFill(0x99, 0xD1, 0xFF, 0x59);
const QColor kSelectionBorder(0xCC, 0xE8, 0xFF, 0xA6);

// Up to two lines, broken at the last space that fits, the second one elided --
// what the desktop does with a caption that is not selected.
QStringList captionLines(const QString& text, const QFontMetrics& fm, int width)
{
    if (text.isEmpty() || fm.horizontalAdvance(text) <= width)
        return {text};
    int cut = -1;
    for (int i = text.size() - 1; i > 0; --i) {
        if (text.at(i) == QLatin1Char(' ') && fm.horizontalAdvance(text.left(i)) <= width) {
            cut = i;
            break;
        }
    }
    if (cut < 0) {
        // One long word: broken where it stops fitting.
        cut = 1;
        while (cut < text.size() && fm.horizontalAdvance(text.left(cut + 1)) <= width)
            ++cut;
        return {text.left(cut), fm.elidedText(text.mid(cut), Qt::ElideRight, width)};
    }
    return {text.left(cut), fm.elidedText(text.mid(cut + 1), Qt::ElideRight, width)};
}

void paintDesktop(QPainter& painter, const QPoint& origin, const QVector<BackdropCell>& cells,
                  const IconGrid::IconProvider& iconFor, const BackdropLayout::Metrics& metrics,
                  qreal scale, const QSet<QString>& selected)
{
    painter.save();
    painter.translate(-origin);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(qMax(9, qRound(12.0 * scale)));
    painter.setFont(font);
    const QFontMetrics fm(font);
    const int pad = qRound(2 * scale);
    const int inset = qRound(4 * scale);
    const int lineHeight = fm.height();

    for (const BackdropCell& cell : cells) {
        const QStringList lines = captionLines(cell.item.name, fm, cell.label.width() - 2 * inset);
        const int textTop = cell.label.y() + pad;

        if (selected.contains(BackdropLayout::pathKey(cell.item.path))) {
            const int top = cell.icon.y() - pad;
            const QRect highlight(cell.label.x() + inset, top, cell.label.width() - 2 * inset,
                                  textTop + lines.size() * lineHeight + pad - top);
            painter.setPen(QPen(kSelectionBorder, 1));
            painter.setBrush(kSelectionFill);
            painter.drawRect(highlight.adjusted(0, 0, -1, -1));
        }

        const QPixmap icon = iconFor ? iconFor(cell.item, metrics.iconSize) : QPixmap();
        if (!icon.isNull()) {
            const QPixmap scaled =
                icon.size() == cell.icon.size()
                    ? icon
                    : icon.scaled(cell.icon.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter.drawPixmap(cell.icon.x() + (cell.icon.width() - scaled.width()) / 2,
                               cell.icon.y() + (cell.icon.height() - scaled.height()) / 2, scaled);
        }

        for (int i = 0; i < lines.size(); ++i) {
            const QRect line(cell.label.x(), textTop + i * lineHeight, cell.label.width(),
                             lineHeight);
            const int flags = Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine;
            painter.setPen(kDesktopShadow);
            painter.drawText(line.translated(1, 1), flags, lines.at(i));
            painter.drawText(line.translated(0, 1), flags, lines.at(i));
            painter.setPen(kDesktopText);
            painter.drawText(line, flags, lines.at(i));
        }
    }
    painter.restore();
}

// The two halves of the same decision (T-041): a surface that answers the empty
// desktop's gestures has to receive the mouse to answer them, and one that
// answers nothing stays out of the way. The drop target is deliberately not part
// of it -- that one is registered from a drop handler, and the backdrop has none
// (T-052: dropping onto the desktop moves a file, a different decision).
void applyGestureMode(platform::win::DesktopSurface* surface, bool gestures, QObject* owner)
{
    surface->setClickThrough(!gestures);
    surface->setInputTarget(gestures ? owner : nullptr);
}

} // namespace

DesktopBackdrop::DesktopBackdrop(QObject* parent) : QObject(parent)
{
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(kRenderCoalesceMs);
    connect(&m_renderTimer, &QTimer::timeout, this, &DesktopBackdrop::onRenderTimer);
    // Session 63: a gesture's rectangle is repainted on the next turn of the
    // event loop -- every move that arrived meanwhile is one frame -- and only
    // where it was and where it is.
    m_partialTimer.setSingleShot(true);
    m_partialTimer.setInterval(0);
    connect(&m_partialTimer, &QTimer::timeout, this, &DesktopBackdrop::onPartialTimer);
}

DesktopBackdrop::~DesktopBackdrop()
{
    detach();
}

void DesktopBackdrop::setIconProvider(IconGrid::IconProvider provider)
{
    m_iconProvider = std::move(provider);
}

void DesktopBackdrop::setScreens(const ScreenLayout& layout)
{
    const ScreenInfo* oldPrimary = m_screens.primary();
    const ScreenInfo* newPrimary = layout.primary();
    const qreal oldScale = oldPrimary ? oldPrimary->devicePixelRatio : 1.0;
    const qreal newScale = newPrimary ? newPrimary->devicePixelRatio : 1.0;
    if (m_screens.count() == layout.count() && m_screens.bounds() == layout.bounds()
        && qFuzzyCompare(oldScale, newScale))
        return;
    m_screens = layout;
    if (!m_enabled)
        return;
    // The virtual screen changed size: the window has to follow, and the frame
    // with it. Re-attaching is also how the new size reaches the surface.
    detach();
    attach();
}

qreal DesktopBackdrop::scale() const
{
    const ScreenInfo* primary = m_screens.primary();
    return primary ? qMax(1.0, primary->devicePixelRatio) : 1.0;
}

BackdropLayout::Metrics DesktopBackdrop::physicalMetrics() const
{
    const qreal s = scale();
    BackdropLayout::Metrics physical;
    physical.iconSize = qMax(1, qRound(m_metrics.iconSize * s));
    physical.cellWidth = qMax(physical.iconSize, qRound(m_metrics.cellWidth * s));
    physical.labelHeight = qMax(0, qRound(m_metrics.labelHeight * s));
    physical.gap = qMax(0, qRound(m_metrics.gap * s));
    physical.margin = qMax(0, qRound(m_metrics.margin * s));
    return physical;
}

QImage DesktopBackdrop::buildFrame() const
{
    const QRect box = area();
    if (box.isEmpty())
        return {};
    QImage frame(box.size(), QImage::Format_ARGB32_Premultiplied);
    if (drawsWholeDesktop()) {
        // ADR-21: the whole desktop, opaque -- the wallpaper the shell renders
        // (captured while its list is hidden), then the icons where the shell
        // keeps them, then the selection. No input floor, no holes: nothing of
        // the desktop is under this window any more.
        frame.fill(Qt::black);
        paintArea(frame, box);
        return frame;
    }
    // NOT fully transparent: see DesktopBackdrop::kInputAlphaFloor. The one alpha
    // level is what makes the frame a rectangle the cursor can find, and without
    // it the empty desktop's gestures are unreachable through any mouse (T-060).
    frame.fill(QColor(0, 0, 0, kInputAlphaFloor));
    // ⚠️ T-069 -- and the icons' own rectangles are cut *out* of our layer here.
    // A per-pixel layered window is not a window for the mouse where its pixels
    // are at alpha 0 (T-060, measured in both directions on this very class), and
    // that is the *only* step-aside that reaches another process: answering
    // HTTRANSPARENT to WM_NCHITTEST stops inside our own thread, so the shell's
    // list view never heard about the click. Measured with the backdrop hosted:
    // **0 of 4** points on real icons delivered, against **1 of 4** without
    // StarFish -- the desktop's icons were visible and untouchable, which is
    // exactly what the user reported ("pendant que starfish est lancé, le bureau
    // n'est plus atteignable"). The WndProc keeps its answer -- it costs nothing
    // and it is the right answer -- but the holes are what deliver the click.
    // Cut before anything is painted, so the rubber band still draws over them.
    if (!m_nativeRects.isEmpty()) {
        QPainter cutter(&frame);
        cutter.setCompositionMode(QPainter::CompositionMode_Source);
        cutter.setPen(Qt::NoPen);
        cutter.setBrush(Qt::transparent);
        for (const QRect& rect : m_nativeRects)
            cutter.drawRect(rect.translated(-box.topLeft()));
    }
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // ADR-13: nothing is painted on the fence-less desktop by default -- the
    // native icons are there, they are the ones the user arranged, and drawing a
    // second copy of them would be exactly the duplication this decision is
    // about. What is left on screen is the rubber band, and the one alpha level
    // that keeps the frame findable.
    if (m_drawFreeIcons)
        paintCells(painter, box.topLeft(), m_cells, m_iconProvider, physicalMetrics(), scale());

    paintOverlays(painter, box);
    painter.end();
    return frame;
}

void DesktopBackdrop::paintArea(QImage& frame, const QRect& dirty) const
{
    const QRect box = area();
    const QRect local = dirty.intersected(box).translated(-box.topLeft()).intersected(frame.rect());
    if (local.isEmpty())
        return;
    QPainter painter(&frame);
    painter.setClipRect(local);
    if (m_wallpaper.size() == box.size()) {
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(local.topLeft(), m_wallpaper, local);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else {
        painter.fillRect(local, Qt::black);
        painter.drawImage(QRect(QPoint(0, 0), box.size()), m_wallpaper);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QSet<QString> selected;
    for (const QString& path : m_selection)
        selected.insert(BackdropLayout::pathKey(path));
    // Only the icons that touch the dirty pixels (their caption may run two
    // lines and its shadow one pixel further than the cell says).
    const int margin = qRound(8 * scale());
    const int caption = qRound(16 * scale());
    QVector<BackdropCell> touched;
    if (dirty.contains(box)) {
        touched = m_cells;
    } else {
        for (const BackdropCell& cell : m_cells) {
            if (cell.icon.united(cell.label).adjusted(-margin, -margin, margin, margin + caption).intersects(dirty))
                touched.append(cell);
        }
    }
    paintDesktop(painter, box.topLeft(), touched, m_iconProvider, physicalMetrics(), scale(), selected);
    paintOverlays(painter, box);
}

void DesktopBackdrop::paintOverlays(QPainter& painter, const QRect& box) const
{
    // The rubber band carries virtual-screen pixels like the cells do, so the
    // painter is translated once.
    painter.save();
    painter.translate(-box.topLeft());
    if (!m_marquee.isEmpty()) {
        // The desktop's own selection rectangle (Windows 11 colours).
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(0x00, 0x78, 0xD7), 1));
        painter.setBrush(QColor(0x00, 0x78, 0xD7, 0x40));
        painter.drawRect(m_marquee.adjusted(0, 0, -1, -1));
    }
    if (!m_rubberBand.isEmpty()) {
        // The fence being drawn (T-041), shown as the fence it will be (T-087b,
        // session 65): its panel, its corners, its title band -- translucent,
        // with a dashed outline, so it still reads as "not made yet".
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = qMax(1.0, scale());
        const QRectF band = QRectF(m_rubberBand).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = qMin<qreal>(m_previewRadius * s, qMin(band.width(), band.height()) / 2);
        QPainterPath shape;
        shape.addRoundedRect(band, radius, radius);
        QColor panel = m_previewPanel;
        panel.setAlpha(0xA0);
        painter.fillPath(shape, panel);
        painter.save();
        painter.setClipRect(QRectF(band.left(), band.top(), band.width(), qMin<qreal>(30 * s, band.height())));
        painter.fillPath(shape, QColor(255, 255, 255, 0x22));
        painter.restore();
        QPen pen(kRubberBandPen, s);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(shape);
    }
    painter.restore();
}

QRect DesktopBackdrop::overlayBounds(const QRect& rect) const
{
    if (rect.isNull())
        return {};
    const int pen = qCeil(qMax(1.0, scale())) + 2;
    return rect.normalized().adjusted(-pen, -pen, pen, pen);
}

void DesktopBackdrop::invalidate(const QRect& dirty)
{
    if (dirty.isEmpty())
        return;
    // The transparent layer (not the drawn desktop) keeps its full render: it
    // is pushed per pixel, whole, anyway.
    if (!drawsWholeDesktop() || !isAttached() || m_frame.isNull() || m_frame.size() != area().size()) {
        refresh();
        return;
    }
    m_dirty += dirty;
    if (!m_partialTimer.isActive())
        m_partialTimer.start();
}

void DesktopBackdrop::onPartialTimer()
{
    if (m_dirty.isEmpty())
        return;
    if (m_renderTimer.isActive() || m_frame.isNull() || m_frame.size() != area().size()) {
        m_dirty = QRegion(); // a full render is on its way and covers it
        return;
    }
    QElapsedTimer cost;
    cost.start();
    const QRect box = area();
    for (const QRect& rect : m_dirty)
        paintArea(m_frame, rect);
    const QRect bounds = m_dirty.boundingRect().intersected(box);
    m_dirty = QRegion();
    if (m_surface)
        m_surface->updateFrame(m_frame, bounds.translated(-box.topLeft()));
    noteFrameShown(cost.nsecsElapsed() / 1000);
    emit frameChanged(bounds);
}

void DesktopBackdrop::setBand(const QRect& band)
{
    if (band == m_rubberBand)
        return;
    const QRect before = overlayBounds(m_rubberBand);
    m_rubberBand = band;
    invalidate(before);
    invalidate(overlayBounds(m_rubberBand));
}

void DesktopBackdrop::setMarqueeRect(const QRect& marquee)
{
    if (marquee == m_marquee)
        return;
    const QRect before = overlayBounds(m_marquee);
    m_marquee = marquee;
    invalidate(before);
    invalidate(overlayBounds(m_marquee));
}

QRegion DesktopBackdrop::selectionDelta(const QStringList& before, const QStringList& after) const
{
    QSet<QString> a, b;
    for (const QString& p : before)
        a.insert(BackdropLayout::pathKey(p));
    for (const QString& p : after)
        b.insert(BackdropLayout::pathKey(p));
    QRegion region;
    const int margin = qRound(8 * scale());
    const int caption = qRound(16 * scale());
    for (const BackdropCell& cell : m_cells) {
        const QString key = BackdropLayout::pathKey(cell.item.path);
        if (a.contains(key) != b.contains(key))
            region += cell.icon.united(cell.label).adjusted(-margin, -margin, margin, margin + caption);
    }
    return region;
}

void DesktopBackdrop::setNativeIconRects(const QVector<QRect>& rects)
{
    if (m_nativeRects == rects)
        return;
    m_nativeRects = rects;
    // ADR-21: the drawn desktop steps aside for nothing -- the list is hidden.
    if (m_surface && !drawsWholeDesktop())
        m_surface->setNativeIconRects(rects);
    // The rects are no longer only an answer to WM_NCHITTEST: they are cut out of
    // the frame (see buildFrame), so a change has to reach the screen -- otherwise
    // an icon the user just dropped on the desktop would stay untouchable until
    // the next render happened to come along.
    refresh();
}

void DesktopBackdrop::setNativeIconBoxes(const QHash<QString, QRect>& boxes)
{
    if (m_nativeBoxes == boxes)
        return;
    m_nativeBoxes = boxes;
    refresh();
}

void DesktopBackdrop::setDrawFreeIcons(bool draw)
{
    if (m_drawFreeIcons == draw)
        return;
    const bool was = drawsWholeDesktop();
    m_drawFreeIcons = draw;
    // T-042: the icons of this layer are only ours to carry while it draws them.
    // Turning the drawing off mid-carry puts the icon back rather than leaving a
    // path armed for a release that is no longer about anything.
    m_pressedIcon.clear();
    remodel(was);
}

void DesktopBackdrop::setWallpaper(const QImage& wallpaper)
{
    const bool was = drawsWholeDesktop();
    m_wallpaper = wallpaper;
    remodel(was);
}

void DesktopBackdrop::remodel(bool wasWholeDesktop)
{
    // The two modes are two different windows as far as Windows is concerned: a
    // per-pixel layer with holes, or an opaque blit surface that takes every
    // click (ADR-15 -- the layered modes cannot be switched on a live child).
    if (wasWholeDesktop != drawsWholeDesktop() && isAttached()) {
        detach();
        attach();
    }
    refresh();
}

void DesktopBackdrop::setSelection(const QStringList& paths)
{
    if (m_selection == paths)
        return;
    const QRegion changed = selectionDelta(m_selection, paths);
    m_selection = paths;
    // Session 63: only the icons whose highlight changed are repainted.
    for (const QRect& rect : changed)
        invalidate(rect);
}

void DesktopBackdrop::setSelectionInternal(const QStringList& paths)
{
    if (m_selection == paths)
        return;
    const QRegion changed = selectionDelta(m_selection, paths);
    m_selection = paths;
    emit selectionChanged(m_selection);
    for (const QRect& rect : changed)
        invalidate(rect);
}

bool DesktopBackdrop::overNativeIcon(const QPoint& screen) const
{
    for (const QRect& icon : m_nativeRects) {
        if (icon.contains(screen))
            return true;
    }
    return false;
}

void DesktopBackdrop::setDesktopItems(QList<core::DesktopItem> items)
{
    m_desktop.clear();
    m_desktop.reserve(items.size());
    for (const core::DesktopItem& item : items) {
        // The Shell does not draw hidden entries unless the user asked it to, and
        // a backdrop that drew them would not look like the desktop it replaces.
        if (item.isHidden)
            continue;
        m_desktop.append(item);
    }
    refresh();
}

void DesktopBackdrop::setOccupiedPaths(const QSet<QString>& keys)
{
    if (m_occupied == keys)
        return;
    m_occupied = keys;
    refresh();
}

void DesktopBackdrop::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (m_enabled) {
        if (attach())
            renderNow();
    } else {
        // A draw in progress ends here: the window it was being drawn on is
        // about to go away, and a rubber band left behind would be repainted by
        // the next attach.
        m_gesture.cancel();
        m_rubberBand = {};
        detach();
        m_renderTimer.stop();
        m_frame = QImage();
    }
}

void DesktopBackdrop::setGestureEnabled(bool enabled)
{
    if (m_gestures == enabled)
        return;
    m_gestures = enabled;
    if (!m_gestures) {
        const BackdropGesture::Result result = m_gesture.cancel();
        if (result.action == BackdropGesture::Action::Cancelled) {
            m_rubberBand = {};
            refresh();
        }
    }
    applyGestureModeToSurface();
}

void DesktopBackdrop::applyGestureModeToSurface()
{
    if (!m_surface)
        return;
    applyGestureMode(m_surface, m_gestures, this);
}

bool DesktopBackdrop::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        emit pressed();
        return handleMouse(static_cast<QMouseEvent*>(event));
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        return handleMouse(static_cast<QMouseEvent*>(event));
    case QEvent::KeyPress:
        return handleKey(static_cast<QKeyEvent*>(event));
    default:
        return QObject::event(event);
    }
}

int DesktopBackdrop::cellOf(const QString& path) const
{
    const QString key = BackdropLayout::pathKey(path);
    for (int i = 0; i < m_cells.size(); ++i) {
        if (BackdropLayout::pathKey(m_cells.at(i).item.path) == key)
            return i;
    }
    return -1;
}

QRect DesktopBackdrop::labelRectOf(const QString& path) const
{
    const int i = cellOf(path);
    return i < 0 ? QRect() : m_cells.at(i).label;
}

QImage DesktopBackdrop::dragImage(const QStringList& paths, const QPoint& press, QPoint* hotspot) const
{
    QSet<QString> keys;
    for (const QString& p : paths)
        keys.insert(BackdropLayout::pathKey(p));
    const qreal s = scale();
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(qMax(9, qRound(12.0 * s)));
    const QFontMetrics fm(font);
    const int inset = qRound(4 * s);
    const int reach = qRound(360 * s);
    QList<DragTile> tiles;
    for (const BackdropCell& cell : m_cells) {
        if (!keys.contains(BackdropLayout::pathKey(cell.item.path)))
            continue;
        const QPoint c = cell.icon.center();
        if (qAbs(c.x() - press.x()) > reach || qAbs(c.y() - press.y()) > reach)
            continue;
        DragTile tile;
        tile.icon = cell.icon;
        tile.label = QRect(cell.label.x() + inset, cell.label.y() + qRound(2 * s),
                           cell.label.width() - 2 * inset, qMin(cell.label.height(), 2 * fm.height()));
        tile.pixmap = m_iconProvider ? m_iconProvider(cell.item, physicalMetrics().iconSize) : QPixmap();
        tile.text = cell.item.name;
        tiles.append(tile);
    }
    QPoint topLeft;
    const QImage image = DragImage::compose(tiles, font, DragImage::kOpacity, 1.0, &topLeft);
    if (hotspot)
        *hotspot = press - topLeft;
    return image;
}

QRect DesktopBackdrop::iconRectOf(const QString& path) const
{
    const int i = cellOf(path);
    return i < 0 ? QRect() : m_cells.at(i).icon;
}

void DesktopBackdrop::cancelGesture()
{
    m_gesture.cancel();
    m_pressedIcon.clear();
    m_narrowOnRelease.clear();
    m_marqueeArmed = false;
    setMarqueeRect({});
    setBand({});
}

bool DesktopBackdrop::handleKey(QKeyEvent* key)
{
    if (!drawsWholeDesktop())
        return false;
    const Qt::KeyboardModifiers mods = key->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    if (ctrl && key->key() == Qt::Key_A) {
        QStringList all;
        for (const BackdropCell& cell : m_cells)
            all << cell.item.path;
        setSelectionInternal(all);
        return true;
    }
    if (key->key() == Qt::Key_Escape) {
        setSelectionInternal({});
        return true;
    }
    const bool arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right
                       || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down;
    if (arrow) {
        // The nearest icon in that direction from the last selected one (the
        // first icon when nothing is selected), as the desktop's own list does.
        if (m_cells.isEmpty())
            return true;
        const int from = m_selection.isEmpty() ? -1 : cellOf(m_selection.constLast());
        if (from < 0) {
            setSelectionInternal({m_cells.first().item.path});
            return true;
        }
        const QPoint c = m_cells.at(from).icon.center();
        int best = -1;
        qint64 bestScore = 0;
        for (int i = 0; i < m_cells.size(); ++i) {
            if (i == from)
                continue;
            const QPoint d = m_cells.at(i).icon.center() - c;
            int along = 0;
            int across = 0;
            switch (key->key()) {
            case Qt::Key_Left: along = -d.x(); across = d.y(); break;
            case Qt::Key_Right: along = d.x(); across = d.y(); break;
            case Qt::Key_Up: along = -d.y(); across = d.x(); break;
            default: along = d.y(); across = d.x(); break;
            }
            if (along <= 0 || qAbs(across) > along * 2)
                continue;
            const qint64 score = qint64(along) * along + qint64(across) * across * 4;
            if (best < 0 || score < bestScore) {
                best = i;
                bestScore = score;
            }
        }
        if (best >= 0)
            setSelectionInternal({m_cells.at(best).item.path});
        return true;
    }
    emit keyCommand(key->key(), mods);
    return true;
}

bool DesktopBackdrop::handleMouse(QMouseEvent* mouse)
{
    if (!m_gestures)
        return false;

    // Both buttons are forwarded: the right one draws a fence and then opens the
    // desktop's menu (F-001), the left one carries F-022's double-click. The
    // machine decides which of them means anything -- it is the piece that can be
    // tested without a desktop, and duplicating the rule here is how the two
    // halves would drift apart.
    const BackdropGesture::Button button =
        mouse->button() == Qt::RightButton ? BackdropGesture::Button::Right
                                          : BackdropGesture::Button::Left;

    // Measured at the *screen* (T-043): the frame moves under the cursor while a
    // gesture runs, and the cells are in virtual-screen pixels anyway. The
    // surface posts physical screen coordinates here -- which is exactly the
    // space a fence's stored rect uses (T-014), so the drawn rect needs no
    // conversion at all.
    const QPoint screen = mouse->globalPosition().toPoint();
    // Which icons are under the cursor depends on which ones are *there*: the
    // native ones (ADR-13, the default) or the ones this layer would draw. Asking
    // the wrong set is how a right press on an icon would open the background
    // menu -- the cells are laid out in columns for a desktop that is no longer
    // in columns. Only the icons we draw have a path to carry (T-042): the native
    // ones are the shell's, and its own behaviour is untouched because the
    // surface steps the mouse aside over their rectangles.
    const int iconIndex = m_drawFreeIcons ? BackdropGesture::iconAt(m_cells, screen) : -1;
    const bool onIcon = m_drawFreeIcons ? iconIndex >= 0 : overNativeIcon(screen);

    // ADR-21 -- the drawn desktop's own clicks, the ones the native list used to
    // answer: selection, and the right button on an icon. Read before the
    // machine, which knows nothing about selections.
    const bool whole = drawsWholeDesktop();
    const QString hit = iconIndex >= 0 ? m_cells.at(iconIndex).item.path : QString();
    const bool ctrl = mouse->modifiers().testFlag(Qt::ControlModifier);
    const auto selected = [this](const QString& path) {
        const QString key = BackdropLayout::pathKey(path);
        for (const QString& s : m_selection) {
            if (BackdropLayout::pathKey(s) == key)
                return true;
        }
        return false;
    };
    if (whole && mouse->button() == Qt::RightButton) {
        if (mouse->type() == QEvent::MouseButtonPress && !hit.isEmpty()) {
            // As on the real desktop: the icon joins the selection first (or
            // becomes it), and the menu acts on the selection.
            m_rightOnIcon = true;
            if (!selected(hit))
                setSelectionInternal({hit});
            return true;
        }
        if (mouse->type() == QEvent::MouseButtonRelease && m_rightOnIcon) {
            m_rightOnIcon = false;
            if (!m_selection.isEmpty())
                emit iconMenuRequested(m_selection, screen);
            return true;
        }
        if (mouse->type() == QEvent::MouseButtonPress && !ctrl)
            setSelectionInternal({});
    }
    if (whole && mouse->button() == Qt::LeftButton
        && mouse->type() == QEvent::MouseButtonPress) {
        m_narrowOnRelease.clear();
        if (hit.isEmpty()) {
            if (!ctrl)
                setSelectionInternal({});
        } else if (ctrl) {
            QStringList next = m_selection;
            if (selected(hit)) {
                const QString key = BackdropLayout::pathKey(hit);
                next.erase(std::remove_if(next.begin(), next.end(),
                                          [&key](const QString& s) {
                                              return BackdropLayout::pathKey(s) == key;
                                          }),
                           next.end());
            } else {
                next.append(hit);
            }
            setSelectionInternal(next);
        } else if (!selected(hit)) {
            setSelectionInternal({hit});
        } else {
            // Already selected: a drag carries the whole selection, a plain
            // click narrows it to this icon -- on the release.
            m_narrowOnRelease = hit;
        }
    }

    // ADR-21 lot 3: the selection rectangle -- a left press on the bare desktop
    // that travels. Ctrl adds to what was selected.
    if (whole) {
        if (mouse->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            m_marqueeArmed = hit.isEmpty();
            m_marqueeOrigin = screen;
            m_marqueeBase = ctrl ? m_selection : QStringList();
            m_marquee = {};
            if (!hit.isEmpty())
                m_iconPressPoint = screen;
        } else if (mouse->type() == QEvent::MouseMove && m_marqueeArmed
                   && mouse->buttons().testFlag(Qt::LeftButton)) {
            const QPoint d = screen - m_marqueeOrigin;
            if (!m_marquee.isNull()
                || qAbs(d.x()) + qAbs(d.y()) >= m_gesture.metrics().dragThreshold) {
                setMarqueeRect(QRect(m_marqueeOrigin, screen).normalized());
                QStringList next = m_marqueeBase;
                for (const BackdropCell& cell : m_cells) {
                    if (cell.icon.united(cell.label).intersects(m_marquee)
                        && !next.contains(cell.item.path))
                        next << cell.item.path;
                }
                noteGestureMove();
                setSelectionInternal(next);
                return true;
            }
        } else if (mouse->type() == QEvent::MouseButtonRelease
                   && mouse->button() == Qt::LeftButton && m_marqueeArmed) {
            m_marqueeArmed = false;
            if (!m_marquee.isNull()) {
                setMarqueeRect({});
                m_gesture.cancel();
                return true;
            }
        }
    }

    BackdropGesture::Result result;
    switch (mouse->type()) {
    case QEvent::MouseButtonPress:
        // T-042: the identity of the carried icon is taken here, at the press --
        // the cells can be recomputed while the button is held (a fence takes the
        // icon, the document changes), and a drop must file what was picked up.
        m_pressedIcon = (mouse->button() == Qt::LeftButton && iconIndex >= 0)
                            ? m_cells.at(iconIndex).item.path
                            : QString();
        result = m_gesture.press(screen, button, onIcon);
        break;
    case QEvent::MouseMove:
        result = m_gesture.move(screen);
        break;
    case QEvent::MouseButtonRelease:
        result = m_gesture.release(screen, button);
        break;
    case QEvent::MouseButtonDblClick:
        result = m_gesture.doubleClick(screen, onIcon);
        break;
    default:
        return false;
    }
    if (whole && mouse->type() == QEvent::MouseButtonRelease
        && mouse->button() == Qt::LeftButton) {
        if (!m_narrowOnRelease.isEmpty() && result.action != BackdropGesture::Action::IconDrop)
            setSelectionInternal({m_narrowOnRelease});
        m_narrowOnRelease.clear();
    }
    return applyGesture(result);
}

bool DesktopBackdrop::applyGesture(const BackdropGesture::Result& result)
{
    switch (result.action) {
    case BackdropGesture::Action::None:
        return true;

    case BackdropGesture::Action::Drawing:
    case BackdropGesture::Action::Cancelled:
        // The rubber band moved, or went away: repaint. Coalesced like every
        // other render -- a drag emits ~60 moves a second and each one uploads
        // the whole virtual screen.
        // T-042: a carry that was taken away drops nothing, so the icon it was
        // holding is forgotten with the band.
        if (result.action == BackdropGesture::Action::Cancelled)
            m_pressedIcon.clear();
        noteGestureMove();
        setBand(m_gesture.rubberBand());
        return true;

    case BackdropGesture::Action::IconDrag:
        // ADR-21 lot 3: on the drawn desktop the carry is the shell's own drag
        // loop -- to a fence, to an Explorer window, to another application, or
        // back onto the desktop to move the icon. What is carried is the
        // selection when the icon is part of it.
        if (drawsWholeDesktop() && !m_pressedIcon.isEmpty()) {
            const QString pressed = m_pressedIcon;
            QStringList carried;
            for (const QString& s : m_selection) {
                if (BackdropLayout::pathKey(s) == BackdropLayout::pathKey(pressed)) {
                    carried = m_selection;
                    break;
                }
            }
            if (carried.isEmpty())
                carried = {pressed};
            const QPoint origin = m_iconPressPoint;
            cancelGesture();
            emit iconDragRequested(carried, origin);
            return true;
        }
        // T-042: the carry is under way. What the user *sees* while carrying is
        // T-087 (a ghost of the tile, and the fence under the cursor); the
        // gesture is complete without it, so nothing is painted yet -- and the
        // product still gets the drop, which is the half that files an icon.
        return true;

    case BackdropGesture::Action::IconDrop: {
        const QString path = m_pressedIcon;
        m_pressedIcon.clear();
        // Same rule as the activate above: an icon is only ours to file while we
        // are the ones drawing it.
        if (path.isEmpty() || !m_drawFreeIcons)
            return true;
        emit iconDropRequested(path, result.point);
        return true;
    }

    case BackdropGesture::Action::IconActivate: {
        // Only the icons *this layer draws* are ours to open (F-040): with the
        // native layer visible the double-click is the shell's, and it opens the
        // entry itself -- a second open from here would be a doubled launch.
        // (The window's own HTTRANSPARENT hole over a native icon is what keeps
        // this unreachable, but the rule belongs in the code, not in a hit test.)
        if (!m_drawFreeIcons)
            return true;
        // Read from the point rather than from the press: a double-click arrives
        // as press, release, double-click, and the activate is the answer to the
        // *last* point the user aimed at.
        const int index = BackdropGesture::iconAt(m_cells, result.point);
        if (index < 0)
            return true;
        emit iconActivateRequested(m_cells.at(index).item.path);
        return true;
    }

    case BackdropGesture::Action::DesktopMenu:
        setBand({});
        // The backdrop says what happened; the product decides what to make of
        // it, exactly like every other gesture in this project.
        emit desktopMenuRequested(result.rect, result.point);
        return true;

    case BackdropGesture::Action::ToggleVisibility:
        setBand({});
        emit quickHideRequested();
        return true;
    }
    return false;
}

QRect DesktopBackdrop::area() const
{
    const QRect box = m_screens.bounds();
    if (!box.isValid() || box.isEmpty())
        return QRect(0, 0, 1920, 1080);
    return box;
}

bool DesktopBackdrop::attach()
{
    if (isAttached())
        return true;
    if (!platform::win::DesktopSurface::isDesktopAvailable()) {
        qCWarning(lcBackdrop) << "no desktop to host the backdrop on";
        return false;
    }

    const QRect box = area();
    if (box.isEmpty())
        return false;

    // Render first: a layered window with nothing pushed is invisible, and the
    // frame is what proves the per-pixel path works before the native icons are
    // taken away.
    recompute();
    m_frame = buildFrame();

    auto* surface = new platform::win::DesktopSurface(this);
    surface->setLayer(platform::win::DesktopSurface::Layer::Backdrop);
    applyGestureMode(surface, m_gestures, this);
    surface->setDevicePixelRatio(1.0); // the frame is already in physical pixels
    // ADR-21: the drawn desktop is the opaque blit path (ADR-15 -- the only
    // layered mode that reaches the screen on a child of Progman), and owns
    // every click. The transparent layer keeps the per-pixel path and its holes.
    const bool opaque = drawsWholeDesktop();
    surface->setNativeIconRects(opaque ? QVector<QRect>() : m_nativeRects);
    surface->setOwnsAllInput(opaque);
    if (opaque && m_dropFactory)
        surface->setDropTargetFactory(m_dropFactory);
    surface->setFrame(m_frame);
    if (!opaque && !surface->setPerPixelAlpha(true)) {
        qCWarning(lcBackdrop) << "the per-pixel path was refused; no backdrop";
        delete surface;
        return false;
    }
    if (!surface->attach(box)) {
        qCWarning(lcBackdrop) << "could not host the backdrop (no desktop?)";
        delete surface;
        return false;
    }

    // attach() pushes the frame through UpdateLayeredWindow. If that push failed,
    // DesktopSurface has already fallen back to the opaque WM_PAINT path -- which
    // for a full-screen window would hide the wallpaper. Refuse instead.
    if (!opaque && !surface->perPixelAlpha()) {
        qCCritical(lcBackdrop)
            << "the per-pixel push failed (UpdateLayeredWindow): an opaque full-screen "
               "window would hide the wallpaper, so the backdrop is refused and the native "
               "icons must stay visible";
        surface->detach();
        delete surface;
        return false;
    }

    connect(surface, &platform::win::DesktopSurface::desktopLost, this,
            [this] { emit desktopLost(); });

    m_surface = surface;
    qCInfo(lcBackdrop).noquote() << QStringLiteral("backdrop hosted at %1,%2 %3x%4, %5 free "
                                                   "icon(s)")
                                        .arg(box.x())
                                        .arg(box.y())
                                        .arg(box.width())
                                        .arg(box.height())
                                        .arg(m_free.size());
    emit rendered(m_free.size());
    return true;
}

void DesktopBackdrop::detach()
{
    m_renderTimer.stop();
    // The window a gesture was being drawn on is going away (monitor change,
    // shutdown, the native layer coming back): nothing is created from a draw
    // that never ended.
    m_gesture.cancel();
    m_rubberBand = {};
    if (!m_surface)
        return;
    m_surface->detach();
    delete m_surface;
    m_surface = nullptr;
}

bool DesktopBackdrop::isAttached() const
{
    return m_surface != nullptr && m_surface->isAttached();
}

void DesktopBackdrop::recompute()
{
    m_free = BackdropLayout::freeItems(m_desktop, m_occupied);
    // The shell's own boxes are handed to the layout: an icon the user can see
    // through no fault of ours must not move because StarFish started drawing.
    m_cells = BackdropLayout::compute(area(), physicalMetrics(), m_free, m_nativeBoxes);
}

void DesktopBackdrop::refresh()
{
    recompute();
    if (!m_enabled)
        return;
    if (!isAttached() && !attach())
        return;
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

void DesktopBackdrop::onRenderTimer()
{
    renderNow();
}

void DesktopBackdrop::noteGestureMove()
{
    if (!m_pendingSince.isValid())
        m_pendingSince.start();
}

void DesktopBackdrop::noteFrameShown(qint64 costUs)
{
    if (!m_pendingSince.isValid())
        return;
    const qint64 latency = m_pendingSince.nsecsElapsed() / 1000;
    m_pendingSince.invalidate();
    ++m_stats.updates;
    m_stats.costUs += costUs;
    m_stats.maxCostUs = qMax(m_stats.maxCostUs, costUs);
    m_stats.latencyUs += latency;
    m_stats.maxLatencyUs = qMax(m_stats.maxLatencyUs, latency);
}

void DesktopBackdrop::renderNow()
{
    QElapsedTimer cost;
    cost.start();
    recompute();
    m_frame = buildFrame();
    if (m_frame.isNull())
        return;
    if (m_surface)
        m_surface->setFrame(m_frame);
    m_dirty = QRegion();
    noteFrameShown(cost.nsecsElapsed() / 1000);
    emit rendered(m_free.size());
    emit frameChanged(area());
}

QString DesktopBackdrop::statusText() const
{
    if (!m_enabled)
        return tr("desktop backdrop off");
    if (!isAttached())
        return tr("desktop backdrop: %1 icon(s), not hosted").arg(m_free.size());
    return tr("desktop backdrop: %1 icon(s) on the desktop, %2 held by fences%3")
        .arg(m_free.size())
        .arg(m_occupied.size())
        .arg(m_drawFreeIcons ? QString()
                             : tr(" (drawn by the desktop, not by us)"));
}

} // namespace starfish::ui
