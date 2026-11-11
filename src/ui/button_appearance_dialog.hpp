#pragma once

// Session 110: appearance settings that apply to a Bouton, kept apart from
// the classic zone dialog (title bar, roll-up, grid flow, ...). New
// bouton-only options should be added here so the two dialogs stay
// specialised (docs/state/decisions/zone-shapes.md).

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <functional>

#include "core/jsonstore.hpp"
#include "ui/fence_container.hpp"
#include "ui/zone_shape.hpp"
#include "ui/button_skin.hpp"
#include "ui/fence_material.hpp"
#include "ui/percent_slider.hpp"
#include "ui/recent_colours.hpp"

namespace starfish::ui {

inline void paintButtonAppearanceSwatch(QPushButton* button, const QString& colour)
{
    const QColor c(colour);
    button->setText(c.isValid() ? QColor(c.red(), c.green(), c.blue()).name().toUpper() : QString());
    button->setStyleSheet(c.isValid() ? QStringLiteral("QPushButton { background: %1; color: %2; }")
                                            .arg(QColor(c.red(), c.green(), c.blue()).name(),
                                                 c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#FFF"))
                                      : QString());
}

inline QDialog* createButtonAppearanceDialog(FenceContainer* container, const QString& id,
                                             QWidget* parent = nullptr)
{
    const core::FenceStyle before = container->fenceStyle(id);
    const int defaultPercent = PercentSlider::percentFromAlpha(container->defaultFenceAlpha());
    QPointer<FenceContainer> safe(container);

    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("buttonAppearanceDialog"));
    dialog->setWindowTitle(QCoreApplication::translate("ButtonAppearanceDialog", "Button appearance"));
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    auto* material = new QComboBox(dialog);
    material->setObjectName(QStringLiteral("material"));
    for (const QString& preset : FenceMaterial::presetIds())
        material->addItem(FenceMaterial::presetLabel(preset), preset);
    material->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Custom"), QString());
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Material"), material);

    auto* useDefault = new QCheckBox(QCoreApplication::translate("ButtonAppearanceDialog", "Use the default transparency (%1)")
                                         .arg(PercentSlider::percentText(defaultPercent)),
                                     dialog);
    useDefault->setObjectName(QStringLiteral("useDefault"));
    QSlider* slider = nullptr;
    QWidget* sliderRow = PercentSlider::make(0, dialog, &slider, nullptr);
    slider->setObjectName(QStringLiteral("transparencySlider"));
    auto* transparency = new QVBoxLayout;
    transparency->addWidget(useDefault);
    transparency->addWidget(sliderRow);
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Transparency"), transparency);

    auto* effect = new QComboBox(dialog);
    effect->setObjectName(QStringLiteral("effect"));
    effect->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Plain"), QString());
    effect->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Acrylic (blurred desktop behind)"),
                    QStringLiteral("acrylic"));
    effect->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Textured (fine grain)"),
                    QStringLiteral("textured"));
    effect->setToolTip(QCoreApplication::translate("ButtonAppearanceDialog",
                                                   "Acrylic shows through a transparent button."));
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Effect"), effect);

    auto* fadeIcons = new QCheckBox(QCoreApplication::translate("ButtonAppearanceDialog", "The material on the icons too"),
                                    dialog);
    fadeIcons->setObjectName(QStringLiteral("fadeIcons"));
    form->addRow(QString(), fadeIcons);

    auto* panel = new QPushButton(dialog);
    panel->setObjectName(QStringLiteral("panelColour"));
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Background colour"), panel);

    auto* hoverCombo = new QComboBox(dialog);
    hoverCombo->setObjectName(QStringLiteral("hoverEffect"));
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Default (Settings)"), QString());
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Nothing"), QStringLiteral("none"));
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Lighter"), QStringLiteral("lighter"));
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Less transparent"), QStringLiteral("opacity"));
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Tinted with the border colour"),
                        QStringLiteral("colour"));
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Border hidden"), QStringLiteral("noborder"));
    hoverCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Glow"), QStringLiteral("glow"));
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "On hover"), hoverCombo);

    auto* borderWidth = new QComboBox(dialog);
    borderWidth->setObjectName(QStringLiteral("borderWidth"));
    borderWidth->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Default (%1 px)")
                             .arg(container->defaultBorderWidth()), -1);
    for (int px = 0; px <= 12; ++px)
        borderWidth->addItem(px == 0 ? QCoreApplication::translate("ButtonAppearanceDialog", "None")
                                     : QStringLiteral("%1 px").arg(px), px);
    auto* borderColour = new QPushButton(dialog);
    borderColour->setObjectName(QStringLiteral("borderColour"));
    auto* borderDefault = new QCheckBox(QCoreApplication::translate("ButtonAppearanceDialog", "Default"), dialog);
    borderDefault->setObjectName(QStringLiteral("borderColourDefault"));
    auto* borderRow = new QHBoxLayout;
    borderRow->addWidget(borderWidth, 1);
    borderRow->addWidget(borderColour, 1);
    borderRow->addWidget(borderDefault);
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Border"), borderRow);

    auto* iconSize = new QComboBox(dialog);
    iconSize->setObjectName(QStringLiteral("iconSize"));
    for (int px : {32, 40, 48, 64, 80, 96})
        iconSize->addItem(QStringLiteral("%1 px").arg(px), px);
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Icon size"), iconSize);

    auto* collapsedDiameter = new QComboBox(dialog);
    collapsedDiameter->setObjectName(QStringLiteral("collapsedDiameter"));
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Diameter"), collapsedDiameter);

    auto* ringCount = new QComboBox(dialog);
    ringCount->setObjectName(QStringLiteral("ringCount"));
    ringCount->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Automatic"), 0);
    for (int n = 1; n <= zone_shape::kButtonRingMax; ++n)
        ringCount->addItem(QString::number(n), n);
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Ring count"), ringCount);

    auto* skinCombo = new QComboBox(dialog);
    skinCombo->setObjectName(QStringLiteral("buttonSkin"));
    skinCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Simple"),
                       QString::fromUtf8(zone_shape::kButtonSkinPlain));
    skinCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Shutter (demo)"),
                       QString::fromUtf8(zone_shape::kButtonSkinShutter));
    skinCombo->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Animated GIF..."),
                       QString::fromUtf8(zone_shape::kButtonSkinGif));
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Skin"), skinCombo);

    auto* skinLayer = new QComboBox(dialog);
    skinLayer->setObjectName(QStringLiteral("buttonSkinLayer"));
    skinLayer->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Behind the icons"),
                       QString::fromUtf8(zone_shape::kButtonSkinLayerBehind));
    skinLayer->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "In front of the icons"),
                       QString::fromUtf8(zone_shape::kButtonSkinLayerFront));
    form->addRow(QCoreApplication::translate("ButtonAppearanceDialog", "Image position"), skinLayer);

    auto* showShortcutNames = new QCheckBox(
        QCoreApplication::translate("ButtonAppearanceDialog", "Show shortcut names"), dialog);
    showShortcutNames->setObjectName(QStringLiteral("showShortcutNames"));
    form->addRow(showShortcutNames);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);

    auto refresh = [safe, id, material, useDefault, slider, fadeIcons, panel, defaultPercent,
                    borderWidth, borderColour, borderDefault, effect, hoverCombo, iconSize,
                    collapsedDiameter, ringCount, skinCombo, skinLayer, showShortcutNames] {
        if (!safe || !safe->fenceIds().contains(id))
            return;
        const QSignalBlocker b1(material), b2(useDefault), b3(slider), b4(fadeIcons),
            b7(borderWidth), b8(borderDefault), b10(effect), b15(hoverCombo), b16(iconSize),
            b17(showShortcutNames), b18(collapsedDiameter), b19(ringCount),
            b20(skinCombo), b21(skinLayer);
        const core::FenceStyle style = safe->fenceStyle(id);
        const int at = material->findData(safe->fenceMaterial(id));
        material->setCurrentIndex(at >= 0 ? at : material->count() - 1);
        const bool followed = safe->fenceFollowsDefaultOpacity(id);
        useDefault->setChecked(followed);
        slider->setEnabled(!followed);
        slider->setValue(followed ? defaultPercent : PercentSlider::percentFromAlpha(safe->fenceEffectiveAlpha(id)));
        fadeIcons->setChecked(style.fadeIcons);
        fadeIcons->setEnabled(FenceMaterial::isTranslucent(safe->fenceEffectiveAlpha(id)));
        paintButtonAppearanceSwatch(panel, style.background);
        const int ownBorder = style.borderWidth < 0 ? -1 : style.borderWidth;
        int bw = borderWidth->findData(ownBorder);
        if (bw < 0)
            bw = borderWidth->findData(-1);
        borderWidth->setCurrentIndex(qMax(0, bw));
        borderDefault->setChecked(style.borderColor.isEmpty());
        paintButtonAppearanceSwatch(borderColour, style.borderColor.isEmpty() ? safe->defaultBorderColor()
                                                                              : style.borderColor);
        effect->setCurrentIndex(qMax(0, effect->findData(style.effect)));
        hoverCombo->setCurrentIndex(qMax(0, hoverCombo->findData(style.hoverEffect)));
        const int iconPx = style.iconSize > 0 ? style.iconSize : 48;
        int ic = iconSize->findData(iconPx);
        if (ic < 0) {
            iconSize->addItem(QStringLiteral("%1 px").arg(iconPx), iconPx);
            ic = iconSize->count() - 1;
        }
        iconSize->setCurrentIndex(ic);
        const int iconPxNow = style.iconSize > 0 ? style.iconSize : 48;
        const int defaultDiam = zone_shape::buttonCollapsedDiameter(iconPxNow, 0);
        collapsedDiameter->clear();
        collapsedDiameter->addItem(QCoreApplication::translate("ButtonAppearanceDialog", "Default (%1 px)")
                                       .arg(defaultDiam), 0);
        for (int px = zone_shape::kButtonDiameterMin; px <= zone_shape::kButtonDiameterMax;
             px += zone_shape::kButtonDiameterStep) {
            collapsedDiameter->addItem(QStringLiteral("%1 px").arg(px), px);
        }
        const int storedDiam = style.buttonDiameter > 0 ? style.buttonDiameter : 0;
        int di = collapsedDiameter->findData(storedDiam);
        if (di < 0 && storedDiam > 0) {
            collapsedDiameter->addItem(QStringLiteral("%1 px").arg(storedDiam), storedDiam);
            di = collapsedDiameter->count() - 1;
        }
        collapsedDiameter->setCurrentIndex(qMax(0, di));
        int ri = ringCount->findData(style.buttonRings > 0 ? style.buttonRings : 0);
        ringCount->setCurrentIndex(qMax(0, ri));
        QString skinId = style.buttonSkin;
        if (skinId.isEmpty())
            skinId = QString::fromUtf8(zone_shape::kButtonSkinPlain);
        int si = skinCombo->findData(skinId);
        if (si < 0)
            si = skinCombo->findData(QString::fromUtf8(zone_shape::kButtonSkinPlain));
        skinCombo->setCurrentIndex(qMax(0, si));
        const int li = skinLayer->findData(style.buttonSkinLayer == QLatin1String("front")
                                               ? QString::fromUtf8(zone_shape::kButtonSkinLayerFront)
                                               : QString::fromUtf8(zone_shape::kButtonSkinLayerBehind));
        skinLayer->setCurrentIndex(qMax(0, li));
        showShortcutNames->setChecked(style.showLabels != QLatin1String("off"));
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
        if (safe && !useDefault->isChecked())
            safe->setFenceOpacity(id, PercentSlider::alphaFromPercent(percent));
    });
    QObject::connect(slider, &QSlider::sliderReleased, dialog, refresh);
    QObject::connect(fadeIcons, &QCheckBox::toggled, dialog, [safe, id, refresh](bool on) {
        if (safe)
            safe->setFenceIconsFollowMaterial(id, on);
        refresh();
    });
    QObject::connect(panel, &QPushButton::clicked, dialog, [safe, id, dialog, restyle] {
        if (!safe)
            return;
        const QColor picked = RecentColours::pick(QColor(safe->fenceStyle(id).background),
                                                  QCoreApplication::translate("ButtonAppearanceDialog", "Background colour"),
                                                  dialog);
        if (picked.isValid())
            restyle([&picked](core::FenceStyle& style) { style.background = picked.name(QColor::HexArgb).toUpper(); });
    });
    QObject::connect(hoverCombo, &QComboBox::currentIndexChanged, dialog, [hoverCombo, restyle](int) {
        const QString chosen = hoverCombo->currentData().toString();
        restyle([chosen](core::FenceStyle& style) { style.hoverEffect = chosen; });
    });
    QObject::connect(effect, &QComboBox::currentIndexChanged, dialog, [effect, restyle](int) {
        const QString chosen = effect->currentData().toString();
        restyle([chosen](core::FenceStyle& style) { style.effect = chosen; });
    });
    QObject::connect(iconSize, &QComboBox::currentIndexChanged, dialog, [iconSize, restyle](int) {
        const int px = iconSize->currentData().toInt();
        restyle([px](core::FenceStyle& style) { style.iconSize = px; });
    });
    QObject::connect(collapsedDiameter, &QComboBox::currentIndexChanged, dialog, [collapsedDiameter, restyle](int) {
        const int px = collapsedDiameter->currentData().toInt();
        restyle([px](core::FenceStyle& style) { style.buttonDiameter = px; });
    });
    QObject::connect(ringCount, &QComboBox::currentIndexChanged, dialog, [ringCount, restyle](int) {
        const int n = ringCount->currentData().toInt();
        restyle([n](core::FenceStyle& style) { style.buttonRings = n; });
    });
    QObject::connect(skinCombo, &QComboBox::currentIndexChanged, dialog,
                     [safe, id, skinCombo, dialog, restyle, refresh](int) {
        const QString chosen = skinCombo->currentData().toString();
        if (chosen == QLatin1String(zone_shape::kButtonSkinGif)) {
            const core::FenceStyle current = safe ? safe->fenceStyle(id) : core::FenceStyle{};
            const QString start = current.buttonSkinPath.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                : QFileInfo(current.buttonSkinPath).absolutePath();
            const QString file = QFileDialog::getOpenFileName(
                dialog,
                QCoreApplication::translate("ButtonAppearanceDialog", "Choose an animated GIF"),
                start,
                QCoreApplication::translate("ButtonAppearanceDialog", "GIF images (*.gif)"));
            if (file.isEmpty()) {
                refresh();
                return;
            }
            restyle([file](core::FenceStyle& style) {
                style.buttonSkin = QString::fromUtf8(zone_shape::kButtonSkinGif);
                style.buttonSkinPath = file;
            });
            return;
        }
        restyle([chosen](core::FenceStyle& style) {
            style.buttonSkin = (chosen == QLatin1String(zone_shape::kButtonSkinPlain))
                                   ? QString()
                                   : chosen;
        });
    });
    QObject::connect(skinLayer, &QComboBox::currentIndexChanged, dialog, [skinLayer, restyle](int) {
        const QString layer = skinLayer->currentData().toString();
        restyle([layer](core::FenceStyle& style) {
            style.buttonSkinLayer = (layer == QLatin1String(zone_shape::kButtonSkinLayerFront))
                                        ? layer
                                        : QString();
        });
    });
    QObject::connect(showShortcutNames, &QCheckBox::toggled, dialog, [restyle](bool on) {
        restyle([on](core::FenceStyle& style) {
            style.showLabels = on ? QStringLiteral("on") : QStringLiteral("off");
        });
    });
    QObject::connect(borderWidth, &QComboBox::currentIndexChanged, dialog, [borderWidth, restyle](int) {
        const int px = borderWidth->currentData().toInt();
        restyle([px](core::FenceStyle& style) { style.borderWidth = px; });
    });
    QObject::connect(borderColour, &QPushButton::clicked, dialog, [safe, id, dialog, restyle] {
        if (!safe)
            return;
        const core::FenceStyle style = safe->fenceStyle(id);
        const QColor picked = RecentColours::pick(
            QColor(style.borderColor.isEmpty() ? safe->defaultBorderColor() : style.borderColor),
            QCoreApplication::translate("ButtonAppearanceDialog", "Border colour"), dialog);
        if (picked.isValid())
            restyle([&picked](core::FenceStyle& s) { s.borderColor = picked.name(QColor::HexArgb).toUpper(); });
    });
    QObject::connect(borderDefault, &QCheckBox::toggled, dialog, [safe, restyle](bool same) {
        const QString fallback = safe ? safe->defaultBorderColor() : QString();
        restyle([same, fallback](core::FenceStyle& style) { style.borderColor = same ? QString() : fallback; });
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
