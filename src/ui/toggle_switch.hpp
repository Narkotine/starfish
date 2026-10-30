#pragma once

#include <QCheckBox>

namespace starfish::ui {

// Windows 11's on/off switch: a pill with a knob, its label on the right. A
// QCheckBox underneath (keyboard, accessibility, `toggled`), drawn by hand
// because a style sheet cannot draw the knob.
class ToggleSwitch : public QCheckBox
{
public:
    explicit ToggleSwitch(const QString& text = {}, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override { return rect().contains(pos); }
};

} // namespace starfish::ui
