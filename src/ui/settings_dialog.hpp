#pragma once

#include <QDialog>
#include <QString>

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
// What it holds is the one thing no other surface says in words: **one line
// describing what is running** (fences, rules, watched folders, hot keys). The
// desktop-icon switch T-088 had added here is gone (ADR-20, session 57) -- the
// user tried it and rejected it, and a mode the product no longer offers needs
// no check box. The rules screen (T-032) is the other thing still assigned to
// this page, which is why the window exists rather than being deleted.
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

private:
    QLabel* m_status = nullptr;
};

} // namespace starfish::ui
