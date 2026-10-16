// StarFish -- read-only probe of the native desktop icons.
//
// Why this file exists
// --------------------
// The product is about to leave the native desktop icons in place and only
// park the ones a fence holds (ADR-12 and after). That needs two things the
// Shell does not document and that have to be measured on this machine:
//
//   1. the icons' positions, read from outside Explorer -- LVM_GETITEMPOSITION
//      writes through its lParam, which Explorer dereferences in *its* address
//      space, so the POINT has to be VirtualAllocEx'd in Explorer and brought
//      back with ReadProcessMemory;
//   2. the icons' names, read the same way through LVM_GETITEMTEXTW.
//
// If either comes back empty or wrong, the plan changes, so this probe answers
// both with numbers instead of belief.
//
// It MOVES NOTHING. There is not one LVM_SETITEMPOSITION in this file: the user
// is working on this machine and another instance of the product is running, so
// a probe that parked an icon would fight it for the desktop.
//
// Build (same toolchain as the product), one line:
//   g++ -std=c++17 -O1 -o "$TMPDIR/probe_native_icons.exe" experiments/probe_native_icons.cpp -luser32 -lgdi32 -lole32
//
// Run:
//   "$TMPDIR/probe_native_icons.exe"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h> // LVM_* messages and LVITEMW

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// --- reporting ---------------------------------------------------------------

int g_pass = 0;
int g_fail = 0;

void logf(const char* fmt, ...)
{
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::fputs(buffer, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Icon names are Unicode and the console is not: converting to UTF-8 and
// printing through %s is the only encoding that survives MinGW's msvcrt.
std::string utf8(const std::wstring& wide)
{
    if (wide.empty())
        return std::string();
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

void check(const char* what, bool ok, const std::string& detail = std::string())
{
    if (ok)
        ++g_pass;
    else
        ++g_fail;
    logf("[%s] %s%s%s", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : " -- ",
         detail.c_str());
}

// --- desktop resolution ------------------------------------------------------

// Progman -> SHELLDLL_DefView -> SysListView32 on Windows 11; on Windows 10 the
// DefView lives inside a top-level WorkerW instead, so both shapes are tried.
HWND findDesktopListView(HWND* defViewOut)
{
    HWND defView = nullptr;
    if (HWND progman = ::FindWindowW(L"Progman", nullptr))
        defView = ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);

    if (!defView) {
        HWND worker = nullptr;
        while ((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
            if ((defView = ::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)))
                break;
        }
    }

    if (defViewOut)
        *defViewOut = defView;
    return defView ? ::FindWindowExW(defView, nullptr, L"SysListView32", nullptr) : nullptr;
}

// The list view's client origin is only physical pixels for a DPI-aware caller:
// an unaware process has its coordinates virtualised, which would silently shift
// every position. The product's manifest asks for per-monitor-v2 (ADR-4), so the
// probe asks for the same mode before measuring anything.
void makeProcessDpiAware()
{
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4))
#endif
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        // GetProcAddress returns a FARPROC, whose type carries no signature:
        // the cast is unavoidable and its "incompatible types" warning is noise.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
        auto setContext =
            reinterpret_cast<SetContextFn>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
#pragma GCC diagnostic pop
        if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;
    }
    ::SetProcessDPIAware();
}

// --- cross-process item reading ----------------------------------------------

constexpr int kTextChars = 260;

struct RemoteScratch {
    POINT point;
    LVITEMW item;
    wchar_t text[kTextChars];
};

struct Icon {
    int index = 0;
    POINT position{}; // client coordinates, as LVM_GETITEMPOSITION answers
    std::wstring name;
    bool positionValid = false;
    bool nameRead = false;
};

bool readItemPosition(HWND listView, HANDLE process, void* remotePoint, int index, POINT* out)
{
    if (::SendMessageW(listView, LVM_GETITEMPOSITION, static_cast<WPARAM>(index),
                       reinterpret_cast<LPARAM>(remotePoint)) == 0)
        return false;

    POINT point{};
    if (!::ReadProcessMemory(process, remotePoint, &point, sizeof(point), nullptr))
        return false;

    *out = point;
    return true;
}

