#pragma once

#include <QPoint>
#include <QSize>
#include <QString>
#include <QVector>

namespace starfish::platform::win {

// One native desktop icon, as Explorer's own list view holds it.
//
// The desktop's icons are not windows: they are items of the SysListView32
// inside SHELLDLL_DefView, drawn by Explorer. Reading them therefore means
// reading another process's list view, and parking one means asking that
// process to move an item -- both go through the LVM_* messages below.
//
// UNITS: screen pixels, physical (T-014). LVM_GETITEMPOSITION answers in the
// list view's *client* coordinates, so the reader adds the view's client origin
// (ClientToScreen) to every position: the product's click zones come from
// WM_NCHITTEST, which hands the cursor in screen coordinates, and the two
// spaces have to coincide. `slot` is a cell step, which translation leaves
// unchanged, so it is the same size in either space.
struct NativeDesktopIcon {
    int index = -1;  // the item's index in the list view (its identity here)
    QPoint position; // top-left corner of the icon, physical screen pixels
    QSize slot;      // cell step from LVM_GETITEMSPACING (cx, cy)
    QString name;    // the item's text
};

// Reads every item of the native desktop list view: count, cell spacing, then
// each item's position and text.
//
// LVM_GETITEMPOSITION and LVM_GETITEMTEXTW write through their lParam, which
// Explorer dereferences in its own address space, so the buffers are allocated
// in Explorer (VirtualAllocEx) and read back with ReadProcessMemory. Returns an
// EMPTY vector, with a warning, when the desktop is not available or any step
// fails -- never a made-up value. An empty desktop returns an empty vector
// without warning: it is a valid answer, not a failure.
QVector<NativeDesktopIcon> readNativeDesktopIcons();

// Parks one native icon by moving it off screen: LVM_SETITEMPOSITION with
// (-32000, -32000), outside every monitor, where the list view clips it away so
// it stops being painted. `index` is the item's index as
// readNativeDesktopIcons() reported it. The desktop must be laid out in
// free-placement mode (auto-arrange off) for a parked item to stay parked.
bool parkNativeDesktopIcon(int index);

// Puts a parked icon back at `position`, a physical screen pixel value as
// readNativeDesktopIcons() returned it.
//
// Both calls report success only when the item's position, re-read after the
// move, is the requested one: LVM_SETITEMPOSITION answers a BOOL, but the view
// can still undo the move (auto-arrange, a layout pass the DefView runs on its
// own repaint), and a move that did not stick must not be reported as one.
bool restoreNativeDesktopIcon(int index, const QPoint& position);

} // namespace starfish::platform::win
