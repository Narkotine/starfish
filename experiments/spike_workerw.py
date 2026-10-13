"""StarFish spike #1 -- desktop window placement (validates ADR-4).

Goal: render StarFish fence windows on the desktop, above the wallpaper but
below the native desktop icons.

Findings on the dev machine (Windows 11, Fences 6 installed and running):

  * Windows 11 (recent builds) keeps SHELLDLL_DefView as a *direct child of
    Progman*. The classic Win10-era WorkerW trick (0x052C, DefView re-parenting)
    does not happen here; existing WorkerW windows belong to third-party tools.
  * Tk(inter) toplevels cannot be used for the probe: Tk re-stacks its windows
    after SetWindowPos (overrideredirect + internal raise), which forces them
    above the icon layer. The window below is a pure Win32 window built with
    ctypes -- the same sequence the Qt implementation will use.
  * z-order verification uses the Progman child chain (authoritative), plus
    ChildWindowFromPointEx hit tests. Note: with Fences active the native
    DefView is DISABLED, so hit tests may skip it -- the child chain is the
    reliable verdict.

Run (system Python; pure ctypes, no third-party deps):

    python experiments/spike_workerw.py --seconds 3
    python experiments/spike_workerw.py --dump

Shows an orange test window for N seconds, saving a screenshot of the desktop
while it is up. Never modifies the registry; the icon-hiding procedure is only
printed (--test-hide-icons).
"""

import argparse
import ctypes
import os
import struct
import sys
import zlib

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

# --- ctypes type declarations -------------------------------------------------

class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class PAINTSTRUCT(ctypes.Structure):
    _fields_ = [("hdc", ctypes.c_void_p), ("fErase", ctypes.c_int),
                ("rcPaint", RECT), ("fRestore", ctypes.c_int),
                ("fIncUpdate", ctypes.c_int), ("rgbReserved", ctypes.c_byte * 32)]


WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_uint,
                             ctypes.c_void_p, ctypes.c_void_p)

VOID_P = ctypes.c_void_p

user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowW.restype = VOID_P
user32.FindWindowExW.argtypes = [VOID_P, VOID_P, ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowExW.restype = VOID_P
user32.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_long]
user32.EnumChildWindows.argtypes = [VOID_P, ctypes.c_void_p, ctypes.c_long]
user32.GetClassNameW.argtypes = [VOID_P, ctypes.c_wchar_p, ctypes.c_uint]
user32.GetWindowRect.argtypes = [VOID_P, ctypes.c_void_p]
user32.GetClientRect.argtypes = [VOID_P, ctypes.c_void_p]
user32.GetWindowLongW.argtypes = [VOID_P, ctypes.c_int]
user32.GetWindowLongW.restype = ctypes.c_long
user32.SetWindowLongW.argtypes = [VOID_P, ctypes.c_int, ctypes.c_long]
user32.SetWindowLongW.restype = ctypes.c_long
user32.SetParent.argtypes = [VOID_P, VOID_P]
user32.SetWindowPos.argtypes = [VOID_P, VOID_P, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.ShowWindow.argtypes = [VOID_P, ctypes.c_int]
user32.UpdateWindow.argtypes = [VOID_P]
user32.IsWindowVisible.argtypes = [VOID_P]
user32.GetWindow.argtypes = [VOID_P, ctypes.c_uint]
user32.GetWindow.restype = VOID_P
user32.WindowFromPoint.argtypes = [POINT]
user32.WindowFromPoint.restype = VOID_P
user32.ChildWindowFromPointEx.argtypes = [VOID_P, POINT, ctypes.c_uint]
user32.ChildWindowFromPointEx.restype = VOID_P
user32.BeginPaint.argtypes = [VOID_P, ctypes.c_void_p]
user32.BeginPaint.restype = VOID_P
user32.EndPaint.argtypes = [VOID_P, ctypes.c_void_p]
user32.FillRect.argtypes = [VOID_P, ctypes.c_void_p, VOID_P]
user32.DrawTextW.argtypes = [VOID_P, ctypes.c_wchar_p, ctypes.c_int,
                             ctypes.c_void_p, ctypes.c_uint]
user32.DefWindowProcW.argtypes = [VOID_P, ctypes.c_uint, VOID_P, VOID_P]
user32.DefWindowProcW.restype = ctypes.c_long
user32.CreateWindowExW.argtypes = [ctypes.c_uint, ctypes.c_wchar_p,
                                   ctypes.c_wchar_p, ctypes.c_uint,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, VOID_P, VOID_P, VOID_P, VOID_P]
user32.CreateWindowExW.restype = VOID_P
user32.SetTimer.argtypes = [VOID_P, VOID_P, ctypes.c_uint, VOID_P]
user32.DestroyWindow.argtypes = [VOID_P]
user32.PostQuitMessage.argtypes = [ctypes.c_int]
user32.GetMessageW.argtypes = [ctypes.c_void_p, VOID_P, ctypes.c_uint, ctypes.c_uint]
user32.DispatchMessageW.argtypes = [ctypes.c_void_p]

gdi32.CreateSolidBrush.argtypes = [ctypes.c_uint]
gdi32.CreateSolidBrush.restype = VOID_P
gdi32.DeleteObject.argtypes = [VOID_P]

GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_DISABLED = 0x8000000
WS_EX_TOPMOST = 0x00000008
HWND_BOTTOM = 1
SW_HIDE = 0
SW_SHOW = 5
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040
SRCCOPY = 0x00CC0020
WM_PAINT = 0x000F
WM_ERASEBKGND = 0x0014
WM_TIMER = 0x0113
WM_DESTROY = 0x0002
DT_CENTER = 0x0001
DT_VCENTER = 0x0004
DT_SINGLELINE = 0x0020

ORANGE = 0x00227EE6  # COLORREF 0x00BBGGRR of #E67E22


# --- helpers -------------------------------------------------------------------

def find_window(cls, title=None):
    return user32.FindWindowW(cls, title)


def find_child(parent, cls):
    return user32.FindWindowExW(parent, None, cls, None)


def rect(hwnd):
    r = RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(r)):
        return None
    return (r.left, r.top, r.right, r.bottom)


