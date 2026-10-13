import ctypes, ctypes.wintypes as w
u32, g32 = ctypes.windll.user32, ctypes.windll.gdi32

WS_EX_LAYERED = 0x80000; WS_POPUP = 0x80000000; WS_CHILD = 0x40000000
ULW_ALPHA = 2; AC_SRC_ALPHA = 1; SRCCOPY = 0xCC0020

class BLENDFUNCTION(ctypes.Structure):
    _fields_ = [("BlendOp", ctypes.c_ubyte), ("BlendFlags", ctypes.c_ubyte),
                ("SourceConstantAlpha", ctypes.c_ubyte), ("AlphaFormat", ctypes.c_ubyte)]

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", w.DWORD), ("biWidth", w.LONG), ("biHeight", w.LONG),
                ("biPlanes", w.WORD), ("biBitCount", w.WORD), ("biCompression", w.DWORD),
                ("biSizeImage", w.DWORD), ("biXPelsPerMeter", w.LONG), ("biYPelsPerMeter", w.LONG),
                ("biClrUsed", w.DWORD), ("biClrImportant", w.DWORD)]
class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", w.DWORD*3)]

def make_dib(wpx=64, hpx=64):
    hdc = g32.CreateCompatibleDC(None)
    bi = BITMAPINFO(); bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = wpx; bi.bmiHeader.biHeight = -hpx
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = 0
    bits = ctypes.c_void_p()
    dib = g32.CreateDIBSection(hdc, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    ctypes.memset(bits, 0x80, wpx*hpx*4)  # semi-transparent grey, premultiplied
    g32.SelectObject(hdc, dib)
    return hdc, dib

def try_ulw(hwnd, x, y, label):
    hdcSrc, dib = make_dib()
    dst = w.POINT(x, y); size = w.SIZE(64, 64); src = w.POINT(0, 0)
    blend = BLENDFUNCTION(0, 0, 255, AC_SRC_ALPHA)
    ctypes.windll.kernel32.SetLastError(0)
    ok = u32.UpdateLayeredWindow(hwnd, None, ctypes.byref(dst), ctypes.byref(size),
                                 hdcSrc, ctypes.byref(src), 0, ctypes.byref(blend), ULW_ALPHA)
    err = ctypes.GetLastError()
    print(f"{label}: UpdateLayeredWindow -> {ok}, err {err}")
    g32.DeleteObject(dib); g32.DeleteDC(hdcSrc)
    return ok

progman = u32.FindWindowW("Progman", None)
defview = u32.FindWindowExW(progman, None, "SHELLDLL_DefView", None)
print(f"progman={progman:#x} defview={defview:#x}")

# A: child of Progman (cross-process), like DesktopSurface
a = u32.CreateWindowExW(WS_EX_LAYERED, "Static", None, WS_POPUP, 200, 200, 64, 64, None, None, None, None)
u32.SetParent(a, progman)
st = u32.GetWindowLongW(a, -16)  # GWL_STYLE
val = ctypes.c_long(((st | WS_CHILD) & ~WS_POPUP) & 0xFFFFFFFF).value
u32.SetWindowLongW(a, -16, val)
u32.ShowWindow(a, 1)
okA = try_ulw(a, 200, 200, "A: child of Progman (cross-process)")

# B: same-process child of our own top-level window
parent = u32.CreateWindowExW(0, "Static", None, WS_POPUP | 0x10000000, 400, 200, 200, 200, None, None, None, None)
b = u32.CreateWindowExW(WS_EX_LAYERED, "Static", None, WS_CHILD | 0x10000000, 0, 0, 64, 64, parent, None, None, None)
u32.ShowWindow(b, 1)
okB = try_ulw(b, 400, 200, "B: same-process child")

# C: top-level layered window (control)
c = u32.CreateWindowExW(WS_EX_LAYERED, "Static", None, WS_POPUP | 0x10000000, 700, 200, 64, 64, None, None, None, None)
u32.ShowWindow(c, 1)
okC = try_ulw(c, 700, 200, "C: top-level layered")

u32.DestroyWindow(a); u32.DestroyWindow(parent); u32.DestroyWindow(c)
print("SUMMARY:", "cross-process REFUSED" if not okA else "cross-process OK", "|",
      "same-process OK" if okB else "same-process FAILED", "|",
      "top-level OK" if okC else "top-level FAILED")
