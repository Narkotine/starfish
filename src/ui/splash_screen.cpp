#include "ui/splash_screen.hpp"

#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QScreen>
#include <QStandardPaths>
#include <QVariantAnimation>
#include <QWindow>
#include <QtMath>

#include "platform/win/desktop_surface.hpp"
#include "platform/win/startup_timeline.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <dwmapi.h>
#include <windows.h>

namespace starfish::ui {

namespace {

// A five-armed starfish: rounded arm tips, a soft inner curve between arms.
QPainterPath starfishPath(const QPointF& centre, qreal outer, qreal inner, qreal turn)
{
    QPainterPath path;
    const int arms = 5;
    for (int i = 0; i < arms; ++i) {
        const qreal a = turn + i * 2.0 * M_PI / arms - M_PI / 2.0;
        const qreal b = a + M_PI / arms;
        const QPointF tip = centre + QPointF(qCos(a), qSin(a)) * outer;
        const QPointF valley = centre + QPointF(qCos(b), qSin(b)) * inner;
        const QPointF tipL = centre + QPointF(qCos(a - 0.16), qSin(a - 0.16)) * (outer * 0.93);
        const QPointF tipR = centre + QPointF(qCos(a + 0.16), qSin(a + 0.16)) * (outer * 0.93);
        if (i == 0)
            path.moveTo(tipL);
        else
            path.lineTo(tipL);
        path.quadTo(tip + (tip - centre) * 0.06, tipR);
        const QPointF ctrl = centre + QPointF(qCos(b), qSin(b)) * (inner * 0.82);
        const QPointF next = centre + QPointF(qCos(a + 2.0 * M_PI / arms - 0.16), qSin(a + 2.0 * M_PI / arms - 0.16))
                                          * (outer * 0.93);
        path.quadTo(ctrl, (valley + next) / 2.0);
    }
    path.closeSubpath();
    return path;
}

QString wallpaperPath()
{
    wchar_t buffer[MAX_PATH] = {};
    if (::SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buffer, 0) && buffer[0]) {
        const QString path = QString::fromWCharArray(buffer);
        if (QFileInfo::exists(path))
            return path;
    }
    // Slideshows and some themes leave the setting empty: Windows' own copy.
    const QString transcoded = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                               + QStringLiteral("/Microsoft/Windows/Themes/TranscodedWallpaper");
    return QFileInfo::exists(transcoded) ? transcoded : QString();
}

// One full-monitor pane: the blurred wallpaper, the starfish, nothing else.
class SplashPane : public QWidget
{
public:
    explicit SplashPane(QScreen* screen)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                               | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint)
    {
        setObjectName(QStringLiteral("starfishSplashPane"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFocusPolicy(Qt::NoFocus);
        setScreen(screen);
        setGeometry(screen->geometry());
        m_background = SplashScreen::blurredWallpaper(
            screen->geometry().size() * screen->devicePixelRatio(),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/StarFish/splash"));
        if (!m_background.isNull())
            m_background.setDevicePixelRatio(screen->devicePixelRatio());
    }
    qreal phase = 0.0;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        SplashScreen::paintScene(painter, rect(), phase, m_background,
                                 QCoreApplication::translate("SplashScreen", "Setting out your fences..."));
    }

private:
    QImage m_background;
};

} // namespace

SplashScreen::SplashScreen(QObject* parent)
    : QObject(parent)
{
    setObjectName(QStringLiteral("starfishSplash"));
    m_clock.start();
    m_tick.setInterval(33);
    connect(&m_tick, &QTimer::timeout, this, &SplashScreen::tick);
}

SplashScreen::~SplashScreen()
{
    for (QObject* watched : std::as_const(m_waiting))
        watched->removeEventFilter(this);
    for (const QPointer<QWidget>& pane : std::as_const(m_panes))
        delete pane.data();
}

namespace {

// The desktop is out of sight on a monitor a maximised or full-screen window
// (not the shell's own) fills: no cover there -- never over someone's game.
bool desktopHiddenOn(const QRect& monitorPx)
{
    HWND foreground = ::GetForegroundWindow();
    if (!foreground || !::IsWindowVisible(foreground))
        return false;
    wchar_t klass[64] = {};
    ::GetClassNameW(foreground, klass, 64);
    const QString name = QString::fromWCharArray(klass);
    if (name == QLatin1String("Progman") || name == QLatin1String("WorkerW") || name == QLatin1String("Shell_TrayWnd")
        || name == QLatin1String("Shell_SecondaryTrayWnd"))
        return false;
    RECT r{};
    if (!::GetWindowRect(foreground, &r))
        return false;
    const QRect window(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
    if (!window.intersects(monitorPx))
        return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    ::GetMonitorInfoW(::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &info);
    const QRect work(QPoint(info.rcWork.left, info.rcWork.top), QPoint(info.rcWork.right - 1, info.rcWork.bottom - 1));
    const QRect monitor(QPoint(info.rcMonitor.left, info.rcMonitor.top),
                        QPoint(info.rcMonitor.right - 1, info.rcMonitor.bottom - 1));
    if (monitor != monitorPx)
        return false;
    return ::IsZoomed(foreground) || window.contains(work);
}

// The monitor a (created, not yet shown) pane sits on, in physical pixels.
QRect monitorPixelsOf(QWidget* pane)
{
    HMONITOR monitor = ::MonitorFromWindow(reinterpret_cast<HWND>(pane->winId()), MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info))
        return {};
    return QRect(QPoint(info.rcMonitor.left, info.rcMonitor.top),
                 QPoint(info.rcMonitor.right - 1, info.rcMonitor.bottom - 1));
}

} // namespace

bool SplashScreen::showAll()
{
    platform::win::StartupTimeline::mark("splash: building panes");
    for (QScreen* screen : QGuiApplication::screens()) {
        auto* pane = new SplashPane(screen);
        if (desktopHiddenOn(monitorPixelsOf(pane))) {
            delete pane;
            continue;
        }
        m_panes << pane;
    }
    if (m_panes.isEmpty()) {
        platform::win::StartupTimeline::mark("splash: skipped (desktop out of sight)");
        return false;
    }
    for (const QPointer<QWidget>& pane : std::as_const(m_panes)) {
        pane->show();
        pane->repaint(); // painted and flushed to the window now
    }
    // On screen for real: DWM composes the next frame with the panes in it.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    ::DwmFlush();
    ::DwmFlush();
    platform::win::StartupTimeline::mark("splash composited (on screen)");
    m_tick.start();
    return true;
}

QList<QWidget*> SplashScreen::panes() const
{
    QList<QWidget*> list;
    for (const QPointer<QWidget>& pane : m_panes)
        if (pane)
            list << pane.data();
    return list;
}

void SplashScreen::finishWhenPainted(const QList<QWidget*>& fences)
{
    for (QWidget* fence : fences) {
        if (fence && fence->isVisible()) {
            m_waiting.insert(fence);
            fence->installEventFilter(this);
            connect(fence, &QObject::destroyed, this, [this, fence] { m_waiting.remove(fence); });
        }
    }
    m_lastFrames = platform::win::DesktopSurface::framesPushed();
    m_settled.start();
    m_watching = true;
    if (!m_tick.isActive())
        m_tick.start();
    tick();
}

bool SplashScreen::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Paint && m_waiting.remove(watched))
        watched->removeEventFilter(this);
    return QObject::eventFilter(watched, event);
}

