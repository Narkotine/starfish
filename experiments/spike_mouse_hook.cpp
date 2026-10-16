// T-070 spike (ADR-10) -- can F-001 (a right-drag on the bare desktop draws a
// fence) be carried by a *low-level mouse hook* instead of by a full-screen
// window of ours?
//
// The user chose this path: "the desktop stays 100% the shell's, the gesture is
// intercepted". Today that gesture needs DesktopBackdrop -- a window of ours over
// the whole desktop -- and that window is what three sessions kept tripping over
// (it swallows clicks: HTTRANSPARENT stops inside our own thread, and the alpha
// holes of T-069 were the patch for that). With a hook there is no window of ours
// on the desktop at all: the wallpaper, the icons, the shell's selection
// rectangle and its menus all stay the shell's, and the gesture is *observed*.
//
// What this spike measures, and nothing else (pure Win32, no Qt, no product code):
//   1. does WH_MOUSE_LL install in a plain, unelevated process?
//   2. do we see the gesture's events, with usable coordinates?
//   3. what does the callback *cost*? (a slow low-level hook makes the whole
//      machine's mouse sluggish AND is silently dropped by Windows)
//   4. is the hook still alive after many events? (eviction is invisible unless
//      it is counted)
//   5. what does returning 1 (swallow) do -- and to whom?
//   6. does the classification hold: a drag that starts on the shell's list view
//      is ours, one that starts on our own window is not?
//
// Usage: spike_mouse_hook.exe [--seconds N] [--report FILE] [--drag-test] [--swallow]
//        [--inject-child "command line"]
//   --inject-child runs a command (the python driver next to this spike) *after*
//                start-up: a low-level hook is not called for input the installing
//                process injects itself, so a cross-process injector is the only
//                honest way to ask "does the hook see a real right-drag?".
//   --drag-test  inject a right-drag on the spike's *own* window (a private
//                target: nothing of the user's is touched) and report what the
//                hook saw of it.
//   --swallow    swallow right-ups of a drag (the real F-001 behaviour) instead
//                of letting them through. Pair it with a hand-made right-drag on
//                the wallpaper to check that the shell's desktop menu does *not*
//                appear.
//
// ⚠️ Measured while writing the harness, and it costs a session to rediscover:
// a low-level hook is **not** called for input the installing process injects
// itself (SendInput from the same process: 0 events seen), so --drag-test proves
// only that the plumbing runs. And a second process started from the agent's
// terminal could not see the spike's window at all (nor could a background
// launch survive), so the cross-process injector never got to run. **The gesture
// has to be made by hand**: run the spike, right-drag on the wallpaper, then
// right-click once.
//
// Build (portable MinGW, as the other spikes):
//   $QT/Tools/mingw1310_64/bin/g++ -std=c++17 -O1 experiments/spike_mouse_hook.cpp \
//       -o spike_mouse_hook.exe -luser32 -lgdi32

#ifndef _WIN32
#error "This spike is Windows only (like the product)."
#endif

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr UINT kTimerId = 1;
constexpr int kDefaultSeconds = 30;
constexpr int kDragThreshold = 5; // SM_CXDRAG-ish; the product uses its own

struct Stats {
    long long events = 0;
    long long rightDowns = 0;
    long long rightUps = 0;
    long long moves = 0;
    long long drags = 0;          // right-drag beyond the threshold
    long long dragsOnListView = 0; // ... that started on the shell's list view
    long long swallowed = 0;
    long long classified = 0;
    double worstCallbackUs = 0.0;
    double totalCallbackUs = 0.0;
};

HWND g_window = nullptr;   // the spike's own window (a private drag target)
HHOOK g_hook = nullptr;
Stats g_stats;

bool g_dragging = false;
bool g_dragged = false;
bool g_swallowDrags = false;
POINT g_start{};
POINT g_current{};
bool g_startedOnListView = false;
char g_startClass[64] = {0};

LARGE_INTEGER g_frequency{};

double elapsedUs(const LARGE_INTEGER& from)
{
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    return double(now.QuadPart - from.QuadPart) * 1e6 / double(g_frequency.QuadPart);
}

