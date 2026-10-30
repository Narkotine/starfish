// spike_folderview -- read-only: the desktop's own IFolderView, cross-process
// (IShellWindows -> SID_STopLevelBrowser -> IShellView), and its background menu.
#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <cstdio>

static void dumpMenu(HMENU m, int depth)
{
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; ++i) {
        wchar_t buf[256]{};
        MENUITEMINFOW mi{}; mi.cbSize = sizeof(mi); mi.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE;
        mi.dwTypeData = buf; mi.cch = 255;
        GetMenuItemInfoW(m, i, TRUE, &mi);
        wprintf(L"%*s[%u] %ls%ls%ls\n", depth * 2, L"", mi.wID, (mi.fType & MFT_SEPARATOR) ? L"----" : buf,
                mi.hSubMenu ? L"  >" : L"", (mi.fType & MFT_OWNERDRAW) ? L" (ownerdraw)" : L"");
    }
}

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellWindows* sw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw));
    printf("ShellWindows %08lx\n", hr);
    if (FAILED(hr)) return 1;
    VARIANT loc; VariantInit(&loc); loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    VARIANT empty; VariantInit(&empty);
    long hwnd = 0; IDispatch* disp = nullptr;
    hr = sw->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp);
    printf("FindWindowSW %08lx hwnd %lx\n", hr, hwnd);
    if (FAILED(hr) || !disp) return 1;
    IServiceProvider* sp = nullptr; disp->QueryInterface(IID_PPV_ARGS(&sp));
    IShellBrowser* browser = nullptr;
    hr = sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    printf("browser %08lx\n", hr);
    IShellView* view = nullptr; hr = browser->QueryActiveShellView(&view);
    printf("view %08lx\n", hr);
    IFolderView2* fv = nullptr; hr = view->QueryInterface(IID_PPV_ARGS(&fv));
    printf("IFolderView2 %08lx\n", hr);
    FOLDERVIEWMODE mode{}; int iconSize = 0;
    hr = fv->GetViewModeAndIconSize(&mode, &iconSize);
    printf("mode %d iconSize %d (%08lx)\n", mode, iconSize, hr);
    POINT spacing{}; fv->GetSpacing(&spacing); printf("spacing %ld x %ld\n", spacing.x, spacing.y);
    DWORD flags = 0; fv->GetCurrentFolderFlags(&flags); printf("folder flags %08lx (autoarrange %d snap %d)\n", flags, !!(flags & FWF_AUTOARRANGE), !!(flags & FWF_SNAPTOGRID));
    int count = 0; fv->ItemCount(SVGIO_ALLVIEW, &count); printf("items %d\n", count);
    IShellFolder* folder = nullptr; hr = fv->GetFolder(IID_PPV_ARGS(&folder)); printf("folder %08lx\n", hr);
    for (int i = 0; i < count && i < 200; ++i) {
        PITEMID_CHILD child = nullptr;
        if (FAILED(fv->Item(i, &child))) continue;
        POINT pt{}; HRESULT hp = fv->GetItemPosition(child, &pt);
        STRRET sr{}; wchar_t name[MAX_PATH]{}, parse[MAX_PATH]{};
        if (folder && SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &sr))) StrRetToBufW(&sr, child, name, MAX_PATH);
        if (folder && SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr))) StrRetToBufW(&sr, child, parse, MAX_PATH);
        if (i < 12 || i > count - 4) wprintf(L"%3d (%ld,%ld) %08lx  %ls  |  %ls\n", i, pt.x, pt.y, hp, name, parse);
        CoTaskMemFree(child);
    }
    IContextMenu* cm = nullptr;
    hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&cm));
    printf("background IContextMenu %08lx\n", hr);
    if (cm) {
        HMENU m = CreatePopupMenu();
        hr = cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
        printf("QueryContextMenu %08lx\n", hr);
        dumpMenu(m, 0);
        IContextMenu3* cm3 = nullptr; printf("IContextMenu3 %08lx\n", cm->QueryInterface(IID_PPV_ARGS(&cm3)));
        for (int i = 0; i < GetMenuItemCount(m); ++i) {
            HMENU sub = GetSubMenu(m, i);
            if (!sub) continue;
            LRESULT lr = 0;
            HRESULT h3 = cm3 ? cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)sub, i, &lr) : E_NOINTERFACE;
            printf("-- submenu %d init %08lx\n", i, h3);
            dumpMenu(sub, 1);
            for (int j = 0; j < GetMenuItemCount(sub); ++j) {
                char verb[128]{}; UINT id = GetMenuItemID(sub, j);
                if (id != (UINT)-1 && id >= 1 && SUCCEEDED(cm->GetCommandString(id - 1, GCS_VERBA, nullptr, verb, sizeof verb))) printf("     verb[%u]=%s\n", id, verb);
            }
        }
        for (int j = 0; j < GetMenuItemCount(m); ++j) { char verb[128]{}; UINT id = GetMenuItemID(m, j);
            if (id != (UINT)-1 && id >= 1 && id < 0x7000 && SUCCEEDED(cm->GetCommandString(id - 1, GCS_VERBA, nullptr, verb, sizeof verb))) printf("  top verb[%u]=%s\n", id, verb); }
        DestroyMenu(m);
    }
    return 0;
}


