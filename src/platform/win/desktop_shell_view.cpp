#include "platform/win/desktop_shell_view.hpp"

#include <QLoggingCategory>
#include <QPainter>

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h> // SEE_MASK_* behind CMIC_MASK_* (left out by WIN32_LEAN_AND_MEAN)
#include <commctrl.h> // SetWindowSubclass
#include <exdisp.h>   // IShellWindows
#include <shlguid.h>  // SID_STopLevelBrowser
#include <shlobj.h>
#include <shlwapi.h>  // StrRetToBufW
#include <shobjidl.h> // IFolderView2

namespace starfish::platform::win {

namespace {

Q_LOGGING_CATEGORY(lc, "starfish.shellview")

// The shell's commands are numbered from here; ours start far above them, so a
// returned id says by itself whose entry it was.
constexpr UINT kShellIdFirst = 0x0001;
constexpr UINT kShellIdLast = 0x6FFF;
constexpr UINT kOwnIdBase = 0x7000;

// RPC failures that mean "the explorer.exe we were talking to is gone": the
// proxies are dead and the connection has to be made again.
bool isDisconnected(HRESULT hr)
{
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
           || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == RPC_E_SERVERFAULT
           || hr == CO_E_OBJNOTCONNECTED;
}

template <typename T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ensureCom()
{
    static const bool once = [] {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
            qCWarning(lc) << "CoInitializeEx failed" << Qt::hex << hr;
        return true;
    }();
    Q_UNUSED(once);
}

HWND desktopListView()
{
    if (HWND progman = ::FindWindowW(L"Progman", nullptr)) {
        if (HWND defView = ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
            return ::FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    }
    HWND worker = nullptr;
    while ((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        if (HWND defView = ::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr))
            return ::FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    }
    return nullptr;
}

HWND desktopDefView()
{
    HWND list = desktopListView();
    if (list)
        return ::GetParent(list);
    if (HWND progman = ::FindWindowW(L"Progman", nullptr))
        return ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    return nullptr;
}

QString displayNameOf(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags)
{
    STRRET ret{};
    if (!folder || FAILED(folder->GetDisplayNameOf(child, flags, &ret)))
        return {};
    wchar_t buffer[MAX_PATH * 2]{};
    if (FAILED(::StrRetToBufW(&ret, child, buffer, MAX_PATH * 2)))
        return {};
    return QString::fromWCharArray(buffer);
}

QString verbOf(IContextMenu* menu, UINT offset)
{
    wchar_t buffer[128]{};
    if (FAILED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
                                      reinterpret_cast<LPSTR>(buffer), 127)))
        return {};
    return QString::fromWCharArray(buffer);
}

QString labelOf(HMENU menu, int position)
{
    wchar_t buffer[256]{};
    const int n = ::GetMenuStringW(menu, position, buffer, 255, MF_BYPOSITION);
    return n > 0 ? QString::fromWCharArray(buffer) : QString();
}

// The submenus StarFish builds itself: never handed to the shell (it has no
// business with them, and would see an HMENU it never created).
thread_local std::vector<HMENU> t_ownMenus;

bool isOwnMenu(HMENU menu)
{
    return std::find(t_ownMenus.begin(), t_ownMenus.end(), menu) != t_ownMenus.end();
}

// ⚠️ T-071 -- the only menu messages forwarded to the shell are the ones whose
// arguments are plain values: WM_INITMENUPOPUP (an HMENU, a position) is what
// fills "New", "View", "Sort by", "Send to"; WM_MENUCHAR (a character, an HMENU)
// is the keyboard accelerator. WM_DRAWITEM and WM_MEASUREITEM carry a *pointer
// into this process* in lParam: the menu object lives in explorer.exe, so
// forwarding them would hand it an address it does not own -- the exact crash
// LVM_HITTEST caused in session 40. Owner-drawn entries are therefore drawn by
// nobody; none of the desktop's own entries is owner-drawn (measured).
LRESULT CALLBACK menuOwnerSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR, DWORD_PTR ref)
{
    const HMENU target = reinterpret_cast<HMENU>(msg == WM_INITMENUPOPUP ? wParam : lParam);
    if ((msg == WM_INITMENUPOPUP || msg == WM_MENUCHAR) && !isOwnMenu(target)) {
        auto* menu = reinterpret_cast<IContextMenu*>(ref);
        IContextMenu3* menu3 = nullptr;
        if (menu && SUCCEEDED(menu->QueryInterface(IID_PPV_ARGS(&menu3))) && menu3) {
            LRESULT result = 0;
            const HRESULT hr = menu3->HandleMenuMsg2(msg, wParam, lParam, &result);
            menu3->Release();
            if (SUCCEEDED(hr))
                return msg == WM_MENUCHAR ? result : 0;
        } else {
            IContextMenu2* menu2 = nullptr;
            if (menu && SUCCEEDED(menu->QueryInterface(IID_PPV_ARGS(&menu2))) && menu2) {
                const HRESULT hr = menu2->HandleMenuMsg(msg, wParam, lParam);
                menu2->Release();
                if (SUCCEEDED(hr) && msg == WM_INITMENUPOPUP)
                    return 0;
            }
        }
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// By the item's type, not GetMenuState(): for an entry with a submenu that
// call packs the submenu's item count into the word it returns, and our own
// View submenu was taken for a separator and tidied away (measured).
bool isSeparator(HMENU menu, int position)
{
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE;
    return ::GetMenuItemInfoW(menu, position, TRUE, &info) && (info.fType & MFT_SEPARATOR);
}

// Removes leading, trailing and doubled separators -- what is left after
// entries were taken out of a menu.
void tidySeparators(HMENU menu)
{
    bool previousWasSeparator = true; // a leading separator goes
    for (int i = 0; i < ::GetMenuItemCount(menu);) {
        const bool separator = isSeparator(menu, i);
        if (separator && previousWasSeparator) {
            ::DeleteMenu(menu, i, MF_BYPOSITION);
            continue;
        }
        previousWasSeparator = separator;
        ++i;
    }
    const int count = ::GetMenuItemCount(menu);
    if (count > 0 && isSeparator(menu, count - 1))
        ::DeleteMenu(menu, count - 1, MF_BYPOSITION);
}

// Inserts StarFish's entries at `position`, submenus included. Returns the
// position after the last one.
int insertEntries(HMENU menu, int position, const std::vector<DesktopShellView::MenuEntry>& entries,
                  UINT idBase)
{
    for (const DesktopShellView::MenuEntry& entry : entries) {
        const auto* text = reinterpret_cast<LPCWSTR>(entry.label.utf16());
        if (!entry.children.empty()) {
            HMENU sub = ::CreatePopupMenu();
            t_ownMenus.push_back(sub);
            insertEntries(sub, 0, entry.children, idBase);
            ::InsertMenuW(menu, position++, MF_BYPOSITION | MF_POPUP | MF_STRING,
                          reinterpret_cast<UINT_PTR>(sub), text);
        } else {
            ::InsertMenuW(menu, position++,
                          MF_BYPOSITION | MF_STRING | (entry.checked ? MF_CHECKED : MF_UNCHECKED),
                          idBase + entry.id, text);
        }
        if (entry.separatorAfter)
            ::InsertMenuW(menu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }
    return position;
}

bool invokeOffset(IContextMenu* menu, UINT offset, const QString& verb, const QPoint& screen,
                  HWND owner)
{
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (::GetKeyState(VK_CONTROL) < 0)
        info.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (::GetKeyState(VK_SHIFT) < 0)
        info.fMask |= CMIC_MASK_SHIFT_DOWN;
    info.hwnd = owner;
    const std::wstring wideVerb = verb.toStdWString();
    const std::string narrowVerb = verb.toStdString();
    if (verb.isEmpty()) {
        info.lpVerb = MAKEINTRESOURCEA(offset);
        info.lpVerbW = MAKEINTRESOURCEW(offset);
    } else {
        info.lpVerb = narrowVerb.c_str();
        info.lpVerbW = wideVerb.c_str();
    }
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = POINT{screen.x(), screen.y()};
    const HRESULT hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    if (FAILED(hr)) {
        qCWarning(lc).noquote() << QStringLiteral("InvokeCommand(%1) failed: 0x%2")
                                       .arg(verb.isEmpty() ? QString::number(offset) : verb)
                                       .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

} // namespace

struct DesktopShellView::Impl {
    IShellView* view = nullptr;
    IFolderView2* folderView = nullptr;
    IShellFolder* folder = nullptr;
    // The DefView the connection was made through. Explorer can rebuild the
    // desktop's view without dying (and a restarted Explorer can hand back a
    // proxy that still answers): a different window means a different view.
    HWND defView = nullptr;

    ~Impl() { reset(); }

    void reset()
    {
        release(folder);
        release(folderView);
        release(view);
    }

    bool ensure()
    {
        if (view && folderView && folder) {
            if (desktopDefView() == defView)
                return true;
            qCInfo(lc) << "the desktop's view was rebuilt: reconnecting";
        }
        reset();
        defView = desktopDefView();
        ensureCom();

        IShellWindows* windows = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&windows));
        if (FAILED(hr) || !windows) {
            qCWarning(lc) << "CLSID_ShellWindows unavailable" << Qt::hex << hr;
            return false;
        }
        VARIANT location;
        ::VariantInit(&location);
        location.vt = VT_I4;
        location.lVal = CSIDL_DESKTOP;
        VARIANT empty;
        ::VariantInit(&empty);
        long hwnd = 0;
        IDispatch* dispatch = nullptr;
        hr = windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH,
                                   &dispatch);
        windows->Release();
        if (FAILED(hr) || !dispatch) {
            qCWarning(lc) << "FindWindowSW(SWC_DESKTOP) failed" << Qt::hex << hr;
            return false;
        }
        IServiceProvider* provider = nullptr;
        hr = dispatch->QueryInterface(IID_PPV_ARGS(&provider));
        dispatch->Release();
        if (FAILED(hr) || !provider)
            return false;
        IShellBrowser* browser = nullptr;
        hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
        provider->Release();
        if (FAILED(hr) || !browser)
            return false;
        hr = browser->QueryActiveShellView(&view);
        browser->Release();
        if (FAILED(hr) || !view) {
            view = nullptr;
            return false;
        }
        if (FAILED(view->QueryInterface(IID_PPV_ARGS(&folderView))) || !folderView) {
            reset();
            return false;
        }
        if (FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || !folder) {
            reset();
            return false;
        }
        return true;
    }

    // Checks a call's result and drops the connection when explorer went away.
    bool ok(HRESULT hr)
    {
        if (isDisconnected(hr)) {
            qCWarning(lc) << "the desktop view was disconnected (Explorer restarted?)";
            reset();
        }
        return SUCCEEDED(hr);
    }

    // The child id of the item whose parsing name is `name`, or null. Caller frees.
    PITEMID_CHILD find(const QString& name, int* indexOut = nullptr)
    {
        int count = 0;
        if (!ok(folderView->ItemCount(SVGIO_ALLVIEW, &count)))
            return nullptr;
        for (int i = 0; i < count; ++i) {
            PITEMID_CHILD child = nullptr;
            if (!ok(folderView->Item(i, &child)) || !child)
                continue;
            if (displayNameOf(folder, child, SHGDN_FORPARSING).compare(name, Qt::CaseInsensitive)
                == 0) {
                if (indexOut)
                    *indexOut = i;
                return child;
            }
            ::CoTaskMemFree(child);
        }
        return nullptr;
    }

    int runMenu(IContextMenu* menu, UINT flags, const QPoint& screen, HWND owner,
                const QList<MenuEntry>& extra, const QStringList& hidden, bool* rename)
    {
        HMENU popup = ::CreatePopupMenu();
        if (!popup)
            return -1;
        if (menu) {
            // Judged by what it put in the menu, not by its HRESULT: through the
            // proxy, CMF_CANRENAME answers E_INVALIDARG *and* fills the menu,
            // Rename included (measured, experiments/spike_selection.cpp).
            const HRESULT hr = menu->QueryContextMenu(popup, 0, kShellIdFirst, kShellIdLast, flags);
            ok(hr);
            if (::GetMenuItemCount(popup) <= 0)
                qCWarning(lc) << "the shell's menu came back empty" << Qt::hex << hr;
        }
        // The verbs a caller replaces with its own entry (our registry verb, for
        // one: the drawn desktop answers it at the click point instead).
        if (menu && !hidden.isEmpty()) {
            for (int i = ::GetMenuItemCount(popup) - 1; i >= 0; --i) {
                const UINT id = ::GetMenuItemID(popup, i);
                if (id == static_cast<UINT>(-1) || id < kShellIdFirst || id > kShellIdLast)
                    continue;
                const QString verb = verbOf(menu, id - kShellIdFirst);
                const QString label = labelOf(popup, i);
                for (const QString& h : hidden) {
                    if (verb.compare(h, Qt::CaseInsensitive) == 0
                        || label.compare(h, Qt::CaseInsensitive) == 0) {
                        ::DeleteMenu(popup, i, MF_BYPOSITION);
                        break;
                    }
                }
            }
        }
        // Ours on top, in the order given.
        const int position =
            insertEntries(popup, 0, std::vector<MenuEntry>(extra.begin(), extra.end()), kOwnIdBase);
        // The top level is not the shell's to fix up either: QueryContextMenu
        // already filled it, and only its submenus are filled on demand.
        if (!extra.empty())
            t_ownMenus.push_back(popup);
        if (!extra.isEmpty() && ::GetMenuItemCount(popup) > position
            && !isSeparator(popup, position - 1))
            ::InsertMenuW(popup, position, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        tidySeparators(popup);
        {
            QStringList shown;
            for (int i = 0; i < ::GetMenuItemCount(popup); ++i)
                shown << labelOf(popup, i);
            qCDebug(lc).noquote() << QStringLiteral("menu: %1").arg(shown.join(QStringLiteral(" | ")));
        }

        if (menu)
            ::SetWindowSubclass(owner, menuOwnerSubclass, 1, reinterpret_cast<DWORD_PTR>(menu));
        // A popup closes on an outside click only when its owner's thread is in the
        // foreground (TrackPopupMenu remarks).
        ::SetForegroundWindow(owner);
        const int chosen = ::TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                                              screen.x(), screen.y(), owner, nullptr);
        ::PostMessageW(owner, WM_NULL, 0, 0);
        if (menu)
            ::RemoveWindowSubclass(owner, menuOwnerSubclass, 1);

        int result = -1;
        if (chosen >= static_cast<int>(kOwnIdBase)) {
            result = chosen - static_cast<int>(kOwnIdBase);
        } else if (menu && chosen >= static_cast<int>(kShellIdFirst)) {
            const UINT offset = static_cast<UINT>(chosen) - kShellIdFirst;
            const QString verb = verbOf(menu, offset);
            qCInfo(lc).noquote() << QStringLiteral("shell verb chosen: \"%1\"").arg(verb);
            if (rename && verb.compare(QLatin1String("rename"), Qt::CaseInsensitive) == 0)
                *rename = true;
            else
                invokeOffset(menu, offset, QString(), screen, owner);
        }
        ::DestroyMenu(popup); // our submenus go with it
        t_ownMenus.clear();
        return result;
    }
};

DesktopShellView::DesktopShellView() : d(new Impl) {}

DesktopShellView::~DesktopShellView()
{
    delete d;
}

ShellViewState DesktopShellView::read()
{
    ShellViewState state;
    if (!d->ensure())
        return state;

    int count = 0;
    if (!d->ok(d->folderView->ItemCount(SVGIO_ALLVIEW, &count))) {
        // A dead connection is retried once: Explorer may just have restarted.
        if (!d->ensure() || !d->ok(d->folderView->ItemCount(SVGIO_ALLVIEW, &count)))
            return state;
    }

    // Positions are in the list view's client space; the drawn desktop works in
    // screen pixels. The list view keeps its geometry while hidden.
    POINT origin{0, 0};
    if (HWND list = desktopListView())
        ::ClientToScreen(list, &origin);

    state.items.reserve(count);
    for (int i = 0; i < count; ++i) {
        PITEMID_CHILD child = nullptr;
        if (!d->ok(d->folderView->Item(i, &child)) || !child)
            continue;
        ShellViewItem item;
        POINT pt{};
        d->folderView->GetItemPosition(child, &pt);
        item.position = QPoint(pt.x + origin.x, pt.y + origin.y);
        item.parsingName = displayNameOf(d->folder, child, SHGDN_FORPARSING);
        item.displayName = displayNameOf(d->folder, child, SHGDN_NORMAL);
        ::CoTaskMemFree(child);
        if (!item.parsingName.isEmpty())
            state.items.append(item);
    }

    POINT spacing{};
    if (SUCCEEDED(d->folderView->GetSpacing(&spacing)))
        state.spacing = QSize(spacing.x, spacing.y);
    FOLDERVIEWMODE mode{};
    int iconSize = 0;
    if (SUCCEEDED(d->folderView->GetViewModeAndIconSize(&mode, &iconSize)) && iconSize > 0)
        state.iconSize = iconSize;
    DWORD flags = 0;
    if (SUCCEEDED(d->folderView->GetCurrentFolderFlags(&flags))) {
        state.iconsShown = (flags & FWF_NOICONS) == 0;
        state.autoArrange = (flags & FWF_AUTOARRANGE) != 0;
        state.snapToGrid = (flags & FWF_SNAPTOGRID) != 0;
    }
    state.ok = true;
    return state;
}

bool DesktopShellView::select(const QStringList& parsingNames)
{
    if (!d->ensure())
        return false;
    d->view->SelectItem(nullptr, SVSI_DESELECTOTHERS);
    bool first = true;
    for (const QString& name : parsingNames) {
        int index = -1;
        PITEMID_CHILD child = d->find(name, &index);
        if (!child)
            continue;
        ::CoTaskMemFree(child);
        UINT flags = SVSI_SELECT;
        if (first)
            flags |= SVSI_DESELECTOTHERS | SVSI_FOCUSED;
        d->ok(d->folderView->SelectItem(index, flags));
        first = false;
    }
    return !first || parsingNames.isEmpty();
}

int DesktopShellView::showBackgroundMenu(const QPoint& screen, quintptr owner,
                                         const QList<MenuEntry>& extra,
                                         const QStringList& hiddenVerbs)
{
    IContextMenu* menu = nullptr;
    if (d->ensure()) {
        if (!d->ok(d->view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&menu))))
            menu = nullptr;
    }
    if (!menu)
        qCWarning(lc) << "the desktop's background menu is unavailable; only StarFish's entries";
    const int result = d->runMenu(menu, CMF_NORMAL, screen, reinterpret_cast<HWND>(owner), extra,
                                  hiddenVerbs, nullptr);
    release(menu);
    return result;
}