def visible(hwnd):
    return bool(user32.IsWindowVisible(hwnd))


def enumerate_toplevel():
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


def capture_screen(path):
    """Screenshot of the primary screen to PNG (pure ctypes + zlib, no deps)."""
    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [
            ("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
            ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
            ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
            ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
            ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
            ("biClrImportant", ctypes.c_uint32),
        ]

    w = user32.GetSystemMetrics(0)
    h = user32.GetSystemMetrics(1)
    hdc = user32.GetDC(0)
    mem = gdi32.CreateCompatibleDC(hdc)
    hbmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, hbmp)
    gdi32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY)

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h  # negative = top-down rows
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0  # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    lines = gdi32.GetDIBits(mem, hbmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(0, hdc)

    if lines != h:
        print(f"[spike] capture FAILED (GetDIBits returned {lines})")
        return False

    src = buf.raw
    size = w * h
    out = bytearray(size * 4)
    out[0::4] = src[2::4]
    out[1::4] = src[1::4]
    out[2::4] = src[0::4]
    out[3::4] = b"\xff" * size

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    row = w * 4
    for y in range(h):
        raw.append(0)  # PNG filter: none
        raw += out[y * row:(y + 1) * row]

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    print(f"[spike] screenshot saved   : {path} ({w}x{h})")
    return True


def diagnose_progman_children(progman, our_hwnd=None):
    """Print Progman's child windows in z-order + DefView style.

    The child z-order chain is the authoritative ordering between our window
    and SHELLDLL_DefView (hit tests are unreliable: a disabled DefView -- the
    case with Fences active -- is skipped by hit tests).
    """
    children = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        children.append((hwnd, buf.value))
        return True

    user32.EnumChildWindows(progman, cb, 0)
    print(f"[spike] Progman children    : {len(children)} (z-order, top first)")
    our_idx = defview_idx = -1
    for i, (h, cls) in enumerate(children):
        mark = ""
        if our_hwnd and h == our_hwnd:
            our_idx = i
            mark = " <-- our window"
        if cls == "SHELLDLL_DefView":
            defview_idx = i
        print(f"[spike]   [{i:>2}] {h:#010x} {cls!r:24} {rect(h)}{mark}")
    if defview_idx >= 0:
        style = user32.GetWindowLongW(children[defview_idx][0], GWL_STYLE)
        print(f"[spike] DefView DISABLED    : {bool(style & WS_DISABLED)}")
    if our_idx >= 0 and defview_idx >= 0:
        ok = our_idx > defview_idx
        print(f"[spike] z-order verdict     : ours=[{our_idx}] defview=[{defview_idx}]"
              f" -> {'BELOW icon layer (good)' if ok else 'ABOVE icon layer (BAD)'}")
        return ok
    return False


