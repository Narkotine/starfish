// spike_layer_alpha_shape.cpp -- T-060 ---------------------------------------
//
// QUESTION
//   StarFish's desktop backdrop (T-039) is a full-screen per-pixel layered
//   window that stands in for the desktop while the native icon layer is
//   hidden. Its frame used to be filled with transparency ("the wallpaper shows
//   through"), and the user reported that a right-drag on the bare desktop --
//   the gesture of T-055 -- did nothing at all.
//
// HYPOTHESIS
//   A per-pixel layered window is hit-tested against its own alpha: Windows
//   lets the mouse through wherever the alpha is zero (MSDN, "Layered
//   Windows"). A frame that is transparent nearly everywhere is therefore a
//   window no click can reach, and the gesture could never start.
//
// WHAT THE PROBES COULD NOT ANSWER
//   They post QMouseEvents straight at the object, so they measure the machine
//   and not the delivery. And they cannot use WindowFromPoint on the backdrop
//   itself: --backdrop-demo reported that even at alpha 1 the cursor found the
//   user's browser, because *every* child of Progman sits below *every*
//   top-level window. That is an environment reading, not a code reading.
//
// THIS SPIKE MEASURES, AND WHAT IT FOUND (2026-10-16, on the dev machine)
//   1. The rule, on a window nothing can be above -- a TOPMOST one of ours:
//      **at alpha 0 the cursor finds something else, at alpha 1 it finds the
//      window itself.** The rule holds.
//   2. The same rule on a layered CHILD window, which is what the backdrop is --
//      a plain parent of ours on top with a per-pixel layered child inside it,
//      the same parent/child relationship the backdrop has with Progman:
//      **at alpha 0 the cursor finds the parent (the child is skipped) and at
//      alpha 1 it finds the child.** That is the bug of T-060 reproduced, and the
//      floor is what fixes it.
//   3. What *cannot* be read: GetWindowRgn reports no region on a layered window
//      (measured -- so there is no alpha-derived shape to inspect), the geometric
//      child test is blind to alpha, and a child of Progman cannot be sampled at
//      all while an application covers the point (every child of Progman is below
//      every top-level window). Hence the pairing of our own two windows above.
//
// BUILD (portable MinGW, no Qt)
//   $QT/Tools/mingw1310_64/bin/g++ -std=c++17 -O1 spike_layer_alpha_shape.cpp
//       -o spike_layer_alpha_shape.exe -lgdi32 -luser32 -ldwmapi
//
// RUN
//   From an interactive session (a desktop must exist). It creates a 64x64
//   TOPMOST layered window and a 200x200 parent with a 100x100 layered child,
//   pushes alpha 0 and alpha 1 into them, and destroys everything. At alpha 1 the
//   frames are invisible (1/255 black); the parent is colourless but present for
//   a few hundred milliseconds. Nothing is written anywhere.

#include <cstdio>
#include <dwmapi.h>
#include <windows.h>

namespace {

constexpr int kChildAlpha0 = 0;
constexpr int kChildAlpha1 = 1;

LRESULT CALLBACK probeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool registerClass(const wchar_t* name)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &probeProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    return ::RegisterClassExW(&wc) != 0;
}

// Pushes a 32 bpp frame of one premultiplied colour through UpdateLayeredWindow
// -- the same call DesktopSurface uses in per-pixel mode. Black at `alpha` in
// premultiplied ARGB is (0, 0, 0, alpha): the frame is a rectangle of one alpha
// level and nothing else, which is exactly the question being asked.
//
// TRAP, measured the hard way. `pptDst` is not decoration: UpdateLayeredWindow
// *moves* the window there. The first version of this spike passed (0, 0) "for
// a child, which keeps its rect", so the TOPMOST window jumped into the
// top-left corner and was no longer where the cursor was sampled. Two runs
// agreed that "one alpha level is not enough" -- a wrong answer produced by the
// probe, not by Windows. `position` is the screen point the frame belongs at;
// NULL leaves the position alone, which is what a child window wants.
bool pushFrame(HWND hwnd, int width, int height, int alpha, const POINT* position)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HDC screenDc = ::GetDC(nullptr);
    const HDC memDc = ::CreateCompatibleDC(screenDc);
    const HBITMAP dib = ::CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bits || !dib || !memDc) {
        printf("      push: CreateDIBSection/CreateCompatibleDC failed, error %lu\n",
               ::GetLastError());
        return false;
    }
    const DWORD pixel = (DWORD(alpha) << 24); // premultiplied black
    for (long i = 0; i < long(width) * long(height); ++i)
        static_cast<DWORD*>(bits)[i] = pixel;

    const HGDIOBJ old = ::SelectObject(memDc, dib);
    POINT dst = position ? *position : POINT{0, 0};
    POINT src{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::SetLastError(0);
    const BOOL ok = ::UpdateLayeredWindow(hwnd, screenDc, &dst, &size, memDc, &src, 0, &blend,
                                          ULW_ALPHA);
    const DWORD error = ::GetLastError();
    ::SelectObject(memDc, old);
    ::DeleteObject(dib);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);
    if (!ok)
        printf("      push: UpdateLayeredWindow failed, error %lu\n", error);
    else if (position)
        printf("      push: alpha %d at %ld,%ld\n", alpha, position->x, position->y);
    else
        printf("      push: alpha %d (position left alone)\n", alpha);
    return ok != FALSE;
}

