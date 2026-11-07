#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

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
//  * the menu is built from state that already exists (roll-up, material, colours,
//    radius) and stores nothing, so a menu can never become a second source of
//    truth;
//  * nothing is offered that the product cannot do: there is no greyed
//    "coming soon" line, because a disabled entry is a promise (that is why the
//    crash-restore of T-042 is absent, and why "same as the panel" only exists
//    once a fence has a band of its own);
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
    // T-098: the fence's transparency, as a number (a dialog with a slider).
    Transparency,
    // T-080: the fence's own appearance. `Colours` is a submenu (its children come
    // from colourEntries()), `Radius` a submenu whose children carry the value in
    // `Entry::value`, and the other two are the colour pickers themselves.
    Colours,
    PanelColor,
    TitleBarColor,
    ClearTitleBarColor, // "same as the panel" -- back to the pre-T-080 look
    Radius,
    Remove, // F-007 -- the fence goes, the files never move (AGENTS.md golden rule)
    // Session 65: the menu keeps the behaviour; the look moved to a dialog.
    Sort,       // the submenu; its children come from sortEntries()
    SortBy,     // value = index into sortKeys()
    Appearance, // "Appearance...": material, transparency, corners, colours
    ToggleTitleBar, // session 68: show / hide the title bar, checkable
    HoverUnroll,       // session 71: the submenu; children from hoverUnrollEntries()
    HoverUnrollChoice, // value: 0 default (Settings), 1 yes, 2 no
    Resize,            // session 76: the submenu; children from resizeEntries()
    ResizeChoice,      // value: 0 default (Settings), 1 whole icons, 2 free
    Template,          // session 76: the submenu; children from templateEntries()
    ApplyTemplate,     // value: index into State::templates
    AddWidget,         // session 79: the submenu; children from widgetEntries()
    AddWidgetChoice,   // value: index into State::widgetKinds
    ToggleLock,        // session 94: lock / unlock the fence
};

struct Entry {
    Kind kind = Kind::Action;
    Action action = Action::None;
    // Only for Action::Material entries: the FenceMaterial preset id.
    QString material;
    // T-080: only for Action::Radius entries -- the radius in pixels the entry
    // means. A value rather than a name: a radius has no id, and the document
    // stores the number.
    int value = -1;
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
    // T-080: the radius in force (the document's, not the one the geometry allows
    // -- the menu shows what the fence *is*, and an entry is marked only when it
    // names that value), and whether the fence draws a band of its own.
    int radius = 0;
    bool hasTitleBarColor = false;
    QString sortBy = QStringLiteral("name");
    bool titleBarShown = true; // session 68
    QString hoverUnroll;       // session 71: "", "on", "off" (the fence's own)
    bool hoverUnrollDefault = false; // ... and what "" means today
    QString snapToCells;             // session 76: "", "on", "off" (the fence's own)
    bool snapToCellsDefault = false; // ... and what "" means today
    QStringList templates;           // session 76: the saved templates' names
    QStringList widgetKinds;         // session 79: the widgets one can add (labels)
    bool locked = false;             // session 94
};

// Session 79: the widgets, one click each.
inline QList<Entry> widgetEntries(const State& state)
{
    QList<Entry> entries;
    for (int i = 0; i < state.widgetKinds.size(); ++i) {
        Entry entry;
        entry.action = Action::AddWidgetChoice;
        entry.value = i;
        entry.label = state.widgetKinds.at(i);
        entries.append(entry);
    }
    return entries;
}

// Session 76: resizing in whole icons or freely -- the default, or this fence's own.
inline QStringList resizeKeys()
{
    return {QString(), QStringLiteral("on"), QStringLiteral("off")};
}

inline QList<Entry> resizeEntries(const State& state)
{
    QList<Entry> entries;
    const QStringList keys = resizeKeys();
    const QStringList labels{
        state.snapToCellsDefault ? QCoreApplication::translate("FenceMenu", "Default (whole icons)")
                                 : QCoreApplication::translate("FenceMenu", "Default (free)"),
        QCoreApplication::translate("FenceMenu", "Whole icons"), QCoreApplication::translate("FenceMenu", "Free")};
    for (int i = 0; i < keys.size(); ++i) {
        Entry entry;
        entry.action = Action::ResizeChoice;
        entry.value = i;
        entry.label = labels.at(i);
        entry.checkable = true;
        entry.checked = state.snapToCells == keys.at(i);
        entries.append(entry);
    }
    return entries;
}