def show_test_window(parent, insert_below, x, y, w, h, seconds, capture_path):
    """Pure Win32 window: the reference sequence for the Qt port (ADR-4)."""
    proc_holder = {}

    def paint(hwnd, hdc, rc):
        brush = gdi32.CreateSolidBrush(ORANGE)
        user32.FillRect(hdc, ctypes.byref(rc), brush)
        gdi32.DeleteObject(brush)
        text = ctypes.create_unicode_buffer(
            f"StarFish spike -- {seconds}s (above wallpaper, below icons)")
        user32.DrawTextW(hdc, text, -1, ctypes.byref(rc),
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE)

    def wnd_proc(hwnd, msg, wparam, lparam):
        msg = int(msg)
        if msg == WM_ERASEBKGND:
            rc = RECT()
            user32.GetClientRect(hwnd, ctypes.byref(rc))
            paint(hwnd, int(wparam), rc)
            return 1
        if msg == WM_PAINT:
            ps = PAINTSTRUCT()
            hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
            paint(hwnd, hdc, ps.rcPaint)
            user32.EndPaint(hwnd, ctypes.byref(ps))
            return 0
        if msg == WM_TIMER:
            tid = int(wparam)
            if tid == 1:
                user32.DestroyWindow(hwnd)
                return 0
            if tid == 2 and capture_path:
                capture_screen(capture_path)
                user32.KillTimer(hwnd, 2)
                return 0
        if msg == WM_DESTROY:
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    proc = WNDPROC(wnd_proc)
    proc_holder["proc"] = proc  # keep alive for the class lifetime

    class WNDCLASSEX(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("style", ctypes.c_uint),
                    ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                    ("cbWndExtra", ctypes.c_int), ("hInstance", ctypes.c_void_p),
                    ("hIcon", ctypes.c_void_p), ("hCursor", ctypes.c_void_p),
                    ("hbrBackground", ctypes.c_void_p),
                    ("lpszMenuName", ctypes.c_wchar_p),
                    ("lpszClassName", ctypes.c_wchar_p),
                    ("hIconSm", ctypes.c_void_p)]

    cls_name = "StarFishSpikeWnd"
    hinst = kernel32.GetModuleHandleW(None)
    wc = WNDCLASSEX()
    wc.cbSize = ctypes.sizeof(WNDCLASSEX)
    wc.lpfnWndProc = proc
    wc.hInstance = hinst
    wc.lpszClassName = cls_name
    if not user32.RegisterClassExW(ctypes.byref(wc)):
        print(f"[spike] RegisterClassEx failed: {kernel32.GetLastError()}",
              file=sys.stderr)
        return False

    hwnd = user32.CreateWindowExW(
        0, cls_name, None, WS_POPUP, x, y, w, h, 0, 0, hinst, 0)
    if not hwnd:
        print(f"[spike] CreateWindowEx failed: {kernel32.GetLastError()}",
              file=sys.stderr)
        return False
    print(f"[spike] test window hwnd   : {hwnd:#x} {rect(hwnd)}")

    user32.ShowWindow(hwnd, SW_SHOW)
    user32.UpdateWindow(hwnd)

    ok = user32.SetParent(hwnd, parent)
    print(f"[spike] SetParent -> Progman {parent:#x}: ok={bool(ok)}")

    style = user32.GetWindowLongW(hwnd, GWL_STYLE)
    user32.SetWindowLongW(hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP)
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST)

    insert_after = insert_below if insert_below else HWND_BOTTOM
    ok = user32.SetWindowPos(hwnd, insert_after, x, y, w, h,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW)
    user32.UpdateWindow(hwnd)
    print(f"[spike] SetWindowPos        : ok={bool(ok)} rect={rect(hwnd)}")
    print(f"[spike] visible             : {visible(hwnd)}")

    ok_chain = diagnose_progman_children(parent, hwnd)

    user32.SetTimer(hwnd, 1, seconds * 1000, None)
    if capture_path:
        user32.SetTimer(hwnd, 2, 1000, None)

    print(f"[spike] window visible for {seconds}s ...")
    class MSG(ctypes.Structure):
        _fields_ = [("hwnd", ctypes.c_void_p), ("message", ctypes.c_uint),
                    ("wParam", ctypes.c_void_p), ("lParam", ctypes.c_void_p),
                    ("time", ctypes.c_uint), ("pt", POINT)]

    msg = MSG()
    while user32.GetMessageW(ctypes.byref(msg), 0, 0, 0) > 0:
        user32.DispatchMessageW(ctypes.byref(msg))

    print("[spike] closed cleanly")
    return ok_chain


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=10,
                        help="how long the test window stays visible")
    parser.add_argument("--x", type=int, default=1030)
    parser.add_argument("--y", type=int, default=590)
    parser.add_argument("--w", type=int, default=500)
    parser.add_argument("--h", type=int, default=260,
                        help="window position/size in screen pixels")
    parser.add_argument("--capture", default="experiments/out/spike_check.png",
                        help="screenshot path taken while the window is up "
                             "(empty string to disable)")
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

    if not progman or not defview:
        print("[spike] FAIL: Progman or DefView not found", file=sys.stderr)
        return 1

    ok = show_test_window(progman, defview, args.x, args.y, args.w, args.h,
                          args.seconds, args.capture or None)

    if args.test_hide_icons:
        print("[spike] icon hiding (procedure only): ShowWindow(SW_HIDE) on the "
              "SHELLDLL_DefView window, restore with SW_SHOW on exit; no "
              "registry write needed on this Win11 build.")

    print(f"[spike] {'OK' if ok else 'Z-ORDER NOT BELOW ICON LAYER'}")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
