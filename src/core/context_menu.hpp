#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <vector>

namespace starfish::core {

// T-092 -- the right-click menu StarFish draws, as data. Pure Qt Core: the
// shell's menu comes in as a list of entries (read from the real IContextMenu
// by platform::win::DesktopShellView), the Windows 11 profile and the user's
// configuration decide what shows and in what order, and the UI draws the
// result. Everything here is testable without a window.

struct ContextMenuItem {
    QString key;      // stable name: "refresh", "open", "starfish:create-fence", "label:<text>"
    QString label;    // what the menu writes
    QString shortcut; // "Ctrl+Z", may be empty
    QString glyph;    // a Segoe Fluent Icons code point, may be empty
    bool separator = false;
    bool enabled = true;
    bool checked = false;
    bool radio = false;
    bool isDefault = false;
    bool topBar = false;  // Windows 11's row of icons (Cut, Copy, Rename, Share, Delete)
    int command = -1;     // the shell's command offset; -1 when not a shell entry
    int ownId = -1;       // a StarFish entry's id; -1 otherwise
    int customIndex = -1; // a user entry (ContextMenuProfile::custom); -1 otherwise
    std::vector<ContextMenuItem> children;
};

enum class ContextMenuKind { Background, Selection };

// A user entry: a program run with the selection (or the desktop folder).
// In `arguments`, "%paths%" expands to every selected path, "%path%" to the
// first one, "%desktop%" to the desktop folder.
struct ContextMenuCustomEntry {
    QString label;
    QString program;
    QStringList arguments;
    QString glyph;
};

struct ContextMenuProfile {
    QStringList hidden; // keys taken out
    QStringList shown;  // keys Windows 11 leaves under "Show more options", brought back
    QStringList order;  // keys put first, in this order
    QList<ContextMenuCustomEntry> custom;
};

struct ContextMenuConfig {
    bool modern = true; // false: the classic menu, as on Windows 10
    ContextMenuProfile background;
    ContextMenuProfile items;

    const ContextMenuProfile& profile(ContextMenuKind kind) const
    {
        return kind == ContextMenuKind::Background ? background : items;
    }

    // %LOCALAPPDATA%/StarFish/context-menu.json (GenericDataLocation)
    static QString defaultPath();
    // A missing or unreadable file is the default configuration.
    static ContextMenuConfig load(const QString& path);
    bool save(const QString& path) const;
};

// The stable key of a shell entry, from its label (the verbs do not survive the
// proxy -- see DesktopShellView): "refresh", "undo", "open-with"... in French
// and English; anything unknown is "label:<label>".
QString contextMenuKey(const QString& label, bool hasChildren);
// The Segoe Fluent Icons glyph Windows 11 shows for a key, or empty.
QString contextMenuGlyph(const QString& key);

// The ownId of "Show more options" (the classic menu).
constexpr int kMoreOptionsId = -100;

// Windows 11's menu from the classic one: the entries Windows 11 shows, in its
// order and groups, the top bar for Cut/Copy/Rename/Share/Delete (Paste on the
// background); the user's `hidden`, `shown`, `order` and `custom`; the StarFish
// entries (`ours`, with ownId and key set) before "Show more options", which is
// always last. `classic` entries need no key: it is computed here.
// `omitted` (optional) receives the keys of what went under "Show more options".
std::vector<ContextMenuItem> composeWindows11Menu(const std::vector<ContextMenuItem>& classic,
                                                  ContextMenuKind kind,
                                                  const ContextMenuConfig& config,
                                                  const std::vector<ContextMenuItem>& ours,
                                                  const QString& moreOptionsLabel,
                                                  QStringList* omitted = nullptr);

// The program and arguments of a custom entry, placeholders expanded.
QStringList expandCustomArguments(const ContextMenuCustomEntry& entry, const QStringList& paths,
                                  const QString& desktop);

} // namespace starfish::core
