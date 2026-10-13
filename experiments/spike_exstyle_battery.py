"""StarFish -- ex-style battery, Python side (T-020).

Companion to `experiments/spike_surface_min.cpp`.

The C++ reproducer measured this on a child of Progman:

    WS_EX_TOPMOST     0x8         -> REFUSED   (expected: a child cannot be topmost)
    WS_EX_TRANSPARENT 0x20        -> STUCK
    WS_EX_TOOLWINDOW  0x80        -> STUCK
    WS_EX_NOACTIVATE  0x8000000   -> STUCK
    WS_EX_LAYERED     0x80000     -> REFUSED   <-- only this one

...which says GWL_EXSTYLE is perfectly writable and the LAYERED bit alone is
refused. But `spike_workerw.py` sets exactly that bit on exactly that kind of
window, *after* SetParent, and reads it back as 0x80000.

This script runs the same battery through ctypes on the same kind of window, so
the two tables can be compared line by line. If Python's LAYERED write sticks
where C++'s does not, the divergence is environmental (process, not API) and the
C++ side must be brought in line. If Python's is refused too, then
spike_workerw.py's success comes from something else in its sequence.

Run:  python experiments/spike_exstyle_battery.py
"""

import ctypes
import sys

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

VOID_P = ctypes.c_void_p
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_long, VOID_P, ctypes.c_uint, VOID_P, VOID_P)

user32.CreateWindowExW.argtypes = [ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                   ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, VOID_P, VOID_P, VOID_P, VOID_P]
user32.CreateWindowExW.restype = VOID_P
user32.GetWindowLongW.argtypes = [VOID_P, ctypes.c_int]
user32.GetWindowLongW.restype = ctypes.c_long
user32.SetWindowLongW.argtypes = [VOID_P, ctypes.c_int, ctypes.c_long]
user32.SetWindowLongW.restype = ctypes.c_long
user32.SetParent.argtypes = [VOID_P, VOID_P]
user32.SetParent.restype = VOID_P
user32.GetParent.argtypes = [VOID_P]
user32.GetParent.restype = VOID_P
user32.ShowWindow.argtypes = [VOID_P, ctypes.c_int]
user32.UpdateWindow.argtypes = [VOID_P]
user32.SetLayeredWindowAttributes.argtypes = [VOID_P, ctypes.c_uint, ctypes.c_ubyte,
                                              ctypes.c_uint]
user32.SetLayeredWindowAttributes.restype = ctypes.c_int
user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowW.restype = VOID_P
user32.FindWindowExW.argtypes = [VOID_P, VOID_P, ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowExW.restype = VOID_P
user32.GetClassNameW.argtypes = [VOID_P, ctypes.c_wchar_p, ctypes.c_uint]
user32.RegisterClassExW.argtypes = [VOID_P]
user32.RegisterClassExW.restype = ctypes.c_ushort
user32.DefWindowProcW.argtypes = [VOID_P, ctypes.c_uint, VOID_P, VOID_P]
user32.DefWindowProcW.restype = ctypes.c_long
gdi32.CreateSolidBrush.argtypes = [ctypes.c_uint]
gdi32.CreateSolidBrush.restype = VOID_P
kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
kernel32.GetModuleHandleW.restype = VOID_P

GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_EX_TOPMOST = 0x00000008
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_LAYERED = 0x00080000
LWA_ALPHA = 0x2
SW_SHOW = 5
ORANGE = 0x00227EE6


class WNDCLASSEX(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("style", ctypes.c_uint),
                ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", VOID_P),
                ("hIcon", VOID_P), ("hCursor", VOID_P), ("hbrBackground", VOID_P),
                ("lpszMenuName", ctypes.c_wchar_p), ("lpszClassName", ctypes.c_wchar_p),
                ("hIconSm", VOID_P)]


_proc_holder = {}


def make_class(name):
    proc = WNDPROC(lambda hwnd, msg, wp, lp: user32.DefWindowProcW(hwnd, msg, wp, lp))
    _proc_holder[name] = proc
    wc = WNDCLASSEX()
    wc.cbSize = ctypes.sizeof(WNDCLASSEX)
    wc.lpfnWndProc = proc
    wc.hInstance = kernel32.GetModuleHandleW(None)
    wc.lpszClassName = name
    wc.hbrBackground = gdi32.CreateSolidBrush(ORANGE)
    if not user32.RegisterClassExW(ctypes.byref(wc)):
        print(f"RegisterClassExW({name}) failed: {kernel32.GetLastError()}")
        return False
    return True


def class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value or "<null>"


def set_bits(hwnd, bits, label):
    base = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    kernel32.SetLastError(0)
    previous = user32.SetWindowLongW(hwnd, GWL_EXSTYLE, base | bits)
    error = kernel32.GetLastError()
    read = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    verdict = "STUCK" if (read & bits) else "REFUSED"
    print(f"  {label:<26} wrote {base | bits:#x} prev {previous:#x} "
          f"read {read:#x} err {error} -> {verdict}")
    if read & bits:
        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, base)