const char* classNameOf(HWND hwnd)
{
    static char buffer[64];
    buffer[0] = '\0';
    if (hwnd)
        ::GetClassNameA(hwnd, buffer, sizeof(buffer));
    return buffer;
}

LRESULT CALLBACK lowLevelMouse(int code, WPARAM message, LPARAM data)
{
    if (code != HC_ACTION)
        return ::CallNextHookEx(g_hook, code, message, data);

    LARGE_INTEGER started{};
    ::QueryPerformanceCounter(&started);

    const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
    ++g_stats.events;
    g_current = info->pt;

    bool swallow = false;
    switch (message) {
    case WM_RBUTTONDOWN: {
        ++g_stats.rightDowns;
        g_dragging = true;
        g_dragged = false;
        g_start = info->pt;
        // Classification: who is under the point *right now*. The product has the
        // icons' own rectangles from the shell's list view; here the window class
        // is enough to prove the hook can tell the cases apart at all.
        HWND under = ::WindowFromPoint(info->pt);
        ::strncpy(g_startClass, classNameOf(under), sizeof(g_startClass) - 1);
        g_startClass[sizeof(g_startClass) - 1] = '\0';
        g_startedOnListView = under && ::strcmp(g_startClass, "SysListView32") == 0;
        ++g_stats.classified;
        std::printf("  right-down at %ld,%ld  under=\"%s\"%s\n", info->pt.x, info->pt.y,
                    g_startClass, g_startedOnListView ? "  (the shell's list view)" : "");
        break;
    }
    case WM_MOUSEMOVE:
        ++g_stats.moves;
        if (g_dragging) {
            const int dx = g_current.x - g_start.x;
            const int dy = g_current.y - g_start.y;
            if (dx * dx + dy * dy > kDragThreshold * kDragThreshold)
                g_dragged = true;
        }
        break;
    case WM_RBUTTONUP: {
        ++g_stats.rightUps;
        const int width = ::abs(g_current.x - g_start.x);
        const int height = ::abs(g_current.y - g_start.y);
        if (g_dragging && g_dragged) {
            ++g_stats.drags;
            if (g_startedOnListView)
                ++g_stats.dragsOnListView;
            const LONG left = g_start.x < g_current.x ? g_start.x : g_current.x;
            const LONG top = g_start.y < g_current.y ? g_start.y : g_current.y;
            const LONG right = g_start.x < g_current.x ? g_current.x : g_start.x;
            const LONG bottom = g_start.y < g_current.y ? g_current.y : g_start.y;
            std::printf("  right-up   -> drag %dx%d from %ld,%ld to %ld,%ld  (start class \"%s\")\n",
                        width, height, g_start.x, g_start.y, g_current.x, g_current.y,
                        g_startClass);
            (void)left;
            (void)top;
            (void)right;
            (void)bottom;
            if (g_startedOnListView && g_swallowDrags) {
                swallow = true;
                ++g_stats.swallowed;
                std::printf("             swallowed: the shell never sees this drag\n");
            }
        }
        g_dragging = false;
        g_dragged = false;
        std::fflush(stdout);
        break;
    }
    default:
        break;
    }

    const double cost = elapsedUs(started);
    g_stats.totalCallbackUs += cost;
    if (cost > g_stats.worstCallbackUs)
        g_stats.worstCallbackUs = cost;

    if (swallow)
        return 1; // and the shell is never told about it
    return ::CallNextHookEx(g_hook, code, message, data);
}

