#include "platform/win/shell_icon_cache.hpp"

#include <QDebug>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QLoggingCategory>
#include <QtGlobal>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>      // ILD_TRANSPARENT
#include <commoncontrols.h> // IImageList, IID_IImageList
#include <shellapi.h>       // SHGetFileInfoW, SHGetImageList, SHIL_*
#include <shlobj.h>         // SHParseDisplayName

namespace starfish::platform::win {

namespace {

QLoggingCategory lc("starfish.desktop.icons");

// Edge lengths of the system image lists: 16 (SHIL_SMALL), 32 (SHIL_LARGE),
// 48 (SHIL_EXTRALARGE) and 256 (SHIL_JUMBO).
constexpr int kMinSizePx = 16;
constexpr int kMaxSizePx = 256;

inline const wchar_t* wide(const QString& path)
{
    return reinterpret_cast<const wchar_t*>(path.utf16());
}

#if defined(__MINGW32__)
// MinGW-w64 declares IID_IImageList in <commoncontrols.h> but ships no import
// library entry for it, so the linker cannot resolve the symbol. Value taken
// from the Windows SDK (commoncontrols.idl):
//   uuid(46EB5926-582E-4017-9FDF-E8998DAA0950)
const GUID kImageListIid = {0x46EB5926,
                            0x582E,
                            0x4017,
                            {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};
const IID& imageListIid()
{
    return kImageListIid;
}
#else
const IID& imageListIid()
{
    return IID_IImageList;
}
#endif

// Smallest system image list that is at least as large as the request, so we
// always scale down (never up).
int imageListFor(int sizePx)
{
    if (sizePx <= 16)
        return SHIL_SMALL;
    if (sizePx <= 32)
        return SHIL_LARGE;
    if (sizePx <= 48)
        return SHIL_EXTRALARGE;
    return SHIL_JUMBO;
}

QPixmap pixmapFromIcon(HICON icon)
{
    if (!icon)
        return {};
    // Qt's Windows backend converts an HICON through GetIconInfo/GetDIBits and
    // keeps the alpha channel, so no manual BitBlt is needed.
    const QImage image = QImage::fromHICON(icon);
    return image.isNull() ? QPixmap() : QPixmap::fromImage(image);
}

QPixmap scaledTo(const QPixmap& source, int sizePx)
{
    if (source.isNull())
        return source;
    if (source.width() == sizePx && source.height() == sizePx)
        return source;
    return source.scaled(sizePx, sizePx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Last resort: the classic 32 px lookup, used when the system image list is
// unavailable (it is populated by the Shell, so it can lag right after logon).
QPixmap fallbackIcon(const QString& nativePath)
{
    SHFILEINFOW info{};
    if (::SHGetFileInfoW(wide(nativePath), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) == 0
        || !info.hIcon) {
        return {};
    }
    const QPixmap pixmap = pixmapFromIcon(info.hIcon);
    ::DestroyIcon(info.hIcon);
    return pixmap;
}

} // namespace

QPixmap ShellIconCache::loadIcon(const QString& path, int sizePx, bool overlays)
{
    if (path.isEmpty())
        return {};

    const int target = qBound(kMinSizePx, sizePx, kMaxSizePx);
    const QString nativePath = QDir::toNativeSeparators(path);

    // SHGFI_OVERLAYINDEX: the shortcut arrow (and any other overlay -- sync
    // state, shared) is part of how the desktop draws an icon; since ADR-21 the
    // desktop is ours to draw, so it travels with the icon. The overlay comes
    // back in the top byte of iIcon.
    SHFILEINFOW info{};
    DWORD_PTR found = 0;
    if (nativePath.startsWith(QLatin1String("::"))) {
        // ADR-21: a namespace item of the desktop (This PC, the Recycle Bin, the
        // user's folder) has a parsing name, not a path -- resolved to its ID list.
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(wide(nativePath), nullptr, &pidl, 0, nullptr)) && pidl) {
            found = ::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info),
                                     SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_LARGEICON
                                         | SHGFI_OVERLAYINDEX | SHGFI_ICON);
            ::CoTaskMemFree(pidl);
        }
        if (found == 0)
            return {};
    } else {
        found = ::SHGetFileInfoW(wide(nativePath), 0, &info, sizeof(info),
                                 SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_OVERLAYINDEX | SHGFI_ICON);
        if (found == 0)
            return scaledTo(fallbackIcon(nativePath), target);
    }
    // SHGFI_OVERLAYINDEX only works together with SHGFI_ICON (documented); the
    // icon itself is not used -- the image list gives the right size.
    if (info.hIcon)
        ::DestroyIcon(info.hIcon);
    const int iconIndex = info.iIcon & 0x00FFFFFF;
    const UINT overlay = (static_cast<UINT>(info.iIcon) >> 24) & 0x0F;

