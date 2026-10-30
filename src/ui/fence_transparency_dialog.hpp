#pragma once

// T-098 (session 63): a fence's own transparency -- the fence menu's
// "Transparency..." entry. A slider with its number, and "Use the default".
// Every move shows on the fence at once; Cancel puts back what was there.

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QSlider>
#include <QVBoxLayout>

#include "core/jsonstore.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_material.hpp"
#include "ui/percent_slider.hpp"

namespace starfish::ui {

inline QDialog* createFenceTransparencyDialog(FenceContainer* container, const QString& id, QWidget* parent = nullptr)
{
    const bool followed = container->fenceFollowsDefaultOpacity(id);
    const core::FenceStyle before = container->fenceStyle(id);
    const int defaultPercent = PercentSlider::percentFromAlpha(container->defaultFenceAlpha());

    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("fenceTransparencyDialog"));
    dialog->setWindowTitle(QCoreApplication::translate("FenceTransparencyDialog", "Fence transparency"));
    auto* layout = new QVBoxLayout(dialog);
    auto* useDefault = new QCheckBox(QCoreApplication::translate("FenceTransparencyDialog", "Use the default setting (%1)").arg(PercentSlider::percentText(defaultPercent)),
                                     dialog);
    useDefault->setObjectName(QStringLiteral("useDefault"));
    useDefault->setChecked(followed);
    layout->addWidget(useDefault);
    QSlider* slider = nullptr;
    layout->addWidget(PercentSlider::make(PercentSlider::percentFromAlpha(container->fenceEffectiveAlpha(id)), dialog,
                                          &slider, nullptr));
    slider->setEnabled(!followed);
    auto* hint = new QLabel(QCoreApplication::translate("FenceTransparencyDialog", "Background and title bar. Icons and their names stay opaque."), dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);

    QPointer<FenceContainer> safe(container);
    QObject::connect(slider, &QSlider::valueChanged, dialog, [safe, id, useDefault](int percent) {
        if (safe && !useDefault->isChecked())
            safe->setFenceOpacity(id, PercentSlider::alphaFromPercent(percent));
    });
    QObject::connect(useDefault, &QCheckBox::toggled, dialog, [safe, id, slider, defaultPercent](bool on) {
        slider->setEnabled(!on);
        if (!safe)
            return;
        if (on) {
            safe->setFenceOpacity(id, -1);
            slider->blockSignals(true);
            slider->setValue(defaultPercent);
            slider->blockSignals(false);
        } else {
            safe->setFenceOpacity(id, PercentSlider::alphaFromPercent(slider->value()));
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(dialog, &QDialog::rejected, dialog, [safe, id, before] {
        if (safe && safe->fenceIds().contains(id))
            safe->setFenceStyle(id, before);
    });
    return dialog;
}

} // namespace starfish::ui
