#include "ui/settings_dialog.hpp"

#include "ui/percent_slider.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "core/context_menu.hpp"
#include "ui/context_menu_editor.hpp"
#include "ui/fence_material.hpp"
#include "ui/modern_menu.hpp"
#include "ui/toggle_switch.hpp"
#include "ui/tray_icon.hpp"

namespace starfish::ui {

namespace {

QLabel* pageTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageTitle"));
    return label;
}

QLabel* sectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QFrame* card(QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    return frame;
}

// A Windows 11 settings row: a glyph, a title and its line of explanation, the
// control on the right.
QFrame* row(const QString& glyph, const QString& title, const QString& hint, QWidget* control, QWidget* parent)
{
    QFrame* frame = card(parent);
    auto* icon = new QLabel(frame);
    icon->setPixmap(glyphIcon(glyph, themeText(ModernMenuStyle::current())).pixmap(20, 20));
    icon->setFixedWidth(28);
    auto* titleLabel = new QLabel(title, frame);
    auto* hintLabel = new QLabel(hint, frame);
    hintLabel->setObjectName(QStringLiteral("hint"));
    hintLabel->setWordWrap(true);
    auto* text = new QVBoxLayout;
    text->setSpacing(1);
    text->addWidget(titleLabel);
    if (!hint.isEmpty())
        text->addWidget(hintLabel);
    else
        hintLabel->hide();
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(12);
    layout->addWidget(icon);
    layout->addLayout(text, 1);
    if (control) {
        control->setParent(frame);
        layout->addWidget(control);
    }
    return frame;
}

QWidget* page(QWidget* parent, QVBoxLayout** layout)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("scrollBody"));
    *layout = new QVBoxLayout(body);
    (*layout)->setContentsMargins(28, 22, 28, 22);
    (*layout)->setSpacing(6);
    scroll->setWidget(body);
    return scroll;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("StarFish settings"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    resize(940, 640);
    setMinimumSize(720, 480);

    // --- the left column: the mark and the pages -----------------------------
    auto* logo = new QLabel(this);
    logo->setPixmap(TrayIcon::buildIcon().pixmap(32, 32));
    auto* name = new QLabel(QStringLiteral("StarFish"), this);
    name->setStyleSheet(QStringLiteral("font-size: 12pt; font-weight: 600;"));
    auto* brand = new QHBoxLayout;
    brand->setContentsMargins(16, 16, 16, 12);
    brand->setSpacing(10);
    brand->addWidget(logo);
    brand->addWidget(name, 1);

    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(230);
    m_nav->setIconSize(QSize(18, 18));
    m_nav->setFocusPolicy(Qt::NoFocus);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildGeneral());
    m_pages->addWidget(buildFences());
    m_pages->addWidget(buildMenu());
    m_pages->addWidget(buildAbout());

    auto* left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 8);
    left->addLayout(brand);
    left->addWidget(m_nav, 1);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(left);
    root->addWidget(m_pages, 1);

    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    applyTheme();
    m_nav->setCurrentRow(General);
}

void SettingsDialog::applyTheme()
{
    const ModernMenuStyle style = ModernMenuStyle::current();
    m_dark = style.dark;
    applyWindowTheme(this, style);
    const QColor glyph = themeText(style);
    const struct {
        ushort glyph;
        const char* text;
    } entries[] = {{0xE80F, QT_TR_NOOP("General")},
                   {0xF0E2, QT_TR_NOOP("Fences")},
                   {0xE700, QT_TR_NOOP("Right-click menu")},
                   {0xE946, QT_TR_NOOP("About")}};
    const int current = m_nav->currentRow();
    m_nav->clear();
    for (const auto& e : entries)
        new QListWidgetItem(glyphIcon(QString(QChar(e.glyph)), glyph), tr(e.text), m_nav);
    m_nav->setCurrentRow(current < 0 ? 0 : current);
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    // The theme may have changed since the window was built (light by day,
    // dark at night): read it again each time it opens.
    if (ModernMenuStyle::current().dark != m_dark)
        applyTheme();
    QDialog::showEvent(event);
}

