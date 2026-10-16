#include "ui/desktop_backdrop.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
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
const QColor kRubberBandFill(0x3B, 0x82, 0xF6, 0x24);

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
    // NOT fully transparent: see DesktopBackdrop::kInputAlphaFloor. The one alpha
    // level is what makes the frame a rectangle the cursor can find, and without
    // it the empty desktop's gestures are unreachable through any mouse (T-060).
    frame.fill(QColor(0, 0, 0, kInputAlphaFloor));
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // ADR-13: nothing is painted on the fence-less desktop by default -- the
    // native icons are there, they are the ones the user arranged, and drawing a
    // second copy of them would be exactly the duplication this decision is
    // about. What is left on screen is the rubber band, and the one alpha level
    // that keeps the frame findable.
    if (m_drawFreeIcons)
        paintCells(painter, box.topLeft(), m_cells, m_iconProvider, physicalMetrics(), scale());

    // The fence being drawn (T-041). The rubber band carries virtual-screen
    // pixels like the cells do, so the painter is translated once -- and only
    // here, since paintCells() restored its own state.
    if (!m_rubberBand.isEmpty()) {
        painter.save();
        painter.translate(-box.topLeft());
        QPen pen(kRubberBandPen, qMax(1.0, scale()));
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(kRubberBandFill);
        painter.drawRect(m_rubberBand);
        painter.restore();
    }

    return frame;
}

void DesktopBackdrop::setNativeIconRects(const QVector<QRect>& rects)
{
    if (m_nativeRects == rects)
        return;
    m_nativeRects = rects;
    if (m_surface)
        m_surface->setNativeIconRects(rects);
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
    m_drawFreeIcons = draw;
    refresh();
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
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        return handleMouse(static_cast<QMouseEvent*>(event));
    default:
        return QObject::event(event);
    }
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
    // in columns.
    const bool onIcon = m_drawFreeIcons ? BackdropGesture::iconAt(m_cells, screen) >= 0
                                        : overNativeIcon(screen);

    BackdropGesture::Result result;
    switch (mouse->type()) {
    case QEvent::MouseButtonPress:
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
        m_rubberBand = m_gesture.rubberBand();
        refresh();
        return true;

    case BackdropGesture::Action::DesktopMenu:
        m_rubberBand = {};
        refresh();
        // The backdrop says what happened; the product decides what to make of
        // it, exactly like every other gesture in this project.
        emit desktopMenuRequested(result.rect, result.point);
        return true;

    case BackdropGesture::Action::ToggleVisibility:
        m_rubberBand = {};
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
    surface->setNativeIconRects(m_nativeRects);
    surface->setFrame(m_frame);
    if (!surface->setPerPixelAlpha(true)) {
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
    if (!surface->perPixelAlpha()) {
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

void DesktopBackdrop::renderNow()
{
    recompute();
    m_frame = buildFrame();
    if (m_frame.isNull())
        return;
    if (m_surface)
        m_surface->setFrame(m_frame);
    emit rendered(m_free.size());
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