int DesktopShellView::showSelectionMenu(const QStringList& parsingNames, const QPoint& screen,
                                        quintptr owner, const QList<MenuEntry>& extra,
                                        bool* renameRequested)
{
    if (!select(parsingNames))
        return -1;
    IContextMenu* menu = nullptr;
    if (!d->ok(d->view->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&menu))) || !menu) {
        qCWarning(lc) << "the selection has no menu";
        return -1;
    }
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (::GetKeyState(VK_SHIFT) < 0)
        flags |= CMF_EXTENDEDVERBS;
    const int result = d->runMenu(menu, flags, screen, reinterpret_cast<HWND>(owner), extra, {},
                                  renameRequested);
    release(menu);
    return result;
}

bool DesktopShellView::invokeSelectionVerb(const QStringList& parsingNames, const QString& verb)
{
    if (!select(parsingNames))
        return false;
    IContextMenu* menu = nullptr;
    if (!d->ok(d->view->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&menu))) || !menu)
        return false;
    bool done = false;
    HMENU popup = ::CreatePopupMenu();
    if (popup
        && SUCCEEDED(menu->QueryContextMenu(popup, 0, kShellIdFirst, kShellIdLast,
                                            verb.isEmpty() ? CMF_DEFAULTONLY : CMF_NORMAL))) {
        if (verb.isEmpty()) {
            const UINT id = ::GetMenuDefaultItem(popup, FALSE, 0);
            if (id != static_cast<UINT>(-1) && id >= kShellIdFirst)
                done = invokeOffset(menu, id - kShellIdFirst, QString(), QPoint(), nullptr);
        } else {
            done = invokeOffset(menu, 0, verb, QPoint(), nullptr);
        }
    }
    if (popup)
        ::DestroyMenu(popup);
    release(menu);
    return done;
}

