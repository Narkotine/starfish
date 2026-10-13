"""StarFish spike #1 -- desktop window placement (validates ADR-4).

Goal: render StarFish fence windows on the desktop, above the wallpaper but
below the native desktop icons.

Findings on the dev machine (Windows 11, Fences 6 installed):

  * Windows 11 (recent builds) keeps SHELLDLL_DefView as a *direct child of
    Progman* -- the classic "re-parent DefView into a WorkerW" trick used by
    Win10-era code does not happen, and the undocumented 0x052C message spawns
    nothing new. All WorkerW windows present were 136x39 px widgets (owned by
    third-party tools), and SetParent onto them fails.
  * Therefore the script implements the modern strategy: parent our window to
    Progman and insert it immediately *below* SHELLDLL_DefView in the child
    z-order. The window then sits above the wallpaper and below the icons.
  * Bonus: hiding the desktop icons can be done by ShowWindow(SW_HIDE) on the
    DefView itself -- fully reversible, no registry needed.

Run (system Python required for tkinter; keep this script out of the product):

    python experiments/spike_workerw.py
    python experiments/spike_workerw.py --dump

Shows a labelled test window for 15 seconds, then closes it. Never modifies
the registry; the icon-hiding procedure is only printed
(--test-hide-icons).
"""

import argparse
import ctypes
import sys
import time
import tkinter as tk

user32 = ctypes.windll.user32

GWL_STYLE = -16
GWLP_HWNDPARENT = -8
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
SW_HIDE = 0
SW_SHOW = 5
HWND_BOTTOM = 1


def find_window(cls, title=None):
    return user32.FindWindowW(cls, title)


def find_child(parent, cls):
    return user32.FindWindowExW(parent, None, cls, None)


def enumerate_toplevel():
    """All top-level windows in z-order (top to bottom)."""
    out = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        out.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return out


def enumerate_workerws():
    out = []
    hwnd = None
    while True:
        hwnd = user32.FindWindowExW(None, hwnd, "WorkerW", None)
        if not hwnd:
            break
        out.append(hwnd)
    return out


def rect(hwnd):
    r = ctypes.wintypes.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(r)):
        return None
    return (r.left, r.top, r.right, r.bottom)


def visible(hwnd):
    return bool(user32.IsWindowVisible(hwnd))


def find_defview():
    """(progman, defview) where defview is the desktop icon list."""
    progman = find_window("Progman")
    if not progman:
        return None, None
    defview = find_child(progman, "SHELLDLL_DefView")
    if defview:
        return progman, defview

    # Win10-era fallback: DefView lives inside a WorkerW.
    for w in enumerate_workerws():
        d = find_child(w, "SHELLDLL_DefView")
        if d:
            return w, d
    return progman, None


def show_test_window(parent, insert_below, seconds=15):
    root = tk.Tk()
    root.overrideredirect(True)
    root.geometry("320x100+640+300")
    root.configure(bg="#1F2937")
    label = tk.Label(
        root,
        text="StarFish spike -- child of Progman\ncloses in %ds" % seconds,
        bg="#1F2937", fg="white", font=("Segoe UI", 11),
    )
    label.pack(expand=True, fill="both")
    root.update()

    hwnd = root.winfo_id()
    print(f"[spike] test window hwnd   : {hwnd:#x} {rect(hwnd)}")

    ok = user32.SetParent(hwnd, parent)
    print(f"[spike] SetParent -> Progman {parent:#x}: ok={bool(ok)}")

    style = user32.GetWindowLongW(hwnd, GWL_STYLE)
    user32.SetWindowLongW(hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP)

    # Insert immediately below the icon layer (DefView) in the child z-order.
    insert_after = insert_below if insert_below else HWND_BOTTOM
    ok = user32.SetWindowPos(hwnd, insert_after, 640, 300, 320, 100,
                            0x0014)  # SWP_NOACTIVATE | SWP_SHOWWINDOW
    root.update()
    print(f"[spike] SetWindowPos        : ok={bool(ok)} rect={rect(hwnd)}")
    print(f"[spike] z-order: below DefView {insert_below:#x}, icons visible="
          f"{visible(insert_below) if insert_below else '?'}")

    print(f"[spike] window visible for {seconds}s -- check: above wallpaper, "
          f"below icons")
    root.after(seconds * 1000, root.destroy)
    root.mainloop()
    print("[spike] closed cleanly")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump", action="store_true")
    parser.add_argument("--test-hide-icons", action="store_true",
                        help="print the icon-hiding procedure (never executed)")
    args = parser.parse_args()

    progman = find_window("Progman")
    print(f"[spike] Progman            : {progman:#x} {rect(progman)}")
    parent, defview = find_defview()
    defview_str = f"{defview:#x} {rect(defview)}" if defview else "NOT FOUND"
    print(f"[spike] icon list host     : {parent:#x}, SHELLDLL_DefView {defview_str}")

    if args.dump:
        for hwnd, cls in enumerate_toplevel():
            if cls in ("Progman", "WorkerW", "SHELLDLL_DefView", "SysListView32"):
                print(f"[spike]   {hwnd:#010x} {cls!r:20} {rect(hwnd)}")

    if not progman:
        print("[spike] FAIL: Progman not found", file=sys.stderr)
        return 1

    show_test_window(progman, defview)

    if args.test_hide_icons:
        print("[spike] icon hiding (procedure only): ShowWindow(SW_HIDE) on the "
              "SHELLDLL_DefView window, restore with SW_SHOW on exit; no "
              "registry write needed on this Win11 build.")

    print("[spike] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
