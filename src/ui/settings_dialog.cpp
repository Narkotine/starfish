#include "ui/settings_dialog.hpp"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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

    // --- the empty rest of the page -----------------------------------------
    // T-088 put one check box here ("StarFish draws the desktop icons", with its
    // price written underneath). ADR-20 removed it, at the user's own request: the
    // mode changes the desktop's right button and does not give it back when it is
    // switched off, and a mode that does not suit him has no business sitting in
    // the settings window. The space it left is where the rules screen (T-032)
    // goes; until then the status line simply has the window to itself.
    auto* closeButton = new QPushButton(tr("Close"), this);
    auto* bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeButton);

    auto* root = new QVBoxLayout(this);
    root->addWidget(statusGroup, 1);
    root->addLayout(bottomRow);

    // Closing hides rather than destroys: the tray icon brings the window back,
    // and quitting StarFish is the tray's job.
    connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);
}

void SettingsDialog::setStatusText(const QString& text)
{
    m_status->setText(text);
}

} // namespace starfish::ui
