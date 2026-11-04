#include "ui/context_menu_editor.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/modern_menu.hpp"
#include "ui/toggle_switch.hpp"

namespace starfish::ui {

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kCustomRole = Qt::UserRole + 1;

bool isTopBar(const QString& key, core::ContextMenuKind kind)
{
    const QStringList keys = core::windows11Keys(kind);
    // The top bar comes first in windows11Keys: Paste on the background, the
    // five icons on a selection.
    const int bar = kind == core::ContextMenuKind::Background ? 1 : 5;
    return keys.indexOf(key) >= 0 && keys.indexOf(key) < bar;
}

// Session 82: the icon pool, shown as the grid of buttons it is. One entry per
// icon of core::menuGlyphPool(), plus "No icon"; the dialog returns the code
// point, the document keeps it as it always did.
bool askGlyph(QWidget* parent, QString& glyph)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("glyphPicker"));
    dialog.setWindowTitle(ContextMenuEditor::tr("Choose an icon"));
    applyWindowTheme(&dialog, ModernMenuStyle::current());
    const QColor colour = themeText(ModernMenuStyle::current());
    QString chosen = glyph;

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    int index = 0;
    for (const core::MenuGlyph& entry : core::menuGlyphPool()) {
        auto* button = new QToolButton(&dialog);
        button->setObjectName(QStringLiteral("glyph:") + entry.name);
        button->setIcon(glyphIcon(entry.glyph, colour, 20));
        button->setIconSize(QSize(20, 20));
        button->setToolTip(entry.label);
        button->setAutoRaise(true);
        button->setFixedSize(34, 34);
        const QString picked = entry.glyph;
        QObject::connect(button, &QToolButton::clicked, &dialog, [&chosen, &dialog, picked] {
            chosen = picked;
            dialog.accept();
        });
        grid->addWidget(button, index / 8, index % 8);
        ++index;
    }
    auto* none = new QPushButton(ContextMenuEditor::tr("No icon"), &dialog);
    none->setObjectName(QStringLiteral("glyph:none"));
    QObject::connect(none, &QPushButton::clicked, &dialog, [&chosen, &dialog] {
        chosen.clear();
        dialog.accept();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(ContextMenuEditor::tr("Cancel"));
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* root = new QVBoxLayout(&dialog);
    root->addLayout(grid);
    root->addWidget(none);
    root->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    glyph = chosen;
    return true;
}

// The program, its arguments and a label, asked in a small dialog.
bool askCustomEntry(QWidget* parent, core::ContextMenuCustomEntry& entry)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(ContextMenuEditor::tr("Entry of my own"));
    applyWindowTheme(&dialog, ModernMenuStyle::current());
    auto* label = new QLineEdit(entry.label, &dialog);
    auto* program = new QLineEdit(entry.program, &dialog);
    auto* browse = new QPushButton(ContextMenuEditor::tr("Browse..."), &dialog);
    auto* arguments = new QLineEdit(&dialog);
    QStringList quoted;
    for (const QString& a : entry.arguments)
        quoted << (a.contains(QLatin1Char(' ')) ? QLatin1Char('"') + a + QLatin1Char('"') : a);
    arguments->setText(entry.arguments.isEmpty() ? QStringLiteral("%paths%") : quoted.join(QLatin1Char(' ')));
    auto* hint = new QLabel(ContextMenuEditor::tr("%paths%: the selected items -- %path%: the first one -- "
                                                  "%desktop%: the desktop folder"),
                            &dialog);
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);
    // Session 82: the icon, from the pool the menu's own glyphs come from.
    QString glyph = entry.glyph;
    auto* icon = new QPushButton(&dialog);
    icon->setObjectName(QStringLiteral("iconButton"));
    const auto showIcon = [icon](const QString& current) {
        QString text = ContextMenuEditor::tr("No icon");
        for (const core::MenuGlyph& g : core::menuGlyphPool()) {
            if (g.glyph == current)
                text = g.label;
        }
        icon->setIcon(current.isEmpty() ? QIcon()
                                        : glyphIcon(current, themeText(ModernMenuStyle::current()), 16));
        icon->setText(text);
    };
    showIcon(glyph);
    QObject::connect(icon, &QPushButton::clicked, &dialog, [&] {
        if (askGlyph(&dialog, glyph))
            showIcon(glyph);
    });
    auto* programRow = new QHBoxLayout;
    programRow->addWidget(program, 1);
    programRow->addWidget(browse);
    auto* form = new QFormLayout;
    form->addRow(ContextMenuEditor::tr("Label"), label);
    form->addRow(ContextMenuEditor::tr("Icon"), icon);
    form->addRow(ContextMenuEditor::tr("Program"), programRow);
    form->addRow(ContextMenuEditor::tr("Arguments"), arguments);
    form->addRow(QString(), hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("accent"));
    buttons->button(QDialogButtonBox::Ok)->setText(ContextMenuEditor::tr("Save"));
    buttons->button(QDialogButtonBox::Cancel)->setText(ContextMenuEditor::tr("Cancel"));
    auto* root = new QVBoxLayout(&dialog);
    root->addLayout(form);
    root->addWidget(buttons);
    dialog.resize(520, dialog.sizeHint().height());
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const QString file = QFileDialog::getOpenFileName(&dialog, ContextMenuEditor::tr("Program"),
                                                          QFileInfo(program->text()).absolutePath(),
                                                          ContextMenuEditor::tr("Programs (*.exe *.bat *.cmd)"));
        if (!file.isEmpty()) {
            program->setText(QDir::toNativeSeparators(file));
            if (label->text().isEmpty())
                label->setText(QFileInfo(file).completeBaseName());
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        // An action entry (the light/dark switch) has no program of its own.
        if (!label->text().trimmed().isEmpty()
            && (!program->text().trimmed().isEmpty() || !entry.action.isEmpty()))
            dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    entry.label = label->text().trimmed();
    entry.program = program->text().trimmed();
    entry.arguments = QProcess::splitCommand(arguments->text());
    entry.glyph = glyph;
    return true;
}

} // namespace

