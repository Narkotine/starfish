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

    // Session 66: an icon that could not be read (a shortcut still being
    // written, a target being rebuilt) is not left blank for good -- retried
    // a few times; true when one came back, so the caller repaints.
    bool retryFailed();
    // Drops every cached size of the items whose path starts with `prefix`
    // (case-insensitive); true when something was dropped.
    bool forget(const QString& prefix);
    // The Recycle Bin: 1 full, 0 empty, -1 unknown (SHQueryRecycleBinW).
    static int recycleBinState();
    static QString recycleBinPath() { return QStringLiteral("::{645FF040-5081-101B-9F08-00AA002F954E}"); }

private:
    struct Failed {
        QString path;
        int sizePx = 0;
        bool overlays = true;
        int tries = 0;
    };
    QHash<QString, QPixmap> m_cache;
    QHash<QString, Failed> m_failed;
};

} // namespace starfish::platform::win
