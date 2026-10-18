#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>

#include "ui/fence_material.hpp"

namespace starfish::ui {

// T-076 (whose first piece is T-064) -- StarFish's **own** menu for one fence.
//
// The user, session 42: « un menu rien qu'à nous, de type fermer,
// afficher/masquer barre des titres et je ne sais quoi ». Until now the only menu
// a fence could show was the *Shell's* menu for the tile under the cursor
// (`ShellContextMenu::showForPath`, T-006): right-clicking a fence's title bar, or
// the empty part of its panel, did nothing at all -- and the per-fence choice of
// material lived only in the settings window that T-059 wants to demote (T-064).
//
// What this header owns is the **content** of that menu and nothing else: which
// entries exist, what they say, and which one is currently in force. It is pure
// -- Qt Core, no QMenu, no window -- for the same reason fence_geometry and
// fence_material are: the rules worth checking (each action appears once, the
// check mark follows the document, the roll entry names what it *will* do and not
// what *is*) are checked without a desktop, and the product is left with the
// mechanical part -- turning an entry into a call.
//
// Three deliberate limits:
//  * the menu is built from state that already exists (roll-up, material) and
//    stores nothing, so a menu can never become a second source of truth;
//  * nothing is offered that the product cannot do: there is no greyed
//    "coming soon" line, because a disabled entry is a promise (that is why the
//    per-fence colour/radius of T-080 and the crash-restore of T-042 are absent);
//  * the title bar cannot be hidden yet. A fence is moved by its title bar
//    (T-007's drag band), so hiding it would trade one entry for the loss of a
//    gesture; that half of T-076 waits for a move that survives it.
namespace FenceMenu {

enum class Kind {
    Action,
    Submenu,
    Separator,
};

// What the product has to *do*. The entries carry an action rather than a string
// id so the product switches on an enum: a menu whose meaning is spelled in
// strings drifts the day one of them is renamed.
enum class Action {
    None,
    Material,   // the submenu itself; its children come from materialEntries()
    ToggleRoll, // F-020
    Rename,
    // ⚠️ T-082: the material applies to the fence's own chrome (panel + title bar)
    // and *not* to its icons -- F-038's wording, and the user's request after
    // trying the opposite. This entry, on a fence whose panel is translucent,
    // asks for the other behaviour: the material on the icons too.
    FadeIcons,
    Remove, // F-007 -- the fence goes, the files never move (AGENTS.md golden rule)
};

struct Entry {
    Kind kind = Kind::Action;
    Action action = Action::None;
    // Only for Action::Material entries: the FenceMaterial preset id.
    QString material;
    QString label;
    bool checkable = false;
    bool checked = false;
    bool enabled = true;
};

// The state the menu describes. Both fields are read from the fence every time
// the menu opens -- never cached -- so the menu cannot disagree with the document.
struct State {
    bool rolledUp = false;
    // The material in force, as FenceContainer::fenceMaterial() reports it: a
    // FenceMaterial preset id, or empty when the fence's alpha is a value of its
    // own (a hand-written document, or a preset that no longer exists).
    QString material;
    // T-082: whether that material leaves the panel below full opacity. It is the
    // only case where "keep the icons out of it" is a choice at all, so the entry
    // is *absent* on an opaque fence rather than greyed out (a disabled entry is a
    // promise -- the rule the whole menu is written under).
    bool materialIsTranslucent = false;
    // ... and whether the choice is currently on: the document's `fadeIcons`, read
    // from the fence every time the menu opens.
    bool fadeIcons = false;
};

// The three presets, as the "Material" submenu shows them: one radio mark, on the
// preset in force -- and on **none** of them when the fence carries a custom
// alpha, because marking the nearest preset would be a lie about the document.
inline QList<Entry> materialEntries(const State& state)
{
    QList<Entry> entries;
    const QStringList ids = FenceMaterial::presetIds();
    entries.reserve(ids.size());
    for (const QString& id : ids) {
        Entry entry;
        entry.kind = Kind::Action;
        entry.action = Action::Material;
        entry.material = id;
        entry.label = FenceMaterial::presetLabel(id);
        entry.checkable = true;
        entry.checked = !state.material.isEmpty() && state.material == id;
        entries.append(entry);
    }
    return entries;
}

// The menu itself, top to bottom. The order is the one the user named -- the
// material choice first (it is what T-064 is about, and it is the entry a fence
// owner reaches for), the two harmless toggles next, and the one that makes the
// fence disappear last, behind its own separator so it is never a neighbour of a
// toggle.
inline QList<Entry> entries(const State& state)
{
    QList<Entry> out;

    Entry material;
    material.kind = Kind::Submenu;
    material.action = Action::Material;
    material.label = QCoreApplication::translate("FenceMenu", "Material");
    out.append(material);

    // T-082, right under the material it qualifies: what the choice *covers*. On
    // a fence whose panel is still fully opaque there is nothing to decide, so the
    // entry is not there at all.
    if (state.materialIsTranslucent) {
        Entry icons;
        icons.kind = Kind::Action;
        icons.action = Action::FadeIcons;
        icons.label = QCoreApplication::translate("FenceMenu", "Material on the icons too");
        icons.checkable = true;
        icons.checked = state.fadeIcons;
        out.append(icons);
    }

    Entry separator;
    separator.kind = Kind::Separator;
    out.append(separator);

    Entry roll;
    roll.kind = Kind::Action;
    roll.action = Action::ToggleRoll;
    // The label says what the click does, not what the fence is: an entry reading
    // "Rolled up" that does nothing when clicked is a state display, and this
    // menu has no room for one.
    roll.label = state.rolledUp ? QCoreApplication::translate("FenceMenu", "Roll down")
                                : QCoreApplication::translate("FenceMenu", "Roll up");
    out.append(roll);

    Entry rename;
    rename.kind = Kind::Action;
    rename.action = Action::Rename;
    rename.label = QCoreApplication::translate("FenceMenu", "Rename...");
    out.append(rename);

    out.append(separator);

    Entry remove;
    remove.kind = Kind::Action;
    remove.action = Action::Remove;
    remove.label = QCoreApplication::translate("FenceMenu", "Remove this fence");
    out.append(remove);

    return out;
}

} // namespace FenceMenu

} // namespace starfish::ui
