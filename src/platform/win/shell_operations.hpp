#pragma once

#include <QPoint>
#include <QString>
#include <QStringList>

#include <functional>

namespace starfish::platform::win {

// ADR-21 lot 3 (T-091) -- the gestures the drawn desktop hands to the shell.
// Everything here runs **in this process**, through the documented shell
// objects (IShellItem, IFileOperation, the desktop folder's own IDropTarget,
// SHDoDragDrop): nothing is sent to explorer.exe as a window message, so no
// pointer crosses a process (T-071). File operations carry
// FOFX_ADDUNDORECORD, which puts them on Explorer's own undo stack -- the
// desktop's Ctrl+Z (and "Undo" in its background menu) takes them back.

// The name the shell lets the user edit (SIGDN_PARENTRELATIVEEDITING): no
// extension when Explorer hides extensions, exactly what F2 shows.
QString editingNameOf(const QString& parsingName);

// Renames through IFileOperation. `newName` is an edited name in the sense
// above: when the extension is hidden and the user did not type one, the
// item's own extension is kept. `newPath` receives the new parsing name.
bool renameShellItem(const QString& parsingName, const QString& newName, quintptr owner,
                     QString* newPath = nullptr);

enum class DeleteMode {
    RecycleBin,      // Delete: undoable, the shell's own confirmation if any
    Permanent,       // Shift+Delete: the shell asks first
    PermanentSilent, // the probes, on their own files only
};
bool deleteShellItems(const QStringList& parsingNames, DeleteMode mode, quintptr owner);

// SetForegroundWindow on one of our windows, before a popup (T-092): a popup
// closes on an outside click, and gets the keys, only when its thread is in front.
void bringToForeground(quintptr hwnd);

// The items as one shell data object (IShellItemArray -> BHID_DataObject):
// CF_HDROP for files, the shell's ID list for every item, virtual ones
// included. Returned as an IDataObject* (AddRef'd), or null.
void* createShellDataObject(const QStringList& parsingNames);
void releaseComObject(void* object);
// For the probes: which formats a data object offers.
bool shellDataObjectFormats(void* dataObject, bool* hdrop, bool* idList);

// Drags the items with the shell's own drag loop and images (SHDoDragDrop).
// Modal: returns when the button comes up, with the DROPEFFECT the target
// chose. While it runs, internalDragActive() is true and says what is carried.
int dragShellItems(const QStringList& parsingNames, const QPoint& origin, quintptr owner);
bool internalDragActive();
QStringList internalDragPaths();
QPoint internalDragOrigin();
// The probes stand in for a drag: what dragShellItems would set.
void beginInternalDrag(const QStringList& parsingNames, const QPoint& origin);
void endInternalDrag();

// The drawn desktop's drop target (an IDropTarget*, refcount 1). A drag of
// our own icons (internalDragActive) is ours: `internalDrop(point, paths,
// origin)` moves them. Anything else -- files from an Explorer window, an
// attachment -- goes to the desktop folder's own drop target, which copies or
// moves it onto the Desktop exactly as the real desktop would.
using InternalDrop = std::function<void(const QPoint& screen, const QStringList& paths,
                                        const QPoint& origin)>;
// `foreignDrop` is told where a drop from elsewhere happens, just before the
// desktop folder takes it, so what it creates can be placed under the cursor;
// `stems` are the dropped files' base names (CF_HDROP), empty when it has none.
using ForeignDrop = std::function<void(const QPoint& screen, const QStringList& stems)>;
void* createDesktopDropTarget(quintptr hwnd, InternalDrop internalDrop, ForeignDrop foreignDrop = {});

} // namespace starfish::platform::win