bool readItemText(HWND listView, HANDLE process, void* remoteItem, void* remoteText, int index,
                  std::wstring* out)
{
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.iSubItem = 0;
    item.pszText = static_cast<LPWSTR>(remoteText);
    item.cchTextMax = kTextChars;
    if (!::WriteProcessMemory(process, remoteItem, &item, sizeof(item), nullptr))
        return false;

    const LRESULT length = ::SendMessageW(listView, LVM_GETITEMTEXTW, static_cast<WPARAM>(index),
                                          reinterpret_cast<LPARAM>(remoteItem));
    if (length <= 0) {
        out->clear();
        return true; // an empty label is an answer, not a failure
    }

    int chars = static_cast<int>(length);
    if (chars > kTextChars - 1)
        chars = kTextChars - 1;

    wchar_t buffer[kTextChars];
    if (!::ReadProcessMemory(process, remoteText, buffer,
                             static_cast<SIZE_T>(chars) * sizeof(wchar_t), nullptr))
        return false;

    out->assign(buffer, buffer + chars);
    return true;
}

} // namespace

int main()
{
    makeProcessDpiAware();

    logf("=== StarFish native desktop icons probe (read-only) ===");

    HWND defView = nullptr;
    const HWND listView = findDesktopListView(&defView);
    if (!listView) {
        check("desktop list view (SysListView32) found", false,
              defView ? "SHELLDLL_DefView found but no SysListView32 under it"
                      : "neither Progman->SHELLDLL_DefView nor WorkerW->SHELLDLL_DefView");
        logf("summary: %d PASS, %d FAIL", g_pass, g_fail);
        return 1;
    }

    logf("SHELLDLL_DefView        : 0x%p", reinterpret_cast<void*>(defView));
    logf("SysListView32           : 0x%p", reinterpret_cast<void*>(listView));
    check("desktop list view (SysListView32) found", true);

    RECT windowRect{};
    RECT clientRect{};
    ::GetWindowRect(listView, &windowRect);
    ::GetClientRect(listView, &clientRect);
    POINT origin{0, 0};
    ::ClientToScreen(listView, &origin);

    // GetWindowRect and ClientToScreen only agree on the screen rect when the
    // caller is DPI aware; printing both makes a mismatch visible rather than
    // silently shifting every position.
    logf("list view window rect   : (%ld,%ld)-(%ld,%ld)", windowRect.left, windowRect.top,
         windowRect.right, windowRect.bottom);
    logf("list view client rect   : (%ld,%ld)-(%ld,%ld)", clientRect.left, clientRect.top,
         clientRect.right, clientRect.bottom);
    logf("client origin on screen : (%ld,%ld)", origin.x, origin.y);

    const int count = static_cast<int>(::SendMessageW(listView, LVM_GETITEMCOUNT, 0, 0));
    const DWORD spacing =
        static_cast<DWORD>(::SendMessageW(listView, LVM_GETITEMSPACING, FALSE, 0));
    const int stepX = static_cast<int>(LOWORD(spacing));
    const int stepY = static_cast<int>(HIWORD(spacing));

    logf("item count              : %d", count);
    logf("cell step (LVM_GETITEMSPACING) : %dx%d", stepX, stepY);

    check("item count > 0", count > 0,
          "count=" + std::to_string(count) + " (an empty desktop has nothing to read)");
    check("cell step non-zero", stepX > 0 && stepY > 0,
          std::to_string(stepX) + "x" + std::to_string(stepY));

    if (count <= 0) {
        logf("summary: %d PASS, %d FAIL", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(listView, &pid);
    HANDLE process = ::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
                                       | PROCESS_QUERY_INFORMATION,
                                   FALSE, pid);
    check("OpenProcess on Explorer", process != nullptr,
          process ? "pid=" + std::to_string(pid) : "GetLastError=" + std::to_string(::GetLastError()));
    if (!process) {
        logf("summary: %d PASS, %d FAIL", g_pass, g_fail);
        return 1;
    }

    void* const remote = ::VirtualAllocEx(process, nullptr, sizeof(RemoteScratch),
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check("VirtualAllocEx in Explorer", remote != nullptr,
          remote ? std::string() : "GetLastError=" + std::to_string(::GetLastError()));
    if (!remote) {
        ::CloseHandle(process);
        logf("summary: %d PASS, %d FAIL", g_pass, g_fail);
        return 1;
    }

    void* const remotePoint = remote;
    void* const remoteItem = static_cast<char*>(remote) + offsetof(RemoteScratch, item);
    void* const remoteText = static_cast<char*>(remote) + offsetof(RemoteScratch, text);

    std::vector<Icon> icons(static_cast<size_t>(count));
    int positionsRead = 0;
    int namesRead = 0;
    int emptyNames = 0;

    for (int index = 0; index < count; ++index) {
        Icon& icon = icons[static_cast<size_t>(index)];
        icon.index = index;
        if (readItemPosition(listView, process, remotePoint, index, &icon.position)) {
            icon.positionValid = true;
            ++positionsRead;
        }
        if (readItemText(listView, process, remoteItem, remoteText, index, &icon.name)) {
            icon.nameRead = true;
            ++namesRead;
            if (icon.name.empty())
                ++emptyNames;
        }
    }

    check("every item position read", positionsRead == count,
          std::to_string(positionsRead) + "/" + std::to_string(count));
    check("every item name read", namesRead == count,
          std::to_string(namesRead) + "/" + std::to_string(count) + ", " +
              std::to_string(emptyNames) + " empty");

    logf("");
    logf("first 5 items (position in client coords, as LVM_GETITEMPOSITION answers):");
    for (int index = 0; index < count && index < 5; ++index) {
        const Icon& icon = icons[static_cast<size_t>(index)];
        const std::string name = utf8(icon.name);
        logf("  [%d] pos=(%ld,%ld) slot=%dx%d name=\"%s\"", icon.index,
             icon.positionValid ? icon.position.x : -1L,
             icon.positionValid ? icon.position.y : -1L, stepX, stepY, name.c_str());
    }
    logf("... total items: %d", count);

    // The first item has to sit inside the list view's own screen rect: if it
    // does not, the position read does not describe this view.
    const bool firstValid = !icons.empty() && icons[0].positionValid;
    if (firstValid) {
        const long screenX = icons[0].position.x + origin.x;
        const long screenY = icons[0].position.y + origin.y;
        const bool inside = screenX >= windowRect.left && screenX < windowRect.right
                            && screenY >= windowRect.top && screenY < windowRect.bottom;
        check("item 0 position inside the list view screen rect", inside,
              "(" + std::to_string(screenX) + "," + std::to_string(screenY) + ") vs (" +
                  std::to_string(windowRect.left) + "," + std::to_string(windowRect.top) + ")-(" +
                  std::to_string(windowRect.right) + "," + std::to_string(windowRect.bottom) + ")");
    } else {
        check("item 0 position inside the list view screen rect", false,
              "item 0 position was not readable");
    }

    // Two icons cannot occupy the same origin: a repeat means the reads landed
    // on the same item, i.e. the index was not what it looked like.
    bool duplicate = false;
    for (size_t i = 0; i < icons.size() && !duplicate; ++i) {
        if (!icons[i].positionValid)
            continue;
        for (size_t j = i + 1; j < icons.size(); ++j) {
            if (!icons[j].positionValid)
                continue;
            if (icons[i].position.x == icons[j].position.x
                && icons[i].position.y == icons[j].position.y) {
                logf("  duplicate position at items %d and %d: (%ld,%ld)", icons[i].index,
                     icons[j].index, icons[i].position.x, icons[i].position.y);
                duplicate = true;
                break;
            }
        }
    }
    check("no two items share a position", !duplicate);

    ::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    ::CloseHandle(process);

    logf("");
    logf("summary: %d PASS, %d FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
