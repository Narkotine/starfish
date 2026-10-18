#include "ui/settings_dialog.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace starfish::ui {

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("StarFish settings"));
    resize(560, 320);

    // --- what is running ----------------------------------------------------
    // T-059: one line. The surface that used to hold the fences must not become a
    // second way to change them (a fence's own menu is the way, ADR-16) -- and it
    // cannot, because it holds no container and no widget that could write the
    // document. What the probe asks for by name is this label; "nothing else here
    // writes the document" is checked rather than assumed.
    auto* statusGroup = new QGroupBox(tr("StarFish"), this);
    m_status = new QLabel(statusGroup);
    m_status->setObjectName(QStringLiteral("statusText")); // the probe's handle
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* statusLayout = new QVBoxLayout(statusGroup);
    statusLayout->addWidget(m_status);
    statusLayout->addStretch(1);

    // --- the desktop's icons (T-088) ----------------------------------------
    // The one switch in this window, and it is here because the user asked for a
    // place to configure it: « alors si ce n'est pas le défaut du produit, fais une
    // case où je peux le configurer ». It is a **preference** (core::Preferences),
    // never document state: the product applies it, and this widget only reports it
    // and asks for it -- exactly like the tray entries above it.
    //
    // The cost is written under the checkbox rather than left to be discovered: the
    // icons StarFish draws do not answer the mouse yet (T-042), so a desktop it has
    // taken over is a desktop that answers nothing (T-066, session 36 -- « là en
    // l'état c'est inutilisable »). A switch whose price is on screen is the
    // difference between a choice and a trap.
    auto* iconsGroup = new QGroupBox(tr("Desktop icons"), this);
    m_ownIcons = new QCheckBox(tr("StarFish draws the desktop icons"), iconsGroup);
    m_ownIcons->setObjectName(QStringLiteral("ownDesktopIcons")); // the probe's handle
    auto* explanation = new QLabel(
        tr("An icon filed in a fence is then drawn only in its fence, and no longer on the desktop. The icons left outside the fences are still drawn, where the Shell put them, but they stop answering a click, a double click or the right button until this is turned off -- \"Restore my icons\" in the tray brings the desktop back at any time."),
        iconsGroup);
    explanation->setWordWrap(true);
    explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* iconsLayout = new QVBoxLayout(iconsGroup);
    iconsLayout->addWidget(m_ownIcons);
    iconsLayout->addWidget(explanation);
    iconsLayout->addStretch(1);

    connect(m_ownIcons, &QCheckBox::toggled, this, &SettingsDialog::desktopIconsOwnedToggled);

    auto* closeButton = new QPushButton(tr("Close"), this);
    auto* bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeButton);

    auto* root = new QVBoxLayout(this);
    root->addWidget(statusGroup, 1);
    root->addWidget(iconsGroup);
    root->addLayout(bottomRow);

    // Closing hides rather than destroys: the tray icon brings the window back,
    // and quitting StarFish is the tray's job.
    connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);
}

void SettingsDialog::setStatusText(const QString& text)
{
    m_status->setText(text);
}

void SettingsDialog::setDesktopIconsOwned(bool owned)
{
    // The signals are blocked for this one write: what is being reported is the
    // product's state, and re-emitting the toggle would send it straight back as a
    // *request* -- the checkbox would then be arguing with the thing that set it.
    const QSignalBlocker blocker(m_ownIcons);
    m_ownIcons->setChecked(owned);
}

bool SettingsDialog::desktopIconsOwned() const
{
    return m_ownIcons->isChecked();
}

} // namespace starfish::ui
