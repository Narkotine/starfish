#pragma once

// Session 116: interpolate two unpremultiplied QColors in OKLab, with alpha
// lerped linearly and separately. Endpoints (t <= 0 / t >= 1) are returned
// exactly. A zero-alpha colour still contributes its RGB through OKLab so a
// fade does not pick a muddy sRGB midpoint.
//
// Coefficients: Björn Ottosson, "A perceptual color space for image
// processing" (https://bottosson.github.io/posts/oklab/).

#include <QtGui/QColor>
#include <QtMath>
#include <cmath>

namespace starfish {
namespace core {

namespace color_mix_detail {

inline qreal toLinear(qreal channel)
{
    return (channel >= 0.04045) ? std::pow((channel + 0.055) / 1.055, 2.4)
                                : (channel / 12.92);
}

inline qreal toSrgb(qreal channel)
{
    if (channel <= 0.0)
        return 0.0;
    if (channel >= 1.0)
        return 1.0;
    return (channel >= 0.0031308) ? (1.055 * std::pow(channel, 1.0 / 2.4) - 0.055)
                                  : (12.92 * channel);
}

struct Oklab {
    qreal L = 0;
    qreal a = 0;
    qreal b = 0;
};

inline Oklab srgbToOklab(qreal r, qreal g, qreal b)
{
    const qreal lr = toLinear(r);
    const qreal lg = toLinear(g);
    const qreal lb = toLinear(b);
    const qreal l = std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
    const qreal m = std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
    const qreal s = std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);
    Oklab lab;
    lab.L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    lab.a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    lab.b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return lab;
}

inline void oklabToSrgb(const Oklab& lab, qreal& r, qreal& g, qreal& b)
{
    const qreal l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const qreal m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const qreal s_ = lab.L - 0.0897333028 * lab.a - 1.2914855480 * lab.b;
    const qreal l = l_ * l_ * l_;
    const qreal m = m_ * m_ * m_;
    const qreal s = s_ * s_ * s_;
    r = toSrgb(+4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    g = toSrgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    b = toSrgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

inline int toByte(qreal channel)
{
    const qreal clamped = qBound(0.0, channel, 1.0);
    return qRound(clamped * 255.0);
}

} // namespace color_mix_detail

// Mix `from` toward `to` by t in [0, 1]. `from` and `to` are unpremultiplied
// QColors (straight alpha). RGB travels in OKLab; alpha is lerped in a
// straight line and applied after, so a transparent endpoint does not collapse
// the midpoint toward black.
inline QColor mixOklab(const QColor& from, const QColor& to, qreal t)
{
    if (t <= 0.0)
        return from;
    if (t >= 1.0)
        return to;
    if (from == to)
        return from;

    const QColor src = from.toRgb();
    const QColor dst = to.toRgb();
    const auto a = color_mix_detail::srgbToOklab(src.redF(), src.greenF(), src.blueF());
    const auto b = color_mix_detail::srgbToOklab(dst.redF(), dst.greenF(), dst.blueF());
    color_mix_detail::Oklab mid;
    mid.L = a.L + (b.L - a.L) * t;
    mid.a = a.a + (b.a - a.a) * t;
    mid.b = a.b + (b.b - a.b) * t;

    qreal r = 0, g = 0, bl = 0;
    color_mix_detail::oklabToSrgb(mid, r, g, bl);
    const qreal alpha = src.alphaF() + (dst.alphaF() - src.alphaF()) * t;
    return QColor::fromRgb(color_mix_detail::toByte(r),
                           color_mix_detail::toByte(g),
                           color_mix_detail::toByte(bl),
                           color_mix_detail::toByte(alpha));
}

} // namespace core
} // namespace starfish
