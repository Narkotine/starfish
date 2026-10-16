#include "ui/desktop_backdrop.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
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

const QColor kLabelText(0xF2, 0xF4, 0xF8);
const QColor kLabelShadow(0x00, 0x00, 0x00, 0xB4);
constexpr QRgb kIconPlaceholder = 0x37;

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
    frame.fill(Qt::transparent);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintCells(painter, box.topLeft(), m_cells, m_iconProvider, physicalMetrics(), scale());
    return frame;
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
        detach();
        m_renderTimer.stop();
        m_frame = QImage();
    }
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
    surface->setClickThrough(true);
    surface->setDevicePixelRatio(1.0); // the frame is already in physical pixels
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
    m_cells = BackdropLayout::compute(area(), physicalMetrics(), m_free);
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
    return tr("desktop backdrop: %1 icon(s) on the desktop, %2 held by fences")
        .arg(m_free.size())
        .arg(m_occupied.size());
}

} // namespace starfish::ui
