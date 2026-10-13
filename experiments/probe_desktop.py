"""StarFish desktop probe -- read-only diagnostics for the spike.

Answers: is Progman visible? which window paints the wallpaper? where does a
Progman child window sit relative to the visible desktop surface?

    python experiments/probe_desktop.py
"""

import ctypes

u = ctypes.windll.user32
VOID_P = ctypes.c_void_p
u.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
u.FindWindowW.restype = VOID_P
u.FindWindowExW.argtypes = [VOID_P, VOID_P, ctypes.c_wchar_p, ctypes.c_wchar_p]
u.FindWindowExW.restype = VOID_P
u.GetWindow.argtypes = [VOID_P, ctypes.c_uint]
u.GetWindow.restype = VOID_P
u.GetClassNameW.argtypes = [VOID_P, ctypes.c_wchar_p, ctypes.c_uint]
u.GetWindowRect.argtypes = [VOID_P, ctypes.c_void_p]
u.IsWindowVisible.argtypes = [VOID_P]
u.WindowFromPoint.argtypes = [ctypes.c_void_p]
u.WindowFromPoint.restype = VOID_P
u.GetAncestor.argtypes = [VOID_P, ctypes.c_uint]
u.GetAncestor.restype = VOID_P

GW_CHILD, GW_HWNDNEXT, GW_HWNDPREV, GW_OWNER = 5, 2, 3, 4
GA_ROOT = 2


class RECT(ctypes.Structure):
    _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long),
                ("r", ctypes.c_long), ("b", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def rect(h):
    r = RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.l, r.t, r.r, r.b)


def vis(h):
    return bool(u.IsWindowVisible(h))


def title(h):
    b = ctypes.create_unicode_buffer(512)
    u.GetWindowTextW(h, b, 512)
    return b.value


def chain(parent, label):
    print(f"--- {label}: immediate children (true z-order, top -> bottom) ---")
    h = u.GetWindow(parent, GW_CHILD)
    i = 0
    while h:
        print(f"  [{i:>2}] {h:#010x} {cls(h)!r:24} vis={str(vis(h)):5} "
              f"rect={rect(h)} title={title(h)[:40]!r}")
        h = u.GetWindow(h, GW_HWNDNEXT)
        i += 1
    if i == 0:
        print("  (none)")


def main():
    prog = u.FindWindowW("Progman", None)
    print(f"Progman {prog:#x} vis={vis(prog)} rect={rect(prog)}")
    dv = u.FindWindowExW(prog, None, "SHELLDLL_DefView", None)
    print(f"DefView {dv:#x} vis={vis(dv)} rect={rect(dv)}")
    lv = u.FindWindowExW(dv, None, "SysListView32", None) if dv else None
    print(f"  SysListView32 {lv:#x} vis={vis(lv)} rect={rect(lv)}")

    chain(prog, "Progman")

    print("--- top-level WorkerW windows (potential wallpaper hosts) ---")
    h = None
    while True:
        h = u.FindWindowExW(None, h, "WorkerW", None)
        if not h:
            break
        r = rect(h)
        if (r[2] - r[0]) > 1000 or (r[3] - r[1]) > 1000:
            print(f"  {h:#010x} WorkerW vis={vis(h)} rect={r}")

    print("--- hit test on desktop points ---")
    for p in [(400, 1000), (1280, 720), (150, 900)]:
        pt = POINT(p[0], p[1])
        hw = u.WindowFromPoint(ctypes.byref(pt))
        root = u.GetAncestor(hw, GA_ROOT)
        print(f"  {p} -> {hw:#x} {cls(hw)!r} (root {root:#x} {cls(root)!r} "
              f"vis={vis(root)})")


if __name__ == "__main__":
    main()