// "0x1234 Chrome_WidgetWin_1 2560x1440" -- the way every report in this project
// spells a window.
void describeWindow(HWND window)
{
    wchar_t cls[128] = {0};
    RECT r{};
    if (window) {
        ::GetClassNameW(window, cls, 127);
        ::GetWindowRect(window, &r);
    }
    printf("0x%llx %ls %ldx%ld", (unsigned long long)(uintptr_t)window, cls, r.right - r.left,
           r.bottom - r.top);
}

// Why a window of ours is not the one the cursor finds. Prints where it sits in
// the top-level z-order and what state Windows thinks it is in -- the answer
// decides whether WindowFromPoint is usable at all from this session.
void diagnose(HWND ours)
{
    RECT rect{};
    ::GetWindowRect(ours, &rect);
    const LONG ex = static_cast<LONG>(::GetWindowLongW(ours, GWL_EXSTYLE));
    BOOL cloaked = FALSE;
    constexpr DWORD kDwmwaCloaked = 14;
    ::DwmGetWindowAttribute(ours, kDwmwaCloaked, &cloaked, sizeof(cloaked));
    printf("      ours: visible %d, rect %ld,%ld %ldx%ld, topmost %d, layered %d, cloaked %d\n",
           ::IsWindowVisible(ours) ? 1 : 0, rect.left, rect.top, rect.right - rect.left,
           rect.bottom - rect.top, (ex & WS_EX_TOPMOST) ? 1 : 0, (ex & WS_EX_LAYERED) ? 1 : 0,
           cloaked);

    printf("      foreground: ");
    describeWindow(::GetForegroundWindow());
    printf("\n");

    int rank = 0;
    int oursRank = -1;
    printf("      top-level z-order, topmost first:\n");
    for (HWND w = ::GetTopWindow(nullptr); w && rank < 12; w = ::GetWindow(w, GW_HWNDNEXT)) {
        const LONG wex = static_cast<LONG>(::GetWindowLongW(w, GWL_EXSTYLE));
        BOOL off = FALSE;
        ::DwmGetWindowAttribute(w, kDwmwaCloaked, &off, sizeof(off));
        printf("        [%2d] ", rank);
        describeWindow(w);
        printf("%s%s%s\n", (wex & WS_EX_TOPMOST) ? " topmost" : "",
               (wex & WS_EX_LAYERED) ? " layered" : "", off ? " CLOAKED" : "");
        if (w == ours && oursRank < 0)
            oursRank = rank;
        ++rank;
    }
    printf("      ours sits at rank %d%s\n", oursRank,
           oursRank < 0 ? " (past the first 12: Windows keeps a dozen topmost helper windows "
                          "of its own above everything)"
                        : "");
}

// 1. The rule, on a window no application can be above.
void measureTopmostRule()
{
    printf("\n1. a TOPMOST layered window of ours (no application can be above it)\n");
    const int size = 64;
    const POINT at{300, 300};
    const HWND hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
                                            | WS_EX_NOACTIVATE,
                                        L"StarFishSpikeTopmost", L"", WS_POPUP, at.x, at.y, size,
                                        size, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                        nullptr);
    if (!hwnd) {
        printf("    CreateWindowExW failed, error %lu\n", ::GetLastError());
        return;
    }
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::SetWindowPos(hwnd, HWND_TOPMOST, at.x, at.y, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    diagnose(hwnd);

    const POINT centre{at.x + size / 2, at.y + size / 2};
    const auto sample = [&centre, hwnd, &at](int alpha) {
        if (!pushFrame(hwnd, size, size, alpha, &at))
            return;
        ::Sleep(100);
        const HWND under = ::WindowFromPoint(centre);
        printf("      alpha %-3d sampled at %ld,%ld -> the cursor finds ", alpha, centre.x,
               centre.y);
        describeWindow(under);
        if (alpha == 0)
            printf("  %s\n",
                   under == hwnd
                       ? "== the window (the rule does NOT hold: alpha 0 was not skipped)"
                       : "!= the window (the rule holds: alpha 0 lets the mouse through)");
        else
            printf("  %s\n", under == hwnd ? "== the window (the rule holds: alpha 1 arrives)"
                                           : "!= the window (the rule does NOT hold)");
    };
    sample(0);
    sample(1);
    ::DestroyWindow(hwnd);
}

