#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>

namespace starfish::ui {

// Session 76: a short card while the fences draw themselves at startup -- the
// user saw them pop in one by one. Never takes focus (no activation, a tool
// window), stays at least kMinimumMs, closes once every watched fence has
// painted (or after kMaximumMs whatever happens), with a short fade.
class SplashScreen : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kMinimumMs = 600;
    static constexpr int kMaximumMs = 4000;

    explicit SplashScreen(QWidget* parent = nullptr);

    // Closes (and deletes itself) once every one of `fences` painted once.
    void finishWhenPainted(const QList<QWidget*>& fences);
    bool finishing() const { return m_finishing; }

    // The identity, drawn: also used by the probe for a capture.
    static void paintCard(class QPainter& painter, const QRect& rect, qreal phase, const QString& subtitle);

protected:
    void paintEvent(class QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void tryFinish();
    QElapsedTimer m_clock;
    QTimer m_tick;
    QSet<QObject*> m_waiting;
    bool m_watching = false;
    bool m_finishing = false;
    qreal m_phase = 0.0;
};

} // namespace starfish::ui
