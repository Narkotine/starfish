#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <cstdio>
const GUID kIID = {0x46EB5926,0x582E,0x4017,{0x9F,0xDF,0xE8,0x99,0x8D,0xAA,0x09,0x50}};
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SHFILEINFOW info{};
    DWORD_PTR r = SHGetFileInfoW(argv[1], 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_OVERLAYINDEX | SHGFI_ICON);
    printf("r=%llu iIcon=%08x overlay=%u\n", (unsigned long long)r, info.iIcon, ((unsigned)info.iIcon >> 24) & 0xF);
    IImageList* il = nullptr; SHGetImageList(SHIL_EXTRALARGE, kIID, (void**)&il);
    for (int o = 1; o <= 4; ++o) { int idx = -1; HRESULT hr = il->GetOverlayImage(o, &idx); printf("overlay %d -> hr %08lx idx %d\n", o, hr, idx); }
    return 0;
}
