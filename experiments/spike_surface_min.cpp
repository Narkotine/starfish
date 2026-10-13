// StarFish -- minimal Win32 reproducer for the T-020 divergence.
//
// Why this file exists
// --------------------
// The Python spike (experiments/spike_workerw.py) hosts a child of Progman and
// is composited by DWM: 148340 of 153600 pixels of the test rect change colour.
//
// src/platform/win/desktop_surface.cpp replays what looks like the same
// sequence and is not composited -- and, worse, its SetWindowLongW(GWL_EXSTYLE)
// write is silently refused there: the call returns 0, GetLastError() stays 0,
// and the style read back is unchanged. SetLayeredWindowAttributes then fails
// with ERROR_INVALID_PARAMETER (87), which is exactly what you get on a window
// that does not have WS_EX_LAYERED.
//
// This program removes every variable that is not the Win32 API itself: no Qt,
// no widget, no watchdog, no offscreen rendering. It replays the reference
// sequence step by step, prints every return value and every read-back, then
// captures the screen and counts the pixels of the test colour -- so one run
// answers both questions (does the flag stick? is the window composited?).
//
// Build (same toolchain as the product):
//   g++ -std=c++17 -O1 -o experiments/out/spike_surface_min.exe
//       experiments/spike_surface_min.cpp -lgdi32 -luser32
//
// Run:
//   spike_surface_min.exe                    # no DPI call (python.exe-like)
//   spike_surface_min.exe --dpi 3            # per-monitor-v2, like Qt
//   spike_surface_min.exe --dpi 0            # unaware
//   spike_surface_min.exe --no-layered       # control: expected to be invisible
//   spike_surface_min.exe --layered-before-parent
//
// The console window is hidden at startup: it is a real top-level window and
// would cover the test rect, which has already produced two false negatives on
// this project (a maximised browser in session 7, the IDE in session 8).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- logging -----------------------------------------------------------------