void injectDragTest()
{
    // A right-drag on *our own* window: the hook must see it, and letting it
    // through must do nothing at all to the user's desktop.
    RECT box{};
    ::GetWindowRect(g_window, &box);
    const POINT from{(box.left + box.right) / 2, (box.top + box.bottom) / 2};
    ::SetCursorPos(from.x, from.y);
    ::Sleep(120);

    INPUT inputs[5] = {};
    auto mouse = [](DWORD flags, int dx, int dy) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = flags;
        in.mi.dx = dx;
        in.mi.dy = dy;
        return in;
    };
    const POINT to{from.x + 130, from.y + 90};
    // Relative moves: MOUSEEVENTF_ABSOLUTE wants 0..65535 normalised coordinates,
    // and this spike has no business getting that wrong (it would move the cursor
    // somewhere else entirely).
    inputs[0] = mouse(MOUSEEVENTF_RIGHTDOWN, 0, 0);
    inputs[1] = mouse(MOUSEEVENTF_MOVE, 50, 35);
    inputs[2] = mouse(MOUSEEVENTF_MOVE, 50, 35);
    inputs[3] = mouse(MOUSEEVENTF_MOVE, 30, 20);
    inputs[4] = mouse(MOUSEEVENTF_RIGHTUP, 0, 0);
    std::printf("-- injecting a right-drag on the spike's own window (%ld,%ld) -> (%ld,%ld) --\n",
                from.x, from.y, to.x, to.y);
    for (const INPUT& in : inputs) {
        ::SendInput(1, const_cast<INPUT*>(&in), sizeof(INPUT));
        ::Sleep(60);
    }
    ::Sleep(250);
}

std::wstring widen(const char* text)
{
    if (!text || !*text)
        return std::wstring();
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(size > 0 ? size - 1 : 0, L'\0');
    if (size > 1)
        ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    return out;
}

void writeReport(const char* path, int seconds)
{
    std::FILE* file = std::fopen(path, "w");
    if (!file) {
        std::printf("cannot write %s\n", path);
        return;
    }
    std::fprintf(file, "T-070 spike -- low-level mouse hook for F-001 (right-drag on the bare desktop)\n");
    std::fprintf(file, "hook installed ..................... %s\n",
                 g_hook ? "yes (WH_MOUSE_LL, unelevated)" : "NO");
    std::fprintf(file, "ran ................................ %d s\n", seconds);
    std::fprintf(file, "swallow drags ...................... %s\n", g_swallowDrags ? "yes" : "no");
    std::fprintf(file, "events seen ........................ %lld (down %lld, move %lld, up %lld)\n",
                 g_stats.events, g_stats.rightDowns, g_stats.moves, g_stats.rightUps);
    std::fprintf(file, "right-drags beyond %d px ........... %lld (on the shell's list view: %lld)\n",
                 kDragThreshold, g_stats.drags, g_stats.dragsOnListView);
    std::fprintf(file, "drag-ups swallowed ................. %lld\n", g_stats.swallowed);
    std::fprintf(file, "points classified at right-down .... %lld\n", g_stats.classified);
    std::fprintf(file, "callback cost ...................... worst %.0f us, mean %.0f us "
                        "(Windows drops a low-level hook that is too slow, and the mouse "
                        "goes sluggish machine-wide -- this number is the risk)\n",
                 g_stats.worstCallbackUs,
                 g_stats.events ? g_stats.totalCallbackUs / double(g_stats.events) : 0.0);
    std::fprintf(file, "hook still installed at the end .... %s\n",
                 g_hook ? "yes (it was never evicted)" : "no");
    std::fprintf(file, "own window class under the point ... \"%s\"\n", g_startClass);
    std::fclose(file);
    std::printf("report written to %s\n", path);
}

} // namespace

