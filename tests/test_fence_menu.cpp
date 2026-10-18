#include <QtTest>

#include <QString>
#include <QStringList>

#include "core/jsonstore.hpp"
#include "ui/fence_material.hpp"
#include "ui/fence_menu.hpp"

using namespace starfish::ui;

// T-076 / T-064: the pure half of the fence's own menu.
//
// What is worth checking here is not the wording but three promises the product
// leans on:
//
//  * **each action appears exactly once**, so a menu can never offer two ways to
//    do the same thing (or two check marks for one state);
//  * the **check mark follows the document** -- a radio mark on a preset the
//    fence is not shown in would be a lie about the file the user is looking at;
//  * the **roll entry says what it will do** ("Roll up" on an expanded fence),
//    because the same entry is also the only place the state is readable.
//
// The last section checks the ids the menu offers against FenceMaterial itself:
// a menu that offered a preset the container would refuse would be a click that
// silently does nothing.
class TestFenceMenu : public QObject
{
    Q_OBJECT

private:
    static int countOf(const QList<FenceMenu::Entry>& entries, FenceMenu::Action action)
    {
        int count = 0;
        for (const FenceMenu::Entry& entry : entries) {
            if (entry.kind == FenceMenu::Kind::Action && entry.action == action)
                ++count;
        }
        return count;
    }

    static const FenceMenu::Entry* find(const QList<FenceMenu::Entry>& entries,
                                        FenceMenu::Action action)
    {
        for (const FenceMenu::Entry& entry : entries) {
            if (entry.action == action)
                return &entry;
        }
        return nullptr;
    }

private slots:
    void theMenuOffersEachActionExactlyOnce()
    {
        FenceMenu::State state;
        const QList<FenceMenu::Entry> entries = FenceMenu::entries(state);

        QCOMPARE(countOf(entries, FenceMenu::Action::Material), 0); // the submenu is not an Action
        QCOMPARE(countOf(entries, FenceMenu::Action::ToggleRoll), 1);
        QCOMPARE(countOf(entries, FenceMenu::Action::Rename), 1);
        QCOMPARE(countOf(entries, FenceMenu::Action::Remove), 1);
        // T-082: and *not* the icons entry -- the state here is a fence whose panel
        // is fully opaque, where the material has nothing else to cover.
        QCOMPARE(countOf(entries, FenceMenu::Action::FadeIcons), 0);
        // T-080: the appearance is offered as submenus too, so none of its actions
        // is a top-level entry -- and "same as the panel" is absent while the fence
        // has no band of its own (that is what State::hasTitleBarColor is for).
        QCOMPARE(countOf(entries, FenceMenu::Action::PanelColor), 0);
        QCOMPARE(countOf(entries, FenceMenu::Action::TitleBarColor), 0);
        QCOMPARE(countOf(entries, FenceMenu::Action::ClearTitleBarColor), 0);
        QCOMPARE(countOf(entries, FenceMenu::Action::Radius), 0);

        QStringList submenuActions;
        for (const FenceMenu::Entry& entry : entries) {
            if (entry.kind == FenceMenu::Kind::Submenu) {
                submenuActions << QString::number(static_cast<int>(entry.action));
                QVERIFY(!entry.label.isEmpty());
            }
            if (entry.kind == FenceMenu::Kind::Separator)
                QCOMPARE(entry.action, FenceMenu::Action::None);
        }
        // T-064's material, then T-080's two.
        QCOMPARE(submenuActions.size(), 3);
        QVERIFY(submenuActions.contains(QString::number(static_cast<int>(FenceMenu::Action::Material))));
        QVERIFY(submenuActions.contains(QString::number(static_cast<int>(FenceMenu::Action::Colours))));
        QVERIFY(submenuActions.contains(QString::number(static_cast<int>(FenceMenu::Action::Radius))));
    }

    void theColoursSubmenuOffersTheTwoPickersAndTheWayBack()
    {
        // T-080. The two pickers are always there; the way back to "the title bar
        // is the panel" only exists once the fence has a band of its own -- the same
        // rule as the material-on-the-icons entry, and for the same reason: an entry
        // that removes nothing is a promise.
        FenceMenu::State plain;
        plain.hasTitleBarColor = false;
        const QList<FenceMenu::Entry> without =
            FenceMenu::colourEntries(plain);
        QCOMPARE(without.size(), 2);
        QCOMPARE(countOf(without, FenceMenu::Action::PanelColor), 1);
        QCOMPARE(countOf(without, FenceMenu::Action::TitleBarColor), 1);
        QCOMPARE(countOf(without, FenceMenu::Action::ClearTitleBarColor), 0);
        for (const FenceMenu::Entry& entry : without) {
            QCOMPARE(entry.kind, FenceMenu::Kind::Action);
            // Commands, not a state: no radio mark on a colour picker.
            QVERIFY(!entry.checkable);
            QVERIFY(!entry.label.isEmpty());
            QVERIFY(entry.enabled);
        }

        FenceMenu::State banded;
        banded.hasTitleBarColor = true;
        const QList<FenceMenu::Entry> with = FenceMenu::colourEntries(banded);
        QCOMPARE(with.size(), 3);
        QCOMPARE(countOf(with, FenceMenu::Action::ClearTitleBarColor), 1);
    }