void logf(const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::fputs(buffer, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// --- small helpers -----------------------------------------------------------

struct Rect {
    long left = 0, top = 0, right = 0, bottom = 0;
    long width() const { return right - left; }
    long height() const { return bottom - top; }
};

Rect windowRect(HWND hwnd)
{
    RECT r{};
    ::GetWindowRect(hwnd, &r);
    return Rect{r.left, r.top, r.right, r.bottom};
}

std::string className(HWND hwnd)
{
    wchar_t buffer[256] = {};
    ::GetClassNameW(hwnd, buffer, 256);
    char utf8[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return std::string(utf8);
}

HWND findDefView(HWND* iconHostOut)
{
    if (HWND progman = ::FindWindowW(L"Progman", nullptr)) {
        if (HWND defView = ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *iconHostOut = progman;
            return defView;
        }
    }
    HWND worker = nullptr;
    while ((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        if (HWND defView = ::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *iconHostOut = worker;
            return defView;
        }
    }
    return nullptr;
}

// Top-level windows that overlap the test rect. A capture taken while one of
// them is on top measures nothing; the check is mandatory before believing a
// pixel count (see the project's "do not" list).
std::vector<std::string> coveringWindows(const Rect& probe)
{
    struct Ctx {
        Rect probe;
        std::vector<std::string>* out;
    } ctx{probe, new std::vector<std::string>()};

    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
                return TRUE;
            const std::string cls = className(hwnd);
            if (cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd"
                || cls == "Shell_SecondaryTrayWnd" || cls == "SysShadow"
                || cls.compare(0, 2, "Qt") == 0)
                return TRUE;
            const Rect r = windowRect(hwnd);
            if (r.left < c->probe.right && r.right > c->probe.left && r.top < c->probe.bottom
                && r.bottom > c->probe.top) {
                char line[256];
                std::snprintf(line, sizeof(line), "%s 0x%p %ld,%ld %ldx%ld", cls.c_str(),
                              reinterpret_cast<void*>(hwnd), r.left, r.top, r.width(), r.height());
                c->out->push_back(line);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    std::vector<std::string> result = *ctx.out;
    delete ctx.out;
    return result;
}

// --- screen capture + pixel count --------------------------------------------

// Counts pixels of the test colour inside `probe` on the primary screen.
// Returns -1 when the capture itself failed, so "capture broken" and "nothing
// drawn" can never be confused.
long countTestColour(const Rect& probe, COLORREF colour, long* totalOut)
{
    const int w = probe.width();
    const int h = probe.height();
    *totalOut = static_cast<long>(w) * h;

    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ previous = ::SelectObject(memory, bitmap);
    const BOOL blitted = ::BitBlt(memory, 0, 0, w, h, screen, probe.left, probe.top, SRCCOPY);

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = w;
    header.biHeight = -h; // top-down
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;

    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    const int lines = ::GetDIBits(memory, bitmap, 0, h, pixels.data(),
                                  reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);

    ::SelectObject(memory, previous);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);

    if (!blitted || lines != h)
        return -1;

    const unsigned char wantB = GetBValue(colour);
    const unsigned char wantG = GetGValue(colour);
    const unsigned char wantR = GetRValue(colour);

    long hits = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] == wantB && pixels[i + 1] == wantG && pixels[i + 2] == wantR)
            ++hits;
    }
    return hits;
}

// --- the test window ---------------------------------------------------------

constexpr COLORREF kTestColour = RGB(0xE6, 0x7E, 0x22); // the spike's orange
constexpr wchar_t kWindowClass[] = L"StarFishMinSurfaceWnd";

void paintWindow(HWND hwnd, HDC dc, const RECT& rc)
{
    const HBRUSH brush = ::CreateSolidBrush(kTestColour);
    ::FillRect(dc, &rc, brush);
    ::DeleteObject(brush);
}

LRESULT CALLBACK surfaceProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        paintWindow(hwnd, reinterpret_cast<HDC>(wParam), rc);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        const HDC dc = ::BeginPaint(hwnd, &ps);
        paintWindow(hwnd, dc, ps.rcPaint);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// --- DPI awareness -----------------------------------------------------------

typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);
typedef HANDLE(WINAPI* GetThreadDpiAwarenessContextFn)();
typedef int(WINAPI* GetAwarenessFromDpiAwarenessContextFn)(HANDLE);

void reportDpiAwareness(const char* when)
{
    auto getContext = reinterpret_cast<GetThreadDpiAwarenessContextFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                                 "GetThreadDpiAwarenessContext")));
    auto getAwareness = reinterpret_cast<GetAwarenessFromDpiAwarenessContextFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                                 "GetAwarenessFromDpiAwarenessContext")));
    if (!getContext || !getAwareness) {
        logf("[min] dpi (%s)               : API unavailable", when);
        return;
    }
    const int awareness = getAwareness(getContext());
    const char* name = awareness == 0   ? "UNAWARE"
                       : awareness == 1 ? "SYSTEM_AWARE"
                       : awareness == 2 ? "PER_MONITOR_AWARE"
                                        : "UNKNOWN";
    logf("[min] dpi (%s)               : %d (%s)", when, awareness, name);
}

} // namespace