// Session 76: the saved templates, one click each.
inline QList<Entry> templateEntries(const State& state)
{
    QList<Entry> entries;
    for (int i = 0; i < state.templates.size(); ++i) {
        Entry entry;
        entry.action = Action::ApplyTemplate;
        entry.value = i;
        entry.label = state.templates.at(i);
        entries.append(entry);
    }
    return entries;
}

// Session 71: unroll on hover -- the default, or this fence's own yes / no.
inline QStringList hoverUnrollKeys()
{
    return {QString(), QStringLiteral("on"), QStringLiteral("off")};
}

inline QList<Entry> hoverUnrollEntries(const State& state)
{
    QList<Entry> entries;
    const QStringList keys = hoverUnrollKeys();
    const QStringList labels{
        state.hoverUnrollDefault ? QCoreApplication::translate("FenceMenu", "Default (yes)")
                                 : QCoreApplication::translate("FenceMenu", "Default (no)"),
        QCoreApplication::translate("FenceMenu", "Yes"), QCoreApplication::translate("FenceMenu", "No")};
    for (int i = 0; i < keys.size(); ++i) {
        Entry entry;
        entry.action = Action::HoverUnrollChoice;
        entry.value = i;
        entry.label = labels.at(i);
        entry.checkable = true;
        entry.checked = state.hoverUnroll == keys.at(i);
        entries.append(entry);
    }
    return entries;
}

// Session 65: what a fence can be sorted by (core::sortForDisplay's keys).
inline QStringList sortKeys()
{
    return {QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("date"), QStringLiteral("size"),
            QStringLiteral("manual")};
}

inline QString sortLabel(const QString& key)
{
    if (key == QLatin1String("type"))
        return QCoreApplication::translate("FenceMenu", "Type");
    if (key == QLatin1String("date"))
        return QCoreApplication::translate("FenceMenu", "Date modified");
    if (key == QLatin1String("size"))
        return QCoreApplication::translate("FenceMenu", "Size");
    if (key == QLatin1String("manual"))
        return QCoreApplication::translate("FenceMenu", "My own order");
    return QCoreApplication::translate("FenceMenu", "Name");
}

inline QList<Entry> sortEntries(const State& state)
{
    QList<Entry> entries;
    const QStringList keys = sortKeys();
    for (int i = 0; i < keys.size(); ++i) {
        Entry entry;
        entry.action = Action::SortBy;
        entry.value = i;
        entry.label = sortLabel(keys.at(i));
        entry.checkable = true;
        entry.checked = state.sortBy == keys.at(i);
        entries.append(entry);
    }
    return entries;
}

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

// T-080: the radii the menu offers, in pixels. A short ladder rather than a
// number box, because this is an eye decision (F-004) and every value on the
// ladder is one the user can recognise on screen -- "Square" is the fence with no
// rounding at all, and 10 is what the schema has always used.
//
// The list is *offered*, never enforced: the document may hold any radius (a
// hand-written file), and the widget bounds what it can draw (FenceView: never
// more than half the shorter side). A radius of 13 therefore marks no entry --
// the same honesty as a custom alpha marking no material.
inline QList<int> radiusPresets()
{
    return {0, 6, 10, 14, 20, 30, 40}; // 30 / 40: T-084
}

inline QString radiusLabel(int px)
{
    if (px <= 0)
        return QCoreApplication::translate("FenceMenu", "Square");
    if (px == 10) // the schema's default (core::FenceStyle::radius)
        return QCoreApplication::translate("FenceMenu", "10 px (default)");
    return QCoreApplication::translate("FenceMenu", "%1 px").arg(px);
}

inline QList<Entry> radiusEntries(const State& state)
{
    QList<Entry> entries;
    for (const int px : radiusPresets()) {
        Entry entry;
        entry.kind = Kind::Action;
        entry.action = Action::Radius;
        entry.value = px;
        entry.label = radiusLabel(px);
        entry.checkable = true;
        entry.checked = state.radius == px;
        entries.append(entry);
    }
    return entries;
}

