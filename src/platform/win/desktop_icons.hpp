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

// True while the shell is actually *drawing* its icons (SysListView32 visible).
//
// This is the precondition of the read, and it had to be measured to be believed:
// with the icon layer hidden -- which is what another instance of this very
// application does when it takes the desktop -- the same list view still answers
// `LVM_GETITEMCOUNT` = 88, but with **every name empty** and positions running
// 1,1 / 1,2 / 1,3: a layout no desktop has. Nothing reads positions or names from
// it while it is hidden, and the product's timer skips the read entirely.
bool nativeDesktopIconsVisible();

// There is deliberately **no** "park an icon" here. Moving a single icon out of
// the way was written, measured and abandoned: `LVM_SETITEMPOSITION` with
// (-32000, -32000) reads back as (8,2) on this desktop -- the item lands in the
// top-left corner, which is worse than the duplicate it was meant to avoid, and
// the position it left cannot be recovered once the record of it is gone. What a
// caller wants instead is in the product: hide the *list* (never the DefView, so
// the wallpaper survives) and draw its icons at the positions read above.

} // namespace starfish::platform::win
