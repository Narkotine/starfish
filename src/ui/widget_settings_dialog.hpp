#pragma once

#include <QDialog>

#include "core/desktop_item.hpp"

class QCheckBox;
class QLabel;
class QComboBox;
class QSpinBox;

namespace starfish::ui {

// Session 88: a widget's settings, in a window of their own (opened by
// "Configure widget..." in the widget's menu). One dialog for every built-in
// widget: the size first, then what the type offers -- the clock's toggles,
// a system widget's used/free, display, unit and graph history, the group's
// metrics. Only the keys it shows are written back; any other key in the
// widget's settings (a newer StarFish's) is kept as it was.
class WidgetSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit WidgetSettingsDialog(const core::DesktopItem& widget, QWidget* parent = nullptr);
    // The widget with the choices applied.
    core::DesktopItem result() const;
    // Whether the dialog has anything beyond the size to offer for `type`.
    static bool hasOptions(const QString& type);
    // Session 95: the picture chooser (image widget); empty when cancelled.
    static QString chooseImage(QWidget* parent, const QString& current);
    void setImagePath(const QString& path); // the probe, without the file dialog

private:
    void updateEnabled();

    core::DesktopItem m_widget;
    QComboBox* m_size = nullptr;
    // clock
    QCheckBox* m_seconds = nullptr;
    QCheckBox* m_date = nullptr;
    QCheckBox* m_h12 = nullptr;
    // image (session 95)
    QString m_imagePath;
    QLabel* m_imageLabel = nullptr;
    QComboBox* m_fit = nullptr;
    // system widgets
    QComboBox* m_show = nullptr;
    QComboBox* m_mode = nullptr;
    QComboBox* m_unit = nullptr;
    QSpinBox* m_history = nullptr;
    QSpinBox* m_interval = nullptr;
    QList<QCheckBox*> m_metrics; // the group's, in core::Metric order
};

} // namespace starfish::ui
