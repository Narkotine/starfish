#include "ui/splash_screen.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QScreen>
#include <QtMath>

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

} // namespace

SplashScreen::SplashScreen(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("starfishSplash"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::NoFocus);
    resize(340, 150);
    if (QScreen* screen = QGuiApplication::primaryScreen())
        move(screen->availableGeometry().center() - rect().center());
    m_clock.start();
    m_tick.setInterval(33);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        m_phase = m_clock.elapsed() / 1000.0;
        update();
        tryFinish();
    });
    m_tick.start();
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
    m_watching = true;
    tryFinish();
}

bool SplashScreen::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Paint && m_waiting.remove(watched))
        watched->removeEventFilter(this);
    return QWidget::eventFilter(watched, event);
}

void SplashScreen::tryFinish()
{
    if (m_finishing || !m_watching)
        return;
    const qint64 elapsed = m_clock.elapsed();
    if (elapsed < kMinimumMs || (!m_waiting.isEmpty() && elapsed < kMaximumMs))
        return;
    m_finishing = true;
    for (QObject* watched : std::as_const(m_waiting))
        watched->removeEventFilter(this);
    m_waiting.clear();
    auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(180);
    fade->setStartValue(windowOpacity());
    fade->setEndValue(0.0);
    connect(fade, &QPropertyAnimation::finished, this, &QWidget::close);
    fade->start();
}

void SplashScreen::paintCard(QPainter& painter, const QRect& rect, qreal phase, const QString& subtitle)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF card = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient sea(card.topLeft(), card.bottomRight());
    sea.setColorAt(0.0, QColor(0x0B, 0x1E, 0x3A, 0xF2));
    sea.setColorAt(1.0, QColor(0x07, 0x3B, 0x4C, 0xF2));
    painter.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    painter.setBrush(sea);
    painter.drawRoundedRect(card, 16, 16);

    // The starfish, slowly turning.
    const QPointF centre(card.left() + 72, card.center().y() - 4);
    const QPainterPath star = starfishPath(centre, 44, 19, phase * 0.6);
    QRadialGradient coral(centre, 46);
    coral.setColorAt(0.0, QColor(0xFF, 0xB3, 0x6B));
    coral.setColorAt(0.55, QColor(0xFF, 0x7A, 0x59));
    coral.setColorAt(1.0, QColor(0xD9, 0x40, 0x7A));
    painter.setPen(Qt::NoPen);
    painter.setBrush(coral);
    painter.drawPath(star);
    painter.setBrush(QColor(255, 240, 220, 150));
    for (int i = 0; i < 5; ++i) {
        const qreal a = phase * 0.6 + i * 2.0 * M_PI / 5 - M_PI / 2.0;
        for (const qreal r : {12.0, 22.0, 31.0})
            painter.drawEllipse(centre + QPointF(qCos(a), qSin(a)) * r, 2.0, 2.0);
    }

    QFont title = painter.font();
    title.setPixelSize(28);
    title.setBold(true);
    painter.setFont(title);
    painter.setPen(QColor(0xF5, 0xF7, 0xFA));
    const QRectF text(card.left() + 132, card.top() + 34, card.width() - 148, 40);
    painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("StarFish"));
    QFont small = painter.font();
    small.setPixelSize(12);
    small.setBold(false);
    painter.setFont(small);
    painter.setPen(QColor(0xB8, 0xC7, 0xD9));
    painter.drawText(text.translated(0, 34), Qt::AlignLeft | Qt::AlignVCenter, subtitle);

    // A thin wave, back and forth: something is happening.
    const QRectF track(text.left(), card.bottom() - 34, text.width() - 8, 3);
    painter.setBrush(QColor(255, 255, 255, 30));
    painter.drawRoundedRect(track, 1.5, 1.5);
    const qreal t = 0.5 - 0.5 * qCos(phase * 3.2);
    const qreal w = track.width() * 0.3;
    painter.setBrush(QColor(0xFF, 0x9A, 0x62));
    painter.drawRoundedRect(QRectF(track.left() + t * (track.width() - w), track.top(), w, track.height()), 1.5, 1.5);
    painter.restore();
}

void SplashScreen::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    paintCard(painter, rect(), m_phase, tr("Setting out your fences..."));
}

} // namespace starfish::ui
