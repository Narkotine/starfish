#pragma once

#include <QRect>
#include <QList>
#include <QString>
#include <optional>

namespace starfish::core {

// Visual style of a fence (docs/produit.md F-004/F-005).
struct FenceStyle {
    QString background = "#1F2937CC"; // ARGB hex (QColor::name(QColor::HexArgb))
    int radius = 10;
    int padding = 12;
    int iconSize = 48;
    int gap = 8;
    bool titleBar = true;
    QString iconTint; // empty = no tint (F-015)
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
};

// Atomic JSON persistence via QSaveFile (docs/technique.md ADR-5).
class JsonStore
{
public:
    // Writes the layout atomically. Returns false with an error message.
    static bool save(const Layout& layout, const QString& filePath, QString* error = nullptr);

    // Reads a layout. Returns nullopt with an error message on failure.
    static std::optional<Layout> load(const QString& filePath, QString* error = nullptr);
};

} // namespace starfish::core