void SplashScreen::tick()
{
    const qreal phase = m_clock.elapsed() / 1000.0;
    for (const QPointer<QWidget>& pane : std::as_const(m_panes)) {
        if (auto* p = static_cast<SplashPane*>(pane.data())) {
            p->phase = phase;
            p->update(animatedRect(p->rect()));
        }
    }
    if (m_finishing)
        return;
    const qint64 elapsed = m_clock.elapsed();
    if (elapsed >= kMaximumMs) { // the safety net, whatever is still drawing
        startFade();
        return;
    }
    if (!m_watching)
        return;
    const quint64 frames = platform::win::DesktopSurface::framesPushed();
    if (frames != m_lastFrames) {
        m_lastFrames = frames;
        m_settled.restart();
    }
    if (elapsed >= kMinimumMs && m_waiting.isEmpty() && m_settled.elapsed() >= kSettleMs)
        startFade();
}

void SplashScreen::startFade()
{
    m_finishing = true;
    for (QObject* watched : std::as_const(m_waiting))
        watched->removeEventFilter(this);
    m_waiting.clear();
    const QList<QWidget*> list = panes();
    for (QWidget* pane : list) {
        // From now on a click goes through to what is underneath.
        pane->setAttribute(Qt::WA_TransparentForMouseEvents);
        if (QWindow* window = pane->windowHandle())
            window->setFlag(Qt::WindowTransparentForInput, true);
    }
    auto* fade = new QVariantAnimation(this);
    fade->setDuration(list.isEmpty() ? 0 : kFadeMs);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        for (QWidget* pane : panes())
            pane->setWindowOpacity(value.toReal());
    });
    connect(fade, &QVariantAnimation::finished, this, [this] {
        m_tick.stop();
        for (QWidget* pane : panes())
            pane->hide();
        deleteLater(); // the panes with it
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

QImage SplashScreen::blurredWallpaper(const QSize& size, const QString& cacheDir)
{
    if (size.isEmpty())
        return {};
    const QString path = wallpaperPath();
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    QString cached;
    if (!cacheDir.isEmpty()) {
        const QByteArray key = QStringLiteral("%1|%2|%3|%4x%5")
                                   .arg(path).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch())
                                   .arg(size.width()).arg(size.height()).toUtf8();
        cached = cacheDir + QStringLiteral("/%1x%2-").arg(size.width()).arg(size.height())
                 + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16))
                 + QStringLiteral(".png");
        const QImage small(cached);
        if (!small.isNull()) {
            QImage full = small.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB32);
            return full;
        }
    }
    QImage source(path);
    if (source.isNull())
        return {};
    // Cover-fit, then a cheap wide blur: down to a few dozen pixels and back.
    const QImage cover = source.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QImage fitted = cover.copy((cover.width() - size.width()) / 2, (cover.height() - size.height()) / 2,
                                     size.width(), size.height());
    const QSize tiny(qMax(8, size.width() / 40), qMax(6, size.height() / 40));
    QImage small = fitted.scaled(tiny, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .scaled(tiny * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .scaled(tiny * 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .scaled(tiny * 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_RGB32);
    {
        QPainter dim(&small);
        dim.fillRect(small.rect(), QColor(0x06, 0x12, 0x22, 150));
    }
    if (!cached.isEmpty() && QDir().mkpath(cacheDir)) {
        // One copy per wallpaper and size: the old ones go.
        const QString sameSize = QStringLiteral("%1x%2-*.png").arg(size.width()).arg(size.height());
        for (const QFileInfo& old : QDir(cacheDir).entryInfoList({sameSize}, QDir::Files))
            QFile::remove(old.absoluteFilePath());
        small.save(cached);
    }
    return small.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
}

QRect SplashScreen::animatedRect(const QRect& rect)
{
    const int side = 360;
    return QRect(rect.center().x() - side / 2, rect.center().y() - side / 2 - 40, side, side + 80);
}

void SplashScreen::paintScene(QPainter& painter, const QRect& rect, qreal phase, const QImage& background,
                              const QString& subtitle)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!background.isNull()) {
        painter.drawImage(rect, background);
    } else {
        QRadialGradient sea(rect.center(), qMax(rect.width(), rect.height()) * 0.7);
        sea.setColorAt(0.0, QColor(0x0E, 0x3A, 0x52));
        sea.setColorAt(1.0, QColor(0x06, 0x12, 0x22));
        painter.fillRect(rect, sea);
    }

    // A soft glow behind the starfish.
    const QPointF centre(rect.center().x(), rect.center().y() - 40);
    QRadialGradient glow(centre, 150);
    glow.setColorAt(0.0, QColor(0xFF, 0x9A, 0x62, 70));
    glow.setColorAt(1.0, QColor(0xFF, 0x9A, 0x62, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(centre, 150, 150);

    const qreal turn = phase * 0.5;
    const QPainterPath star = starfishPath(centre, 78, 33, turn);
    QRadialGradient coral(centre, 82);
    coral.setColorAt(0.0, QColor(0xFF, 0xB3, 0x6B));
    coral.setColorAt(0.55, QColor(0xFF, 0x7A, 0x59));
    coral.setColorAt(1.0, QColor(0xD9, 0x40, 0x7A));
    painter.setBrush(coral);
    painter.drawPath(star);
    painter.setBrush(QColor(255, 240, 220, 150));
    for (int i = 0; i < 5; ++i) {
        const qreal a = turn + i * 2.0 * M_PI / 5 - M_PI / 2.0;
        for (const qreal r : {20.0, 38.0, 55.0})
            painter.drawEllipse(centre + QPointF(qCos(a), qSin(a)) * r, 3.2, 3.2);
    }

    QFont title = painter.font();
    title.setPixelSize(40);
    title.setBold(true);
    painter.setFont(title);
    painter.setPen(QColor(0xF5, 0xF7, 0xFA));
    const QRectF text(rect.center().x() - 200, centre.y() + 104, 400, 50);
    painter.drawText(text, Qt::AlignCenter, QStringLiteral("StarFish"));
    QFont small = painter.font();
    small.setPixelSize(14);
    small.setBold(false);
    painter.setFont(small);
    painter.setPen(QColor(0xC8, 0xD4, 0xE3));
    painter.drawText(text.translated(0, 42), Qt::AlignCenter, subtitle);

    const QRectF track(rect.center().x() - 110, text.bottom() + 58, 220, 3);
    painter.setBrush(QColor(255, 255, 255, 40));
    painter.drawRoundedRect(track, 1.5, 1.5);
    const qreal t = 0.5 - 0.5 * qCos(phase * 3.2);
    const qreal w = track.width() * 0.3;
    painter.setBrush(QColor(0xFF, 0x9A, 0x62));
    painter.drawRoundedRect(QRectF(track.left() + t * (track.width() - w), track.top(), w, track.height()), 1.5, 1.5);
    painter.restore();
}

} // namespace starfish::ui