bool DesktopShellView::invokeBackgroundVerb(const QString& verb)
{
    if (!d->ensure())
        return false;
    IContextMenu* menu = nullptr;
    if (!d->ok(d->view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&menu))) || !menu)
        return false;
    bool done = false;
    HMENU popup = ::CreatePopupMenu();
    if (popup && SUCCEEDED(menu->QueryContextMenu(popup, 0, kShellIdFirst, kShellIdLast, CMF_NORMAL)))
        done = invokeOffset(menu, 0, verb, QPoint(), nullptr);
    if (popup)
        ::DestroyMenu(popup);
    release(menu);
    return done;
}

bool DesktopShellView::positionItem(const QString& parsingName, const QPoint& screen)
{
    if (!d->ensure())
        return false;
    PITEMID_CHILD child = d->find(parsingName);
    if (!child)
        return false;
    POINT pt{screen.x(), screen.y()};
    if (HWND list = desktopListView())
        ::ScreenToClient(list, &pt);
    PCUITEMID_CHILD items[] = {child};
    const bool done = d->ok(d->folderView->SelectAndPositionItems(1, items, &pt, SVSI_POSITIONITEM));
    ::CoTaskMemFree(child);
    return done;
}

namespace {

QStringList labelsOf(IContextMenu* menu, UINT flags)
{
    QStringList labels;
    HMENU popup = ::CreatePopupMenu();
    if (popup) {
        // See runMenu: the menu, not the HRESULT, says whether it worked.
        menu->QueryContextMenu(popup, 0, kShellIdFirst, kShellIdLast, flags);
        IContextMenu3* menu3 = nullptr;
        menu->QueryInterface(IID_PPV_ARGS(&menu3));
        for (int i = 0; i < ::GetMenuItemCount(popup); ++i) {
            if (isSeparator(popup, i))
                continue;
            const QString label = labelOf(popup, i).remove(QLatin1Char('&'));
            labels << label;
            HMENU sub = ::GetSubMenu(popup, i);
            if (!sub)
                continue;
            // The same message a popup sends, with the same plain-value
            // arguments (T-071: an HMENU and a position, no pointer).
            if (menu3) {
                LRESULT ignored = 0;
                menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), i, &ignored);
            }
            for (int j = 0; j < ::GetMenuItemCount(sub); ++j) {
                if (isSeparator(sub, j))
                    continue;
                labels << label + QStringLiteral(" > ") + labelOf(sub, j).remove(QLatin1Char('&'));
            }
        }
        if (menu3)
            menu3->Release();
    }
    if (popup)
        ::DestroyMenu(popup);
    return labels;
}

} // namespace

