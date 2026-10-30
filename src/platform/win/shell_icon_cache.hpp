#pragma once

#include <QHash>
#include <QPixmap>
#include <QString>

namespace starfish::platform::win {

// Shell icon lookup (T-004): the very icon the Shell would draw, resolved
// through the system image list and cached per (path, size).
//
//   SHGetFileInfoW(SHGFI_SYSICONINDEX)  -> index into the system image list
//   SHGetImageList(SHIL_*)              -> IImageList, GetIcon() -> HICON
//   QImage::fromHICON                   -> QPixmap
//
// The system image list is used instead of SHGFI_ICON because it exposes the
// 48 px and 256 px variants: desktop icons are drawn well above 32 px, and
// scaling a 32 px icon up looks blurry. SHGFI_ICON stays as a fallback.
//
// The icon of a .lnk file is its target's icon, which is what the Shell shows;
// no shortcut resolution is needed here.
class ShellIconCache
{
public:
    // `sizePx` is the requested edge length in logical pixels. The closest
    // system image list is used and the result is scaled smoothly, keeping the
    // aspect ratio. Returns a null pixmap when the Shell has no icon for it.
    // `overlays` false leaves out the shell's overlay -- the shortcut arrow
    // above all: fence tiles show it or not by the user's setting.
    QPixmap iconFor(const QString& path, int sizePx, bool overlays = true);

    // Same lookup, bypassing the cache.
    static QPixmap loadIcon(const QString& path, int sizePx, bool overlays = true);

    void clear();
    int cachedCount() const;

private:
    QHash<QString, QPixmap> m_cache;
};

} // namespace starfish::platform::win