QWidget* SettingsDialog::buildGeneral()
{
    QVBoxLayout* l = nullptr;
    QWidget* p = page(this, &l);
    l->addWidget(pageTitle(tr("General"), p));
    l->addSpacing(8);

    // What is running -- the product's own line (T-059), the probe's handle.
    QFrame* status = card(p);
    auto* icon = new QLabel(status);
    icon->setPixmap(TrayIcon::buildIcon().pixmap(48, 48));
    auto* running = new QLabel(tr("StarFish is running"), status);
    running->setStyleSheet(QStringLiteral("font-size: 12pt; font-weight: 600;"));
    m_status = new QLabel(status);
    m_status->setObjectName(QStringLiteral("statusText"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* text = new QVBoxLayout;
    text->addWidget(running);
    text->addWidget(m_status);
    auto* statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(18, 16, 18, 16);
    statusLayout->setSpacing(16);
    statusLayout->addWidget(icon, 0, Qt::AlignTop);
    statusLayout->addLayout(text, 1);
    l->addWidget(status);

    m_startWithWindows = new ToggleSwitch;
    m_startWithWindows->setObjectName(QStringLiteral("startWithWindowsSwitch"));
    connect(m_startWithWindows, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit startWithWindowsToggled(on);
    });
    QWidget* startRow = row(QString(QChar(0xE7E8)), tr("Start with Windows"),
                            tr("StarFish opens when you sign in."), m_startWithWindows, p);
    for (QLabel* label : startRow->findChildren<QLabel*>()) {
        if (label->text() == tr("StarFish opens when you sign in.")) {
            m_startHint = label;
            m_startHint->setProperty("startWithWindowsHint", true); // its name styles it
        }
    }
    l->addWidget(startRow);

    l->addWidget(sectionTitle(tr("Actions"), p));
    auto* create = new QPushButton(tr("Create"));
    create->setObjectName(QStringLiteral("accent"));
    connect(create, &QPushButton::clicked, this, &SettingsDialog::newFenceRequested);
    l->addWidget(row(QString(QChar(0xE710)), tr("New fence"),
                     tr("A fence in the middle of the screen; right-click the desktop to put one where you are."),
                     create, p));
    auto* folder = new QPushButton(tr("Open"));
    connect(folder, &QPushButton::clicked, this, [] {
        const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                                .filePath(QStringLiteral("StarFish"));
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    l->addWidget(row(QString(QChar(0xE8B7)), tr("Settings folder"),
                     tr("Where StarFish keeps its fences, its menu and its logs."), folder, p));
    auto* restore = new QPushButton(tr("Restore"));
    connect(restore, &QPushButton::clicked, this, &SettingsDialog::restoreIconsRequested);
    m_restoreRow = row(QString(QChar(0xE777)), tr("Restore my icons"),
                       tr("Shows Windows' own desktop icons again, whatever StarFish believes."), restore, p);
    m_restoreRow->setVisible(false);
    l->addWidget(m_restoreRow);
    l->addStretch(1);
    return p;
}

QWidget* SettingsDialog::buildFences()
{
    QVBoxLayout* l = nullptr;
    QWidget* p = page(this, &l);
    l->addWidget(pageTitle(tr("Fences"), p));
    auto* intro = new QLabel(tr("What holds for all fences. One fence is set from its own menu "
                                "(right-click its title)."),
                             p);
    intro->setObjectName(QStringLiteral("hint"));
    intro->setWordWrap(true);
    l->addWidget(intro);
    l->addSpacing(8);

    m_showFences = new ToggleSwitch;
    m_showFences->setObjectName(QStringLiteral("showFencesSwitch"));
    connect(m_showFences, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit fencesVisibilityToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE7B3)), tr("Show the fences"),
                     tr("Hidden, they keep their content; the tray shows them again."), m_showFences, p));

    m_arrows = new ToggleSwitch;
    m_arrows->setObjectName(QStringLiteral("arrowsSwitch"));
    connect(m_arrows, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit fenceShortcutArrowsToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE71B)), tr("Shortcut arrows"),
                     tr("The small arrow Windows draws on a shortcut, on the icons inside the fences."),
                     m_arrows, p));

    m_sortOnAdd = new ToggleSwitch;
    m_sortOnAdd->setObjectName(QStringLiteral("sortOnAddSwitch"));
    connect(m_sortOnAdd, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit sortOnAddToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE8CB)), tr("Sort icons alphabetically when added to a fence"),
                     tr("Off: an icon goes where it is dropped and the order stays yours."),
                     m_sortOnAdd, p));

    l->addWidget(sectionTitle(tr("Desktop"), p));
    m_desktopArrows = new ToggleSwitch;
    m_desktopArrows->setObjectName(QStringLiteral("desktopArrowsSwitch"));
    connect(m_desktopArrows, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit desktopShortcutArrowsToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE71B)), tr("Shortcut arrows on desktop icons"),
                     tr("Only on the desktop StarFish draws; Windows' own setting is left alone."),
                     m_desktopArrows, p));
    m_overlap = new ToggleSwitch;
    m_overlap->setObjectName(QStringLiteral("overlapSwitch"));
    connect(m_overlap, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit desktopOverlapToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE81E)), tr("Allow desktop icons and fences to overlap"),
                     tr("Off: an icon under a fence moves to the nearest free place."),
                     m_overlap, p));

    // T-098 (session 63): one number for every fence that has none of its own.
    l->addWidget(sectionTitle(tr("Fence transparency"), p));
    QFrame* transparency = card(p);
    auto* transparencyLayout = new QVBoxLayout(transparency);
    transparencyLayout->setContentsMargins(18, 12, 18, 12);
    transparencyLayout->addWidget(new QLabel(tr("Default for all fences"), transparency));
    QWidget* sliderRow = PercentSlider::make(0, transparency, &m_transparency, nullptr);
    connect(m_transparency, &QSlider::valueChanged, this, [this](int percent) {
        if (!m_updating)
            emit defaultTransparencyChanged(percent);
    });
    transparencyLayout->addWidget(sliderRow);
    auto* transparencyHint = new QLabel(
        tr("Background and title bar. Icons and their names stay opaque. 0 % = opaque. "
           "A fence's menu (Transparency...) can set its own value."),
        transparency);
    transparencyHint->setObjectName(QStringLiteral("hint"));
    transparencyHint->setWordWrap(true);
    transparencyLayout->addWidget(transparencyHint);
    auto* allToDefault = new QPushButton(tr("Every fence follows this value"), transparency);
    allToDefault->setObjectName(QStringLiteral("allFencesToDefault"));
    connect(allToDefault, &QPushButton::clicked, this, &SettingsDialog::allFencesToDefaultRequested);
    transparencyLayout->addWidget(allToDefault, 0, Qt::AlignLeft);
    l->addWidget(transparency);
    l->addStretch(1);
    return p;
}

