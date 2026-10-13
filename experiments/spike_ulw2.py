import ctypes, ctypes.wintypes as w
u32, g32 = ctypes.windll.user32, ctypes.windll.gdi32
WS_EX_LAYERED = 0x80000; WS_POPUP = 0x80000000; WS_CHILD = 0x40000000
ULW_ALPHA = 2; AC_SRC_ALPHA = 1; LWA_ALPHA = 2

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
    ctypes.memset(bits, 0x80, wpx*hpx*4)
    g32.SelectObject(hdc, dib)
    return hdc, dib

def try_ulw(hwnd, x, y, label, hdc_dst=None):
    hdcSrc, dib = make_dib()
    dst = w.POINT(x, y); size = w.SIZE(64, 64); src = w.POINT(0, 0)
    blend = BLENDFUNCTION(0, 0, 255, AC_SRC_ALPHA)
    ctypes.windll.kernel32.SetLastError(0)
    ok = u32.UpdateLayeredWindow(hwnd, hdc_dst, ctypes.byref(dst), ctypes.byref(size),
                                 hdcSrc, ctypes.byref(src), 0, ctypes.byref(blend), ULW_ALPHA)
    print(f"{label}: -> {ok}, err {ctypes.GetLastError()}")
    g32.DeleteObject(dib); g32.DeleteDC(hdcSrc)
    return ok

progman = u32.FindWindowW("Progman", None)

def make_hosted(x):
    h = u32.CreateWindowExW(WS_EX_LAYERED, "Static", None, WS_POPUP, x, 200, 64, 64, None, None, None, None)
    u32.SetParent(h, progman)
    st = u32.GetWindowLongW(h, -16)
    u32.SetWindowLongW(h, -16, ctypes.c_long(((st | WS_CHILD) & ~WS_POPUP) & 0xFFFFFFFF).value)
    u32.ShowWindow(h, 1)
    return h

# D: SetLayeredWindowAttributes FIRST, then ULW (the starfish sequence)
d = make_hosted(200)
r = u32.SetLayeredWindowAttributes(d, 0, 255, LWA_ALPHA)
print(f"D: SetLayeredWindowAttributes -> {r}, err {ctypes.GetLastError()}")
try_ulw(d, 200, 200, "D: ULW after SLWA")

# E: ULW with explicit screen DC (GetDC(NULL)) instead of NULL
e = make_hosted(300)
hdcScreen = u32.GetDC(None)
try_ulw(e, 300, 200, "E: ULW with hdcDst=GetDC(NULL)", hdc_dst=hdcScreen)
u32.ReleaseDC(None, hdcScreen)

u32.DestroyWindow(d); u32.DestroyWindow(e)
