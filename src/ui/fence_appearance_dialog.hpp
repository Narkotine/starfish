#pragma once

// Session 65: a fence's look in one place -- the fence menu keeps the
// behaviour (the user: "moins d'options esthétiques en accès direct au clic
// droit sur fence"). Material, transparency, icons, corners, colours. Every
// change shows at once; Cancel puts the fence back as it was.

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <functional>

#include "core/jsonstore.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_material.hpp"
#include "ui/fence_menu.hpp"
#include "ui/percent_slider.hpp"
#include "ui/recent_colours.hpp"

namespace starfish::ui {

inline void paintSwatch(QPushButton* button, const QString& colour)
{
    const QColor c(colour);
    button->setText(c.isValid() ? QColor(c.red(), c.green(), c.blue()).name().toUpper() : QString());
    button->setStyleSheet(c.isValid() ? QStringLiteral("QPushButton { background: %1; color: %2; }")
                                            .arg(QColor(c.red(), c.green(), c.blue()).name(),
                                                 c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#FFF"))
                                      : QString());
}

inline QDialog* createFenceAppearanceDialog(FenceContainer* container, const QString& id, QWidget* parent = nullptr)
{
    const core::FenceStyle before = container->fenceStyle(id);
    const int defaultPercent = PercentSlider::percentFromAlpha(container->defaultFenceAlpha());
    QPointer<FenceContainer> safe(container);

    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("fenceAppearanceDialog"));
    dialog->setWindowTitle(QCoreApplication::translate("FenceAppearanceDialog", "Fence appearance"));
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    // Material.
    auto* material = new QComboBox(dialog);
    material->setObjectName(QStringLiteral("material"));
    for (const QString& preset : FenceMaterial::presetIds())
        material->addItem(FenceMaterial::presetLabel(preset), preset);
    material->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Custom"), QString());
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Material"), material);

    // Transparency: the default, or the fence's own.
    auto* useDefault = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "Use the default transparency (%1)")
                                         .arg(PercentSlider::percentText(defaultPercent)),
                                     dialog);
    useDefault->setObjectName(QStringLiteral("useDefault"));
    QSlider* slider = nullptr;
    QWidget* sliderRow = PercentSlider::make(0, dialog, &slider, nullptr);
    slider->setObjectName(QStringLiteral("transparencySlider"));
    auto* transparency = new QVBoxLayout;
    transparency->addWidget(useDefault);
    transparency->addWidget(sliderRow);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Transparency"), transparency);

    auto* fadeIcons = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "The material on the icons too"), dialog);
    fadeIcons->setObjectName(QStringLiteral("fadeIcons"));
    form->addRow(QString(), fadeIcons);

    auto* radius = new QComboBox(dialog);
    radius->setObjectName(QStringLiteral("radius"));
    for (int px : FenceMenu::radiusPresets())
        radius->addItem(FenceMenu::radiusLabel(px), px);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Corners"), radius);

    auto* panel = new QPushButton(dialog);
    panel->setObjectName(QStringLiteral("panelColour"));
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Panel colour"), panel);

    auto* bar = new QPushButton(dialog);
    bar->setObjectName(QStringLiteral("titleBarColour"));
    auto* sameAsPanel = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "Same as the panel"), dialog);
    sameAsPanel->setObjectName(QStringLiteral("titleBarSameAsPanel"));
    auto* barRow = new QHBoxLayout;
    barRow->addWidget(bar, 1);
    barRow->addWidget(sameAsPanel);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Title bar colour"), barRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);

    // Every control shows what the fence *is* -- read back after each change,
    // since the material, the transparency and the panel colour are one alpha.
    auto refresh = [safe, id, material, useDefault, slider, fadeIcons, radius, panel, bar, sameAsPanel,
                    defaultPercent] {
        if (!safe || !safe->fenceIds().contains(id))
            return;
        const QSignalBlocker b1(material), b2(useDefault), b3(slider), b4(fadeIcons), b5(radius), b6(sameAsPanel);
        const core::FenceStyle style = safe->fenceStyle(id);
        const int at = material->findData(safe->fenceMaterial(id));
        material->setCurrentIndex(at >= 0 ? at : material->count() - 1);
        const bool followed = safe->fenceFollowsDefaultOpacity(id);
        useDefault->setChecked(followed);
        slider->setEnabled(!followed);
        slider->setValue(followed ? defaultPercent : PercentSlider::percentFromAlpha(safe->fenceEffectiveAlpha(id)));
        fadeIcons->setChecked(style.fadeIcons);
        fadeIcons->setEnabled(FenceMaterial::isTranslucent(safe->fenceEffectiveAlpha(id)));
        int r = radius->findData(style.radius);
        if (r < 0) {
            radius->addItem(FenceMenu::radiusLabel(style.radius), style.radius);
            r = radius->count() - 1;
        }
        radius->setCurrentIndex(r);
        paintSwatch(panel, style.background);
        sameAsPanel->setChecked(style.titleBarColor.isEmpty());
        paintSwatch(bar, style.titleBarColor.isEmpty() ? style.background : style.titleBarColor);
    };
    auto restyle = [safe, id, refresh](const std::function<void(core::FenceStyle&)>& change) {
        if (!safe || !safe->fenceIds().contains(id))
            return;
        core::FenceStyle style = safe->fenceStyle(id);
        change(style);
        safe->setFenceStyle(id, style);
        refresh();
    };
    refresh();

    QObject::connect(material, &QComboBox::currentIndexChanged, dialog, [safe, id, material, refresh](int) {
        const QString preset = material->currentData().toString();
        if (safe && !preset.isEmpty())
            safe->setFenceMaterial(id, preset);
        refresh();
    });
    QObject::connect(useDefault, &QCheckBox::toggled, dialog, [safe, id, slider, refresh](bool on) {
        if (safe)
            safe->setFenceOpacity(id, on ? -1 : PercentSlider::alphaFromPercent(slider->value()));
        refresh();
    });
    QObject::connect(slider, &QSlider::valueChanged, dialog, [safe, id, useDefault](int percent) {
        // No refresh while dragging: the slider would fight its own value.
        if (safe && !useDefault->isChecked())
            safe->setFenceOpacity(id, PercentSlider::alphaFromPercent(percent));
    });
    QObject::connect(slider, &QSlider::sliderReleased, dialog, refresh);
    QObject::connect(fadeIcons, &QCheckBox::toggled, dialog, [safe, id, refresh](bool on) {
        if (safe)
            safe->setFenceIconsFollowMaterial(id, on);
        refresh();
    });
    QObject::connect(radius, &QComboBox::currentIndexChanged, dialog, [radius, restyle](int) {
        const int px = radius->currentData().toInt();
        restyle([px](core::FenceStyle& style) { style.radius = px; });
    });
    QObject::connect(panel, &QPushButton::clicked, dialog, [safe, id, dialog, restyle] {
        if (!safe)
            return;
        const QColor picked = RecentColours::pick(QColor(safe->fenceStyle(id).background),
                                                  QCoreApplication::translate("FenceAppearanceDialog", "Panel colour"), dialog);
        if (picked.isValid())
            restyle([&picked](core::FenceStyle& style) { style.background = picked.name(QColor::HexArgb).toUpper(); });
    });
    QObject::connect(bar, &QPushButton::clicked, dialog, [safe, id, dialog, restyle] {
        if (!safe)
            return;
        const core::FenceStyle style = safe->fenceStyle(id);
        const QColor picked = RecentColours::pick(
            QColor(style.titleBarColor.isEmpty() ? style.background : style.titleBarColor),
            QCoreApplication::translate("FenceAppearanceDialog", "Title bar colour"), dialog);
        if (picked.isValid())
            restyle([&picked](core::FenceStyle& s) { s.titleBarColor = picked.name(QColor::HexArgb).toUpper(); });
    });
    QObject::connect(sameAsPanel, &QCheckBox::toggled, dialog, [restyle](bool same) {
        restyle([same](core::FenceStyle& style) {
            if (same)
                style.titleBarColor.clear();
            else if (style.titleBarColor.isEmpty())
                style.titleBarColor = style.background; // a band of its own, to be coloured
        });
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
