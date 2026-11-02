#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>

namespace starfish::ui {

// Session 76/77: the startup splash. Session 77 (the user: "l'écran entier,
// pour cacher le moment du dessin des zones") -- one pane per monitor, each
// covering the whole monitor, so the fences drawing themselves one by one are
// never seen; it fades once everything painted.
//
// - Never takes focus (tool windows, shown without activation).
// - At least kMinimumMs; "everything painted" = every watched widget painted
//   once AND the desktop surfaces (drawn desktop, composed fences) stopped
//   pushing frames for kSettleMs; whatever happens, gone at kMaximumMs.
// - Topmost only for that time; input-transparent from the fade on, deleted
//   after it. Same process: a crash takes it with it.
class SplashScreen : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMinimumMs = 600;
    static constexpr int kMaximumMs = 6000;
    static constexpr int kSettleMs = 350;
    static constexpr int kFadeMs = 220;

    explicit SplashScreen(QObject* parent = nullptr);
    ~SplashScreen() override;

    // One full-monitor pane per screen where the desktop is in sight (not
    // under a maximised / full-screen window), painted AND composited (DWM)
    // before it returns. False when no screen needed one.
    bool showAll();
    // Starts watching: closes (and deletes itself) once all is drawn.
    void finishWhenPainted(const QList<QWidget*>& fences);
    QList<QWidget*> panes() const;
    bool finishing() const { return m_finishing; }

    // The scene, drawn: the wallpaper blurred and dimmed (or a dark sea when
    // there is none), the coral starfish in the middle, the name under it.
    static void paintScene(class QPainter& painter, const QRect& rect, qreal phase, const QImage& background,
                           const QString& subtitle);
    // The wallpaper, cover-fitted to `size`, blurred and dimmed (null if none).
    // `cacheDir` (session 78): a small blurred copy is kept there, keyed by the
    // wallpaper's path, size and date, so the next start shows it at once.
    static QImage blurredWallpaper(const QSize& size, const QString& cacheDir = QString());
    // The square the animated part lives in (only it repaints).
    static QRect animatedRect(const QRect& rect);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void tick();
    void startFade();
    QElapsedTimer m_clock;
    QElapsedTimer m_settled; // since the desktop surfaces' last frame
    quint64 m_lastFrames = 0;
    QTimer m_tick;
    QSet<QObject*> m_waiting;
    QList<QPointer<QWidget>> m_panes;
    bool m_watching = false;
    bool m_finishing = false;
};

} // namespace starfish::ui
