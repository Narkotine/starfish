#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace starfish::ui {

class FenceContainer;

// The settings window (F-034) -- the first piece of StarFish a user can actually
// click on.
//
// It *reads* the container to draw the fences that exist, and only ever *asks*
// for changes through signals: the container stays the sole owner of the fences,
// exactly as in every other widget of this codebase. Deletion asks for
// confirmation and says plainly that the files are not touched (F-007).
//
// Non-modal on purpose: the fences live on the desktop behind it, so a modal
// window would stop the user from seeing the effect of what they just changed.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(FenceContainer* container, QWidget* parent = nullptr);

    void setDesktopIconsHidden(bool hidden);
    bool desktopIconsHidden() const;
    // Free-form line under the desktop group: rules, watched folders, hot keys.
    void setStatusText(const QString& text);

public slots:
    // Rebuilds the list from the container. The product connects it to the
    // container's lifecycle signals, so the window follows the desktop instead
    // of the other way round.
    void refresh();

signals:
    void newFenceRequested();
    void renameFenceRequested(const QString& id, const QString& title);
    void removeFenceRequested(const QString& id);
    void rollUpToggled(const QString& id, bool rolledUp);
    void desktopIconsToggled(bool hidden);

private:
    void onRenameClicked();
    void onRollUpClicked();
    void onDeleteClicked();
    void updateButtons();
    QString selectedId() const;

    FenceContainer* m_container = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_rename = nullptr;
    QPushButton* m_rollUp = nullptr;
    QPushButton* m_delete = nullptr;
    QCheckBox* m_hideIcons = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace starfish::ui