def battery(hwnd, title):
    print(f"--- battery on {title} ---")
    set_bits(hwnd, WS_EX_TOPMOST, "WS_EX_TOPMOST 0x8")
    set_bits(hwnd, WS_EX_TRANSPARENT, "WS_EX_TRANSPARENT 0x20")
    set_bits(hwnd, WS_EX_TOOLWINDOW, "WS_EX_TOOLWINDOW 0x80")
    set_bits(hwnd, WS_EX_NOACTIVATE, "WS_EX_NOACTIVATE 0x8000000")
    set_bits(hwnd, WS_EX_LAYERED, "WS_EX_LAYERED 0x80000")
    kernel32.SetLastError(0)
    ok = user32.SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)
    print(f"  SetLayeredWindowAttributes -> ok={bool(ok)} err={kernel32.GetLastError()}")


def main():
    # `python spike_exstyle_battery.py child-only` runs the Progman case in a
    # process that has never created a layered window before, to check whether
    # the earlier cases are a precondition for the write to be accepted.
    child_only = len(sys.argv) > 1 and sys.argv[1] == "child-only"

    progman = user32.FindWindowW("Progman", None)
    defview = user32.FindWindowExW(progman, None, "SHELLDLL_DefView", None)
    print(f"Progman {progman:#x}, SHELLDLL_DefView {defview:#x}")
    if not progman or not defview:
        print("FAIL: no desktop", file=sys.stderr)
        return 1

    if child_only:
        print("--- child-only: no other window has been created in this process ---")
        make_class("SfBatteryOnly")
        only = user32.CreateWindowExW(0, "SfBatteryOnly", None, WS_POPUP, 1500, 700, 200, 120,
                                      None, None, kernel32.GetModuleHandleW(None), None)
        user32.ShowWindow(only, SW_SHOW)
        user32.UpdateWindow(only)
        user32.SetParent(only, progman)
        style = user32.GetWindowLongW(only, GWL_STYLE)
        user32.SetWindowLongW(only, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP)
        print(f"  style after hosting: {user32.GetWindowLongW(only, GWL_STYLE):#x}, "
              f"parent {user32.GetParent(only) or 0:#x}")
        battery(only, "a child of Progman in a fresh process")
        user32.DestroyWindow(only)
        return 0

    # 1. Plain top-level window, never reparented: the control.
    make_class("SfBatteryTop")
    top = user32.CreateWindowExW(0, "SfBatteryTop", None, WS_POPUP, 1500, 100, 200, 120,
                                 None, None, kernel32.GetModuleHandleW(None), None)
    user32.ShowWindow(top, SW_SHOW)
    user32.UpdateWindow(top)
    battery(top, "a plain top-level window (no SetParent)")
    user32.DestroyWindow(top)

    # 2. Created with WS_EX_LAYERED, then reparented (the C++ DesktopSurface order
    #    that used to be described as "the only order Windows accepts").
    make_class("SfBatteryPre")
    pre = user32.CreateWindowExW(WS_EX_LAYERED, "SfBatteryPre", None, WS_POPUP,
                                 1500, 300, 200, 120, None, None,
                                 kernel32.GetModuleHandleW(None), None)
    user32.ShowWindow(pre, SW_SHOW)
    user32.UpdateWindow(pre)
    print(f"--- created with WS_EX_LAYERED: read back "
          f"{user32.GetWindowLongW(pre, GWL_EXSTYLE):#x} ---")
    user32.SetParent(pre, progman)
    style = user32.GetWindowLongW(pre, GWL_STYLE)
    user32.SetWindowLongW(pre, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP)
    print(f"--- after SetParent(Progman) + WS_CHILD: ex "
          f"{user32.GetWindowLongW(pre, GWL_EXSTYLE):#x}, parent {user32.GetParent(pre) or 0:#x} ---")
    kernel32.SetLastError(0)
    ok = user32.SetLayeredWindowAttributes(pre, 0, 255, LWA_ALPHA)
    print(f"  SetLayeredWindowAttributes -> ok={bool(ok)} err={kernel32.GetLastError()}")
    user32.DestroyWindow(pre)

    # 3. The spike's own order: plain, then SetParent, then WS_CHILD, then layered.
    make_class("SfBatteryPost")
    post = user32.CreateWindowExW(0, "SfBatteryPost", None, WS_POPUP, 1500, 500, 200, 120,
                                  None, None, kernel32.GetModuleHandleW(None), None)
    user32.ShowWindow(post, SW_SHOW)
    user32.UpdateWindow(post)
    user32.SetParent(post, progman)
    style = user32.GetWindowLongW(post, GWL_STYLE)
    user32.SetWindowLongW(post, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP)
    print(f"--- after SetParent(Progman) + WS_CHILD: parent "
          f"{user32.GetParent(post) or 0:#x} ('{class_name(user32.GetParent(post))}') ---")
    battery(post, "a child of Progman, layered set AFTER SetParent (spike order)")
    user32.DestroyWindow(post)
    return 0


if __name__ == "__main__":
    sys.exit(main())