bool DesktopShellView::setIconSize(int px)
{
    if (!d->ensure())
        return false;
    return d->ok(d->folderView->SetViewModeAndIconSize(FVM_ICON, px));
}

bool DesktopShellView::setViewFlag(ViewFlag flag, bool on)
{
    if (!d->ensure())
        return false;
    DWORD mask = 0;
    switch (flag) {
    case ViewFlag::AutoArrange:
        mask = FWF_AUTOARRANGE;
        break;
    case ViewFlag::SnapToGrid:
        mask = FWF_SNAPTOGRID;
        break;
    case ViewFlag::ShowIcons:
        mask = FWF_NOICONS;
        on = !on; // the flag says "no icons"
        break;
    }
    return d->ok(d->folderView->SetCurrentFolderFlags(mask, on ? mask : 0));
}

QStringList DesktopShellView::backgroundMenuLabels()
{
    if (!d->ensure())
        return {};
    IContextMenu* menu = nullptr;
    if (!d->ok(d->view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&menu))) || !menu)
        return {};
    const QStringList labels = labelsOf(menu, CMF_NORMAL);
    release(menu);
    return labels;
}

QStringList DesktopShellView::selectionMenuLabels(const QStringList& parsingNames)
{
    if (!select(parsingNames))
        return {};
    IContextMenu* menu = nullptr;
    if (!d->ok(d->view->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&menu))) || !menu)
        return {};
    const QStringList labels = labelsOf(menu, CMF_NORMAL | CMF_CANRENAME);
    release(menu);
    return labels;
}

