#include "ui/settings_dialog.hpp"

#include <QCheckBox>
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

    auto* fencesLayout = new QHBoxLayout(fencesGroup);
    fencesLayout->addWidget(m_list, 1);
    fencesLayout->addLayout(buttonColumn);

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
