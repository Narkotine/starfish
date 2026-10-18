#pragma once

#include <QByteArray>
#include <QRect>
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
};

// An item displayed inside a fence: a desktop file or a folder portal.
struct FenceItem {
    QString kind;  // "file" | "portal"
    QString path;
};

struct Fence {
    QString id;
    QString title;
    QString monitor;      // device name, e.g. "\\\\.\\DISPLAY1"
    QRect rect;           // desktop coordinates
    FenceStyle style;
    QString sortBy = "name"; // name | type | size | date
    bool sortAscending = true;
    bool rolledUp = false;   // F-020
    QList<FenceItem> items;
};

// Versioned layout document (docs/technique.md ADR-5 / section 5).
struct Layout {
    int schema = 1;
    QList<Fence> fences;
    // Automation rules (T-010, F-017). They belong to the document rather than
    // to a single fence: a rule names the fence it feeds.
    QList<Rule> rules;
};

// Atomic JSON persistence via QSaveFile (docs/technique.md ADR-5).
class JsonStore
{
public:
    // Serialises a layout to the on-disk document. Deterministic (QJsonObject
    // sorts its keys), so the bytes can be compared to tell whether two layouts
    // are identical -- LayoutStore relies on that to skip pointless writes.
    static QByteArray toJson(const Layout& layout);

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