ContextMenuEditor::ContextMenuEditor(QWidget* parent)
    : QWidget(parent)
    , m_path(core::ContextMenuConfig::defaultPath())
{
    m_modern = new ToggleSwitch(tr("Windows 11 style menu"), this);
    m_modern->setObjectName(QStringLiteral("modernSwitch"));
    auto* modernHint = new QLabel(tr("Off: the classic menu, as on Windows 10. Shift + right-click "
                                     "always opens the classic one."),
                                  this);
    modernHint->setObjectName(QStringLiteral("hint"));
    modernHint->setWordWrap(true);

    m_tabs = new QTabBar(this);
    m_tabs->addTab(tr("Desktop background"));
    m_tabs->addTab(tr("Icons"));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("entries"));
    m_list->setIconSize(QSize(16, 16));
    m_list->setMinimumHeight(260);

    auto* hint = new QLabel(tr("Tick what the menu shows; the order is the menu's. Entries Windows 11 "
                               "leaves under \"Show more options\" are unticked until you bring them back. "
                               "The menus you open teach StarFish the entries of this PC."),
                            this);
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);

    m_up = new QPushButton(tr("Move up"), this);
    m_down = new QPushButton(tr("Move down"), this);
    auto* add = new QPushButton(tr("Add a program..."), this);
    // Session 82: the ready-made entries (Windows settings pages, the light/dark
    // switch) -- one click instead of filling the dialog by hand.
    auto* presets = new QPushButton(tr("Add from the list..."), this);
    presets->setObjectName(QStringLiteral("presets"));
    m_edit = new QPushButton(tr("Edit..."), this);
    m_remove = new QPushButton(tr("Remove"), this);
    auto* reset = new QPushButton(tr("Reset"), this);
    const QColor glyph = themeText(ModernMenuStyle::current());
    m_up->setIcon(glyphIcon(QString(QChar(0xE74A)), glyph));
    m_down->setIcon(glyphIcon(QString(QChar(0xE74B)), glyph));
    add->setIcon(glyphIcon(QString(QChar(0xE710)), glyph));
    presets->setIcon(glyphIcon(QString(QChar(0xE8FD)), glyph));
    m_edit->setIcon(glyphIcon(QString(QChar(0xE70F)), glyph));
    m_remove->setIcon(glyphIcon(QString(QChar(0xE74D)), glyph));
    reset->setIcon(glyphIcon(QString(QChar(0xE777)), glyph));
    auto* buttons = new QVBoxLayout;
    for (QPushButton* b : {m_up, m_down, add, presets, m_edit, m_remove})
        buttons->addWidget(b);
    buttons->addStretch(1);
    buttons->addWidget(reset);

    auto* listRow = new QHBoxLayout;
    listRow->addWidget(m_list, 1);
    listRow->addLayout(buttons);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(m_modern);
    root->addWidget(modernHint);
    root->addSpacing(10);
    root->addWidget(m_tabs);
    root->addLayout(listRow, 1);
    root->addWidget(hint);

    connect(m_modern, &QCheckBox::toggled, this, [this](bool on) {
        if (m_filling)
            return;
        m_config.modern = on;
        save();
    });
    connect(m_tabs, &QTabBar::currentChanged, this, [this] { fill(); });
    connect(m_list, &QListWidget::itemChanged, this, &ContextMenuEditor::onItemChanged);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int custom = item->data(kCustomRole).toInt();
        if (custom >= 0)
            editCustom(custom);
    });
    connect(m_up, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    connect(add, &QPushButton::clicked, this, [this] { editCustom(-1); });
    connect(presets, &QPushButton::clicked, this, [this, presets] {
        QMenu menu;
        styleModernMenu(&menu, ModernMenuStyle::current());
        const QColor colour = themeText(ModernMenuStyle::current());
        const QList<core::MenuPreset> list = core::menuPresets();
        QList<QAction*> actions;
        for (const core::MenuPreset& preset : list) {
            QAction* action = menu.addAction(glyphIcon(core::glyphForName(preset.glyph), colour), preset.label);
            actions << action;
        }
        QAction* chosen = menu.exec(presets->mapToGlobal(QPoint(0, presets->height())));
        const int index = actions.indexOf(chosen);
        if (index < 0)
            return;
        const core::MenuPreset& preset = list.at(index);
        core::ContextMenuCustomEntry entry;
        entry.label = preset.label;
        entry.program = preset.program;
        entry.arguments = preset.arguments;
        entry.glyph = core::glyphForName(preset.glyph);
        entry.action = preset.action;
        addCustomEntry(entry);
    });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem* item = m_list->currentItem())
            editCustom(item->data(kCustomRole).toInt());
    });
    connect(m_remove, &QPushButton::clicked, this, &ContextMenuEditor::removeCurrent);
    connect(reset, &QPushButton::clicked, this, &ContextMenuEditor::resetCurrent);

    reload();
}