// T-080: what the "Colours" submenu shows. Two pickers, and -- only once the
// fence has a band of its own -- the way back to the look that predates it. An
// entry that removed something that is not there would be a promise.
inline QList<Entry> colourEntries(const State& state)
{
    QList<Entry> entries;

    Entry panel;
    panel.kind = Kind::Action;
    panel.action = Action::PanelColor;
    panel.label = QCoreApplication::translate("FenceMenu", "Panel colour...");
    entries.append(panel);

    Entry bar;
    bar.kind = Kind::Action;
    bar.action = Action::TitleBarColor;
    bar.label = QCoreApplication::translate("FenceMenu", "Title bar colour...");
    entries.append(bar);

    if (state.hasTitleBarColor) {
        Entry clear;
        clear.kind = Kind::Action;
        clear.action = Action::ClearTitleBarColor;
        clear.label = QCoreApplication::translate("FenceMenu", "Title bar: same as the panel");
        entries.append(clear);
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
    // Session 65 (the user: "moins d'options esthétiques en accès direct au
    // clic droit sur fence mais plutôt comportement"): behaviour here, the look
    // in one "Appearance..." dialog.
    QList<Entry> out;
    Entry separator;
    separator.kind = Kind::Separator;

    Entry lock;
    lock.action = Action::ToggleLock;
    lock.label = state.locked ? QCoreApplication::translate("FenceMenu", "Unlock")
                              : QCoreApplication::translate("FenceMenu", "Lock");
    if (state.locked) {
        // Session 94 (the user): on a locked zone the menu's first entry is the
        // way out; only rolling stays, the rest waits for the unlock.
        Entry roll;
        roll.action = Action::ToggleRoll;
        roll.label = state.rolledUp ? QCoreApplication::translate("FenceMenu", "Roll down")
                                    : QCoreApplication::translate("FenceMenu", "Roll up");
        out << lock << separator << roll;
        return out;
    }

    // Session 94 (t104u): the lock is the menu's first entry, locked or not.
    out << lock << separator;

    Entry rename;
    rename.action = Action::Rename;
    rename.label = QCoreApplication::translate("FenceMenu", "Rename...");
    out.append(rename);

    Entry roll;
    roll.action = Action::ToggleRoll;
    roll.label = state.rolledUp ? QCoreApplication::translate("FenceMenu", "Roll down")
                                : QCoreApplication::translate("FenceMenu", "Roll up");
    out.append(roll);

    // Session 68 (the user asked for it here): the title bar, shown or not.
    Entry titleBar;
    titleBar.action = Action::ToggleTitleBar;
    titleBar.label = QCoreApplication::translate("FenceMenu", "Show the title bar");
    titleBar.checkable = true;
    titleBar.checked = state.titleBarShown;
    out.append(titleBar);

    Entry hoverUnroll;
    hoverUnroll.kind = Kind::Submenu;
    hoverUnroll.action = Action::HoverUnroll;
    hoverUnroll.label = QCoreApplication::translate("FenceMenu", "Unroll on hover");
    out.append(hoverUnroll);

    Entry sort;
    sort.kind = Kind::Submenu;
    sort.action = Action::Sort;
    sort.label = QCoreApplication::translate("FenceMenu", "Sort by");
    out.append(sort);

    Entry resize; // session 76
    resize.kind = Kind::Submenu;
    resize.action = Action::Resize;
    resize.label = QCoreApplication::translate("FenceMenu", "Resize");
    out.append(resize);

    if (!state.widgetKinds.isEmpty()) { // session 79
        Entry widget;
        widget.kind = Kind::Submenu;
        widget.action = Action::AddWidget;
        widget.label = QCoreApplication::translate("FenceMenu", "Add a widget");
        out.append(widget);
    }

    out.append(separator);

    Entry appearance;
    appearance.action = Action::Appearance;
    appearance.label = QCoreApplication::translate("FenceMenu", "Appearance...");
    out.append(appearance);

    if (!state.templates.isEmpty()) { // session 76: absent rather than empty
        Entry apply;
        apply.kind = Kind::Submenu;
        apply.action = Action::Template;
        apply.label = QCoreApplication::translate("FenceMenu", "Apply a template");
        out.append(apply);
    }

    out.append(separator);

    Entry remove;
    remove.action = Action::Remove;
    remove.label = QCoreApplication::translate("FenceMenu", "Remove this fence");
    out.append(remove);
    return out;
}

} // namespace FenceMenu

} // namespace starfish::ui
