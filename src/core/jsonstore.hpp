#pragma once

#include <QByteArray>
#include <QRect>
#include <QJsonObject>
#include <QSize>
#include <QList>
#include <QString>
#include <optional>

#include "core/rules.hpp"

namespace starfish::core {

// Visual style of a fence (docs/produit.md F-004/F-005).
struct FenceStyle {
    // #AARRGGBB, i.e. exactly what QColor::name(QColor::HexArgb) produces, so
    // the string round-trips through QColor unchanged.
    //
    // T-074: the default is the *opaque* preset. It used to be `#CC1F2937`
    // (alpha 204), which the material presets read back as `translucent`. A
    // material *is* this alpha and a document keeps the one it was saved with,
    // so moving the default changes what a **new** fence looks like, and nothing
    // at all for the fences already in someone's layout.json.
    QString background = "#FF1F2937";
    int radius = 10;
    int padding = 12;
    int iconSize = 48;
    int gap = 8;
    bool titleBar = true;
    QString iconTint; // empty = no tint (F-015)
    // T-082 (F-038): whether the material also fades the **icons**, or only the
    // fence's own chrome -- the panel and its title bar.
    //
    // Off by default, which is the user's request after trying it: « je ne
    // pensais pas que changer material s'appliquerait aussi aux icônes. Est-il
    // possible de ne l'appliquer qu'à la fence ? et du coup de garder l'option de
    // l'appliquer aux icônes en option, par fence ? » Until T-082 a material was
    // the window's uniform alpha (ADR-15), so it faded everything the fence drew.
    //
    // A key with its default and no migration: an absent key reads as `false`,
    // exactly like T-074's, so a document written before this key exists keeps
    // describing the fence it describes -- and a *new* fence gets the new
    // behaviour.
    bool fadeIcons = false;
    // T-080 (F-004): the title bar's **own** colour, in `#AARRGGBB` like the
    // panel's. Empty means what the fence shows today: there is no band at all,
    // the title is drawn straight on the panel.
    //
    // Empty is the default for the same reason `fadeIcons` is `false`: an absent
    // key already describes the fence a document was written with, so this needs
    // no migration and no schema bump -- and a *new* fence keeps the look it has
    // always had until its owner asks for a band.
    //
    // The band is painted *into the frame*, under the window's uniform alpha
    // (ADR-15/ADR-17): a fence in a translucent material fades its title bar with
    // its panel, whatever alpha is written here. This colour is therefore about
    // **which** colour the band is, never about how much shows through.
    QString titleBarColor;
    // T-098 (session 63): whether the fence's transparency is the general
    // default (Réglages -> Zones) rather than its own. Its own is the alpha of
    // `background`, as it has always been. Absent from a document written
    // before this key: a fence that was never made translucent (alpha 255)
    // follows the default, one whose material was chosen keeps it.
    bool defaultOpacity = true;
    // T-094 (session 65): the border. -1 / empty = the default (Settings).
    int borderWidth = -1;
    QString borderColor;
    // T-085 (session 65): the width of an icon's cell, 0 = automatic (T-078).
    int cellWidth = 0;
    // T-099 (session 65): "" none, "acrylic" (the desktop blurred behind a
    // translucent panel), "textured" (a fine grain on the panel).
    QString effect;
    // Session 66: the title in its bar: "" left, "center", "right".
    QString titleAlign;
    // Session 68: the side of the title bar: "" top, "bottom", "left", "right".
    QString titleSide;
    // Session 69 (T-115): "" a full-width bar, "tab" a tab as long as the title.
    QString titleBarShape;
    // Session 69: "" the default (Settings), "none", "lighter", "opacity",
    // "colour", "noborder", "glow".
    QString hoverEffect;
    // Session 71: unroll a rolled-up fence under the pointer: "" the default
    // (Settings), "on", "off".
    QString hoverUnroll;
    // Session 73: "" the default (Settings), "bar" (the strip stays on the title
    // bar's edge), "opposite" (on the other edge).
    QString rollToward;
    // Session 73: resize in whole icon cells: "" the default, "on", "off".
    QString snapToCells;
    QString iconFlow;   // session 76: "" (Settings) or an IconLayout::flowKeys() key
    QString showLabels; // session 76: "" (Settings) | "on" | "off" -- the icons' names
    // Session 101 (T-095): "" classic rectangle + title-bar roll-up, "button" =
    // circular Bouton. Unknown values are kept and drawn as classic until known.
    QString shape;
    // Session 111 (Bouton only): collapsed disc diameter in px, 0 = default
    // (~2x icon size + 8, same as before). Written only when set.
    int buttonDiameter = 0;
    // Session 111 (Bouton only): number of icon rings, 0 = automatic (fewest
    // compact layers). A fixed 1..8 distributes more icons on the outer rings.
    int buttonRings = 0;
    // Session 112: Bouton skin. "" / "plain" = Simple (the disc), "shutter" =
    // the built-in iris demo, "gif" = a file in buttonSkinPath. Unknown ids
    // are kept and painted as Simple until a newer build knows them.
    QString buttonSkin;
    QString buttonSkinPath;  // written only for "gif"
    // "" / "behind" = under the icons, "front" = over them (paint-only).
    QString buttonSkinLayer;
};

// An item displayed inside a fence: a desktop file or a folder portal.
struct FenceItem {
    QString kind;  // "file" | "portal" | "widget" (session 79)
    QString path;
    QString fileId; // session 74: identity kept by a rename (empty: unknown)
    QString target; // session 74: a shortcut's target, the fallback match
    // Session 79, kind "widget": {"type", "span": [w, h], "settings": {...}}.
    // An unknown type is kept as it was read (a newer StarFish wrote it).
    QString widgetType;
    QSize span{1, 1};
    QJsonObject widgetSettings;
};

struct Fence {
    QString id;
    QString title;
    QString monitor;      // device name, e.g. "\\\\.\\DISPLAY1"
    QRect rect;           // desktop coordinates
    FenceStyle style;
    QString sortBy = "name"; // name | type | size | date | manual (session 60: the user's order)
    bool sortAscending = true;
    bool rolledUp = false;   // F-020
    bool locked = false;     // session 94: written only when true (older builds keep it)
    QList<FenceItem> items;
};

// Session 91: a widget on the desktop itself, outside any fence. `item` is a
// kind "widget" FenceItem; `pos` is its top-left in the same space as a fence's
// rect (virtual-screen physical pixels); its size follows from the span and the
// desktop's icon grid. Stored under the root key "desktopWidgets" -- an older
// StarFish ignores it, and keeps it (unknown root key) when it writes.
struct DesktopWidgetEntry {
    FenceItem item;
    QPoint pos;
};

// Versioned layout document (docs/technique.md ADR-5 / section 5).
struct Layout {
    // Session 85: the document's format version. Bumped only for a change an
    // older StarFish could misread; additions with a default stay at the same
    // schema (see docs/mises-a-jour.md). JsonStore::kCurrentSchema is the one
    // this build writes.
    int schema = 1;
    QList<Fence> fences;
    // Automation rules (T-010, F-017). They belong to the document rather than
    // to a single fence: a rule names the fence it feeds.
    QList<Rule> rules;
    QList<DesktopWidgetEntry> desktopWidgets; // session 91
};

// Atomic JSON persistence via QSaveFile (docs/technique.md ADR-5).
class JsonStore
{
public:
    static constexpr int kCurrentSchema = 1;

