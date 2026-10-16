#include "platform/win/desktop_icons.hpp"

#include <cstddef>

#include <QDebug>
#include <QLoggingCategory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h> // LVM_* messages and LVITEMW

namespace starfish::platform::win {

namespace {

QLoggingCategory lc("starfish.icons");

// Comfortably above MAX_PATH, which every desktop item name fits in.
constexpr int kTextChars = 260;

// LVM_SETITEMPOSITION is given the coordinates packed in lParam, i.e. two
// 16-bit fields, so the parking spot has to fit in an int16: this is the
// largest negative the packing can carry, and every monitor is far from it.
constexpr int kParkedOffset = -32000;

// The SysListView32 that owns the desktop's icons (ADR-12): Progman ->
// SHELLDLL_DefView -> SysListView32 on Windows 11, the same chain under a
// top-level WorkerW on Windows 10. It mirrors DesktopSurface's own resolution
// (private to that class) and so depends on user32 alone.
HWND findDesktopListView()
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
    return defView ? ::FindWindowExW(defView, nullptr, L"SysListView32", nullptr) : nullptr;
}

// The list view's client origin, in physical screen pixels. LVM_GETITEMPOSITION
// answers in client coordinates and the product clicks in screen coordinates
// (WM_NCHITTEST), so this is the translation between the two.
QPoint clientOriginOnScreen(HWND window)
{
    POINT origin{0, 0};
    ::ClientToScreen(window, &origin);
    return QPoint(origin.x, origin.y);
}

// RAII for the Explorer process handle the cross-process reads need.
class RemoteProcess
{
public:
    explicit RemoteProcess(HWND window)
    {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(window, &pid);
        if (pid) {
            m_handle = ::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
                                         | PROCESS_QUERY_INFORMATION,
                                     FALSE, pid);
        }
    }

    ~RemoteProcess()
    {
        if (m_handle)
            ::CloseHandle(m_handle);
    }

    RemoteProcess(const RemoteProcess&) = delete;
    RemoteProcess& operator=(const RemoteProcess&) = delete;

    HANDLE handle() const { return m_handle; }
    bool isValid() const { return m_handle != nullptr; }

private:
    HANDLE m_handle = nullptr;
};

// RAII for a block of memory that is valid *inside* the list view's process.
// LVM_GETITEMPOSITION and LVM_GETITEMTEXTW write through their lParam, which
// Explorer dereferences in its own address space: the buffer has to be
// allocated there and read back with ReadProcessMemory, and freed on every
// path, including the ones that fail halfway.
class RemoteMemory
{
public:
    RemoteMemory(HANDLE process, SIZE_T size)
        : m_process(process)
        , m_address(::VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE))
    {
    }

    ~RemoteMemory()
    {
        if (m_address)
            ::VirtualFreeEx(m_process, m_address, 0, MEM_RELEASE);
    }

    RemoteMemory(const RemoteMemory&) = delete;
    RemoteMemory& operator=(const RemoteMemory&) = delete;

    bool isValid() const { return m_address != nullptr; }
    void* address() const { return m_address; }
    void* at(SIZE_T offset) const { return static_cast<char*>(m_address) + offset; }

private:
    HANDLE m_process = nullptr;
    void* m_address = nullptr;
};

// One block holds everything a single item is read through: the POINT
// LVM_GETITEMPOSITION fills, the LVITEMW LVM_GETITEMTEXTW is handed, and the
// text it copies into.
struct RemoteScratch {
    POINT point;
    LVITEMW item;
    wchar_t text[kTextChars];
};

// Reads an item's top-left corner, in the list view's client coordinates.
bool readItemPosition(HWND listView, HANDLE process, void* remotePoint, int index, QPoint* out)
{
    if (::SendMessageW(listView, LVM_GETITEMPOSITION, static_cast<WPARAM>(index),
                       reinterpret_cast<LPARAM>(remotePoint)) == 0)
        return false;

    POINT point{};
    if (!::ReadProcessMemory(process, remotePoint, &point, sizeof(point), nullptr))
        return false;

    *out = QPoint(point.x, point.y);
    return true;
}

// Reads an item's text into `out`. Returns false only when the exchange with
// Explorer failed; an item with an empty label is a valid answer and reports
// true with an empty string.
bool readItemText(HWND listView, HANDLE process, void* remoteItem, void* remoteText, int index,
                  QString* out)
{
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.iSubItem = 0; // the desktop's list view has a single column
    item.pszText = static_cast<LPWSTR>(remoteText);
    item.cchTextMax = kTextChars;
    if (!::WriteProcessMemory(process, remoteItem, &item, sizeof(item), nullptr))
        return false;

    const LRESULT length = ::SendMessageW(listView, LVM_GETITEMTEXTW, static_cast<WPARAM>(index),
                                          reinterpret_cast<LPARAM>(remoteItem));
    if (length <= 0) {
        out->clear();
        return true;
    }

    int chars = static_cast<int>(length);
    if (chars > kTextChars - 1)
        chars = kTextChars - 1;

    wchar_t buffer[kTextChars];
    if (!::ReadProcessMemory(process, remoteText, buffer,
                             static_cast<SIZE_T>(chars) * sizeof(wchar_t), nullptr))
        return false;

    buffer[chars] = L'\0';
    *out = QString::fromWCharArray(buffer, chars);
    return true;
}

