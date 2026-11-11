#pragma once

// Session 113: a classic 12-blade camera diaphragm, drawn procedurally so
// it stays crisp at any disc size / DPR. Opening is a regular dodecagon
// whose apothem follows openProgress (no skin-owned clock). Perfectly
// 12-fold symmetric by construction. Shared by the built-in "Diaphragm"
// skin and by tools/make_shutter_gif.py (same constants).

#include "ui/button_skin.hpp"

#include <QBrush>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRadialGradient>
#include <QtMath>

namespace starfish::ui::zone_shape {

inline constexpr int kIrisBlades = 12;
inline constexpr qreal kIrisApertureMin = 0.03; // apothem as a fraction of the disc radius
inline constexpr qreal kIrisApertureMax = 0.86;

inline qreal irisApertureApothem(qreal radius, qreal progress)
{
    const qreal u = qBound(qreal(0), progress, qreal(1));
    return radius * (kIrisApertureMin + (kIrisApertureMax - kIrisApertureMin) * u);
}

// Paint an opaque shutter (black, slight radial shade, white blade edges)
// with a transparent regular n-gon aperture punched out of the current
// destination. Intended for paintFront: icons already in the painter show
// through the hole.
inline void paintIrisDiaphragm(QPainter& painter, const QRectF& disc, qreal progress, qreal rotation = 0)
{
    if (disc.isEmpty())
        return;
    const QPointF c = disc.center();
    const qreal side = qMin(disc.width(), disc.height());
    const qreal margin = 1.0;
    const qreal R = side / 2.0 - margin;
    if (R <= 1.0)
        return;
    const qreal ap = irisApertureApothem(R, progress);
    const qreal rot = rotation;
    const qreal tau = qreal(6.28318530717958647692);
    const qreal pi = qreal(3.14159265358979323846);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addEllipse(QRectF(c.x() - R, c.y() - R, 2.0 * R, 2.0 * R));
    painter.setClipPath(clip, Qt::IntersectClip);

    // Fill only the blades (disc minus aperture). Do not DestinationOut:
    // that would erase the icons already painted under this front layer.
    const qreal vertexR = ap / qCos(pi / qreal(kIrisBlades));
    QPolygonF poly;
    poly.reserve(kIrisBlades);
    for (int i = 0; i < kIrisBlades; ++i) {
        const qreal ang = rot + (i + 0.5) * (tau / qreal(kIrisBlades));
        poly << QPointF(c.x() + vertexR * qCos(ang), c.y() + vertexR * qSin(ang));
    }
    QPainterPath discPath;
    discPath.addEllipse(QRectF(c.x() - R, c.y() - R, 2.0 * R, 2.0 * R));
    QPainterPath aperture;
    aperture.addPolygon(poly);
    const QPainterPath blades = discPath.subtracted(aperture);

    QRadialGradient shade(c, R);
    shade.setColorAt(0.0, QColor(22, 22, 22, 255));
    shade.setColorAt(1.0, QColor(10, 10, 10, 255));
    painter.setPen(Qt::NoPen);
    painter.setBrush(shade);
    painter.drawPath(blades);

    // One visible edge per blade: a cubic from the rim, inward in a
    // pinwheel, ending at the next side's tangent point and arriving
    // along that side (tangent to the aperture). Lines do not cross the
    // opening or each other. Closed: a tight swirl to a pinpoint. Open:
    // short edge stubs near the rim around a large 12-gon. (Full chords
    // looked like a star web; straight radials, like spokes when closed.)
    const qreal penW = qMax(qreal(1), qreal(0.0046) * side);
    painter.setPen(QPen(QColor(255, 255, 255, 235), penW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(poly);
    for (int i = 0; i < kIrisBlades; ++i) {
        const qreal ang = rot + i * (tau / qreal(kIrisBlades));
        const qreal nx = qCos(ang);
        const qreal ny = qSin(ang);
        const qreal ang2 = ang + tau / qreal(kIrisBlades);
        const qreal n2x = qCos(ang2);
        const qreal n2y = qSin(ang2);
        const QPointF p0(c.x() + R * nx, c.y() + R * ny);
        const QPointF p3(c.x() + ap * n2x, c.y() + ap * n2y);
        const QPointF chord = p3 - p0;
        qreal tx = -n2y;
        qreal ty = n2x;
        if (tx * chord.x() + ty * chord.y() > 0) {
            tx = -tx;
            ty = -ty;
        }
        const qreal L = qSqrt(chord.x() * chord.x() + chord.y() * chord.y());
        const QPointF p2(p3.x() + tx * L * 0.35, p3.y() + ty * L * 0.35);
        const QPointF p1(p0.x() + (-nx * 0.25 + (-ny) * 0.35) * L,
                         p0.y() + (-ny * 0.25 + nx * 0.35) * L);
        QPainterPath edge;
        edge.moveTo(p0);
        edge.cubicTo(p1, p2, p3);
        painter.drawPath(edge);
    }
    painter.restore();
}

class DiaphragmButtonSkin final : public ButtonSkin {
public:
    QString id() const override { return QString::fromUtf8(kButtonSkinDiaphragm); }

    void setAssets(const ButtonSkinAssets& assets) override { m_assets = assets; }

    int frameCount() const override { return 0; } // continuous in openProgress

    void paintBehind(QPainter& painter, const ButtonSkinPaint& ctx) const override
    {
        PlainButtonSkin{}.paintBehind(painter, ctx);
        if (!buttonSkinLayerIsFront(m_assets.layer))
            paintIrisDiaphragm(painter, ctx.disc, ctx.openProgress);
    }

    void paintFront(QPainter& painter, const ButtonSkinPaint& ctx) const override
    {
        if (buttonSkinLayerIsFront(m_assets.layer))
            paintIrisDiaphragm(painter, ctx.disc, ctx.openProgress);
    }

private:
    ButtonSkinAssets m_assets;
};

} // namespace starfish::ui::zone_shape
