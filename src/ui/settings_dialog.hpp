#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QWidget;

namespace starfish::ui {

class ContextMenuEditor;

// The settings window (F-034), redone in session 59 in the Windows 11 manner:
// the theme's colours, a navigation on the left, one page per subject.
//
//  - General: what is running (the product's own status line, T-059), a new
//    fence, the settings folder, the way back to the icons (T-046) when offered;
//  - Fences: what holds for **all** fences -- shown or not, the material, the
//    shortcut arrows. One fence is still its own menu's (T-076, ADR-16): there
//    is no fence list here, so the window is not a second path to a fence;
//  - Right-click menu: the menu editor (T-092);
//  - About.
//
// It still owns no state: every control signals and the product answers with
// the setters below, the tray's contract (the two surfaces cannot disagree).
// Non-modal on purpose: what it describes lives on the desktop behind it.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    enum Page { General = 0, Fences = 1, Menu = 2, About = 3 };

    explicit SettingsDialog(QWidget* parent = nullptr);

    // Free-form line describing what is running, written by the product.
    void setStatusText(const QString& text);
    void setFencesVisible(bool visible);
    void setFenceMaterial(const QString& material);
    void setFenceShortcutArrows(bool shown);
    // Session 60.
    void setSortOnAdd(bool sorted);
    // Session 62: what the registry says (Run + Task Manager's StartupApproved).
    void setStartWithWindows(bool on, bool disabledByUser);
    void setDesktopShortcutArrows(bool shown);
    void setDesktopOverlapAllowed(bool allowed);
    void setRestoreIconsOffered(bool offered);

    ContextMenuEditor* menuEditor() const { return m_menuEditor; }
    void showPage(Page page);
    Page currentPage() const;

signals:
    void newFenceRequested();
    void fencesVisibilityToggled(bool visible);
    void fenceMaterialRequested(const QString& material);
    void fenceShortcutArrowsToggled(bool shown);
    void sortOnAddToggled(bool sorted);
    void startWithWindowsToggled(bool on);
    void desktopShortcutArrowsToggled(bool shown);
    void desktopOverlapToggled(bool allowed);
    void restoreIconsRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QWidget* buildGeneral();
    QWidget* buildFences();
    QWidget* buildMenu();
    QWidget* buildAbout();
    void applyTheme();

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_pages = nullptr;
    QLabel* m_status = nullptr;
    QCheckBox* m_showFences = nullptr;
    QCheckBox* m_arrows = nullptr;
    QCheckBox* m_sortOnAdd = nullptr;
    QCheckBox* m_startWithWindows = nullptr;
    QLabel* m_startHint = nullptr;
    QCheckBox* m_desktopArrows = nullptr;
    QCheckBox* m_overlap = nullptr;
    QHash<QString, QRadioButton*> m_materials;
    QWidget* m_restoreRow = nullptr;
    ContextMenuEditor* m_menuEditor = nullptr;
    bool m_updating = false;
    bool m_dark = false;
};

} // namespace starfish::ui