    void theRadiusSubmenuMarksTheRadiusInForceAndNothingElse()
    {
        // T-080. One radio mark, on the value the document holds -- and none at all
        // for a radius that is not on the ladder (a hand-written document), exactly
        // like a custom alpha marking no material.
        const QList<int> ladder = FenceMenu::radiusPresets();
        QVERIFY(ladder.size() >= 3);
        QVERIFY(ladder.first() <= 0); // "Square" is offered: no rounding is a choice

        for (const int px : ladder) {
            FenceMenu::State state;
            state.radius = px;
            int checked = 0;
            for (const FenceMenu::Entry& entry : FenceMenu::radiusEntries(state)) {
                QCOMPARE(entry.kind, FenceMenu::Kind::Action);
                QCOMPARE(entry.action, FenceMenu::Action::Radius);
                QVERIFY(entry.checkable);
                QVERIFY(entry.enabled);
                QVERIFY(!entry.label.isEmpty());
                QVERIFY(entry.value >= 0);
                if (entry.checked) {
                    ++checked;
                    QCOMPARE(entry.value, px);
                }
            }
            QCOMPARE(checked, 1);
        }

        FenceMenu::State custom;
        custom.radius = 13; // a value of its own: the widget draws it, no entry names it
        int checked = 0;
        for (const FenceMenu::Entry& entry : FenceMenu::radiusEntries(custom))
            checked += entry.checked ? 1 : 0;
        QCOMPARE(checked, 0);
    }

    void theLadderOffersTheSchemaDefault()
    {
        // The label says "(default)" on one entry, and a label that lied about what
        // a new fence looks like would be worse than no label: the ladder has to
        // carry the radius the schema gives a fence nobody has styled.
        QVERIFY(FenceMenu::radiusPresets().contains(starfish::core::FenceStyle().radius));
        QVERIFY(!FenceMenu::radiusLabel(starfish::core::FenceStyle().radius).isEmpty());
        QVERIFY(FenceMenu::radiusLabel(0) != FenceMenu::radiusLabel(6));
    }

    void theOrderPutsTheDestructiveEntryLastBehindASeparator()
    {
        FenceMenu::State state;
        const QList<FenceMenu::Entry> entries = FenceMenu::entries(state);
        QVERIFY(entries.size() >= 4);

        // First entry: the material submenu (the entry T-064 is about).
        QCOMPARE(entries.first().kind, FenceMenu::Kind::Submenu);

        // The last entry is the destructive one, and it is not a neighbour of a
        // toggle: a slip of the mouse must not be able to reach it.
        QCOMPARE(entries.last().action, FenceMenu::Action::Remove);
        QCOMPARE(entries.at(entries.size() - 2).kind, FenceMenu::Kind::Separator);

        // The rename entry is above that separator.
        bool renameBeforeTheSeparator = false;
        for (int i = 0; i < entries.size() - 2; ++i) {
            if (entries.at(i).action == FenceMenu::Action::Rename)
                renameBeforeTheSeparator = true;
        }
        QVERIFY(renameBeforeTheSeparator);
    }

    void theRollEntryNamesTheGestureNotTheState()
    {
        FenceMenu::State expanded;
        expanded.rolledUp = false;
        // The list is kept in a local: find() hands back a pointer *into* it, and a
        // pointer into a temporary is a pointer into freed memory (measured here --
        // the two labels compared equal while pointing at the same dead buffer).
        const QList<FenceMenu::Entry> expandedEntries = FenceMenu::entries(expanded);
        const FenceMenu::Entry* up = find(expandedEntries, FenceMenu::Action::ToggleRoll);
        QVERIFY(up != nullptr);
        QVERIFY(!up->checked); // it is a command, not a state display

        FenceMenu::State rolled;
        rolled.rolledUp = true;
        const QList<FenceMenu::Entry> rolledEntries = FenceMenu::entries(rolled);
        const FenceMenu::Entry* down = find(rolledEntries, FenceMenu::Action::ToggleRoll);
        QVERIFY(down != nullptr);
        QVERIFY(up->label != down->label);
        QVERIFY(!up->label.isEmpty());
    }

