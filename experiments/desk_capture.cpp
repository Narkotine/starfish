// desk_capture -- read-only capture of the desktop windows, for verification.
// Usage: desk_capture <progman|defview|screen> <out.png> [x y w h]
// progman/defview: PrintWindow(PW_RENDERFULLCONTENT) -- the window's own
// composed content, even when application windows cover it. screen: BitBlt of
// what is on screen (only meaningful where the desktop is visible).
#include <windows.h>
#include <QImage>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static QImage grab(HWND h, bool screen, RECT r)
{
    const int w = r.right - r.left, hh = r.bottom - r.top;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    BOOL ok = screen ? BitBlt(mdc, 0, 0, w, hh, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT)
                     : PrintWindow(h, mdc, 2 /*PW_RENDERFULLCONTENT*/);
    std::printf("capture ok=%d %dx%d\n", ok, w, hh);
    QImage img(static_cast<uchar*>(bits), w, hh, w * 4, QImage::Format_RGB32);
    QImage copy = img.copy();
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    return copy;
}

int main(int argc, char** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 3) { std::printf("usage\n"); return 2; }
    HWND pm = FindWindowW(L"Progman", nullptr);
    HWND dv = FindWindowExW(pm, nullptr, L"SHELLDLL_DefView", nullptr);
    const bool screen = std::strcmp(argv[1], "screen") == 0;
    HWND h = std::strcmp(argv[1], "defview") == 0 ? dv : pm;
    RECT r{}; GetWindowRect(h, &r);
    if (argc >= 7) { r.left = atoi(argv[3]); r.top = atoi(argv[4]); r.right = r.left + atoi(argv[5]); r.bottom = r.top + atoi(argv[6]); }
    QImage img = grab(h, screen, r);
    if (argc >= 7 && !screen) img = img.copy(r.left, r.top, r.right - r.left, r.bottom - r.top);
    std::printf("saved=%d\n", img.save(QString::fromLocal8Bit(argv[2])));
    return 0;
}
