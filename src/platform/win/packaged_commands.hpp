#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace starfish::platform::win {

// T-092 follow-up -- the entries packaged (Store / MSIX) apps add to Windows
// 11's menu: "Ouvrir dans le Terminal", "Modifier avec le Bloc-notes", Photos'
// "Modifier"... They are not in the classic IContextMenu at all: each package
// declares them in its manifest (desktop4/desktop5 FileExplorerContextMenus:
// an item type, a verb id, the CLSID of an IExplorerCommand), and the class is
// activated through packaged COM. This reads the same declarations -- the
// manifests of the user's packages, found through the AppModel repository in
// HKCU -- and talks to the commands through their documented interface:
// GetState (hidden / disabled), GetTitle, GetIcon, Invoke.
struct PackagedCommand {
    QString key;        // "app:<package family>/<verb id>"
    QString title;      // GetTitle, as the app words it
    QString iconFile;   // a PNG of GetIcon's icon, cached on disk; may be empty
    bool enabled = true;
    int index = -1;     // for invokePackagedCommand()
};

// The commands that apply to `paths` (files and folders of the desktop), or to
// the desktop folder itself when `background` is true. The objects stay alive
// until the next call (or releasePackagedCommands()).
std::vector<PackagedCommand> packagedCommandsFor(const QStringList& paths, bool background,
                                                 const QString& desktopFolder);
bool invokePackagedCommand(int index);
void releasePackagedCommands();

// What the manifests declare, for the probe: "Type -> verb (CLSID)" per line.
QStringList packagedCommandDeclarations();

} // namespace starfish::platform::win
