#include "platform/win/shell_context_menu.hpp"

#include <QDebug>
#include <QLoggingCategory>

#include <objbase.h>
#include <shlobj.h>
#include <shlguid.h>
#include <windows.h>

namespace starfish::platform::win {
namespace {

Q_LOGGING_CATEGORY(lc, "starfish.shellmenu")

// IContextMenu::QueryContextMenu places the first command ID at this offset;
// the user's selection is `TrackPopupMenuEx` result minus this value, which is
// then the zero-based verb index passed to InvokeCommand.
constexpr UINT kIdCmdFirst = 0x100;
constexpr UINT kIdCmdLast = 0xFFFF;

// The Qt GUI thread is an STA, but the API also works on an explicit
// COINIT_APARTMENTTHREADED apartment. Idempotent on the same thread
// (S_FALSE on the second call), so a guard is enough.
void ensureComInitialized()
{
    static bool initialized = [] {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            qCWarning(lc) << "CoInitializeEx failed, error" << Qt::hex << hr;
        }
        return true;
    }();
    Q_UNUSED(initialized);
}

// RAII helper for the COM pointers the menu pipeline hands back: forgetting to
// Release even one of them is the kind of bug that only shows up under stress,
// so the success path and every early-return share the same destruction.
struct MenuQuery
{
    PIDLIST_ABSOLUTE fullPidl = nullptr;
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD childPidl = nullptr;
    IContextMenu* contextMenu = nullptr;
    HMENU hmenu = nullptr;

    ~MenuQuery()
    {
        if (hmenu)
            ::DestroyMenu(hmenu);
        if (contextMenu)
            contextMenu->Release();
        if (folder)
            folder->Release();
        if (fullPidl)
            ::ILFree(fullPidl);
    }

    // Move-only, but the fields are plain pointers; a defaulted move would
    // double-free. Make it non-copyable and non-movable to keep the cleanup
    // simple and the lifetimes obvious.
    MenuQuery() = default;
    MenuQuery(const MenuQuery&) = delete;
    MenuQuery& operator=(const MenuQuery&) = delete;
};

// Resolve `path` to a PIDL, bind to its parent IShellFolder, and ask for an
// IContextMenu on the child. Returns false (with a warning) on any COM error
// so the caller can bail without leaking anything.
bool prepareMenu(const QString& path, MenuQuery& q)
{
    ensureComInitialized();

    const HRESULT hrParse =
        ::SHParseDisplayName(reinterpret_cast<LPCWSTR>(path.utf16()),
                             nullptr, &q.fullPidl, 0, nullptr);
    if (FAILED(hrParse) || !q.fullPidl) {
        qCWarning(lc) << "SHParseDisplayName failed for" << path
                      << "error" << Qt::hex << hrParse;
        return false;
    }

    const HRESULT hrBind = ::SHBindToParent(q.fullPidl, IID_PPV_ARGS(&q.folder),
                                            &q.childPidl);
    if (FAILED(hrBind) || !q.folder || !q.childPidl) {
        qCWarning(lc) << "SHBindToParent failed, error" << Qt::hex << hrBind;
        return false;
    }

    // GetUIObjectOf's reserved slot is UINT*, not void**, so IID_PPV_ARGS does
    // not fit: the IID goes in argument 4, the reserved UINT* in argument 5
    // (nullptr here), and the interface pointer in argument 6.
    const HRESULT hrCtx = q.folder->GetUIObjectOf(
        nullptr, 1, &q.childPidl, IID_IContextMenu, nullptr,
        reinterpret_cast<void**>(&q.contextMenu));
    if (FAILED(hrCtx) || !q.contextMenu) {
        qCWarning(lc) << "GetUIObjectOf(IContextMenu) failed, error" << Qt::hex << hrCtx;
        return false;
    }
    return true;
}

// Ask the IContextMenu to fill `hmenu`, returning the verb ID range actually
// used (0 on failure). Uses the normal item-popup flags: include only the
// verbs the shell would show to a user (no cut/copy/paste placeholders).
bool populateMenu(IContextMenu* contextMenu, HMENU hmenu, UINT* firstId)
{
    const HRESULT hr = contextMenu->QueryContextMenu(hmenu, 0, kIdCmdFirst,
                                                     kIdCmdLast, CMF_NORMAL | CMF_VERBSONLY);
    if (FAILED(hr)) {
        qCWarning(lc) << "QueryContextMenu failed, error" << Qt::hex << hr;
        return false;
    }
    *firstId = kIdCmdFirst;
    return true;
}

} // namespace

void ShellContextMenu::showForPath(const QString& path, const QPoint& globalPos,
                                   void* ownerHwnd)
{
    MenuQuery q;
    if (!prepareMenu(path, q))
        return;

    q.hmenu = ::CreatePopupMenu();
    if (!q.hmenu) {
        qCWarning(lc) << "CreatePopupMenu failed, error" << ::GetLastError();
        return;
    }
    UINT firstId = 0;
    if (!populateMenu(q.contextMenu, q.hmenu, &firstId))
        return;

    const HWND owner = static_cast<HWND>(ownerHwnd);
    const int selected = ::TrackPopupMenuEx(
        q.hmenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        globalPos.x(), globalPos.y(), owner, nullptr);
    if (selected <= 0)
        return; // user dismissed the menu

    CMINVOKECOMMANDINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = owner;
    // The verb is selected by its zero-based index (selected - firstId),
    // packed via MAKEINTRESOURCE so the COM call sees it as a resource ID.
    // This is the canonical way to call a string-named verb; the W version
    // would also work but the A path is what MSDN's sample uses.
    info.lpVerb = reinterpret_cast<LPCSTR>(MAKEINTRESOURCE(selected - firstId));
    info.nShow = SW_SHOWNORMAL;

    const HRESULT hr = q.contextMenu->InvokeCommand(&info);
    if (FAILED(hr))
        qCWarning(lc) << "InvokeCommand failed, error" << Qt::hex << hr;
}

QStringList ShellContextMenu::menuVerbs(const QString& path)
{
    QStringList verbs;
    MenuQuery q;
    if (!prepareMenu(path, q))
        return verbs;

    q.hmenu = ::CreatePopupMenu();
    if (!q.hmenu)
        return verbs;

    UINT firstId = 0;
    if (!populateMenu(q.contextMenu, q.hmenu, &firstId))
        return verbs;

    const int count = ::GetMenuItemCount(q.hmenu);
    verbs.reserve(count);
    for (int i = 0; i < count; ++i) {
        const UINT id = ::GetMenuItemID(q.hmenu, i);
        if (id == static_cast<UINT>(-1))
            continue; // submenu
        // GCS_VERBA returns the verb as a null-terminated ANSI string in the
        // buffer. 64 chars is what the shell itself uses; verbs like
        // "opennewprocess" or "windows.share" fit comfortably.
        char buffer[64]{};
        const HRESULT hr = q.contextMenu->GetCommandString(
            id - firstId, GCS_VERBA, nullptr, buffer, sizeof(buffer));
        if (SUCCEEDED(hr) && buffer[0] != '\0')
            verbs.append(QString::fromLatin1(buffer));
    }
    return verbs;
}

} // namespace starfish::platform::win
