#include "ui/settings_dialog.hpp"

#include <QSpinBox>
#include <QComboBox>
#include "ui/icon_layout.hpp"
#include "ui/recent_colours.hpp"

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
#include "core/i18n.hpp"
#include "ui/context_menu_editor.hpp"
#include "ui/fence_material.hpp"
#include "ui/modern_menu.hpp"
#include "ui/toggle_switch.hpp"
#include "ui/tray_icon.hpp"

#include <QMessageBox>

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

    // Session 65: the interface language, applied at the next start.
    m_language = new QComboBox;
    m_language->setObjectName(QStringLiteral("languageCombo"));
    m_language->addItem(tr("Automatic (Windows)"), QString());
    // Session 68: every language that ships, found from its catalogue.
    for (const QString& code : core::i18n::availableLanguages())
        m_language->addItem(core::i18n::displayName(code), code);
    connect(m_language, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_updating)
            emit languageChosen(m_language->currentData().toString());
    });
    l->addWidget(row(QString(QChar(0xF2B7)), tr("Language"), tr("Applied the next time StarFish starts."),
                     m_language, p));

    // Session 80: what runs the widgets, and what it weighs -- native widgets
    // need nothing; a web engine (later) reports itself here.
    m_widgetEngine = new QLabel(tr("None (native widgets only)"));
    m_widgetEngine->setObjectName(QStringLiteral("widgetEngineStatus"));
    m_widgetEngine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_widgetEngine->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->addWidget(row(QString(QChar(0xE950)), tr("Widget engine"),
                     tr("What runs the fences' widgets, and the memory it takes."), m_widgetEngine, p));

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

    // Session 64.
    m_fenceOverlap = new ToggleSwitch;
    m_fenceOverlap->setObjectName(QStringLiteral("fenceOverlapSwitch"));
    m_fenceOverlap->setChecked(true);
    connect(m_fenceOverlap, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit fenceOverlapToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE8A9)), tr("Allow fences to overlap"),
                     tr("Off: a fence you move or resize stops against the others. Fences that already "
                        "overlap stay as they are."),
                     m_fenceOverlap, p));

    // Session 65: hover.
    m_hover = new ToggleSwitch;
    m_hover->setObjectName(QStringLiteral("fenceHoverSwitch"));
    m_hover->setChecked(true);
    connect(m_hover, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit hoverHighlightToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE7C9)), tr("Highlight the fence under the pointer"),
                     tr("A touch lighter, its border brighter."), m_hover, p));

    // Session 71: optional, off by default; a fence may say yes or no itself.
    m_hoverUnroll = new ToggleSwitch;
    m_hoverUnroll->setObjectName(QStringLiteral("hoverUnrollSwitch"));
    m_hoverUnroll->setChecked(false);
    connect(m_hoverUnroll, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit hoverUnrollToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE70D)), tr("Unroll a rolled-up fence under the pointer"),
                     tr("It rolls back up when the pointer leaves. A fence can say yes or no itself (its menu)."),
                     m_hoverUnroll, p));

    // Session 73: rolling and resizing.
    const auto toggle = [&](QCheckBox*& box, const char* name, bool on, void (SettingsDialog::*signal)(bool),
                            QChar glyph, const QString& title, const QString& hint) {
        box = new ToggleSwitch;
        box->setObjectName(QString::fromLatin1(name));
        box->setChecked(on);
        connect(box, &QCheckBox::toggled, this, [this, signal](bool value) {
            if (!m_updating)
                emit (this->*signal)(value);
        });
        l->addWidget(row(QString(glyph), title, hint, box, p));
    };
    toggle(m_rollOpposite, "rollOppositeSwitch", false, &SettingsDialog::rollOppositeToggled, QChar(0xE74B),
           tr("Roll fences up toward the opposite side"),
           tr("Off: a rolled-up fence keeps its title bar's edge. A fence can choose for itself (Appearance)."));
    toggle(m_rollAnimated, "rollAnimationSwitch", true, &SettingsDialog::rollAnimatedToggled, QChar(0xE916),
           tr("Animate rolling up and down"), tr("A short slide; off: at once."));
    // Session 95: how fast a roll goes; its duration follows the distance.
    m_rollSpeed = new QComboBox;
    m_rollSpeed->setObjectName(QStringLiteral("rollSpeedCombo"));
    m_rollSpeed->addItem(tr("Slow"), 900);
    m_rollSpeed->addItem(tr("Normal"), 1600);
    m_rollSpeed->addItem(tr("Fast"), 2800);
    m_rollSpeed->setCurrentIndex(1);
    connect(m_rollSpeed, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_updating)
            emit rollSpeedChosen(m_rollSpeed->currentData().toInt());
    });
    l->addWidget(row(QString(QChar(0xEC4A)), tr("Rolling speed"),
                     tr("The same speed for every fence: a tall one takes a little longer than a short one."),
                     m_rollSpeed, p));
    toggle(m_snapToCells, "snapToCellsSwitch", false, &SettingsDialog::snapToCellsToggled, QChar(0xE80A),
           tr("Resize fences in whole icons"),
           tr("Width and height land on whole columns and rows. A fence can choose for itself (Appearance)."));
    toggle(m_showLabels, "showLabelsSwitch", true, &SettingsDialog::showLabelsToggled, QChar(0xE8D2),
           tr("Show the icons' names in fences"),
           tr("Off: icons only, the name on hover. A fence can choose for itself (Appearance)."));
    m_iconFlow = new QComboBox; // session 76
    m_iconFlow->setObjectName(QStringLiteral("iconFlowCombo"));
    for (int f = 0; f < IconLayout::flowKeys().size(); ++f)
        m_iconFlow->addItem(IconLayout::flowLabel(f), IconLayout::flowKeys().at(f));
    connect(m_iconFlow, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_updating)
            emit iconFlowChosen(m_iconFlow->currentData().toString());
    });
    l->addWidget(row(QString(QChar(0xE8CB)), tr("Icon order in fences"),
                     tr("Where the first icon sits and which way the next ones go. A fence can choose for itself (Appearance)."),
                     m_iconFlow, p));
    toggle(m_splash, "splashSwitch", true, &SettingsDialog::splashToggled, QChar(0xE7B5),
           tr("Splash screen at startup"), tr("A short card while the fences draw."));
    toggle(m_desktopDragGrid, "desktopDragGridSwitch", true, &SettingsDialog::desktopDragGridToggled,
           QChar(0xE80A), tr("Show the desktop grid while moving an icon"),
           tr("Faint slots on the desktop, and the one the icon will take. Only while an icon is held."));
    toggle(m_autoRestoreShortcuts, "autoRestoreShortcutsSwitch", false, &SettingsDialog::autoRestoreShortcutsToggled,
           QChar(0xE777), tr("Put back deleted fence shortcuts at startup"),
           tr("StarFish keeps a copy of the shortcuts in your fences. Off: a missing one can still be restored from its menu."));

    // Session 69: optional, off by default.
    m_doubleClickHides = new ToggleSwitch;
    m_doubleClickHides->setObjectName(QStringLiteral("desktopDoubleClickSwitch"));
    m_doubleClickHides->setChecked(false);
    connect(m_doubleClickHides, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating)
            emit desktopDoubleClickHidesToggled(on);
    });
    l->addWidget(row(QString(QChar(0xE7B3)), tr("Double-click the desktop to hide fences and icons"),
                     tr("Again to bring everything back. Your files never move."), m_doubleClickHides, p));

    // T-094 (session 65): the border of every fence without its own.
    l->addWidget(sectionTitle(tr("Fence border"), p));
    QFrame* borderCard = card(p);
    auto* borderLayout = new QHBoxLayout(borderCard);
    borderLayout->setContentsMargins(18, 12, 18, 12);
    borderLayout->addWidget(new QLabel(tr("Thickness"), borderCard));
    m_borderWidth = new QSpinBox(borderCard);
    m_borderWidth->setObjectName(QStringLiteral("borderWidth"));
    m_borderWidth->setRange(0, 12); // session 71: up to 12
    m_borderWidth->setSuffix(QStringLiteral(" px"));
    m_borderWidth->setValue(1);
    borderLayout->addWidget(m_borderWidth);
    borderLayout->addSpacing(16);
    borderLayout->addWidget(new QLabel(tr("Colour"), borderCard));
    m_borderColour = new QPushButton(borderCard);
    m_borderColour->setObjectName(QStringLiteral("borderColour"));
    m_borderColour->setMinimumWidth(90);
    borderLayout->addWidget(m_borderColour);
    borderLayout->addStretch(1);
    auto paintButton = [this] {
        const QColor c(m_borderColor);
        m_borderColour->setText(c.name().toUpper());
        m_borderColour->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; }")
                                          .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#FFF")));
    };
    paintButton();
    connect(m_borderWidth, &QSpinBox::valueChanged, this, [this](int width) {
        if (!m_updating)
            emit defaultBorderChanged(width, m_borderColor);
    });
    connect(m_borderColour, &QPushButton::clicked, this, [this, paintButton] {
        const QColor picked = RecentColours::pick(QColor(m_borderColor), tr("Border colour"), this);
        if (!picked.isValid())
            return;
        m_borderColor = picked.name(QColor::HexArgb).toUpper();
        paintButton();
        emit defaultBorderChanged(m_borderWidth->value(), m_borderColor);
    });
    l->addWidget(borderCard);

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
           "A fence's menu (Appearance...) can set its own value."),
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
    auto* version = new QLabel(tr("Version %1").arg(QCoreApplication::applicationVersion() + QLatin1Char('+') + QCoreApplication::instance()->property("starfishCommit").toString()), box);
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

    // Session 68: where StarFish keeps what it knows -- each folder with a
    // button that opens it in the Explorer.
    l->addWidget(sectionTitle(tr("Folders"), p));
    QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (data.isEmpty())
        data = QDir::homePath() + QStringLiteral("/.starfish");
    const QString local = qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/StarFish");
    auto folderRow = [this, p](const QString& glyph, const QString& title, const QString& path,
                               const QString& objectName) {
        auto* open = new QPushButton(tr("Open"));
        open->setObjectName(objectName);
        const QString native = QDir::toNativeSeparators(path);
        connect(open, &QPushButton::clicked, this, [path] {
            QDir().mkpath(path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        QWidget* r = row(glyph, title, native, open, p);
        r->setProperty("folderPath", native);
        return r;
    };
    l->addWidget(folderRow(QString(QChar(0xE8B7)), tr("Settings and fences"), data,
                           QStringLiteral("openDataFolder")));
    l->addWidget(folderRow(QString(QChar(0xE9D9)), tr("Crash log and cache"), local,
                           QStringLiteral("openLocalFolder")));
    auto* registry = new QLabel(tr("Preferences: registry, HKEY_CURRENT_USER\\Software\\StarFish"), p);
    registry->setObjectName(QStringLiteral("hint"));
    registry->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(registry);

    // Session 86: start the StarFish desktop over. Never touches the desktop
    // folder or Explorer's own layout; the layout is backed up first.
    l->addWidget(sectionTitle(tr("Reset"), p));
    auto* resetHint = new QLabel(tr("Your files and the Windows desktop are never touched. The current layout "
                                    "is saved to the backups folder first."),
                                 p);
    resetHint->setObjectName(QStringLiteral("hint"));
    resetHint->setWordWrap(true);
    l->addWidget(resetHint);
    auto* fromWindows = new QPushButton(tr("Start over from the Windows desktop"), p);
    fromWindows->setObjectName(QStringLiteral("resetFromWindows"));
    auto* wipe = new QPushButton(tr("Reset the StarFish desktop"), p);
    wipe->setObjectName(QStringLiteral("resetWipe"));
    auto* resetRow = new QHBoxLayout;
    resetRow->addWidget(fromWindows);
    resetRow->addWidget(wipe);
    resetRow->addStretch(1);
    l->addLayout(resetRow);
    auto confirm = [this](bool wipeAll) {
        const QString text = wipeAll
            ? tr("Remove every zone and every widget (notes included), and any automatic filing rule? "
                 "Every icon goes back to the place Windows keeps for it.")
            : tr("Remove every zone and every widget (notes included)? Every icon goes back to the place "
                 "Windows keeps for it.");
        if (QMessageBox::question(this, tr("Reset the StarFish desktop"), text,
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
            == QMessageBox::Yes)
            emit resetDesktopRequested(wipeAll);
    };
    connect(fromWindows, &QPushButton::clicked, this, [confirm] { confirm(false); });
    connect(wipe, &QPushButton::clicked, this, [confirm] { confirm(true); });
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

void SettingsDialog::setLanguage(const QString& code)
{
    m_updating = true;
    m_language->setCurrentIndex(qMax(0, m_language->findData(code)));
    m_updating = false;
}

void SettingsDialog::setRollOpposite(bool on)
{
    m_updating = true;
    m_rollOpposite->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setRollAnimated(bool on)
{
    m_updating = true;
    m_rollAnimated->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setRollSpeed(int pixelsPerSecond)
{
    m_updating = true;
    int best = 1;
    for (int i = 0; i < m_rollSpeed->count(); ++i) {
        if (qAbs(m_rollSpeed->itemData(i).toInt() - pixelsPerSecond)
            < qAbs(m_rollSpeed->itemData(best).toInt() - pixelsPerSecond))
            best = i;
    }
    m_rollSpeed->setCurrentIndex(best);
    m_updating = false;
}

void SettingsDialog::setIconFlow(const QString& key)
{
    m_updating = true;
    m_iconFlow->setCurrentIndex(qMax(0, m_iconFlow->findData(key)));
    m_updating = false;
}

void SettingsDialog::setDesktopDragGrid(bool on)
{
    m_updating = true;
    m_desktopDragGrid->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setWidgetEngineStatus(const QString& text)
{
    if (m_widgetEngine)
        m_widgetEngine->setText(text);
}

QString SettingsDialog::widgetEngineStatus() const
{
    return m_widgetEngine ? m_widgetEngine->text() : QString();
}

void SettingsDialog::setSplash(bool on)
{
    m_updating = true;
    m_splash->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setShowLabels(bool on)
{
    m_updating = true;
    m_showLabels->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setAutoRestoreShortcuts(bool on)
{
    m_updating = true;
    m_autoRestoreShortcuts->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setSnapToCells(bool on)
{
    m_updating = true;
    m_snapToCells->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setHoverUnroll(bool on)
{
    m_updating = true;
    m_hoverUnroll->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setDesktopDoubleClickHides(bool on)
{
    m_updating = true;
    m_doubleClickHides->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setHoverHighlight(bool on)
{
    m_updating = true;
    m_hover->setChecked(on);
    m_updating = false;
}

void SettingsDialog::setDefaultBorder(int width, const QString& color)
{
    m_updating = true;
    m_borderWidth->setValue(width);
    m_borderColor = color;
    const QColor c(color);
    m_borderColour->setText(c.name().toUpper());
    m_borderColour->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; }")
                                      .arg(c.name(), c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#FFF")));
    m_updating = false;
}

void SettingsDialog::setFenceOverlapAllowed(bool allowed)
{
    m_updating = true;
    m_fenceOverlap->setChecked(allowed);
    m_updating = false;
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
