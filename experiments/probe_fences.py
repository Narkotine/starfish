"""StarFish desktop probe #2 -- how does a working app (Fences) host windows
on the desktop? Enumerates top-level windows and desktop-hosted children with
their parent / style / ex-style so the spike can copy the working recipe.

    python experiments/probe_fences.py
"""

import ctypes

u = ctypes.windll.user32
VOID_P = ctypes.c_void_p
u.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_long]
u.GetClassNameW.argtypes = [VOID_P, ctypes.c_wchar_p, ctypes.c_uint]
u.GetWindowTextW.argtypes = [VOID_P, ctypes.c_wchar_p, ctypes.c_int]
u.GetWindowRect.argtypes = [VOID_P, ctypes.c_void_p]
u.GetWindowLongW.argtypes = [VOID_P, ctypes.c_int]
u.GetWindowLongW.restype = ctypes.c_long
u.GetWindow.argtypes = [VOID_P, ctypes.c_uint]
u.GetWindow.restype = VOID_P
u.GetAncestor.argtypes = [VOID_P, ctypes.c_uint]
u.GetAncestor.restype = VOID_P
u.IsWindowVisible.argtypes = [VOID_P]

GW_CHILD, GW_HWNDNEXT, GW_OWNER = 5, 2, 4
GA_ROOT, GA_PARENT = 2, 1
GWL_STYLE, GWL_EXSTYLE = -16, -20

WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_LAYERED = 0x00080000


class RECT(ctypes.Structure):
    _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long),
                ("r", ctypes.c_long), ("b", ctypes.c_long)]


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def title(h):
    b = ctypes.create_unicode_buffer(512)
    u.GetWindowTextW(h, b, 512)
    return b.value


def rect(h):
    r = RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.l, r.t, r.r - r.l, r.b - r.t)


def style_str(h):
    s = u.GetWindowLongW(h, GWL_STYLE) & 0xFFFFFFFF
    e = u.GetWindowLongW(h, GWL_EXSTYLE) & 0xFFFFFFFF
    tags = []
    tags.append("CHILD" if s & WS_CHILD else "POPUP" if s & WS_POPUP else "OVERLAP")
    if s & WS_VISIBLE:
        tags.append("VIS")
    if e & WS_EX_TOPMOST:
        tags.append("TOPMOST")
    if e & WS_EX_TOOLWINDOW:
        tags.append("TOOLWIN")
    if e & WS_EX_NOACTIVATE:
        tags.append("NOACT")
    if e & WS_EX_LAYERED:
        tags.append("LAYERED")
    return f"style={s:#010x} ex={e:#010x} [{'|'.join(tags)}]"


def describe(h, indent="  "):
    print(f"{indent}{h:#010x} {cls(h)!r:26} {rect(h)} vis={bool(u.IsWindowVisible(h))}")
    print(f"{indent}   title={title(h)[:40]!r}  {style_str(h)}")
    par = u.GetAncestor(h, GA_PARENT)
    root = u.GetAncestor(h, GA_ROOT)
    own = u.GetWindow(h, GW_OWNER)
    print(f"{indent}   parent={par:#x} {cls(par)!r} root={root:#x} {cls(root)!r} "
          f"owner={(own or 0):#x}")


def main():
    print("=== top-level windows (non-empty title or Fences-ish class) ===")
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        t = title(hwnd)
        c = cls(hwnd)
        if t or "Fence" in c or "fence" in c:
            found.append(hwnd)
        return True

    u.EnumWindows(cb, 0)
    for h in found:
        if "Fence" in cls(h) or "Fence" in title(h) or title(h) in (
                "Games", "Launchers", "IT", "Harnesses", "Editors", "Code Misc",
                "Gyle", "LLM Servers", "DSI", "Salir libre", "Batch script files"):
            describe(h)

    print("\n=== Progman child chain ===")
    prog = u.FindWindowW("Progman", None)
    h = u.GetWindow(prog, GW_CHILD)
    i = 0
    while h:
        describe(h, f"  [{i}] ")
        h = u.GetWindow(h, GW_HWNDNEXT)
        i += 1

    print("\n=== DefView child chain (where the visible zones probably live) ===")
    dv = u.FindWindowExW(prog, None, "SHELLDLL_DefView", None)
    h = u.GetWindow(dv, GW_CHILD)
    i = 0
    while h:
        describe(h, f"  [{i}] ")
        h = u.GetWindow(h, GW_HWNDNEXT)
        i += 1

    print("\n=== Windows hosted under Progman's WorkerW (if any) ===")
    ww = u.FindWindowExW(prog, None, "WorkerW", None)
    while ww:
        r = rect(ww)
        print(f"  WorkerW {ww:#x} {r}")
        c = u.GetWindow(ww, GW_CHILD)
        while c:
            describe(c, "    ")
            c = u.GetWindow(c, GW_HWNDNEXT)
        ww = u.FindWindowExW(prog, ww, "WorkerW", None)


if __name__ == "__main__":
    main()
