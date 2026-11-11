#pragma once

// Session 111: paint-only skins for a Bouton, in the zone_shape architecture.
// Session 112: the first real skin is an animated GIF, scrubbed by the host's
// openProgress (no skin-owned clock). Frame index is
// round(openProgress * (frames-1)): expand goes forward, collapse backward,
// and an interrupted mid-way progress freezes the current frame.

#include <memory>

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>

namespace starfish::ui::zone_shape {

inline constexpr char kButtonSkinPlain[] = "plain";
inline constexpr char kButtonSkinShutter[] = "shutter";
inline constexpr char kButtonSkinGif[] = "gif";
inline constexpr char kButtonSkinShutterResource[] = ":/skins/shutter-demo.gif";
inline constexpr char kButtonSkinLayerBehind[] = "behind";
inline constexpr char kButtonSkinLayerFront[] = "front";

inline int buttonSkinFrameIndex(qreal openProgress, int frameCount)
{
    if (frameCount <= 1)
        return 0;
    const qreal u = qBound(qreal(0), openProgress, qreal(1));
    return int(qRound(u * qreal(frameCount - 1)));
}

inline bool buttonSkinLayerIsFront(const QString& layer)
{
    return layer == QLatin1String(kButtonSkinLayerFront);
}

struct ButtonSkinPaint {
    QRectF disc;              // inscribed circle of the widget (default skin insets 1 px)
    qreal openProgress = 0;   // 0 = collapsed, 1 = fully expanded, clamped
    bool hovered = false;
    bool rolledUp = false;
    bool moveMode = false;
    QColor fill;
    QColor border;
    int borderWidth = 1;
    QColor accent;
    qreal expandedSide = 0;   // logical px of the fully open disc (session 112)
    qreal dpr = 1;
};

// Optional image layers a skin may load (empty for the default disc).
// A GIF skin uses `behind` under the icons and `front` over them; each path
// may be an animated GIF, a still image, or a Qt resource.
struct ButtonSkinAssets {
    QString behind;
    QString front;
};

class ButtonSkin {
public:
    virtual ~ButtonSkin() = default;
    virtual QString id() const = 0;
    virtual void setAssets(const ButtonSkinAssets& assets) { Q_UNUSED(assets); }
    virtual int frameCount() const { return 0; }
    virtual void paintBehind(QPainter& painter, const ButtonSkinPaint& ctx) const = 0;
    virtual void paintFront(QPainter& painter, const ButtonSkinPaint& ctx) const = 0;
};

class PlainButtonSkin final : public ButtonSkin {
public:
    QString id() const override { return QStringLiteral("plain"); }

    void paintBehind(QPainter& painter, const ButtonSkinPaint& ctx) const override
    {
        painter.setPen(QPen(ctx.border, qMax(1, ctx.borderWidth)));
        painter.setBrush(ctx.fill);
        painter.drawEllipse(ctx.disc.adjusted(1, 1, -1, -1));
    }

    void paintFront(QPainter& painter, const ButtonSkinPaint& ctx) const override
    {
        Q_UNUSED(painter);
        Q_UNUSED(ctx);
    }
};

inline const ButtonSkin& defaultButtonSkin()
{
    static const PlainButtonSkin skin;
    return skin;
}

// Resolves the file or resource to load. Empty means "paint as Simple"
// (missing custom GIF, or the plain disc).
QString resolvedButtonSkinPath(const QString& id, const QString& path);
bool buttonSkinPathReadable(const QString& path);

// Unknown / empty / unreadable custom GIF -> the plain disc (silent fallback).
// "shutter" loads the built-in demo GIF; "gif" loads `path`.
std::unique_ptr<ButtonSkin> makeButtonSkin(const QString& id, const QString& path = {});

} // namespace starfish::ui::zone_shape