// 2. The same rule on a layered CHILD window -- the relationship the backdrop
//    has with Progman. The real pairing cannot be measured unless the desktop
//    happens to be clear (every child of Progman is below every top-level
//    window), so this builds the same relationship out of two windows of ours,
//    where nothing can interfere: a parent of ours on top (plain, like Progman,
//    which is not layered), and a per-pixel layered child inside it.
void measureChildRule()
{
    printf("\n2. a layered CHILD window, with its parent behind it\n");
    const int size = 200;
    const POINT at{400, 400};
    const HWND parent = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                          L"StarFishSpikeParent", L"", WS_POPUP, at.x, at.y,
                                          size, size, nullptr, nullptr,
                                          ::GetModuleHandleW(nullptr), nullptr);
    if (!parent) {
        printf("    CreateWindowExW (parent) failed, error %lu\n", ::GetLastError());
        return;
    }
    ::ShowWindow(parent, SW_SHOWNOACTIVATE);
    ::SetWindowPos(parent, HWND_TOPMOST, at.x, at.y, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::UpdateWindow(parent);

    const int childSize = 100;
    const HWND child = ::CreateWindowExW(WS_EX_LAYERED, L"StarFishSpikeChild", L"", WS_POPUP,
                                         at.x + 50, at.y + 50, childSize, childSize, nullptr,
                                         nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!child) {
        printf("    CreateWindowExW (child) failed, error %lu\n", ::GetLastError());
        ::DestroyWindow(parent);
        return;
    }
    if (!::SetParent(child, parent)) {
        printf("    SetParent failed, error %lu\n", ::GetLastError());
        ::DestroyWindow(child);
        ::DestroyWindow(parent);
        return;
    }
    const LONG style = static_cast<LONG>(::GetWindowLongW(child, GWL_STYLE));
    ::SetWindowLongW(child, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);
    ::SetWindowPos(child, nullptr, 50, 50, childSize, childSize, SWP_NOACTIVATE | SWP_NOZORDER);
    ::ShowWindow(child, SW_SHOW);
    ::UpdateWindow(child);
    RECT childRect{};
    ::GetWindowRect(child, &childRect);
    const HWND geometric =
        ::ChildWindowFromPointEx(parent, POINT{50 + childSize / 2, 50 + childSize / 2},
                                 CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
    printf("    parent and child hosted; the child is %ld,%ld %ldx%ld, visible %d, layered %d\n",
           childRect.left, childRect.top, childRect.right - childRect.left,
           childRect.bottom - childRect.top, ::IsWindowVisible(child) ? 1 : 0,
           (::GetWindowLongW(child, GWL_EXSTYLE) & WS_EX_LAYERED) ? 1 : 0);
    printf("    the geometric child test finds %s at the sample point\n",
           geometric == child ? "the child (it is there)" : "something else");

    const auto probe = [child, parent](int alpha) {
        const char* label = alpha == 0 ? "alpha 0" : "alpha 1";
        if (!pushFrame(child, 100, 100, alpha, nullptr))
            return;
        ::Sleep(100);
        // Sampled at the child's *current* centre: UpdateLayeredWindow moves the
        // window, child or not (see pushFrame), so a point fixed before the push
        // may be looking next to it.
        RECT rect{};
        ::GetWindowRect(child, &rect);
        const POINT sample{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
        const HWND under = ::WindowFromPoint(sample);
        printf("      %-10s the child is at %ld,%ld %ldx%ld; sampled at %ld,%ld -> ",
               label, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
               sample.x, sample.y);
        describeWindow(under);
        if (under == child)
            printf("  == the child (the mouse reaches it)\n");
        else if (under == parent)
            printf("  == the parent (the child is skipped: its alpha is zero)\n");
        else
            printf("  != the child and != the parent\n");
    };
    probe(0);
    probe(1);
    ::DestroyWindow(child);
    ::DestroyWindow(parent);
}

} // namespace

int main()
{
    printf("spike: per-pixel layered windows and the mouse (T-060)\n");
    printf("  virtual screen: %d x %d\n", ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
           ::GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (!registerClass(L"StarFishSpikeTopmost") || !registerClass(L"StarFishSpikeChild")
        || !registerClass(L"StarFishSpikeParent")) {
        printf("RegisterClassExW failed, error %lu\n", ::GetLastError());
        return 2;
    }
    measureTopmostRule();
    measureChildRule();
    printf("\ndone\n");
    return 0;
}
