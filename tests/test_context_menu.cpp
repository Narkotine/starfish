// T-092: the drawn right-click menu as data -- the Windows 11 profile and the
// user's configuration, with menus shaped like the ones measured on the desktop.
#include <QTemporaryDir>
#include <QTest>

#include "core/context_menu.hpp"

using namespace starfish::core;

namespace {

ContextMenuItem entry(const QString& label, int command, bool enabled = true)
{
    ContextMenuItem item;
    item.label = label;
    item.command = command;
    item.enabled = enabled;
    return item;
}

ContextMenuItem sep()
{
    ContextMenuItem item;
    item.separator = true;
    return item;
}

ContextMenuItem submenu(const QString& label, std::vector<ContextMenuItem> children)
{
    ContextMenuItem item;
    item.label = label;
    item.children = std::move(children);
    return item;
}

// The desktop's background menu as measured (build/context-menu-probe.txt).
std::vector<ContextMenuItem> background()
{
    return {submenu(QStringLiteral("Affichage"), {entry(QStringLiteral("Grandes icônes"), 16)}),
            submenu(QStringLiteral("Trier par"), {entry(QStringLiteral("Nom"), 8)}),
            entry(QStringLiteral("Actualiser"), 3), sep(),
            entry(QStringLiteral("Coller"), 2, false), entry(QStringLiteral("Annuler Nouveau"), 1),
            entry(QStringLiteral("Open Git Bash here"), 91), sep(),
            submenu(QStringLiteral("Nouveau"), {entry(QStringLiteral("Dossier"), 19)}), sep(),
            entry(QStringLiteral("Paramètres d'affichage"), 95), entry(QStringLiteral("Personnaliser"), 97)};
}

std::vector<ContextMenuItem> fileMenu()
{
    return {entry(QStringLiteral("Ouvrir"), 171), entry(QStringLiteral("Imprimer"), 173),
            submenu(QStringLiteral("7-Zip"), {entry(QStringLiteral("Ajouter à l'archive..."), 150)}),
            submenu(QStringLiteral("Ouvrir avec"), {entry(QStringLiteral("Bloc-notes"), 101)}),
            entry(QString::fromUtf8("Copier en tant que chemin d\u2019accès"), 80),
            entry(QStringLiteral("Partager"), 78), sep(),
            entry(QStringLiteral("Couper"), 24), entry(QStringLiteral("Copier"), 25), sep(),
            entry(QStringLiteral("Supprimer"), 17), entry(QStringLiteral("Renommer"), 18), sep(),
            entry(QStringLiteral("Propriétés"), 19)};
}

QStringList keys(const std::vector<ContextMenuItem>& items)
{
    QStringList out;
    for (const ContextMenuItem& item : items)
        out << (item.separator ? QStringLiteral("|") : (item.topBar ? QStringLiteral("^") : QString()) + item.key);
    return out;
}

ContextMenuItem ours()
{
    ContextMenuItem item;
    item.key = QStringLiteral("starfish:create-fence");
    item.label = QStringLiteral("Create a fence here");
    item.ownId = 1;
    return item;
}

} // namespace

class TestContextMenu : public QObject
{
    Q_OBJECT
private slots:
    void keysFromLabels()
    {
        QCOMPARE(contextMenuKey(QStringLiteral("Actualiser"), false), QStringLiteral("refresh"));
        QCOMPARE(contextMenuKey(QStringLiteral("Annuler Renommer"), false), QStringLiteral("undo"));
        QCOMPARE(contextMenuKey(QStringLiteral("Undo Delete"), false), QStringLiteral("undo"));
        QCOMPARE(contextMenuKey(QString::fromUtf8("Copier en tant que chemin d\u2019accès"), false),
                 QStringLiteral("copy-path"));
        QCOMPARE(contextMenuKey(QStringLiteral("Properties"), false), QStringLiteral("properties"));
        QCOMPARE(contextMenuKey(QStringLiteral("Open Git Bash here"), false),
                 QStringLiteral("label:Open Git Bash here"));
        QVERIFY(!contextMenuGlyph(QStringLiteral("cut")).isEmpty());
    }

    void backgroundIsWindows11s()
    {
        QStringList omitted;
        const auto menu = composeWindows11Menu(background(), ContextMenuKind::Background, {}, {ours()},
                                               QStringLiteral("Afficher plus d'options"), &omitted);
        QCOMPARE(keys(menu).join(QLatin1Char(' ')),
                 QStringLiteral("view sort refresh | undo | new | display-settings personalize | "
                                "starfish:create-fence | more-options"));
        QVERIFY(omitted.contains(QStringLiteral("label:Open Git Bash here")));
        QCOMPARE(menu.back().ownId, kMoreOptionsId);
        // The submenus keep their children, keyed under their parent.
        QCOMPARE(menu.front().children.front().key, QStringLiteral("view/Grandes icônes"));
    }