    void theMaterialSubmenuOffersTheThreePresets()
    {
        FenceMenu::State state;
        const QList<FenceMenu::Entry> presets = FenceMenu::materialEntries(state);
        const QStringList ids = FenceMaterial::presetIds();
        QCOMPARE(presets.size(), ids.size());
        QCOMPARE(static_cast<int>(presets.size()), 3);

        for (int i = 0; i < presets.size(); ++i) {
            QCOMPARE(presets.at(i).kind, FenceMenu::Kind::Action);
            QCOMPARE(presets.at(i).action, FenceMenu::Action::Material);
            QCOMPARE(presets.at(i).material, ids.at(i));
            QVERIFY(presets.at(i).checkable);
            QVERIFY(presets.at(i).enabled);
            QVERIFY(!presets.at(i).label.isEmpty());
            // What the menu offers, the container must be able to apply: an entry
            // whose preset has no alpha would be a click that does nothing.
            QVERIFY(FenceMaterial::alphaFor(presets.at(i).material) >= 0);
        }
    }

    void theCheckMarkFollowsTheFenceAndNothingElse()
    {
        for (const QString& id : FenceMaterial::presetIds()) {
            FenceMenu::State state;
            state.material = id;
            int checked = 0;
            for (const FenceMenu::Entry& entry : FenceMenu::materialEntries(state)) {
                if (entry.checked) {
                    ++checked;
                    QCOMPARE(entry.material, id);
                }
            }
            QCOMPARE(checked, 1); // one radio mark, on the preset in force
        }
    }

    void aCustomAlphaMarksNoPresetAtAll()
    {
        // The document may hold any alpha (a hand-written file, or a preset that
        // no longer exists): the menu then marks nothing rather than the nearest
        // preset, which would describe a state the file is not in.
        FenceMenu::State custom;
        custom.material = QString();
        int checked = 0;
        for (const FenceMenu::Entry& entry : FenceMenu::materialEntries(custom))
            checked += entry.checked ? 1 : 0;
        QCOMPARE(checked, 0);
    }

    void nothingTheProductCannotDoIsOffered()
    {
        // A disabled entry is a promise ("this exists, just not here"): T-076's
        // remaining half -- hiding the title bar -- is deliberately absent rather
        // than greyed, because a fence with no bar has no drag band (T-007).
        FenceMenu::State state;
        for (const FenceMenu::Entry& entry : FenceMenu::entries(state))
            QVERIFY(entry.enabled);
        for (const FenceMenu::Entry& entry : FenceMenu::materialEntries(state))
            QVERIFY(entry.enabled);
    }

    void theIconsEntryExistsOnlyUnderATranslucentPanel()
    {
        // T-082: the material fades the fence's own chrome (panel + title bar) by
        // default, and the entry that asks for it on the icons *too* is absent
        // while the panel is still opaque -- there is nothing to decide there, and
        // an entry that could not change anything would be a promise.
        FenceMenu::State opaque;
        opaque.materialIsTranslucent = false;
        QCOMPARE(countOf(FenceMenu::entries(opaque), FenceMenu::Action::FadeIcons), 0);

        FenceMenu::State translucent;
        translucent.materialIsTranslucent = true;
        // The list is kept in a local: find() hands back a pointer *into* it.
        const QList<FenceMenu::Entry> entries = FenceMenu::entries(translucent);

        // Exactly one, and it is a *state*: checkable, right under the material it
        // qualifies (first entry, then the separator that opens the navigation
        // group).
        QCOMPARE(countOf(entries, FenceMenu::Action::FadeIcons), 1);
        QCOMPARE(entries.at(0).action, FenceMenu::Action::Material);
        QCOMPARE(entries.at(1).action, FenceMenu::Action::FadeIcons);
        const FenceMenu::Entry* entry = find(entries, FenceMenu::Action::FadeIcons);
        QVERIFY(entry != nullptr);
        QVERIFY(entry->checkable);
        QVERIFY(!entry->checked); // the document says `fadeIcons: false`
        QVERIFY(entry->enabled);

        // ...and the mark follows the document.
        translucent.fadeIcons = true;
        const QList<FenceMenu::Entry> asked = FenceMenu::entries(translucent);
        const FenceMenu::Entry* marked = find(asked, FenceMenu::Action::FadeIcons);
        QVERIFY(marked != nullptr);
        QVERIFY(marked->checked);
    }
};

QTEST_MAIN(TestFenceMenu)

#include "test_fence_menu.moc"
