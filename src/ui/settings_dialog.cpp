#include "ui/settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "ui/fence_container.hpp"
#include "ui/fence_material.hpp"

namespace starfish::ui {

SettingsDialog::SettingsDialog(FenceContainer* container, QWidget* parent)
    : QDialog(parent)
    , m_container(container)
{
    setWindowTitle(tr("StarFish settings"));
    resize(480, 400);

    // --- fences -------------------------------------------------------------
    auto* fencesGroup = new QGroupBox(tr("Fences"), this);
    m_list = new QListWidget(fencesGroup);
    // Object names are the probe's handles into this window (there is no API to
    // click a widget from a script otherwise): `--product-probe` drives the
    // material combo and the list exactly as a user would.
    m_list->setObjectName(QStringLiteral("fenceList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* newButton = new QPushButton(tr("New fence"), fencesGroup);
    m_rename = new QPushButton(tr("Rename..."), fencesGroup);
    m_rollUp = new QPushButton(tr("Roll up"), fencesGroup);
    m_delete = new QPushButton(tr("Delete"), fencesGroup);

    auto* buttonColumn = new QVBoxLayout;
    buttonColumn->addWidget(newButton);
    buttonColumn->addWidget(m_rename);
    buttonColumn->addWidget(m_rollUp);
    buttonColumn->addWidget(m_delete);
    buttonColumn->addStretch(1);

    auto* fencesLayout = new QHBoxLayout;
    fencesLayout->addWidget(m_list, 1);
    fencesLayout->addLayout(buttonColumn);

    // F-038 / T-057: the material of the *selected* fence. The tray applies one
    // material to every fence at once; this is the only place a single fence is
    // chosen, which is why the choice lives here rather than disappearing with
    // the rest of this window (T-059 keeps what survives).
    m_material = new QComboBox(fencesGroup);
    m_material->setObjectName(QStringLiteral("fenceMaterial")); // the probe's handle
    for (const FenceMaterial::Preset& preset : FenceMaterial::presets())
        m_material->addItem(FenceMaterial::presetLabel(QString::fromLatin1(preset.id)),
                            QString::fromLatin1(preset.id));
    auto* materialRow = new QHBoxLayout;
    materialRow->addWidget(new QLabel(tr("Material:"), fencesGroup));
    materialRow->addWidget(m_material, 1);

    auto* fencesBox = new QVBoxLayout(fencesGroup);
    fencesBox->addLayout(fencesLayout, 1);
    fencesBox->addLayout(materialRow);

    // --- desktop ------------------------------------------------------------
    auto* desktopGroup = new QGroupBox(tr("Desktop"), this);
    m_hideIcons = new QCheckBox(tr("Hide the desktop icons while StarFish runs"), desktopGroup);
    m_status = new QLabel(desktopGroup);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* desktopLayout = new QVBoxLayout(desktopGroup);
    desktopLayout->addWidget(m_hideIcons);
    desktopLayout->addWidget(m_status);
    desktopLayout->addStretch(1);

    auto* closeButton = new QPushButton(tr("Close"), this);
    auto* bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeButton);

    auto* root = new QVBoxLayout(this);
    root->addWidget(fencesGroup, 1);
    root->addWidget(desktopGroup);
    root->addLayout(bottomRow);

    // --- wiring -------------------------------------------------------------
    connect(newButton, &QPushButton::clicked, this, &SettingsDialog::newFenceRequested);
    connect(m_rename, &QPushButton::clicked, this, &SettingsDialog::onRenameClicked);
    connect(m_rollUp, &QPushButton::clicked, this, &SettingsDialog::onRollUpClicked);
    connect(m_delete, &QPushButton::clicked, this, &SettingsDialog::onDeleteClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &SettingsDialog::updateButtons);
    connect(m_material, &QComboBox::currentIndexChanged, this, [this](int) {
        // Only a real choice travels: the combo is also set programmatically when
        // the selection or the document changes (see updateMaterialCombo), and a
        // signal that bounced back out would write the document on every refresh.
        const QString id = selectedId();
        const QString material = m_material->currentData().toString();
        if (id.isEmpty() || material.isEmpty())
            return;
        emit fenceMaterialRequested(id, material);
    });
    connect(m_hideIcons, &QCheckBox::toggled, this, &SettingsDialog::desktopIconsToggled);
    // Closing hides rather than destroys: the tray icon brings the window back,
    // and quitting StarFish is the tray's job.
    connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);

    refresh();
}

void SettingsDialog::refresh()
{
    const QString previous = selectedId();

    m_list->clear();
    if (m_container) {
        const QStringList ids = m_container->fenceIds();
        for (const QString& id : ids) {
            const QRect r = m_container->fenceRect(id);
            auto* item = new QListWidgetItem(
                tr("%1 -- %2x%3 at %4,%5%6")
                    .arg(m_container->fenceTitle(id))
                    .arg(r.width())
                    .arg(r.height())
                    .arg(r.x())
                    .arg(r.y())
                    .arg(m_container->fenceRolledUp(id) ? tr("  (rolled up)") : QString()));
            item->setData(Qt::UserRole, id); // the id, never the label
            m_list->addItem(item);
            if (id == previous)
                m_list->setCurrentItem(item);
        }
    }
    updateButtons();
}

QString SettingsDialog::selectedId() const
{
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void SettingsDialog::onRenameClicked()
{
    const QString id = selectedId();
    if (id.isEmpty() || !m_container)
        return;

    bool accepted = false;
    const QString title = QInputDialog::getText(this, tr("Rename fence"), tr("Name:"),
                                                QLineEdit::Normal,
                                                m_container->fenceTitle(id), &accepted);
    if (accepted && !title.isEmpty())
        emit renameFenceRequested(id, title);
}

void SettingsDialog::onRollUpClicked()
{
    const QString id = selectedId();
    if (id.isEmpty() || !m_container)
        return;
    emit rollUpToggled(id, !m_container->fenceRolledUp(id));
}

void SettingsDialog::onDeleteClicked()
{
    const QString id = selectedId();
    if (id.isEmpty() || !m_container)
        return;

    const auto answer = QMessageBox::question(
        this, tr("Delete fence"),
        tr("Delete the fence \"%1\"?\n\nThe files stay on the disk: StarFish only ever "
           "decides where they are shown.")
            .arg(m_container->fenceTitle(id)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes)
        emit removeFenceRequested(id);
}

void SettingsDialog::updateButtons()
{
    const QString id = selectedId();
    const bool hasSelection = !id.isEmpty();

    m_rename->setEnabled(hasSelection);
    m_delete->setEnabled(hasSelection);
    m_rollUp->setEnabled(hasSelection);
    if (hasSelection && m_container)
        m_rollUp->setText(m_container->fenceRolledUp(id) ? tr("Roll down") : tr("Roll up"));

    updateMaterialCombo();
}

void SettingsDialog::updateMaterialCombo()
{
    const QString id = selectedId();
    // The combo follows the container; it never leads. Blocked so that putting
    // the document's state on screen is not mistaken for the user choosing it.
    const QSignalBlocker blocker(m_material);

    // Drop the "Custom" entry another selection may have added: it describes one
    // fence's colour, so it must not survive into the next one.
    while (m_material->count() > static_cast<int>(FenceMaterial::presets().size()))
        m_material->removeItem(m_material->count() - 1);

    if (id.isEmpty() || !m_container) {
        m_material->setEnabled(false);
        m_material->setCurrentIndex(-1);
        return;
    }

    m_material->setEnabled(true);
    const QString material = m_container->fenceMaterial(id);
    int index = material.isEmpty() ? -1 : m_material->findData(material);
    if (index < 0) {
        // An alpha that is nobody's preset (a hand-written document): say so
        // instead of rounding it to the nearest preset and lying about it.
        m_material->addItem(tr("Custom"), QString());
        index = m_material->count() - 1;
    }
    m_material->setCurrentIndex(index);
}

void SettingsDialog::setDesktopIconsHidden(bool hidden)
{
    // Blocked: this reflects a state that was changed elsewhere, so it must not
    // bounce back out as a fresh user request.
    const QSignalBlocker blocker(m_hideIcons);
    m_hideIcons->setChecked(hidden);
}

bool SettingsDialog::desktopIconsHidden() const
{
    return m_hideIcons->isChecked();
}

void SettingsDialog::setStatusText(const QString& text)
{
    m_status->setText(text);
}

} // namespace starfish::ui