    void pasteJoinsTheTopBarWhenItCan()
    {
        auto menu = background();
        menu[4].enabled = true; // "Coller"
        QCOMPARE(keys(composeWindows11Menu(menu, ContextMenuKind::Background, {}, {}, QString())).first(),
                 QStringLiteral("^paste"));
    }

    void itemMenuHasTheTopBar()
    {
        const auto menu = composeWindows11Menu(fileMenu(), ContextMenuKind::Selection, {}, {}, QString());
        QCOMPARE(keys(menu).join(QLatin1Char(' ')),
                 QStringLiteral("^cut ^copy ^rename ^share ^delete open open-with | copy-path properties | "
                                "more-options"));
    }

    void configurationHidesReordersBringsBackAndAdds()
    {
        ContextMenuConfig config;
        config.background.hidden << QStringLiteral("refresh") << QStringLiteral("starfish:create-fence");
        config.background.order << QStringLiteral("personalize");
        config.background.shown << QStringLiteral("label:Open Git Bash here");
        config.background.custom << ContextMenuCustomEntry{QStringLiteral("Terminal ici"),
                                                           QStringLiteral("wt.exe"),
                                                           {QStringLiteral("-d"), QStringLiteral("%desktop%")},
                                                           {}};
        const auto menu = composeWindows11Menu(background(), ContextMenuKind::Background, config, {ours()},
                                               QString());
        QCOMPARE(keys(menu).join(QLatin1Char(' ')),
                 QStringLiteral("personalize | view sort | undo | new | display-settings | "
                                "label:Open Git Bash here | custom:Terminal ici | more-options"));
        QCOMPARE(menu.at(menu.size() - 3).customIndex, 0);
        QCOMPARE(expandCustomArguments(config.background.custom.first(), {}, QStringLiteral("C:/D")),
                 (QStringList{QStringLiteral("-d"), QStringLiteral("C:\\D")}));
        ContextMenuCustomEntry all{QStringLiteral("x"), QStringLiteral("x"),
                                   {QStringLiteral("%paths%"), QStringLiteral("--first=%path%")}, {}};
        QCOMPARE(expandCustomArguments(all, {QStringLiteral("C:/a"), QStringLiteral("C:/b")}, QString()),
                 (QStringList{QStringLiteral("C:\\a"), QStringLiteral("C:\\b"), QStringLiteral("--first=C:\\a")}));
    }

    void packagedAppsHaveTheirGroup()
    {
        ContextMenuItem terminal;
        terminal.key = QStringLiteral("terminal"); // a place Windows 11 has for it
        terminal.label = QStringLiteral("Ouvrir dans le Terminal");
        terminal.packagedIndex = 0;
        ContextMenuItem notepad;
        notepad.key = QStringLiteral("app:Microsoft.WindowsNotepad/OpenInNotepad");
        notepad.label = QStringLiteral("Modifier dans Bloc-notes");
        notepad.packagedIndex = 1;
        const auto bg = composeWindows11Menu(background(), ContextMenuKind::Background, {}, {}, QString(),
                                             nullptr, {terminal});
        QVERIFY(keys(bg).join(QLatin1Char(' ')).contains(QStringLiteral("personalize | terminal | more-options")));
        ContextMenuConfig config;
        config.items.hidden << notepad.key;
        const auto hidden = composeWindows11Menu(fileMenu(), ContextMenuKind::Selection, config, {}, QString(),
                                                 nullptr, {notepad});
        QVERIFY(!keys(hidden).contains(notepad.key));
        const auto shown = composeWindows11Menu(fileMenu(), ContextMenuKind::Selection, {}, {}, QString(),
                                                nullptr, {notepad});
        QVERIFY(keys(shown).join(QLatin1Char(' ')).contains(QStringLiteral("properties | app:Microsoft.WindowsNotepad/OpenInNotepad | more-options")));
    }

    void configurationRoundTrips()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sub/context-menu.json"));
        QVERIFY(ContextMenuConfig::load(path).modern); // missing file: the defaults
        ContextMenuConfig config;
        config.modern = false;
        config.items.hidden << QStringLiteral("share");
        config.items.custom << ContextMenuCustomEntry{QStringLiteral("Bloc-notes"), QStringLiteral("notepad.exe"),
                                                      {QStringLiteral("%path%")}, QString(QChar(0xE70F))};
        QVERIFY(config.save(path));
        const ContextMenuConfig back = ContextMenuConfig::load(path);
        QCOMPARE(back.modern, false);
        QCOMPARE(back.items.hidden, QStringList{QStringLiteral("share")});
        QCOMPARE(back.items.custom.size(), 1);
        QCOMPARE(back.items.custom.first().arguments, QStringList{QStringLiteral("%path%")});
        QCOMPARE(back.items.custom.first().glyph, QString(QChar(0xE70F)));
    }
};

QTEST_GUILESS_MAIN(TestContextMenu)
#include "test_context_menu.moc"