int main(int argc, char** argv)
{
    int seconds = kDefaultSeconds;
    const char* report = "build/spike_mouse_hook.txt";
    const char* injectChild = nullptr;
    bool dragTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            report = argv[++i];
        else if (std::strcmp(argv[i], "--drag-test") == 0)
            dragTest = true;
        else if (std::strcmp(argv[i], "--swallow") == 0)
            g_swallowDrags = true;
        else if (std::strcmp(argv[i], "--inject-child") == 0 && i + 1 < argc)
            injectChild = argv[++i];
    }

    ::QueryPerformanceFrequency(&g_frequency);

    // Where does this process actually live? A low-level hook only sees input on
    // the desktop of the thread that installed it, so "we see nothing" has to be
    // separated from "we are looking at the wrong desktop".
    {
        auto objectName = [](HANDLE object) {
            wchar_t buffer[256] = {0};
            DWORD needed = 0;
            ::GetUserObjectInformationW(object, UOI_NAME, buffer, sizeof(buffer), &needed);
            return std::wstring(buffer);
        };
        const std::wstring station = objectName(::GetProcessWindowStation());
        const std::wstring desktop = objectName(::GetThreadDesktop(::GetCurrentThreadId()));
        std::printf("station=\"%ls\" desktop=\"%ls\" progman=%p cursor=%ld,%ld\n",
                    station.c_str(), desktop.c_str(),
                    reinterpret_cast<void*>(::FindWindowW(L"Progman", nullptr)),
                    [] { POINT p{}; ::GetCursorPos(&p); return p.x; }(),
                    [] { POINT p{}; ::GetCursorPos(&p); return p.y; }());
        std::fflush(stdout);
    }

    // A plain window: the private target for --drag-test, and the message pump the
    // hook needs (a low-level hook is called on the thread that installed it).
    WNDCLASSW wc{};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"StarFishHookSpike";
    ::RegisterClassW(&wc);
    g_window = ::CreateWindowExW(0, L"StarFishHookSpike", L"StarFish mouse-hook spike",
                                 WS_OVERLAPPEDWINDOW, 200, 200, 420, 260, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    if (!g_window) {
        std::printf("cannot create the spike window (%lu)\n", ::GetLastError());
        return 1;
    }
    ::ShowWindow(g_window, SW_SHOW);
    ::UpdateWindow(g_window);
    {
        RECT box{};
        ::GetWindowRect(g_window, &box);
        char name[64] = {0};
        ::GetClassNameA(g_window, name, sizeof(name));
        std::printf("spike window hwnd=%p class=\"%s\" rect=%ld,%ld %ldx%ld visible=%d\n",
                    reinterpret_cast<void*>(g_window), name, box.left, box.top,
                    box.right - box.left, box.bottom - box.top, ::IsWindowVisible(g_window) ? 1 : 0);
        std::fflush(stdout);
    }

    g_hook = ::SetWindowsHookExW(WH_MOUSE_LL, lowLevelMouse, nullptr, 0);
    std::printf("WH_MOUSE_LL installed: %s\n", g_hook ? "yes" : "NO");
    if (!g_hook) {
        std::printf("SetWindowsHookEx failed: %lu\n", ::GetLastError());
        return 1;
    }
    std::printf("listening for %d s -- right-drag on the wallpaper (or on one of the\n"
                "desktop's icons) to see the gesture classified, then right-click once.\n",
                seconds);

    ::SetTimer(g_window, kTimerId, DWORD(seconds) * 1000, nullptr);
    if (dragTest)
        ::SetTimer(g_window, kTimerId + 1, 700, nullptr); // inject once, shortly after
    if (injectChild)
        ::SetTimer(g_window, kTimerId + 2, 1500, nullptr); // launch the outside injector

    bool dragTestDone = false;
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER && message.wParam == kTimerId)
            break;
        if (message.message == WM_TIMER && message.wParam == kTimerId + 1 && !dragTestDone) {
            dragTestDone = true;
            ::KillTimer(g_window, kTimerId + 1);
            injectDragTest();
        }
        if (message.message == WM_TIMER && message.wParam == kTimerId + 2) {
            ::KillTimer(g_window, kTimerId + 2);
            std::wstring command = widen(injectChild);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                 nullptr, &startup, &child)) {
                std::printf("-- injector launched (a *different* process): %ls --\n", command.c_str());
                std::printf("   (it needs the spike's window to be findable from outside; under \n"
                            "    the agent harness it was not -- see the header) \n");
                ::CloseHandle(child.hThread);
                ::CloseHandle(child.hProcess);
            } else {
                std::printf("-- could not launch the injector (%lu) --\n", ::GetLastError());
            }
            ::fflush(stdout);
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (g_hook) {
        ::UnhookWindowsHookEx(g_hook);
        // A stale handle would make the report lie, so it is cleared and the
        // question "was it still installed?" is answered before that.
        writeReport(report, seconds);
        g_hook = nullptr;
    } else {
        writeReport(report, seconds);
    }
    std::printf("done: %lld event(s), %lld drag(s), %lld swallowed\n", g_stats.events,
                g_stats.drags, g_stats.swallowed);
    return 0;
}
