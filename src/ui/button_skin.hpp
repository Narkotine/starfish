#pragma once

// Session 111: paint-only skins for a Bouton, in the zone_shape architecture.
// The host drives every frame with openProgress in [0, 1] (0 = collapsed,
// 1 = fully expanded) and the current hover state. A skin must not run its
// own clock: a later animated-GIF skin maps this progress to a frame
// (QMovie / QImageReader::jumpToFrame, or pre-decoded frames), so expand
// scrubs forward, collapse scrubs backward, and leaving mid-way freezes the
// current frame. Optional behind / in-front layers are painted around the
// icons; the default skin is the current plain disc (no visual change).

#include <memory>

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>

namespace starfish::ui::zone_shape {

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
};

// Optional image layers a skin may load (empty for the default disc).
// A later GIF skin uses `behind` under the icons and `front` over them;
// each path may be an animated GIF or a still image, with the frame chosen
// from openProgress.
struct ButtonSkinAssets {
    QString behind;
    QString front;
};

class ButtonSkin {
public:
    virtual ~ButtonSkin() = default;
    virtual QString id() const = 0;
    virtual void setAssets(const ButtonSkinAssets& assets) { Q_UNUSED(assets); }
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

// Unknown or empty id -> the plain disc. A later session can return a GIF
// skin here without changing the paint call sites.
inline std::unique_ptr<ButtonSkin> makeButtonSkin(const QString& id)
{
    Q_UNUSED(id);
    return std::make_unique<PlainButtonSkin>();
}

} // namespace starfish::ui::zone_shape
