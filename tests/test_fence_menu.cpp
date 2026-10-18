#include <QtTest>

#include <QString>
#include <QStringList>

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

        int submenus = 0;
        for (const FenceMenu::Entry& entry : entries) {
            if (entry.kind == FenceMenu::Kind::Submenu) {
                ++submenus;
                QCOMPARE(entry.action, FenceMenu::Action::Material);
                QVERIFY(!entry.label.isEmpty());
            }
            if (entry.kind == FenceMenu::Kind::Separator)
                QCOMPARE(entry.action, FenceMenu::Action::None);
        }
        QCOMPARE(submenus, 1);
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
};

QTEST_MAIN(TestFenceMenu)

#include "test_fence_menu.moc"