    IImageList* imageList = nullptr;
    if (FAILED(::SHGetImageList(imageListFor(target), imageListIid(),
                                reinterpret_cast<void**>(&imageList)))
        || !imageList) {
        return scaledTo(fallbackIcon(nativePath), target);
    }

    HICON icon = nullptr;
    const HRESULT hr = imageList->GetIcon(iconIndex, ILD_TRANSPARENT, &icon);
    // The overlay is an image of the same list, drawn over the icon. Asked for
    // separately: GetIcon() ignores an overlay mask (measured -- the shortcut
    // arrow did not appear).
    // Taken from the *small* list, drawn unscaled in the bottom-left corner:
    // that is the arrow's size on the desktop at 48 px (measured against
    // build\captures\baseline_progman.png -- the 48 px list's own overlay is
    // twice as large). Large icons (96+) use the 32 px one.
    imageList->Release();
    const int overlaySize = target >= 96 ? 32 : 16;
    HICON overlayIcon = nullptr;
    IImageList* overlayList = nullptr;
    if (overlays && overlay > 0
        && SUCCEEDED(::SHGetImageList(imageListFor(overlaySize), imageListIid(),
                                      reinterpret_cast<void**>(&overlayList)))
        && overlayList) {
        int overlayIndex = -1;
        if (SUCCEEDED(overlayList->GetOverlayImage(static_cast<int>(overlay), &overlayIndex))
            && overlayIndex >= 0)
            overlayList->GetIcon(overlayIndex, ILD_TRANSPARENT, &overlayIcon);
        overlayList->Release();
    }

    if (FAILED(hr) || !icon) {
        qCDebug(lc) << "system image list lookup failed for" << path << "-> falling back";
        return scaledTo(fallbackIcon(nativePath), target);
    }

    const QPixmap base = pixmapFromIcon(icon);
    ::DestroyIcon(icon);
    const QPixmap overlayPixmap = overlayIcon ? pixmapFromIcon(overlayIcon) : QPixmap();
    if (overlayIcon)
        ::DestroyIcon(overlayIcon);
    if (base.isNull())
        return scaledTo(fallbackIcon(nativePath), target);

    QPixmap pixmap = scaledTo(base, target);
    if (!overlayPixmap.isNull() && pixmap.width() > overlayPixmap.width()) {
        QPainter painter(&pixmap);
        painter.drawPixmap(0, pixmap.height() - overlayPixmap.height(), overlayPixmap);
    }
    return pixmap;
}

QPixmap ShellIconCache::iconFor(const QString& path, int sizePx, bool overlays)
{
    // '\x1f' cannot appear in a path, so the key is unambiguous.
    const QString key = path + QLatin1Char('\x1f') + QString::number(sizePx)
                        + (overlays ? QLatin1String("") : QLatin1String("\x1fbare"));

    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd())
        return it.value();

    const QPixmap pixmap = loadIcon(path, sizePx, overlays);
    m_cache.insert(key, pixmap);
    if (pixmap.isNull())
        m_failed.insert(key, Failed{path, sizePx, overlays, 0});
    return pixmap;
}

bool ShellIconCache::retryFailed()
{
    bool any = false;
    for (auto it = m_failed.begin(); it != m_failed.end();) {
        const QPixmap pixmap = loadIcon(it->path, it->sizePx, it->overlays);
        if (!pixmap.isNull()) {
            m_cache.insert(it.key(), pixmap);
            any = true;
            it = m_failed.erase(it);
        } else if (++it->tries >= 20) {
            it = m_failed.erase(it); // a minute of trying: what is blank stays blank
        } else {
            ++it;
        }
    }
    return any;
}

bool ShellIconCache::forget(const QString& prefix)
{
    bool any = false;
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key().startsWith(prefix, Qt::CaseInsensitive)) {
            m_failed.remove(it.key());
            it = m_cache.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    return any;
}

int ShellIconCache::recycleBinState()
{
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(::SHQueryRecycleBinW(nullptr, &info)))
        return -1;
    return info.i64NumItems > 0 ? 1 : 0;
}

void ShellIconCache::clear()
{
    m_cache.clear();
    m_failed.clear();
}

int ShellIconCache::cachedCount() const
{
    return static_cast<int>(m_cache.size());
}

} // namespace starfish::platform::win
