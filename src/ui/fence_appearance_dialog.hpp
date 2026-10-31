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
#include <QInputDialog>
#include <QLineEdit>
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
#include "ui/fence_templates.hpp"

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

    // T-099 (session 65): what the panel is made of beyond its colour.
    auto* effect = new QComboBox(dialog);
    effect->setObjectName(QStringLiteral("effect"));
    effect->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Plain"), QString());
    effect->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Acrylic (blurred desktop behind)"), QStringLiteral("acrylic"));
    effect->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Textured (fine grain)"), QStringLiteral("textured"));
    effect->setToolTip(QCoreApplication::translate("FenceAppearanceDialog", "Acrylic shows through a transparent fence."));
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Effect"), effect);

    auto* fadeIcons = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "The material on the icons too"), dialog);
    fadeIcons->setObjectName(QStringLiteral("fadeIcons"));
    form->addRow(QString(), fadeIcons);

    auto* radius = new QComboBox(dialog);
    radius->setObjectName(QStringLiteral("radius"));
    for (int px : FenceMenu::radiusPresets())
        radius->addItem(FenceMenu::radiusLabel(px), px);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Corners"), radius);

    // Session 66: the title bar has its own row -- shown or not, and where the
    // title sits in it.
    auto* titleBar = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "Show the title bar"), dialog);
    titleBar->setObjectName(QStringLiteral("titleBarShown"));
    titleBar->setToolTip(QCoreApplication::translate("FenceAppearanceDialog", "Hidden: the fence moves by the thin band at its top."));
    auto* titleAlign = new QComboBox(dialog);
    titleAlign->setObjectName(QStringLiteral("titleAlign"));
    titleAlign->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Title on the left"), QString());
    titleAlign->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Title centred"), QStringLiteral("center"));
    titleAlign->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Title on the right"), QStringLiteral("right"));
    auto* titleRow = new QHBoxLayout;
    titleRow->addWidget(titleBar);
    titleRow->addWidget(titleAlign, 1);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Title bar"), titleRow);

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

    // T-085: the room each icon gets.
    auto* cell = new QComboBox(dialog);
    cell->setObjectName(QStringLiteral("cellWidth"));
    cell->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Automatic (fits the names)"), 0);
    for (int px : {64, 76, 88, 100, 112, 128})
        cell->addItem(QStringLiteral("%1 px").arg(px), px);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Icon width"), cell);

    // T-094: the border, the default's or its own.
    auto* borderWidth = new QComboBox(dialog);
    borderWidth->setObjectName(QStringLiteral("borderWidth"));
    borderWidth->addItem(QCoreApplication::translate("FenceAppearanceDialog", "Default (%1 px)")
                             .arg(container->defaultBorderWidth()), -1);
    for (int px = 0; px <= 6; ++px)
        borderWidth->addItem(px == 0 ? QCoreApplication::translate("FenceAppearanceDialog", "None")
                                     : QStringLiteral("%1 px").arg(px), px);
    auto* borderColour = new QPushButton(dialog);
    borderColour->setObjectName(QStringLiteral("borderColour"));
    auto* borderDefault = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "Default"), dialog);
    borderDefault->setObjectName(QStringLiteral("borderColourDefault"));
    auto* borderRow = new QHBoxLayout;
    borderRow->addWidget(borderWidth);
    borderRow->addWidget(borderColour, 1);
    borderRow->addWidget(borderDefault);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Border"), borderRow);

    // Session 65: templates -- apply one, save this look as one, and the one
    // new fences get.
    auto* templates = new QComboBox(dialog);
    templates->setObjectName(QStringLiteral("templates"));
    auto* saveTemplate = new QPushButton(QCoreApplication::translate("FenceAppearanceDialog", "Save as template..."), dialog);
    saveTemplate->setObjectName(QStringLiteral("saveTemplate"));
    auto* forNew = new QCheckBox(QCoreApplication::translate("FenceAppearanceDialog", "For new fences"), dialog);
    forNew->setObjectName(QStringLiteral("templateForNewFences"));
    auto* templateRow = new QHBoxLayout;
    templateRow->addWidget(templates, 1);
    templateRow->addWidget(forNew);
    templateRow->addWidget(saveTemplate);
    form->addRow(QCoreApplication::translate("FenceAppearanceDialog", "Template"), templateRow);
    auto fillTemplates = [templates, forNew](const QString& select) {
        const QSignalBlocker b1(templates), b2(forNew);
        templates->clear();
        templates->addItem(QCoreApplication::translate("FenceAppearanceDialog", "(none)"), QString());
        for (const QString& name : FenceTemplates::names())
            templates->addItem(name, name);
        templates->setCurrentIndex(qMax(0, templates->findData(select)));
        forNew->setEnabled(!select.isEmpty());
        forNew->setChecked(!select.isEmpty() && FenceTemplates::defaultName() == select);
    };
    fillTemplates(QString());
    QObject::connect(forNew, &QCheckBox::toggled, dialog, [templates](bool on) {
        const QString name = templates->currentData().toString();
        if (on)
            FenceTemplates::setDefaultName(name);
        else if (FenceTemplates::defaultName() == name)
            FenceTemplates::setDefaultName(QString());
    });
    QObject::connect(saveTemplate, &QPushButton::clicked, dialog, [dialog, safe, id, fillTemplates] {
        if (!safe)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(dialog, QCoreApplication::translate("FenceAppearanceDialog", "Save as template"),
                                                   QCoreApplication::translate("FenceAppearanceDialog", "Name:"),
                                                   QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        FenceTemplates::save(name, safe->fenceStyle(id));
        fillTemplates(name);
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);

    // Every control shows what the fence *is* -- read back after each change,
    // since the material, the transparency and the panel colour are one alpha.
    auto refresh = [safe, id, material, useDefault, slider, fadeIcons, radius, panel, bar, sameAsPanel,
                    defaultPercent, borderWidth, borderColour, borderDefault, cell, effect, titleBar, titleAlign] {
        if (!safe || !safe->fenceIds().contains(id))
            return;
        const QSignalBlocker b1(material), b2(useDefault), b3(slider), b4(fadeIcons), b5(radius), b6(sameAsPanel),
            b7(borderWidth), b8(borderDefault), b9(cell), b10(effect), b11(titleBar), b12(titleAlign);
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
        borderWidth->setCurrentIndex(qMax(0, borderWidth->findData(style.borderWidth < 0 ? -1 : qMin(style.borderWidth, 6))));
        borderDefault->setChecked(style.borderColor.isEmpty());
        int c = cell->findData(style.cellWidth);
        if (c < 0) {
            cell->addItem(QStringLiteral("%1 px").arg(style.cellWidth), style.cellWidth);
            c = cell->count() - 1;
        }
        cell->setCurrentIndex(c);
        effect->setCurrentIndex(qMax(0, effect->findData(style.effect)));
        titleBar->setChecked(style.titleBar);
        titleAlign->setCurrentIndex(qMax(0, titleAlign->findData(style.titleAlign)));
        titleAlign->setEnabled(style.titleBar);
        paintSwatch(borderColour, style.borderColor.isEmpty() ? safe->defaultBorderColor() : style.borderColor);
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

    QObject::connect(templates, &QComboBox::currentIndexChanged, dialog, [templates, forNew, safe, id, refresh](int) {
        const QString name = templates->currentData().toString();
        {
            const QSignalBlocker b(forNew);
            forNew->setEnabled(!name.isEmpty());
            forNew->setChecked(!name.isEmpty() && FenceTemplates::defaultName() == name);
        }
        if (name.isEmpty() || !safe)
            return;
        safe->setFenceStyle(id, FenceTemplates::style(name));
        refresh();
    });
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
            QCoreApplication::translate("FenceAppearanceDialog", "Title bar colour"), dialog,
            /*alpha=*/false); // session 66: the band takes the fence's transparency
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
    QObject::connect(titleBar, &QCheckBox::toggled, dialog, [restyle](bool shown) {
        restyle([shown](core::FenceStyle& style) { style.titleBar = shown; });
    });
    QObject::connect(titleAlign, &QComboBox::currentIndexChanged, dialog, [titleAlign, restyle](int) {
        const QString chosen = titleAlign->currentData().toString();
        restyle([chosen](core::FenceStyle& style) { style.titleAlign = chosen; });
    });
    QObject::connect(effect, &QComboBox::currentIndexChanged, dialog, [effect, restyle](int) {
        const QString chosen = effect->currentData().toString();
        restyle([chosen](core::FenceStyle& style) { style.effect = chosen; });
    });
    QObject::connect(cell, &QComboBox::currentIndexChanged, dialog, [cell, restyle](int) {
        const int px = cell->currentData().toInt();
        restyle([px](core::FenceStyle& style) { style.cellWidth = px; });
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
            QCoreApplication::translate("FenceAppearanceDialog", "Border colour"), dialog);
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
