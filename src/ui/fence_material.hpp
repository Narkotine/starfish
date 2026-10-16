#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <array>

namespace starfish::ui {

// F-038 / T-057 -- the fence materials.
//
// A "material" is not a second kind of style: it is a *choice among presets* for
// one field that already existed. `FenceStyle::background` has been written as
// `#AARRGGBB` since T-008 and its alpha channel has been persisted, defaulted and
// re-read ever since -- and it was **inert**, because a fence took the layered
// path whose GDI blit ignores alpha (docs/technique.md section 22). What makes it
// live is the per-pixel path (`DesktopSurface::setPerPixelAlpha`, T-017), the
// same one the desktop backdrop has been using on a full-screen child of Progman
// since T-039.
//
// So this header owns exactly two things, and nothing else:
//
//   * the *names* of the presets and the alpha each one means, so the interface,
//     the container and the probes cannot disagree about what "translucent" is;
//   * the arithmetic on the `#AARRGGBB` string, kept pure and unit-tested, so the
//     document's spelling of a colour survives a preset change byte for byte.
//
// It does **not** own a colour, a window, or the schema: a document written
// before T-057 (`#CC1F2937`, the schema default) is already the "translucent"
// preset, which is why changing a material never needs a schema bump (ADR-7).
//
// Everything here is Qt Core only and free of widgets, so it is tested without a
// desktop, a window or a mouse -- the same split as fence_geometry and
// backdrop_layout.
namespace FenceMaterial {

// One preset of F-038. `id` is the stable name the interface and the probes use;
// it is **not** persisted (the alpha in the colour is), so renaming one is an
// interface change, not a migration.
struct Preset {
    const char* id;
    int alpha; // the panel's alpha channel, 0-255
};

// opaque            the panel as it looks today: the wallpaper does not show.
// translucent       the schema default (#CC...), kept as the default material.
// very-translucent  the wallpaper reads through the panel.
inline const std::array<Preset, 3>& presets()
{
    static const std::array<Preset, 3> all = {{
        {"opaque", 255},
        {"translucent", 204},
        {"very-translucent", 128},
    }};
    return all;
}

// The alpha a preset means, or -1 when the name is unknown. A caller that hands
// the result to withAlpha() must check for -1: an unknown name is not a material.
inline int alphaFor(const QString& id)
{
    for (const Preset& preset : presets()) {
        if (id == QLatin1String(preset.id))
            return preset.alpha;
    }
    return -1;
}

// The preset an alpha means, or an empty string when it is a value of its own
// (a document may hold any alpha; the interface says "custom" rather than lying).
inline QString presetFor(int alpha)
{
    for (const Preset& preset : presets()) {
        if (preset.alpha == alpha)
            return QString::fromLatin1(preset.id);
    }
    return QString();
}

// Anything below full opacity needs the per-pixel path: that is the whole of the
// decision (DesktopSurface::setPerPixelAlpha), and both the container and the
// probes ask this one function.
inline bool isTranslucent(int alpha)
{
    return alpha < 255;
}

inline QStringList presetIds()
{
    QStringList ids;
    for (const Preset& preset : presets())
        ids << QString::fromLatin1(preset.id);
    return ids;
}

// "opaque (255), translucent (204), very-translucent (128)" -- one place for the
// numbers a report or a journal line has to print.
inline QString summary()
{
    QStringList parts;
    for (const Preset& preset : presets()) {
        parts << QStringLiteral("%1 (%2)")
                     .arg(QString::fromLatin1(preset.id))
                     .arg(preset.alpha);
    }
    return parts.join(QStringLiteral(", "));
}

// --- the #AARRGGBB arithmetic ----------------------------------------------
//
// The schema writes `#AARRGGBB` (jsonstore.hpp), and QColor::name/HexArgb reads
// it back; the two forms the schema can hold are the 8-digit one and the plain
// `#RRGGBB` a hand-written document may use. Anything else (a named colour, an
// empty string) is left strictly alone: a preset that cannot be applied must not
// rewrite what the user wrote.

inline bool parseArgb(const QString& text, int* r, int* g, int* b, int* a)
{
    QString digits = text;
    if (digits.startsWith(QLatin1Char('#')))
        digits = digits.mid(1);
    if (digits.size() != 6 && digits.size() != 8)
        return false;
    for (const QChar c : digits) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
              || (c >= QLatin1Char('A') && c <= QLatin1Char('F'))))
            return false;
    }
    bool ok = false;
    const uint value = digits.toUInt(&ok, 16);
    if (!ok)
        return false;
    if (digits.size() == 6) {
        *a = 255;
        *r = static_cast<int>((value >> 16) & 0xff);
        *g = static_cast<int>((value >> 8) & 0xff);
        *b = static_cast<int>(value & 0xff);
    } else {
        *a = static_cast<int>((value >> 24) & 0xff);
        *r = static_cast<int>((value >> 16) & 0xff);
        *g = static_cast<int>((value >> 8) & 0xff);
        *b = static_cast<int>(value & 0xff);
    }
    return true;
}

// The panel's alpha as the document spells it. An unreadable colour is opaque:
// that is what a fence shows today, so an unparsable document behaves as before
// rather than turning a fence translucent by accident.
inline int alphaOf(const QString& background)
{
    int r = 0, g = 0, b = 0, a = 255;
    if (!parseArgb(background, &r, &g, &b, &a))
        return 255;
    return a;
}

// The same colour with another alpha, in `#AARRGGBB`. The RGB digits are copied
// **verbatim** (case included) so the rest of the string the user or an older
// document wrote is not silently rewritten; only the alpha is ours. Returns the
// input unchanged when it cannot be parsed or the alpha is out of range.
inline QString withAlpha(const QString& background, int alpha)
{
    if (alpha < 0 || alpha > 255)
        return background;
    int r = 0, g = 0, b = 0, a = 0;
    if (!parseArgb(background, &r, &g, &b, &a))
        return background;
    QString digits = background.startsWith(QLatin1Char('#')) ? background.mid(1) : background;
    const QString rgb = digits.right(6);
    const QString alphaHex =
        QString::number(alpha, 16).rightJustified(2, QLatin1Char('0')).toUpper();
    return QStringLiteral("#%1%2").arg(alphaHex, rgb);
}

// The preset a `#AARRGGBB` colour is shown in -- "translucent" for the schema
// default, an empty string for a custom alpha.
inline QString materialOf(const QString& background)
{
    return presetFor(alphaOf(background));
}

// The interface label of a preset. Goes through tr() (source strings in English,
// F-036 / i18n later) and lives here so the interface and the tray cannot label
// the same preset differently.
inline QString presetLabel(const QString& id)
{
    if (id == QLatin1String("opaque"))
        return QCoreApplication::translate("FenceMaterial", "Opaque");
    if (id == QLatin1String("translucent"))
        return QCoreApplication::translate("FenceMaterial", "Translucent");
    if (id == QLatin1String("very-translucent"))
        return QCoreApplication::translate("FenceMaterial", "Very translucent");
    return QCoreApplication::translate("FenceMaterial", "Custom");
}

} // namespace FenceMaterial

} // namespace starfish::ui
