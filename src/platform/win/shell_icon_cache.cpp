#include "platform/win/shell_icon_cache.hpp"

#include <QDebug>
#include <QDir>
#include <QImage>
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

QPixmap ShellIconCache::loadIcon(const QString& path, int sizePx)
{
    if (path.isEmpty())
        return {};

    const int target = qBound(kMinSizePx, sizePx, kMaxSizePx);
    const QString nativePath = QDir::toNativeSeparators(path);

    SHFILEINFOW info{};
    if (::SHGetFileInfoW(wide(nativePath), 0, &info, sizeof(info),
                         SHGFI_SYSICONINDEX | SHGFI_LARGEICON)
        == 0) {
        return scaledTo(fallbackIcon(nativePath), target);
    }

    IImageList* imageList = nullptr;
    if (FAILED(::SHGetImageList(imageListFor(target), imageListIid(),
                                reinterpret_cast<void**>(&imageList)))
        || !imageList) {
        return scaledTo(fallbackIcon(nativePath), target);
    }

    HICON icon = nullptr;
    const HRESULT hr = imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
    imageList->Release();

    if (FAILED(hr) || !icon) {
        qCDebug(lc) << "system image list lookup failed for" << path << "-> falling back";
        return scaledTo(fallbackIcon(nativePath), target);
    }

    const QPixmap pixmap = pixmapFromIcon(icon);
    ::DestroyIcon(icon);
    if (pixmap.isNull())
        return scaledTo(fallbackIcon(nativePath), target);

    return scaledTo(pixmap, target);
}

QPixmap ShellIconCache::iconFor(const QString& path, int sizePx)
{
    // '\x1f' cannot appear in a path, so the key is unambiguous.
    const QString key = path + QLatin1Char('\x1f') + QString::number(sizePx);

    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd())
        return it.value();

    const QPixmap pixmap = loadIcon(path, sizePx);
    m_cache.insert(key, pixmap);
    return pixmap;
}

void ShellIconCache::clear()
{
    m_cache.clear();
}

int ShellIconCache::cachedCount() const
{
    return static_cast<int>(m_cache.size());
}

} // namespace starfish::platform::win