QWidget* SettingsDialog::buildMenu()
{
    QVBoxLayout* l = nullptr;
    QWidget* p = page(this, &l);
    l->addWidget(pageTitle(tr("Right-click menu"), p));
    auto* intro = new QLabel(tr("The menu StarFish draws when you right-click the desktop or an icon."), p);
    intro->setObjectName(QStringLiteral("hint"));
    intro->setWordWrap(true);
    l->addWidget(intro);
    l->addSpacing(8);
    QFrame* box = card(p);
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(18, 14, 18, 14);
    m_menuEditor = new ContextMenuEditor(box);
    boxLayout->addWidget(m_menuEditor);
    l->addWidget(box, 1);
    return p;
}

QWidget* SettingsDialog::buildAbout()
{
    QVBoxLayout* l = nullptr;
    QWidget* p = page(this, &l);
    l->addWidget(pageTitle(tr("About"), p));
    l->addSpacing(8);
    QFrame* box = card(p);
    auto* icon = new QLabel(box);
    icon->setPixmap(TrayIcon::buildIcon().pixmap(96, 96));
    auto* name = new QLabel(QStringLiteral("StarFish"), box);
    name->setStyleSheet(QStringLiteral("font-size: 18pt; font-weight: 600;"));
    auto* version = new QLabel(tr("Version %1").arg(QCoreApplication::applicationVersion()), box);
    version->setObjectName(QStringLiteral("hint"));
    auto* what = new QLabel(tr("Fences to tidy the Windows desktop. Your files never move: a fence shows "
                               "them, the desktop folder keeps them."),
                            box);
    what->setWordWrap(true);
    auto* text = new QVBoxLayout;
    text->addWidget(name);
    text->addWidget(version);
    text->addSpacing(6);
    text->addWidget(what);
    text->addStretch(1);
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(20);
    layout->addWidget(icon, 0, Qt::AlignTop);
    layout->addLayout(text, 1);
    l->addWidget(box);
    l->addStretch(1);
    return p;
}

void SettingsDialog::setStatusText(const QString& text)
{
    m_status->setText(text);
}

void SettingsDialog::setFencesVisible(bool visible)
{
    m_updating = true;
    m_showFences->setChecked(visible);
    m_updating = false;
}

void SettingsDialog::setFenceMaterial(const QString& material)
{
    Q_UNUSED(material); // T-098: shown as the transparency slider now
}

void SettingsDialog::setDefaultTransparency(int percent)
{
    if (!m_transparency)
        return;
    m_updating = true;
    m_transparency->setValue(percent);
    m_updating = false;
}

void SettingsDialog::setFenceShortcutArrows(bool shown)
{
    m_updating = true;
    m_arrows->setChecked(shown);
    m_updating = false;
}

void SettingsDialog::setStartWithWindows(bool on, bool disabledByUser)
{
    m_updating = true;
    m_startWithWindows->setChecked(on && !disabledByUser);
    m_updating = false;
    if (m_startHint)
        m_startHint->setText(on && disabledByUser
                                 ? tr("Turned off in Task Manager; switching it on here turns it back on.")
                                 : tr("StarFish opens when you sign in."));
}

void SettingsDialog::setSortOnAdd(bool sorted)
{
    m_updating = true;
    m_sortOnAdd->setChecked(sorted);
    m_updating = false;
}

void SettingsDialog::setDesktopShortcutArrows(bool shown)
{
    m_updating = true;
    m_desktopArrows->setChecked(shown);
    m_updating = false;
}

void SettingsDialog::setDesktopOverlapAllowed(bool allowed)
{
    m_updating = true;
    m_overlap->setChecked(allowed);
    m_updating = false;
}

void SettingsDialog::setRestoreIconsOffered(bool offered)
{
    m_restoreRow->setVisible(offered);
}

void SettingsDialog::showPage(Page page)
{
    m_nav->setCurrentRow(page);
}

SettingsDialog::Page SettingsDialog::currentPage() const
{
    return static_cast<Page>(m_pages->currentIndex());
}

} // namespace starfish::ui
