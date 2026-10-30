// Read-only spike (ADR-21): the selection's menu from the desktop's own view.
#include <windows.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstdio>
#include <initializer_list>
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellWindows* w = nullptr; CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&w));
    VARIANT loc; VariantInit(&loc); loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP; VARIANT e; VariantInit(&e);
    long hwnd = 0; IDispatch* disp = nullptr;
    w->FindWindowSW(&loc, &e, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp);
    IServiceProvider* sp = nullptr; disp->QueryInterface(IID_PPV_ARGS(&sp));
    IShellBrowser* b = nullptr; sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&b));
    IShellView* v = nullptr; b->QueryActiveShellView(&v);
    IFolderView2* fv = nullptr; v->QueryInterface(IID_PPV_ARGS(&fv));
    HRESULT hr = fv->SelectItem(0, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
    int n = -1; fv->ItemCount(SVGIO_SELECTION, &n);
    printf("SelectItem %08lx, selected %d\n", hr, n);
    for (UINT flags : {(UINT)CMF_NORMAL, (UINT)(CMF_NORMAL | CMF_CANRENAME)}) {
        IContextMenu* cm = nullptr;
        hr = v->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&cm));
        printf("GetItemObject(SELECTION) %08lx\n", hr);
        if (!cm) continue;
        HMENU m = CreatePopupMenu();
        hr = cm->QueryContextMenu(m, 0, 1, 0x6FFF, flags);
        printf("  flags %x -> QueryContextMenu %08lx, %d items\n", flags, hr, GetMenuItemCount(m));
        DestroyMenu(m); cm->Release();
    }
    // in-process alternative: the folder from the view, GetUIObjectOf
    IShellFolder* f = nullptr; fv->GetFolder(IID_PPV_ARGS(&f));
    PITEMID_CHILD c = nullptr; fv->Item(0, &c);
    IContextMenu* cm2 = nullptr;
    hr = f->GetUIObjectOf(nullptr, 1, (PCUITEMID_CHILD_ARRAY)&c, IID_IContextMenu, nullptr, (void**)&cm2);
    printf("proxy folder GetUIObjectOf %08lx\n", hr);
    if (cm2) { HMENU m = CreatePopupMenu(); hr = cm2->QueryContextMenu(m, 0, 1, 0x6FFF, CMF_NORMAL); printf("  -> %08lx, %d items\n", hr, GetMenuItemCount(m)); DestroyMenu(m); cm2->Release(); }
    // in-process desktop folder with the same child id
    IShellFolder* desk = nullptr; SHGetDesktopFolder(&desk);
    IContextMenu* cm3 = nullptr;
    hr = desk->GetUIObjectOf(nullptr, 1, (PCUITEMID_CHILD_ARRAY)&c, IID_IContextMenu, nullptr, (void**)&cm3);
    printf("in-process desktop GetUIObjectOf %08lx\n", hr);
    if (cm3) { HMENU m = CreatePopupMenu(); hr = cm3->QueryContextMenu(m, 0, 1, 0x6FFF, CMF_NORMAL | CMF_CANRENAME); printf("  -> %08lx, %d items\n", hr, GetMenuItemCount(m)); DestroyMenu(m); cm3->Release(); }
    fv->SelectItem(0, SVSI_DESELECT);
    return 0;
}

