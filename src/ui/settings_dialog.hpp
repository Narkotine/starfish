#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;

namespace starfish::ui {

// The settings window (F-034). T-059 -- it is **no longer the fence manager**.
//
// It used to draw every fence with its dimensions, create, rename, roll up and
// delete them, and pick one fence's material. The user named that surface for
// what it was: « il ne devrait pas y avoir de menu séparé comme il y a à présent,
// avec les fences et leurs dimensions, pour créer les fences, ce n'est que pour
// la démo ». Every one of those abilities now has a place of its own -- the tray
// creates a fence and applies a material to all of them, and a fence's **own
// menu** (T-076, ADR-16) is where one fence is renamed, rolled up, removed,
// coloured and materialised (T-064, T-080).
//
// So this window reads no fence, and the strongest form of that is that it no
// longer holds a FenceContainer at all: "a window is not a second path to the
// document" stops being a promise and becomes a property of the code.
//
// What it holds is the two things no other surface says in words:
//   * **one line describing what is running** (fences, rules, watched folders,
//     hot keys);
//   * T-088 -- **the desktop-icon switch the user asked for** (« alors si ce n'est
//     pas le défaut du produit, fais une case où je peux le configurer »). It is a
//     *preference*, not document state: it applies to this desktop through
//     `core::Preferences`, the product decides what it means, and this window only
//     reports it and asks for it -- exactly like a tray entry. T-062's decision
//     stands where it was aimed: the switch is not a trap reached by accident, it
//     says out loud what it costs, and the tray keeps the way back. The rules
//     screen (T-032) is the other thing still assigned to this page, which is why
//     the window exists rather than being deleted.
//
// Non-modal on purpose: what it describes lives on the desktop behind it.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Free-form line describing what is running. Written by the product and never
    // read back: the window owns no state, so nothing it shows can be mistaken for
    // a choice.
    void setStatusText(const QString& text);

    // Reports the **real** state of the preference -- what the product applied,
    // not what was asked for -- and does not emit the toggle signal for a
    // programmatic change: a checkbox that answered its own signal would fight
    // the product that set it.
    void setDesktopIconsOwned(bool owned);
    bool desktopIconsOwned() const;

signals:
    // T-088: the user asked for StarFish to draw the desktop's icons, or asked it
    // to stop. The product decides what that means -- it persists the choice and
    // applies it -- and this widget never does either.
    void desktopIconsOwnedToggled(bool owned);

private:
    QLabel* m_status = nullptr;
    QCheckBox* m_ownIcons = nullptr;
};

} // namespace starfish::ui
