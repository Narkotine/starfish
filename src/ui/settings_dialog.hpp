#pragma once

#include <QSpinBox>
#include <QComboBox>

#include <QDialog>
#include <QHash>
#include <QSlider>
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
    // Session 63 (T-098): the material radios became the default transparency
    // slider; kept as a no-op so callers need not know.
    void setFenceMaterial(const QString& material);
    void setDefaultTransparency(int percent);
    void setFenceOverlapAllowed(bool allowed);
    void setHoverHighlight(bool on);
    void setDesktopDoubleClickHides(bool on); // session 69
    void setHoverUnroll(bool on);             // session 71
    void setRollOpposite(bool on);            // session 73
    void setRollAnimated(bool on);            // session 73
    void setSnapToCells(bool on);             // session 73
    void setAutoRestoreShortcuts(bool on);    // session 75
    void setShowLabels(bool on);              // session 76
    void setSplash(bool on);                  // session 76
    void setDesktopDragGrid(bool on);         // session 80
    void setWidgetEngineStatus(const QString& text); // session 80
    QString widgetEngineStatus() const;
    void setIconFlow(const QString& key);     // session 76
    void setRollSpeed(int pixelsPerSecond);   // session 95
    void setLanguage(const QString& code); // "" = Windows', "fr", "en"
    void setDefaultBorder(int width, const QString& color);
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
    // Live, while the slider moves.
    void defaultTransparencyChanged(int percent);
    void allFencesToDefaultRequested();
    void fenceOverlapToggled(bool allowed);
    void hoverHighlightToggled(bool on);
    void desktopDoubleClickHidesToggled(bool on); // session 69
    void hoverUnrollToggled(bool on);             // session 71
    void rollOppositeToggled(bool on);            // session 73
    void rollAnimatedToggled(bool on);            // session 73
    void snapToCellsToggled(bool on);             // session 73
    void autoRestoreShortcutsToggled(bool on);    // session 75
    void showLabelsToggled(bool on);              // session 76
    void splashToggled(bool on);                  // session 76
    void desktopDragGridToggled(bool on);         // session 80
    void iconFlowChosen(const QString& key);      // session 76
    void rollSpeedChosen(int pixelsPerSecond);    // session 95
    void languageChosen(const QString& code);
    void defaultBorderChanged(int width, const QString& color);
    void fenceShortcutArrowsToggled(bool shown);
    void sortOnAddToggled(bool sorted);
    void startWithWindowsToggled(bool on);
    void desktopShortcutArrowsToggled(bool shown);
    void desktopOverlapToggled(bool allowed);
    void restoreIconsRequested();
    // Session 86: About > reset. `wipeAll` false: the zones go, every icon is back
    // where Explorer keeps it; true: zones, widgets and rules all go. Asked only
    // after the user confirmed; the product backs the layout up first.
    void resetDesktopRequested(bool wipeAll);

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
    QCheckBox* m_fenceOverlap = nullptr;
    QCheckBox* m_hover = nullptr;
    QCheckBox* m_doubleClickHides = nullptr;
    QCheckBox* m_hoverUnroll = nullptr;
    QCheckBox* m_rollOpposite = nullptr;
    QCheckBox* m_rollAnimated = nullptr;
    QCheckBox* m_snapToCells = nullptr;
    QCheckBox* m_autoRestoreShortcuts = nullptr;
    QCheckBox* m_showLabels = nullptr;
    QCheckBox* m_splash = nullptr;
    QCheckBox* m_desktopDragGrid = nullptr; // session 80
    QLabel* m_widgetEngine = nullptr;       // session 80
    QComboBox* m_iconFlow = nullptr;
    QComboBox* m_rollSpeed = nullptr; // session 95
    QComboBox* m_language = nullptr;
    QSpinBox* m_borderWidth = nullptr;
    QPushButton* m_borderColour = nullptr;
    QString m_borderColor = QStringLiteral("#FF3B82F6");
    QHash<QString, QRadioButton*> m_materials;
    QSlider* m_transparency = nullptr;
    QWidget* m_restoreRow = nullptr;
    ContextMenuEditor* m_menuEditor = nullptr;
    bool m_updating = false;
    bool m_dark = false;
};

} // namespace starfish::ui
