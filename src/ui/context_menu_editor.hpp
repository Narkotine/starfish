#pragma once

#include <QMap>
#include <QString>
#include <QWidget>

#include "core/context_menu.hpp"

class QCheckBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTabBar;

namespace starfish::ui {

// T-092 / session 59 -- the right-click menu's configuration, edited with the
// mouse instead of by hand in context-menu.json: show or hide an entry (its
// box), put it higher or lower (the order), add a program of one's own, and go
// back to Windows 11's menu. Every change is written at once, like Windows'
// own settings: the next right-click shows it, there is no "Apply".
//
// It lists what a menu can hold before one was ever opened (Windows 11's own
// entries) and what the menus have actually shown (`setSeenEntries`, the
// product's context-menu-keys.txt): the extensions installed on this PC, the
// packaged apps' entries and StarFish's.
class ContextMenuEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ContextMenuEditor(QWidget* parent = nullptr);

    // Where the configuration is read and written (ContextMenuConfig::defaultPath()
    // unless told otherwise -- the probe points it at a scratch file).
    void setConfigPath(const QString& path);
    QString configPath() const { return m_path; }
    // "background <key>" / "items <key>" -> the label Windows wrote.
    void setSeenEntries(const QMap<QString, QString>& seen);
    // Reads the file again (someone may have edited it by hand).
    void reload();

    // For the probe: the entries the current tab lists, in order, and whether
    // each one shows.
    QStringList listedKeys() const;
    bool isChecked(const QString& key) const;
    void selectKind(core::ContextMenuKind kind);
    void setChecked(const QString& key, bool shown);
    void moveEntry(const QString& key, int delta);
    void addCustomEntry(const core::ContextMenuCustomEntry& entry);
    const core::ContextMenuConfig& config() const { return m_config; }

signals:
    void configChanged();

private:
    core::ContextMenuKind kind() const;
    core::ContextMenuProfile& profile();
    QString labelFor(const QString& key) const;
    void fill();
    void onItemChanged(QListWidgetItem* item);
    void moveCurrent(int delta);
    void writeOrder();
    void editCustom(int index);
    void removeCurrent();
    void resetCurrent();
    void updateButtons();
    void save();

    core::ContextMenuConfig m_config;
    QString m_path;
    QMap<QString, QString> m_seen;
    QCheckBox* m_modern = nullptr;
    QTabBar* m_tabs = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_up = nullptr;
    QPushButton* m_down = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_remove = nullptr;
    bool m_filling = false;
};

} // namespace starfish::ui