void ContextMenuEditor::setConfigPath(const QString& path)
{
    m_path = path;
    reload();
}

void ContextMenuEditor::setSeenEntries(const QMap<QString, QString>& seen)
{
    m_seen = seen;
    fill();
}

void ContextMenuEditor::reload()
{
    m_config = core::ContextMenuConfig::load(m_path);
    m_filling = true;
    m_modern->setChecked(m_config.modern);
    m_filling = false;
    fill();
}

core::ContextMenuKind ContextMenuEditor::kind() const
{
    return m_tabs->currentIndex() == 1 ? core::ContextMenuKind::Selection : core::ContextMenuKind::Background;
}

core::ContextMenuProfile& ContextMenuEditor::profile()
{
    return kind() == core::ContextMenuKind::Background ? m_config.background : m_config.items;
}

void ContextMenuEditor::selectKind(core::ContextMenuKind k)
{
    m_tabs->setCurrentIndex(k == core::ContextMenuKind::Selection ? 1 : 0);
    fill();
}

QString ContextMenuEditor::labelFor(const QString& key) const
{
    const QString prefix = kind() == core::ContextMenuKind::Background ? QStringLiteral("background ")
                                                                         : QStringLiteral("items ");
    // Undo and Redo say what they would undo ("Annuler Nouveau"): the generic
    // name reads better in a list.
    const QString seen = key == QLatin1String("undo") || key == QLatin1String("redo") ? QString()
                                                                                      : m_seen.value(prefix + key);
    if (!seen.isEmpty())
        return QString(seen).remove(QLatin1Char('&'));
    static const QMap<QString, const char*> names{
        {QStringLiteral("view"), QT_TR_NOOP("View")},
        {QStringLiteral("sort"), QT_TR_NOOP("Sort by")},
        {QStringLiteral("refresh"), QT_TR_NOOP("Refresh")},
        {QStringLiteral("undo"), QT_TR_NOOP("Undo")},
        {QStringLiteral("redo"), QT_TR_NOOP("Redo")},
        {QStringLiteral("paste"), QT_TR_NOOP("Paste")},
        {QStringLiteral("new"), QT_TR_NOOP("New")},
        {QStringLiteral("display-settings"), QT_TR_NOOP("Display settings")},
        {QStringLiteral("personalize"), QT_TR_NOOP("Personalize")},
        {QStringLiteral("terminal"), QT_TR_NOOP("Open in Terminal")},
        {QStringLiteral("open"), QT_TR_NOOP("Open")},
        {QStringLiteral("open-new-tab"), QT_TR_NOOP("Open in new tab")},
        {QStringLiteral("open-new-window"), QT_TR_NOOP("Open in new window")},
        {QStringLiteral("edit"), QT_TR_NOOP("Edit")},
        {QStringLiteral("open-with"), QT_TR_NOOP("Open with")},
        {QStringLiteral("open-location"), QT_TR_NOOP("Open file location")},
        {QStringLiteral("run-as-admin"), QT_TR_NOOP("Run as administrator")},
        {QStringLiteral("empty-recycle-bin"), QT_TR_NOOP("Empty Recycle Bin")},
        {QStringLiteral("pin-quick-access"), QT_TR_NOOP("Pin to Quick access")},
        {QStringLiteral("pin-start"), QT_TR_NOOP("Pin to Start")},
        {QStringLiteral("pin-taskbar"), QT_TR_NOOP("Pin to taskbar")},
        {QStringLiteral("favorites"), QT_TR_NOOP("Add to Favorites")},
        {QStringLiteral("copy-path"), QT_TR_NOOP("Copy as path")},
        {QStringLiteral("properties"), QT_TR_NOOP("Properties")},
        {QStringLiteral("cut"), QT_TR_NOOP("Cut")},
        {QStringLiteral("copy"), QT_TR_NOOP("Copy")},
        {QStringLiteral("rename"), QT_TR_NOOP("Rename")},
        {QStringLiteral("share"), QT_TR_NOOP("Share")},
        {QStringLiteral("delete"), QT_TR_NOOP("Delete")},
        {QStringLiteral("starfish:create-fence"), QT_TR_NOOP("Create a fence here")},
    };
    if (const char* name = names.value(key, nullptr))
        return tr(name);
    if (key.startsWith(QLatin1String("label:")))
        return key.mid(6);
    if (key.startsWith(QLatin1String("custom:")))
        return key.mid(7);
    if (key.startsWith(QLatin1String("app:")))
        return key.mid(4).section(QLatin1Char('/'), -1);
    return key;
}