    // Session 85: what a document says about itself, read without parsing it
    // into a Layout. `valid` is false for something that is not a JSON object.
    struct Info {
        bool valid = false;
        int schema = 0;
        QString writtenBy; // the StarFish version that last wrote it ("" before s85)
        QJsonObject root;
    };
    static Info peek(const QByteArray& data, QString* error = nullptr);

    // Brings `root` from schema `from` up to kCurrentSchema, one step at a time
    // (each step is a pure function of the JSON). False with an error when a
    // step is missing or fails -- the caller then keeps the file untouched.
    static bool migrate(QJsonObject& root, int from, QString* error = nullptr);

    // The document for `layout`, carrying over every key `previous` (the file
    // as read) holds that this build does not know -- at the root, per fence
    // (matched by id), its style, per tile (matched by path), its widget
    // object, per rule. A newer StarFish's additions survive an older one's
    // write. `writtenBy` stamps the version.
    static QByteArray toJsonPreserving(const Layout& layout, const QJsonObject& previous,
                                       const QString& writtenBy);
    // Atomic write (QSaveFile: temp file then rename) of ready bytes.
    static bool saveBytes(const QByteArray& payload, const QString& filePath, QString* error = nullptr);

    // Serialises a layout to the on-disk document. Deterministic (QJsonObject
    // sorts its keys), so the bytes can be compared to tell whether two layouts
    // are identical -- LayoutStore relies on that to skip pointless writes.
    static QByteArray toJson(const Layout& layout);
    // Session 65: one fence's look, alone (fence templates).
    static QByteArray styleToBytes(const FenceStyle& style);
    static FenceStyle styleFromBytes(const QByteArray& data);

    // Parses a document. Returns nullopt with an error message on failure,
    // including an unsupported schema version.
    static std::optional<Layout> fromJson(const QByteArray& data, QString* error = nullptr);

    // Writes the layout atomically, creating the parent directory if needed.
    // Returns false with an error message.
    static bool save(const Layout& layout, const QString& filePath, QString* error = nullptr);

    // Reads a layout. Returns nullopt with an error message on failure.
    static std::optional<Layout> load(const QString& filePath, QString* error = nullptr);
};

} // namespace starfish::core