// Moves an item to `target` and confirms the move stuck. LVM_SETITEMPOSITION
// takes the coordinates packed in lParam (MAKELPARAM), so the move itself needs
// no remote memory -- the read-back that verifies it does.
bool placeItem(int index, const QPoint& target)
{
    const HWND listView = findDesktopListView();
    if (!listView) {
        qCWarning(lc) << "placeItem: desktop list view (SysListView32) not found";
        return false;
    }

    RemoteProcess process(listView);
    if (!process.isValid()) {
        qCWarning(lc) << "placeItem: OpenProcess failed, error" << ::GetLastError();
        return false;
    }

    RemoteMemory memory(process.handle(), sizeof(POINT));
    if (!memory.isValid()) {
        qCWarning(lc) << "placeItem: VirtualAllocEx failed, error" << ::GetLastError();
        return false;
    }

    ::SendMessageW(listView, LVM_SETITEMPOSITION, static_cast<WPARAM>(index),
                   MAKELPARAM(target.x(), target.y()));

    // The BOOL that LVM_SETITEMPOSITION answers is not the last word: auto-
    // arrange, or a layout pass the DefView runs on its own repaint, can put
    // the item back once the message has already returned. Success means the
    // position reads back as the requested one.
    QPoint readBack;
    if (!readItemPosition(listView, process.handle(), memory.address(), index, &readBack)) {
        qCWarning(lc) << "placeItem: could not read back the position of item" << index;
        return false;
    }
    if (readBack != target) {
        qCWarning(lc) << "placeItem: item" << index << "landed at" << readBack << "instead of"
                      << target;
        return false;
    }
    return true;
}

} // namespace

QVector<NativeDesktopIcon> readNativeDesktopIcons()
{
    QVector<NativeDesktopIcon> icons;

    const HWND listView = findDesktopListView();
    if (!listView) {
        qCWarning(lc) << "readNativeDesktopIcons: desktop list view (SysListView32) not found";
        return icons;
    }

    const int count = static_cast<int>(::SendMessageW(listView, LVM_GETITEMCOUNT, 0, 0));
    if (count <= 0)
        return icons; // an empty desktop is a valid answer, not a failure

    // wParam FALSE asks for the large-icon metrics, which is the view the
    // desktop uses; the answer packs the cell step as MAKELONG(cx, cy).
    const DWORD spacing =
        static_cast<DWORD>(::SendMessageW(listView, LVM_GETITEMSPACING, FALSE, 0));
    const QSize slot(static_cast<int>(LOWORD(spacing)), static_cast<int>(HIWORD(spacing)));

    RemoteProcess process(listView);
    if (!process.isValid()) {
        qCWarning(lc) << "readNativeDesktopIcons: OpenProcess failed, error" << ::GetLastError();
        return {};
    }

    RemoteMemory memory(process.handle(), sizeof(RemoteScratch));
    if (!memory.isValid()) {
        qCWarning(lc) << "readNativeDesktopIcons: VirtualAllocEx failed, error" << ::GetLastError();
        return {};
    }

    void* const remotePoint = memory.at(offsetof(RemoteScratch, point));
    void* const remoteItem = memory.at(offsetof(RemoteScratch, item));
    void* const remoteText = memory.at(offsetof(RemoteScratch, text));
    const QPoint origin = clientOriginOnScreen(listView);

    icons.reserve(count);
    for (int index = 0; index < count; ++index) {
        NativeDesktopIcon icon;
        icon.index = index;
        icon.slot = slot;

        QPoint clientPosition;
        if (!readItemPosition(listView, process.handle(), remotePoint, index, &clientPosition)) {
            qCWarning(lc) << "readNativeDesktopIcons: LVM_GETITEMPOSITION failed for item" << index;
            return {};
        }
        icon.position = clientPosition + origin;

        if (!readItemText(listView, process.handle(), remoteItem, remoteText, index, &icon.name)) {
            qCWarning(lc) << "readNativeDesktopIcons: LVM_GETITEMTEXTW failed for item" << index;
            return {};
        }

        icons.append(icon);
    }

    return icons;
}

bool nativeDesktopIconsVisible()
{
    const HWND list = findDesktopListView();
    return list && ::IsWindowVisible(list);
}

} // namespace starfish::platform::win