void ContextMenuEditor::fill()
{
    if (!m_list)
        return;
    const core::ContextMenuKind k = kind();
    const core::ContextMenuProfile& p = profile();
    const QString prefix = k == core::ContextMenuKind::Background ? QStringLiteral("background ")
                                                                    : QStringLiteral("items ");
    QStringList keys;
    const auto add = [&keys](const QString& key) {
        if (!key.isEmpty() && key != QLatin1String("more-options") && !keys.contains(key))
            keys << key;
    };
    // The top bar first (its order is Windows', not the list's), then what the
    // user ordered, then Windows 11's entries, the extensions seen, the
    // packaged apps', StarFish's, the user's: the composer's own order.
    for (const QString& key : core::windows11Keys(k)) {
        if (isTopBar(key, k))
            add(key);
    }
    for (const QString& key : p.order)
        add(key);
    for (const QString& key : core::windows11Keys(k))
        add(key);
    QStringList apps;
    QStringList ours;
    for (auto it = m_seen.cbegin(); it != m_seen.cend(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        const QString key = it.key().mid(prefix.size());
        if (key.startsWith(QLatin1String("app:")))
            apps << key;
        else if (key.startsWith(QLatin1String("starfish:")))
            ours << key;
        else if (!key.contains(QLatin1Char('/')) && !key.startsWith(QLatin1String("label:StarFish")))
            add(key); // (the classic "StarFish: create a fence" verb is ours, hidden from the menu)
    }
    if (k == core::ContextMenuKind::Background)
        ours << QStringLiteral("starfish:create-fence");
    for (const QString& key : apps)
        add(key);
    for (const QString& key : ours)
        add(key);
    for (const core::ContextMenuCustomEntry& c : p.custom)
        add(QStringLiteral("custom:") + c.label);

    const QString keep = m_list->currentItem() ? m_list->currentItem()->data(kKeyRole).toString() : QString();
    m_filling = true;
    m_list->clear();
    const QColor glyph = themeText(ModernMenuStyle::current());
    for (const QString& key : keys) {
        QString text = labelFor(key);
        if (isTopBar(key, k))
            text = tr("%1 (icon bar)").arg(text);
        else if (key.startsWith(QLatin1String("custom:")))
            text = tr("%1 (my program)").arg(text);
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(kKeyRole, key);
        int custom = -1;
        for (int i = 0; i < p.custom.size(); ++i) {
            if (QStringLiteral("custom:") + p.custom.at(i).label == key)
                custom = i;
        }
        item->setData(kCustomRole, custom);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(core::isShown(key, k, p) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(key);
        // Session 82: a user entry wears the icon it was given, when it has one.
        QString g = core::contextMenuGlyph(key);
        if (custom >= 0 && custom < p.custom.size() && !p.custom.at(custom).glyph.isEmpty())
            g = p.custom.at(custom).glyph;
        item->setIcon(g.isEmpty() ? glyphIcon(QString(QChar(0xE76C)), glyph.darker(160)) : glyphIcon(g, glyph));
        if (key == keep)
            m_list->setCurrentItem(item);
    }
    m_filling = false;
    updateButtons();
}

QStringList ContextMenuEditor::listedKeys() const
{
    QStringList keys;
    for (int i = 0; i < m_list->count(); ++i)
        keys << m_list->item(i)->data(kKeyRole).toString();
    return keys;
}

bool ContextMenuEditor::isChecked(const QString& key) const
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(kKeyRole).toString() == key)
            return m_list->item(i)->checkState() == Qt::Checked;
    }
    return false;
}