QImage DesktopShellView::captureDesktop(const QRect& area)
{
    HWND progman = ::FindWindowW(L"Progman", nullptr);
    if (!progman || area.isEmpty())
        return {};
    RECT r{};
    ::GetWindowRect(progman, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return {};
    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ previous = ::SelectObject(memory, bitmap);
    QImage result;
    if (::PrintWindow(progman, memory, 2)) {
        const QImage full = QImage::fromHBITMAP(bitmap);
        result = full.copy(area.translated(-r.left, -r.top));
    }
    ::SelectObject(memory, previous);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return result;
}

QImage DesktopShellView::captureWallpaper(const QRect& area)
{
    HWND defView = desktopDefView();
    if (!defView || area.isEmpty())
        return {};
    RECT r{};
    ::GetWindowRect(defView, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return {};

    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    QImage result;
    if (dib && bits) {
        HGDIOBJ previous = ::SelectObject(memory, dib);
        // PW_RENDERFULLCONTENT (2): the window's own composed content, even where
        // application windows cover it.
        if (::PrintWindow(defView, memory, 2)) {
            const QImage captured(static_cast<const uchar*>(bits), w, h, w * 4,
                                  QImage::Format_RGB32);
            result = QImage(area.size(), QImage::Format_ARGB32);
            result.fill(Qt::black);
            QPainter painter(&result);
            painter.drawImage(QPoint(r.left - area.x(), r.top - area.y()), captured);
        } else {
            qCWarning(lc) << "PrintWindow(SHELLDLL_DefView) failed" << ::GetLastError();
        }
        ::SelectObject(memory, previous);
    }
    if (dib)
        ::DeleteObject(dib);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return result;
}

} // namespace starfish::platform::win