int main(int argc, char** argv)
{
    // A console window is a top-level window and would cover the test rect.
    if (HWND console = ::GetConsoleWindow()) {
        bool keep = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--show-console") == 0)
                keep = true;
        }
        if (!keep)
            ::ShowWindow(console, SW_HIDE);
    }

    int dpiAwareness = -1; // -1: do not touch
    bool layered = true;
    bool layeredBeforeParent = false;
    bool battery = false;
    bool prePos = false;
    bool noParent = false;
    bool defaultProc = false;
    bool warmup = false;
    bool childDirect = false;
    bool layeredAtCreate = false;
    int settleMs = 0;
    int seconds = 4;
    long x = 120, y = 120, w = 480, h = 320;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dpi") == 0 && i + 1 < argc)
            dpiAwareness = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--no-layered") == 0)
            layered = false;
        else if (std::strcmp(argv[i], "--layered-before-parent") == 0)
            layeredBeforeParent = true;
        else if (std::strcmp(argv[i], "--battery") == 0)
            battery = true;
        else if (std::strcmp(argv[i], "--pre-pos") == 0)
            prePos = true;
        else if (std::strcmp(argv[i], "--no-parent") == 0)
            noParent = true;
        else if (std::strcmp(argv[i], "--default-proc") == 0)
            defaultProc = true;
        else if (std::strcmp(argv[i], "--warmup") == 0)
            warmup = true;
        else if (std::strcmp(argv[i], "--child-direct") == 0)
            childDirect = true;
        else if (std::strcmp(argv[i], "--layered-at-create") == 0)
            layeredAtCreate = true;
        else if (std::strcmp(argv[i], "--settle-ms") == 0 && i + 1 < argc)
            settleMs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rect") == 0 && i + 4 < argc) {
            x = std::atol(argv[++i]);
            y = std::atol(argv[++i]);
            w = std::atol(argv[++i]);
            h = std::atol(argv[++i]);
        }
    }

    logf("=== spike_surface_min: pure Win32, no Qt ===");
    logf("[min] compiler              : %s", __VERSION__);
    reportDpiAwareness("before");

    if (dpiAwareness >= 0) {
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                                     "SetProcessDpiAwarenessContext")));
        if (!setContext) {
            logf("[min] FAIL: SetProcessDpiAwarenessContext unavailable");
            return 1;
        }
        const HANDLE contexts[] = {reinterpret_cast<HANDLE>(-1), reinterpret_cast<HANDLE>(-2),
                                   reinterpret_cast<HANDLE>(-3), reinterpret_cast<HANDLE>(-4)};
        const int index = dpiAwareness >= 0 && dpiAwareness <= 3 ? dpiAwareness : 3;
        ::SetLastError(0);
        const BOOL ok = setContext(contexts[index]);
        logf("[min] SetProcessDpiAwarenessContext(%d): ok=%d err=%lu", index, ok ? 1 : 0,
             ::GetLastError());
        if (!ok) {
            logf("[min] NOTE: the process DPI awareness was already fixed (manifest?) -- "
                 "the request above had NO effect");
        }
        reportDpiAwareness("after");
    }

    HWND iconHost = nullptr;
    const HWND defView = findDefView(&iconHost);
    if (!iconHost || !defView) {
        logf("[min] FAIL: Progman/DefView not found (iconHost=%p defView=%p)",
             reinterpret_cast<void*>(iconHost), reinterpret_cast<void*>(defView));
        return 1;
    }
    const Rect defViewRect = windowRect(defView);
    logf("[min] icon host             : 0x%p (%s)", reinterpret_cast<void*>(iconHost),
         className(iconHost).c_str());
    logf("[min] SHELLDLL_DefView      : 0x%p %ld,%ld %ldx%ld", reinterpret_cast<void*>(defView),
         defViewRect.left, defViewRect.top, defViewRect.width(), defViewRect.height());

    const Rect probe{x, y, x + w, y + h};
    logf("[min] test rect             : %ld,%ld %ldx%ld", x, y, w, h);
    for (const std::string& cover : coveringWindows(probe))
        logf("[min] WARNING coverer       : %s", cover.c_str());

    const HBRUSH background = ::CreateSolidBrush(kTestColour);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = defaultProc ? ::DefWindowProcW : &surfaceProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = background;
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logf("[min] FAIL: RegisterClassExW err=%lu", ::GetLastError());
        return 1;
    }

    // Optional warm-up: does having already created a layered window in this
    // process change whether the next one is accepted? (The Python battery ran
    // its Progman case *after* two other layered windows had been created.)
    if (warmup) {
        HWND scratch = ::CreateWindowExW(0, kWindowClass, nullptr, WS_POPUP, 1800, 80, 160, 100,
                                         nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (scratch) {
            ::ShowWindow(scratch, SW_SHOW);
            ::UpdateWindow(scratch);
            ::SetWindowLongW(scratch, GWL_EXSTYLE,
                             static_cast<LONG>(::GetWindowLongW(scratch, GWL_EXSTYLE))
                                 | WS_EX_LAYERED);
            const LONG read = static_cast<LONG>(::GetWindowLongW(scratch, GWL_EXSTYLE));
            logf("[min] warm-up layered window : 0x%p, ex 0x%lx (layered=%s)",
                 reinterpret_cast<void*>(scratch), static_cast<unsigned long>(read),
                 (read & WS_EX_LAYERED) ? "yes" : "NO");
            ::DestroyWindow(scratch);
        }
    }

    const DWORD createExStyle = layeredAtCreate ? WS_EX_LAYERED : 0;
    HWND hwnd = nullptr;
    if (childDirect) {
        hwnd = ::CreateWindowExW(createExStyle, kWindowClass, nullptr, WS_CHILD | WS_VISIBLE,
                                 static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
                                 static_cast<int>(h), iconHost, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
    } else {
        hwnd = ::CreateWindowExW(createExStyle, kWindowClass, nullptr, WS_POPUP,
                                 static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
                                 static_cast<int>(h), nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                 nullptr);
    }
    if (!hwnd) {
        logf("[min] FAIL: CreateWindowExW err=%lu", ::GetLastError());
        return 1;
    }
    logf("[min] CreateWindowExW       : hwnd 0x%p %ld,%ld %ldx%ld (exStyle 0x%lx, %s)",
         reinterpret_cast<void*>(hwnd), windowRect(hwnd).left, windowRect(hwnd).top,
         windowRect(hwnd).width(), windowRect(hwnd).height(),
         static_cast<unsigned long>(createExStyle),
         childDirect ? "WS_CHILD of the icon host" : "WS_POPUP, top-level");
    logf("[min] ex-style at creation  : 0x%lx",
         static_cast<unsigned long>(::GetWindowLongW(hwnd, GWL_EXSTYLE)));

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    // ---- hosting sequence ---------------------------------------------------
    //
    // Every alternative below is measured on the *same* window state, so a
    // difference between two lines is a difference between two API calls and
    // nothing else.
    auto setExLayered = [](HWND target, bool usePtr, const char* what) -> LONG {
        const LONG want = (static_cast<LONG>(::GetWindowLongW(target, GWL_EXSTYLE))
                           & ~WS_EX_TOPMOST)
                          | WS_EX_LAYERED;
        ::SetLastError(0);
        LONG previous = 0;
        if (usePtr)
            previous = static_cast<LONG>(::SetWindowLongPtrW(target, GWL_EXSTYLE, want));
        else
            previous = ::SetWindowLongW(target, GWL_EXSTYLE, want);
        const DWORD error = ::GetLastError();
        const LONG read = static_cast<LONG>(::GetWindowLongW(target, GWL_EXSTYLE));
        logf("[min]   %-30s wrote 0x%lx prev 0x%lx read 0x%lx err %lu layered=%s", what,
             static_cast<unsigned long>(want), static_cast<unsigned long>(previous),
             static_cast<unsigned long>(read), error, (read & WS_EX_LAYERED) ? "yes" : "NO");
        return read;
    };

    if (layeredBeforeParent && layered) {
        const LONG before = setExLayered(hwnd, false, "layered BEFORE SetParent");
        logf("[min]   (pre-parent result kept: layered=%s)",
             (before & WS_EX_LAYERED) ? "yes" : "NO");
    }

    if (prePos) {
        ::SetWindowPos(hwnd, nullptr, static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
                       static_cast<int>(h), SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        logf("[min] pre-SetParent SetWindowPos: done (diagnostic extra step)");
    }

    // 2. Reparent to the icon host.
    if (noParent) {
        logf("[min] --no-parent           : skipping SetParent (plain top-level control)");
    } else if (childDirect) {
        logf("[min] --child-direct        : created as WS_CHILD of the icon host; no SetParent, "
             "no style surgery");
    } else {
        ::SetLastError(0);
        const HWND previousParent = ::SetParent(hwnd, iconHost);
        const DWORD setParentError = ::GetLastError();
        logf("[min] SetParent             : err=%lu previous 0x%p ('%s'), now 0x%p ('%s')",
             setParentError, reinterpret_cast<void*>(previousParent),
             className(previousParent).c_str(), reinterpret_cast<void*>(::GetParent(hwnd)),
             className(::GetParent(hwnd)).c_str());
        logf("[min] owner thread          : %lu (current %lu)",
             ::GetWindowThreadProcessId(hwnd, nullptr), ::GetCurrentThreadId());

        // 3. WS_CHILD without WS_POPUP.
        const LONG style = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_STYLE));
        ::SetLastError(0);
        ::SetWindowLongW(hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);
        const DWORD styleError = ::GetLastError();
        const LONG styleRead = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_STYLE));
        logf("[min] style                 : 0x%lx -> wrote 0x%lx, read 0x%lx (err %lu)",
             static_cast<unsigned long>(style),
             static_cast<unsigned long>((style | WS_CHILD) & ~WS_POPUP),
             static_cast<unsigned long>(styleRead), styleError);
    }

    // 4. WS_EX_LAYERED, after SetParent.
    LONG exRead = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_EXSTYLE));
    if (layered) {
        if (exRead & WS_EX_LAYERED) {
            logf("[min] ex-style              : already 0x%lx (carried over from creation)",
                 static_cast<unsigned long>(exRead));
        } else {
            if (settleMs > 0) {
                ::Sleep(static_cast<DWORD>(settleMs));
                logf("[min] settle                : waited %d ms after SetParent", settleMs);
            }
            exRead = setExLayered(hwnd, false, "SetWindowLongW after parent");
        }
        if (!(exRead & WS_EX_LAYERED) && battery) {
            logf("[min] battery: the write did not stick -- which writes DO work on this window?");

            // Is GWL_EXSTYLE frozen as a whole, or is it only the LAYERED bit
            // that is refused? WS_EX_TOPMOST is the control: it is the other bit
            // the reference sequence touches.
            auto tryBits = [&](LONG bits, const char* what) {
                const LONG base = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_EXSTYLE));
                ::SetLastError(0);
                const LONG previous = ::SetWindowLongW(hwnd, GWL_EXSTYLE, base | bits);
                const DWORD error = ::GetLastError();
                const LONG read = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_EXSTYLE));
                logf("[min]   %-30s wrote 0x%lx prev 0x%lx read 0x%lx err %lu -> %s", what,
                     static_cast<unsigned long>(base | bits), static_cast<unsigned long>(previous),
                     static_cast<unsigned long>(read), error,
                     (read & bits) ? "STUCK" : "REFUSED");
                if (read & bits)
                    ::SetWindowLongW(hwnd, GWL_EXSTYLE, base); // undo, keep the state clean
            };
            tryBits(WS_EX_TOPMOST, "WS_EX_TOPMOST 0x8");
            tryBits(WS_EX_TRANSPARENT, "WS_EX_TRANSPARENT 0x20");
            tryBits(WS_EX_TOOLWINDOW, "WS_EX_TOOLWINDOW 0x80");
            tryBits(WS_EX_NOACTIVATE, "WS_EX_NOACTIVATE 0x8000000");
            tryBits(WS_EX_LAYERED, "WS_EX_LAYERED 0x80000");

            setExLayered(hwnd, true, "SetWindowLongPtrW");
            ::SetWindowLongW(hwnd, GWL_EXSTYLE, 0);
            setExLayered(hwnd, false, "zero, then set");
            ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                               | SWP_FRAMECHANGED);
            setExLayered(hwnd, false, "after SWP_FRAMECHANGED");
            ::ShowWindow(hwnd, SW_HIDE);
            setExLayered(hwnd, false, "while hidden");
            ::ShowWindow(hwnd, SW_SHOW);
            setExLayered(hwnd, false, "after re-show");
            ::SendMessageW(hwnd, WM_NULL, 0, 0);
            setExLayered(hwnd, false, "after SendMessage(WM_NULL)");
            ::RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            setExLayered(hwnd, false, "after RedrawWindow");
        }

        ::SetLastError(0);
        const BOOL ok = ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        const DWORD error = ::GetLastError();
        COLORREF key = 0;
        BYTE alpha = 0;
        DWORD flags = 0;
        const BOOL hasLwa = ::GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags);
        logf("[min] SetLayeredWindowAttributes: ok=%d err=%lu; read back hasLwa=%d flags=0x%lx "
             "alpha=%u",
             ok ? 1 : 0, error, hasLwa ? 1 : 0, static_cast<unsigned long>(flags), alpha);
    }

    // 5. Just below the icon layer.
    const HWND insertAfter = noParent ? HWND_TOP : defView;
    const BOOL placed = ::SetWindowPos(hwnd, insertAfter, static_cast<int>(x), static_cast<int>(y),
                                       static_cast<int>(w), static_cast<int>(h),
                                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::UpdateWindow(hwnd);
    logf("[min] SetWindowPos          : ok=%d err=%lu rect %ld,%ld %ldx%ld visible=%d", placed ? 1 : 0,
         ::GetLastError(), windowRect(hwnd).left, windowRect(hwnd).top, windowRect(hwnd).width(),
         windowRect(hwnd).height(), ::IsWindowVisible(hwnd) ? 1 : 0);

    // z-order chain of Progman's children, top first.
    struct ChildCtx {
        HWND ours;
        int index = -1;
        int defViewIndex = -1;
        int count = 0;
    } childCtx{hwnd};
    ::EnumChildWindows(
        iconHost,
        [](HWND child, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<ChildCtx*>(param);
            if (child == c->ours)
                c->index = c->count;
            if (className(child) == "SHELLDLL_DefView")
                c->defViewIndex = c->count;
            ++c->count;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&childCtx));
    logf("[min] z-order               : %d children, ours=[%d] defView=[%d] -> %s", childCtx.count,
         childCtx.index, childCtx.defViewIndex,
         (childCtx.index > childCtx.defViewIndex && childCtx.index >= 0) ? "below icons (good)"
                                                                        : "NOT below icons");

    // Give the desktop a moment to settle, pumping messages so WM_PAINT is
    // actually delivered (a blocked queue would make the count meaningless).
    const DWORD deadline = ::GetTickCount() + 1500;
    MSG msg{};
    while (::GetTickCount() < deadline) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(20);
    }
    ::RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);

    const std::vector<std::string> coverers = coveringWindows(probe);
    if (coverers.empty()) {
        logf("[min] test rect is clear: no top-level window covers it");
    } else {
        logf("[min] WARNING: %zu top-level window(s) cover the test rect -- the capture below "
             "proves NOTHING:",
             coverers.size());
        for (const std::string& cover : coverers)
            logf("[min]   %s", cover.c_str());
    }

    long total = 0;
    const long hits = countTestColour(probe, kTestColour, &total);
    if (hits < 0)
        logf("[min] screen capture        : FAILED (GetDIBits/BitBlt)");
    else
        logf("[min] screen capture        : %ld / %ld pixels are the test colour (%.1f%%)", hits,
             total, total > 0 ? 100.0 * hits / total : 0.0);

    logf("[min] VERDICT               : %s",
         (hits > 0) ? "COMPOSITED -- the desktop shows our window"
                    : "NOT COMPOSITED -- nothing of ours reaches the screen");

    if (seconds > 0) {
        const DWORD until = ::GetTickCount() + static_cast<DWORD>(seconds) * 1000;
        while (::GetTickCount() < until) {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::Sleep(20);
        }
    }

    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd);
    ::DeleteObject(background);
    logf("[min] done");
    return hits > 0 ? 0 : 2;
}