void ContextMenuEditor::setChecked(const QString& key, bool shown)
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(kKeyRole).toString() == key)
            m_list->item(i)->setCheckState(shown ? Qt::Checked : Qt::Unchecked);
    }
}

void ContextMenuEditor::moveEntry(const QString& key, int delta)
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(kKeyRole).toString() == key) {
            m_list->setCurrentRow(i);
            moveCurrent(delta);
            return;
        }
    }
}

void ContextMenuEditor::addCustomEntry(const core::ContextMenuCustomEntry& entry)
{
    profile().custom << entry;
    save();
    fill();
}

void ContextMenuEditor::onItemChanged(QListWidgetItem* item)
{
    if (m_filling)
        return;
    const QString key = item->data(kKeyRole).toString();
    const bool on = item->checkState() == Qt::Checked;
    core::ContextMenuProfile& p = profile();
    p.hidden.removeAll(key);
    p.shown.removeAll(key);
    if (!on)
        p.hidden << key;
    else if (!core::shownByDefault(key, kind()))
        p.shown << key;
    if (!p.order.isEmpty())
        writeOrder();
    save();
}

void ContextMenuEditor::moveCurrent(int delta)
{
    const int row = m_list->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= m_list->count())
        return;
    if (isTopBar(m_list->item(row)->data(kKeyRole).toString(), kind())
        || isTopBar(m_list->item(to)->data(kKeyRole).toString(), kind()))
        return; // the icon bar keeps Windows' order
    m_filling = true;
    QListWidgetItem* item = m_list->takeItem(row);
    m_list->insertItem(to, item);
    m_list->setCurrentItem(item);
    m_filling = false;
    writeOrder();
    save();
    updateButtons();
}

