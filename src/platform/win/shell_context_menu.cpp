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

// --- the desktop's background menu (T-055) ---------------------------------
//
// The command-ID window is deliberately narrower than the item one: the shell
// allocates upwards from kBackgroundIdFirst and must not reach
// kStarfishCommandId, which is where our own entry lives. TrackPopupMenuEx
// returns the id as an int, so everything stays well inside that.
constexpr UINT kBackgroundIdFirst = 0x100;
constexpr UINT kBackgroundIdLast = 0x6FFF;
constexpr UINT kStarfishCommandId = 0x7000;

struct BackgroundMenu
{
    IShellFolder* desktop = nullptr;
    IContextMenu* contextMenu = nullptr;
    HMENU hmenu = nullptr;
    bool hasShellVerbs = false;

    ~BackgroundMenu()
    {
        if (hmenu)
            ::DestroyMenu(hmenu);
        if (contextMenu)
            contextMenu->Release();
        if (desktop)
            desktop->Release();
    }

    BackgroundMenu() = default;
    BackgroundMenu(const BackgroundMenu&) = delete;
    BackgroundMenu& operator=(const BackgroundMenu&) = delete;
};

// Builds the popup, shell entries first, then our entry on top of them. Returns
// false only when there is nothing to show at all.
//
// `ownerHwnd` is the window the popup will belong to; when there is none (the
// headless diagnostic) the shell window stands in, because the desktop
// background menu's view object is created for a window and the shell is not
// happy being handed a null one.
bool prepareBackgroundMenu(BackgroundMenu& menu, const QString& starfishLabel, void* ownerHwnd)
{
    ensureComInitialized();

    menu.hmenu = ::CreatePopupMenu();
    if (!menu.hmenu) {
        qCWarning(lc) << "CreatePopupMenu failed, error" << ::GetLastError();
        return false;
    }

    const HWND owner = ownerHwnd ? static_cast<HWND>(ownerHwnd) : ::GetShellWindow();

    // The desktop namespace folder's *view object* is the IContextMenu of the
    // background itself -- the documented way to obtain the menu SysListView32
    // shows when the wallpaper is right-clicked. It is not an item menu, so
    // there is no PIDL and no GetUIObjectOf.
    if (SUCCEEDED(::SHGetDesktopFolder(&menu.desktop)) && menu.desktop) {
        const HRESULT hr = menu.desktop->CreateViewObject(
            owner, IID_IContextMenu, reinterpret_cast<void**>(&menu.contextMenu));
        if (SUCCEEDED(hr) && menu.contextMenu) {
            const HRESULT hrFill = menu.contextMenu->QueryContextMenu(
                menu.hmenu, 0, kBackgroundIdFirst, kBackgroundIdLast, CMF_NORMAL);
            menu.hasShellVerbs = SUCCEEDED(hrFill);
            if (!menu.hasShellVerbs)
                qCWarning(lc) << "QueryContextMenu (desktop background) failed, error"
                              << Qt::hex << hrFill;
        } else {
            qCWarning(lc) << "the desktop view object has no IContextMenu, error" << Qt::hex << hr;
        }
    } else {
        qCWarning(lc) << "SHGetDesktopFolder failed, error" << ::GetLastError();
    }

    // Ours first, then a separator: the user right-clicked the desktop, and the
    // first thing this product is for is a fence. Each insertion goes at position
    // 0, so inserting the separator first leaves the order entry/separator/shell.
    if (menu.hasShellVerbs)
        ::InsertMenuW(menu.hmenu, 0, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::InsertMenuW(menu.hmenu, 0, MF_BYPOSITION | MF_STRING, kStarfishCommandId,
                  reinterpret_cast<LPCWSTR>(starfishLabel.utf16()));
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

ShellContextMenu::BackgroundChoice ShellContextMenu::showForDesktopBackground(
    const QPoint& globalPos, void* ownerHwnd, const QString& starfishLabel)
{
    BackgroundMenu menu;
    if (!prepareBackgroundMenu(menu, starfishLabel, ownerHwnd))
        return BackgroundChoice::Dismissed;

    const HWND owner = static_cast<HWND>(ownerHwnd);
    const int selected = ::TrackPopupMenuEx(
        menu.hmenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, globalPos.x(), globalPos.y(),
        owner, nullptr);
    if (selected <= 0)
        return BackgroundChoice::Dismissed;
    if (static_cast<UINT>(selected) == kStarfishCommandId)
        return BackgroundChoice::StarfishVerb;

    // One of the shell's own entries. TPM_RETURNCMD asked for the id instead of
    // letting Windows post WM_COMMAND, so invoking the verb is ours to do -- and
    // the verb is selected by its zero-based index, exactly as in showForPath().
    if (menu.contextMenu) {
        CMINVOKECOMMANDINFO info{};
        info.cbSize = sizeof(info);
        info.hwnd = owner;
        info.lpVerb = reinterpret_cast<LPCSTR>(MAKEINTRESOURCE(selected - kBackgroundIdFirst));
        info.nShow = SW_SHOWNORMAL;
        const HRESULT hr = menu.contextMenu->InvokeCommand(&info);
        if (FAILED(hr))
            qCWarning(lc) << "InvokeCommand (desktop background) failed, error" << Qt::hex << hr;
    }
    return BackgroundChoice::ShellVerb;
}

QStringList ShellContextMenu::desktopBackgroundVerbs(QString* starfishLabel)
{
    QStringList verbs;
    BackgroundMenu menu;
    if (!prepareBackgroundMenu(menu, QStringLiteral("Create a fence here"), nullptr)) {
        if (starfishLabel)
            starfishLabel->clear();
        return verbs;
    }

    // Our own entry is the first item, by construction; reading it back is what
    // makes this a measurement rather than a claim.
    if (starfishLabel) {
        wchar_t buffer[256]{};
        const int length = ::GetMenuStringW(menu.hmenu, 0, buffer, 255, MF_BYPOSITION);
        *starfishLabel = length > 0 ? QString::fromWCharArray(buffer) : QString();
    }

    if (!menu.contextMenu)
        return verbs;

    const int count = ::GetMenuItemCount(menu.hmenu);
    verbs.reserve(count);
    for (int i = 0; i < count; ++i) {
        // A separator and a submenu are both "not a verb", but they report
        // differently and the difference matters: GetMenuItemID returns (UINT)-1
        // for a *submenu*, and **0** for a separator. Treating 0 as an id made the
        // verb index underflow (0 - kBackgroundIdFirst = 4294967040) and the shell
        // extension crashed on it. GetMenuState is the call that actually knows.
        const UINT state = ::GetMenuState(menu.hmenu, i, MF_BYPOSITION);
        if (state == static_cast<UINT>(-1) || (state & MF_SEPARATOR))
            continue;
        const UINT id = ::GetMenuItemID(menu.hmenu, i);
        if (id == static_cast<UINT>(-1) || id < kBackgroundIdFirst)
            continue; // a submenu, or something the shell invented below our range
        if (id == kStarfishCommandId)
            continue; // our own entry is not the shell's to name
        char buffer[64]{};
        const HRESULT hr = menu.contextMenu->GetCommandString(id - kBackgroundIdFirst, GCS_VERBA,
                                                             nullptr, buffer, sizeof(buffer));
        if (SUCCEEDED(hr) && buffer[0] != '\0')
            verbs.append(QString::fromLatin1(buffer));
    }
    return verbs;
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
        // See desktopBackgroundVerbs(): a separator reports id 0, not (UINT)-1,
        // and asking the shell about it underflows the verb index.
        const UINT state = ::GetMenuState(q.hmenu, i, MF_BYPOSITION);
        if (state == static_cast<UINT>(-1) || (state & MF_SEPARATOR))
            continue;
        const UINT id = ::GetMenuItemID(q.hmenu, i);
        if (id == static_cast<UINT>(-1) || id < firstId)
            continue; // submenu, or an id outside the range the shell was given
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