void ContextMenuEditor::writeOrder()
{
    // Only what shows: an unticked extension put in `order` would come back
    // (the composer takes what is ordered).
    QStringList order;
    for (int i = 0; i < m_list->count(); ++i) {
        const QString key = m_list->item(i)->data(kKeyRole).toString();
        if (m_list->item(i)->checkState() == Qt::Checked && !isTopBar(key, kind()))
            order << key;
    }
    profile().order = order;
}

void ContextMenuEditor::editCustom(int index)
{
    core::ContextMenuProfile& p = profile();
    core::ContextMenuCustomEntry entry = index >= 0 && index < p.custom.size() ? p.custom.at(index)
                                                                               : core::ContextMenuCustomEntry{};
    const QString oldKey = QStringLiteral("custom:") + entry.label;
    if (!askCustomEntry(this, entry))
        return;
    if (index >= 0 && index < p.custom.size()) {
        p.custom[index] = entry;
        const QString newKey = QStringLiteral("custom:") + entry.label;
        p.order.replaceInStrings(oldKey, newKey);
        p.hidden.replaceInStrings(oldKey, newKey);
    } else {
        p.custom << entry;
    }
    save();
    fill();
}

void ContextMenuEditor::removeCurrent()
{
    QListWidgetItem* item = m_list->currentItem();
    const int index = item ? item->data(kCustomRole).toInt() : -1;
    core::ContextMenuProfile& p = profile();
    if (index < 0 || index >= p.custom.size())
        return;
    const QString key = item->data(kKeyRole).toString();
    p.custom.removeAt(index);
    p.order.removeAll(key);
    p.hidden.removeAll(key);
    save();
    fill();
}

void ContextMenuEditor::resetCurrent()
{
    // Windows 11's menu again; the programs of one's own stay (removing them is
    // their own button).
    core::ContextMenuProfile& p = profile();
    p.hidden.clear();
    p.shown.clear();
    p.order.clear();
    save();
    fill();
}

void ContextMenuEditor::updateButtons()
{
    QListWidgetItem* item = m_list->currentItem();
    const bool custom = item && item->data(kCustomRole).toInt() >= 0;
    const bool bar = item && isTopBar(item->data(kKeyRole).toString(), kind());
    m_up->setEnabled(item && !bar && m_list->currentRow() > 0
                     && !isTopBar(m_list->item(m_list->currentRow() - 1)->data(kKeyRole).toString(), kind()));
    m_down->setEnabled(item && !bar && m_list->currentRow() + 1 < m_list->count());
    m_edit->setEnabled(custom);
    m_remove->setEnabled(custom);
}

void ContextMenuEditor::save()
{
    m_config.save(m_path);
    emit configChanged();
}

} // namespace starfish::ui
