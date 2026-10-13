#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include "core/desktop_item.hpp"
#include "core/zorder.hpp"
#include "platform/win/desktop_host.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/desktop_surface.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "ui/fence_view.hpp"
#include "version.h"

#if defined(_WIN32)
#include <cstdio>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dwmapi.h>
#include <windows.h>
#endif

namespace {

QLoggingCategory lc("starfish.main");

#if defined(_WIN32)
// True when the caller already gave us a usable stdout/stderr (a pipe, a
// file): the redirection must then be preserved.
bool hasRedirectedOutput()
{
    const HANDLE handles[2] = {::GetStdHandle(STD_OUTPUT_HANDLE),
                               ::GetStdHandle(STD_ERROR_HANDLE)};
    for (const HANDLE handle : handles) {
        if (!handle || handle == INVALID_HANDLE_VALUE)
            continue;
        const DWORD type = ::GetFileType(handle);
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK)
            return true;
    }
    return false;
}

// The executable is a GUI app (WIN32 in CMake) and therefore has no console of
// its own: borrow the one it was launched from, otherwise --dump and the log
// output would go nowhere.
void attachParentConsole()
{
    // Never hijack a redirection: freopen on CONOUT$ would silently discard a
    // pipe or a file the caller asked for (e.g. `starfish --dump > log.txt`).
    if (hasRedirectedOutput()) {
        // Qt decides on its own whether stderr is usable and, when it thinks it
        // is not, sends qCInfo/qCWarning to the debugger (OutputDebugString)
        // instead -- which is exactly how CLI diagnostics get lost. We have just
        // established that the caller gave us a real sink, so tell Qt about it.
        qputenv("QT_ASSUME_STDERR_HAS_CONSOLE", "1");
        return;
    }
    if (!::AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE* stream = nullptr;
#if defined(_MSC_VER)
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#else
    stream = freopen("CONOUT$", "w", stdout);
    stream = freopen("CONOUT$", "w", stderr);
#endif
    (void)stream;
}
#endif

// One line per desktop entry, for the --list-desktop diagnostic (T-004).
QString describeDesktopItem(const starfish::core::DesktopItem& item)
{
    const QString kind = item.isDir    ? QStringLiteral("dir ")
                         : item.isLink ? QStringLiteral("link")
                                       : QStringLiteral("file");
    QString line = QStringLiteral("%1  %2  %3")
                       .arg(kind, -4)
                       .arg(item.isHidden ? QStringLiteral("hidden") : QStringLiteral("------"))
                       .arg(item.name);
    if (item.sizeBytes >= 0)
        line += QStringLiteral("  %1 B").arg(item.sizeBytes);
    if (item.fromPublicDesktop)
        line += QStringLiteral("  [public desktop]");
    if (!item.targetPath.isEmpty())
        line += QStringLiteral("  -> %1").arg(item.targetPath);
    return line;
}

// File-system-safe stem for --icon-dump output files.
QString dumpStem(const QString& name)
{
    QString stem = name;
    for (QChar& ch : stem) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_'))
            ch = QLatin1Char('_');
    }
    return stem.isEmpty() ? QStringLiteral("item") : stem;
}

// Counts paint events delivered to a widget: tells apart "Qt never even tries
// to paint the hosted window" from "Qt paints, but the blit is lost" (T-020).
class PaintCounter : public QObject
{
public:
    int count = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint)
            ++count;
        return QObject::eventFilter(watched, event);
    }
};

// Screen grab of the primary monitor. Cross-checked against a before/after
// capture, because a screenshot only proves something if the desktop is
// actually visible (a maximised browser once produced a false negative).
QImage grabPrimaryScreen()
{
    if (QScreen* screen = QGuiApplication::primaryScreen())
        return screen->grabWindow(0).toImage();
    return {};
}

// Bounding box of the pixels that differ between two captures.
QRect diffBounds(const QImage& before, const QImage& after, qint64* changedPixels = nullptr)
{
    if (before.isNull() || after.isNull() || before.size() != after.size())
        return {};

    const QImage a = before.convertToFormat(QImage::Format_RGB32);
    const QImage b = after.convertToFormat(QImage::Format_RGB32);
    int minX = a.width();
    int minY = a.height();
    int maxX = -1;
    int maxY = -1;
    qint64 changed = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if ((ra[x] & 0x00FFFFFF) == (rb[x] & 0x00FFFFFF))
                continue;
            ++changed;
            minX = qMin(minX, x);
            minY = qMin(minY, y);
            maxX = qMax(maxX, x);
            maxY = qMax(maxY, y);
        }
    }
    if (changedPixels)
        *changedPixels = changed;
    if (maxX < 0)
        return {};
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

#if defined(_WIN32)
// Win+D ("show desktop"), used so the wallpaper -- and therefore a
// desktop-hosted window -- is really on screen in the capture. A second press
// restores the windows the user had open.
void pressShowDesktop()
{
    ::keybd_event(VK_LWIN, 0, 0, 0);
    ::keybd_event('D', 0, 0, 0);
    ::keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
    ::keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
}

// The T-020 decision experiment: blit a Qt-rendered image straight into the
// hosted HWND with plain GDI. If this shows up on screen, the raw-Win32
// surface is the viable rendering path (option (a) of docs/technique.md §7),
// independently of whatever Qt does with its own backing store.
bool blitImageToHwnd(HWND hwnd, const QImage& image, QString* detail)
{
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    if (source.isNull()) {
        if (detail)
            *detail = QStringLiteral("null image");
        return false;
    }
    if (source.bytesPerLine() != source.width() * 4) {
        if (detail)
            *detail = QStringLiteral("unexpected stride %1 (expected %2)")
                          .arg(source.bytesPerLine())
                          .arg(source.width() * 4);
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source.width();
    info.bmiHeader.biHeight = -source.height(); // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC dc = ::GetDC(hwnd);
    if (!dc) {
        if (detail)
            *detail = QStringLiteral("GetDC failed, error %1").arg(::GetLastError());
        return false;
    }
    ::SetStretchBltMode(dc, COLORONCOLOR);
    const int lines = ::StretchDIBits(dc, 0, 0, source.width(), source.height(), 0, 0,
                                      source.width(), source.height(), source.constBits(),
                                      &info, DIB_RGB_COLORS, SRCCOPY);
    const DWORD blitError = ::GetLastError();
    ::GdiFlush();
    ::ReleaseDC(hwnd, dc);
    if (lines == GDI_ERROR) {
        if (detail)
            *detail = QStringLiteral("StretchDIBits failed, error %1").arg(blitError);
        return false;
    }
    if (detail)
        *detail = QStringLiteral("%1 scan lines blitted").arg(lines);
    return true;
}

// --- T-020 experiment: the other way round --------------------------------
//
// Instead of asking Qt for a layered window (which is what makes Qt maintain
// WS_EX_LAYERED and, on the translucent path, push the content with
// UpdateLayeredWindow -- a call measured to fail on a cross-process child),
// let Qt own a plain opaque window and add the layered flag ourselves *before*
// reparenting: that is the last moment Windows still accepts the change.
bool hostManually(HWND hwnd, const QRect& rect, QString* detail)
{
    const HWND progman = ::FindWindowW(L"Progman", nullptr);
    const HWND defView =
        progman ? ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!progman || !defView) {
        if (detail)
            *detail = QStringLiteral("Progman / SHELLDLL_DefView not found");
        return false;
    }

    // Layered while the window is still top-level. SetWindowLong only caches
    // the style: SetWindowPos(SWP_FRAMECHANGED) is what makes Windows apply it.
    const LONG_PTR exStyle = (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST)
                             | WS_EX_LAYERED;
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const BOOL attributesOk = ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    const DWORD attributesError = ::GetLastError();
    const LONG_PTR afterSet = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!::SetParent(hwnd, progman)) {
        if (detail)
            *detail = QStringLiteral("SetParent failed, error %1").arg(::GetLastError());
        return false;
    }
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);
    const BOOL placed = ::SetWindowPos(hwnd, defView, rect.x(), rect.y(), rect.width(),
                                       rect.height(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::UpdateWindow(hwnd);

    const LONG_PTR exAfter = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (detail)
        *detail = QStringLiteral("ex-style 0x%1 -> 0x%2, alpha call %3 (err %4), placed %5")
                      .arg(static_cast<qulonglong>(exStyle), 0, 16)
                      .arg(static_cast<qulonglong>(exAfter), 0, 16)
                      .arg(attributesOk ? 1 : 0)
                      .arg(attributesError)
                      .arg(placed ? 1 : 0);
    return placed != FALSE && (exAfter & WS_EX_LAYERED);
}

// DWMWA_CLOAKED: a cloaked window is composited by nobody, so it is invisible
// whatever its z-order and however correct its content is.
bool isDwmCloaked(HWND hwnd)
{
    BOOL cloaked = FALSE;
    constexpr DWORD kDwmwaCloaked = 14;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, kDwmwaCloaked, &cloaked, sizeof(cloaked))))
        return cloaked != FALSE;
    return false;
}

// Read-back of a hosted window's own surface, so the blit can be checked even
// when the desktop is covered. PW_RENDERFULLCONTENT asks DWM for the real
// content instead of a WM_PRINTCLIENT round-trip.
QImage printWindowImage(HWND hwnd, QString* detail){
    RECT r{};
    if (!::GetWindowRect(hwnd, &r)) {
        if (detail)
            *detail = QStringLiteral("GetWindowRect failed");
        return {};
    }
    const int width = r.right - r.left;
    const int height = r.bottom - r.top;
    if (width <= 0 || height <= 0) {
        if (detail)
            *detail = QStringLiteral("empty window rect");
        return {};
    }

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = ::SelectObject(mem, bitmap);
    const BOOL ok = ::PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    const DWORD printError = ::GetLastError();

    QImage image(width, height, QImage::Format_ARGB32);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int lines = ::GetDIBits(mem, bitmap, 0, height, image.bits(), &info, DIB_RGB_COLORS);

    ::SelectObject(mem, previous);
    ::DeleteObject(bitmap);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    if (detail) {
        *detail = QStringLiteral("PrintWindow %1 (error %2), %3/%4 scan lines read back")
                      .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"))
                      .arg(printError)
                      .arg(lines)
                      .arg(height);
    }
    return lines == height ? image : QImage();
}

// --- T-020 experiment: blit from *inside* the paint cycle -------------------
//
// A WS_EX_LAYERED window only accepts GDI drawing that goes through its paint
// cycle: a blit issued with GetDC() between two WM_PAINTs can be dropped
// without any error (measured: StretchDIBits reports success, PrintWindow
// reads back black, the screen never changes). The spike never hit this
// because it painted from its own WndProc on WM_PAINT. This subclass reproduces
// exactly that, on top of Qt's window, to settle the question before any
// surface is rewritten.
QImage g_paintCycleImage;
WNDPROC g_paintCyclePreviousProc = nullptr;
int g_paintCyclePaints = 0;
QString g_paintCycleTrace;

LRESULT CALLBACK paintCycleProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT && !g_paintCycleImage.isNull()) {
        PAINTSTRUCT ps{};
        const HDC dc = ::BeginPaint(hwnd, &ps);
        ++g_paintCyclePaints;
        const QImage& image = g_paintCycleImage;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width();
        info.bmiHeader.biHeight = -image.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const int lines =
            ::StretchDIBits(dc, 0, 0, image.width(), image.height(), 0, 0, image.width(),
                            image.height(), image.constBits(), &info, DIB_RGB_COLORS, SRCCOPY);
        const int clipBox = ::GetClipBox(dc, nullptr);
        ::EndPaint(hwnd, &ps);
        g_paintCycleTrace +=
            QStringLiteral("[paint %1: rcPaint %2,%3 %4x%5, clipBox %6, %7 lines] ")
                .arg(g_paintCyclePaints)
                .arg(ps.rcPaint.left)
                .arg(ps.rcPaint.top)
                .arg(ps.rcPaint.right - ps.rcPaint.left)
                .arg(ps.rcPaint.bottom - ps.rcPaint.top)
                .arg(clipBox)
                .arg(lines);
        return 0; // we own this paint: Qt does not get to draw over it
    }
    if (!g_paintCyclePreviousProc)
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return ::CallWindowProcW(g_paintCyclePreviousProc, hwnd, message, wParam, lParam);
}

bool blitViaPaintCycle(HWND hwnd, const QImage& image, QString* detail)
{
    g_paintCyclePaints = 0;
    g_paintCycleTrace.clear();
    g_paintCycleImage = image.convertToFormat(QImage::Format_ARGB32);
    if (g_paintCycleImage.isNull()) {
        if (detail)
            *detail = QStringLiteral("null image");
        return false;
    }

    const LONG_PTR previous =
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                            reinterpret_cast<LONG_PTR>(&paintCycleProc));
    const DWORD subclassError = ::GetLastError();
    if (!previous) {
        if (detail)
            *detail = QStringLiteral("SetWindowLongPtr(GWLP_WNDPROC) refused, error %1")
                          .arg(subclassError);
        return false;
    }
    g_paintCyclePreviousProc = reinterpret_cast<WNDPROC>(previous);

    ::InvalidateRect(hwnd, nullptr, TRUE);
    ::UpdateWindow(hwnd);
    if (detail)
        *detail = QStringLiteral("%1 WM_PAINT handled -- %2")
                      .arg(g_paintCyclePaints)
                      .arg(g_paintCycleTrace.isEmpty() ? QStringLiteral("(none)")
                                                       : g_paintCycleTrace);
    return true;
}
#endif

} // namespace

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    attachParentConsole();
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StarFish"));
    QApplication::setApplicationVersion(QStringLiteral(STARFISH_VERSION));

    // qCInfo is not guaranteed to be on by default for custom categories.
    QLoggingCategory::setFilterRules(QStringLiteral("starfish.*=true"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("StarFish -- open-source desktop fences for Windows."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption dumpOption(
        QStringLiteral("dump"),
        QStringLiteral("Print the desktop window hierarchy (Progman/DefView) and exit."));
    const QCommandLineOption listDesktopOption(
        QStringLiteral("list-desktop"),
        QStringLiteral("List the desktop folder contents as the Shell sees them and exit."));
    const QCommandLineOption iconsOption(
        QStringLiteral("icons"),
        QStringLiteral("With --list-desktop: also resolve the Shell icon of every entry."));
    const QCommandLineOption iconSizeOption(
        QStringLiteral("icon-size"),
        QStringLiteral("With --list-desktop --icons: requested icon edge length in pixels."),
        QStringLiteral("px"), QStringLiteral("48"));
    const QCommandLineOption iconDumpOption(
        QStringLiteral("icon-dump"),
        QStringLiteral("With --list-desktop --icons: write every resolved icon as a PNG into "
                       "this directory (a non-null pixmap is not proof that it is drawn)."),
        QStringLiteral("dir"));
    const QCommandLineOption demoOption(
        QStringLiteral("host-demo"),
        QStringLiteral("Host an empty fence on the desktop (Phase 1 placement check)."));
    const QCommandLineOption secondsOption(
        QStringList{QStringLiteral("s"), QStringLiteral("seconds")},
        QStringLiteral("How long the demo fence stays up, in seconds."),
        QStringLiteral("seconds"), QStringLiteral("10"));
    const QCommandLineOption hideIconsOption(
        QStringLiteral("hide-icons"),
        QStringLiteral("Hide the native desktop icons while the demo runs (restored on exit)."));
    const QCommandLineOption renderProbeOption(
        QStringLiteral("render-probe"),
        QStringLiteral("T-020 diagnostic: host the fence, then blit a Qt-rendered image straight "
                       "into the hosted HWND with GDI and capture the screen before/after."));
    const QCommandLineOption showDesktopOption(
        QStringLiteral("show-desktop"),
        QStringLiteral("Press Win+D before capturing (and again afterwards) so the wallpaper is "
                       "actually visible: a capture of a covered desktop proves nothing."));
    const QCommandLineOption noParentOption(
        QStringLiteral("no-parent"),
        QStringLiteral("With --render-probe: keep the fence as a plain top-level window instead "
                       "of hosting it on the desktop (control run)."));
    const QCommandLineOption opaqueOption(
        QStringLiteral("opaque"),
        QStringLiteral("With --render-probe: do not ask Qt for a hair under full opacity, i.e. "
                       "let it drop WS_EX_LAYERED (control run)."));
    const QCommandLineOption paintCycleOption(
        QStringLiteral("paint-cycle"),
        QStringLiteral("With --render-probe: blit from inside WM_PAINT through an own WndProc, "
                       "instead of with GetDC between paint cycles."));
    const QCommandLineOption layeredManualOption(
        QStringLiteral("layered-manual"),
        QStringLiteral("With --render-probe: keep Qt opaque and add WS_EX_LAYERED ourselves "
                       "before reparenting, instead of asking Qt for a layered window."));
    const QCommandLineOption surfaceOption(
        QStringLiteral("surface"),
        QStringLiteral("With --render-probe: host a raw Win32 DesktopSurface and push a "
                       "Qt-rendered frame into it (the T-020 rendering path)."));
    const QCommandLineOption withDesktopItemsOption(
        QStringLiteral("with-desktop-items"),
        QStringLiteral("With --render-probe --surface: populate the fence with the real "
                       "desktop items (scanned via T-004) so the icon grid of T-005 is "
                       "verified on screen, and route mouse input for selection."));
    const QCommandLineOption pixelAlphaOption(
        QStringLiteral("pixel-alpha"),
        QStringLiteral("With --render-probe --surface: push frames via UpdateLayeredWindow "
                       "with per-pixel alpha (T-017) instead of the opaque WM_PAINT blit, "
                       "and render the fence translucent so the wallpaper shows through."));

    parser.addOption(dumpOption);
    parser.addOption(listDesktopOption);
    parser.addOption(iconsOption);
    parser.addOption(iconSizeOption);
    parser.addOption(iconDumpOption);
    parser.addOption(demoOption);
    parser.addOption(secondsOption);
    parser.addOption(hideIconsOption);
    parser.addOption(renderProbeOption);
    parser.addOption(showDesktopOption);
    parser.addOption(noParentOption);
    parser.addOption(opaqueOption);
    parser.addOption(paintCycleOption);
    parser.addOption(layeredManualOption);
    parser.addOption(surfaceOption);
    parser.addOption(withDesktopItemsOption);
    parser.addOption(pixelAlphaOption);
    parser.process(app);

    if (parser.isSet(dumpOption)) {
        starfish::platform::win::DesktopHost::dumpDesktopHierarchy();
        return 0;
    }

    if (parser.isSet(listDesktopOption)) {
        const QStringList folders = starfish::platform::win::DesktopScanner::desktopFolders();
        qCInfo(lc).noquote() << QStringLiteral("desktop folders: %1")
                                    .arg(folders.isEmpty() ? QStringLiteral("(none resolved)")
                                                           : folders.join(QStringLiteral(" | ")));

        QString error;
        const QList<starfish::core::DesktopItem> items =
            starfish::platform::win::DesktopScanner::scan(&error);
        if (!error.isEmpty())
            qCWarning(lc).noquote() << error;

        const bool withIcons = parser.isSet(iconsOption);
        bool sizeOk = false;
        const int iconSize = parser.value(iconSizeOption).toInt(&sizeOk);
        starfish::platform::win::ShellIconCache icons;

        QString dumpDir;
        if (parser.isSet(iconDumpOption)) {
            dumpDir = parser.value(iconDumpOption);
            if (!QDir().mkpath(dumpDir)) {
                qCWarning(lc).noquote() << QStringLiteral("could not create %1").arg(dumpDir);
                dumpDir.clear();
            }
        }

        qCInfo(lc).noquote() << QStringLiteral("%1 desktop entries").arg(items.size());
        int index = 0;
        for (const starfish::core::DesktopItem& item : items) {
            QString line = describeDesktopItem(item);
            if (withIcons) {
                const QPixmap icon =
                    icons.iconFor(item.path, sizeOk && iconSize > 0 ? iconSize : 48);
                line += icon.isNull() ? QStringLiteral("  [no icon]")
                                      : QStringLiteral("  [icon %1x%2]")
                                            .arg(icon.width())
                                            .arg(icon.height());
                if (!dumpDir.isEmpty() && !icon.isNull()) {
                    const QString file = QStringLiteral("%1/%2-%3.png")
                                             .arg(dumpDir)
                                             .arg(index, 3, 10, QLatin1Char('0'))
                                             .arg(dumpStem(item.name));
                    if (!icon.save(file))
                        qCWarning(lc).noquote() << QStringLiteral("could not write %1").arg(file);
                }
            }
            qCInfo(lc).noquote() << line;
            ++index;
        }
        if (withIcons) {
            qCInfo(lc).noquote() << QStringLiteral("%1 icons cached")
                                        .arg(icons.cachedCount());
        }
        return 0;
    }

    if (parser.isSet(renderProbeOption)) {
#if defined(_WIN32)
        auto* host = new starfish::platform::win::DesktopHost(&app);
        auto* fence = new starfish::ui::FenceView;
        fence->setTitle(QStringLiteral("StarFish (render probe)"));

        const bool hosted = !parser.isSet(noParentOption);
        const bool opaque = parser.isSet(opaqueOption);
        const bool useSurface = parser.isSet(surfaceOption);
        const bool pixelAlpha = parser.isSet(pixelAlphaOption) && useSurface;
        const QRect geometry(80, 80, 480, 320);

        // The raw surface path renders the widget offscreen: the widget is never
        // shown, only DesktopSurface's Win32 window is.
        auto* surface = useSurface ? new starfish::platform::win::DesktopSurface(&app) : nullptr;
        if (useSurface)
            fence->resize(geometry.size());

        // T-005: fill the fence with the real desktop items and bind the icon
        // source, so the probe verifies the icon grid on the actual desktop. The
        // widget is rendered offscreen into the frame pushed to the surface.
        starfish::platform::win::ShellIconCache probeIcons;
        if (parser.isSet(withDesktopItemsOption)) {
            fence->setIconProvider(
                [&probeIcons](const starfish::core::DesktopItem& item, int sizePx) {
                    return probeIcons.iconFor(item.path, sizePx);
                });
            QString scanErr;
            QList<starfish::core::DesktopItem> items =
                starfish::platform::win::DesktopScanner::scan(&scanErr);
            if (!scanErr.isEmpty())
                qCWarning(lc).noquote() << scanErr;
            qCInfo(lc).noquote() << QStringLiteral("T-005: populated fence with %1 desktop items")
                                        .arg(items.size());
            fence->setItems(std::move(items));

            if (surface) {
                surface->setInputTarget(fence);
                // Live re-render whenever selection changes so the desktop shows it.
                QObject::connect(fence, &starfish::ui::FenceView::contentChanged, &app,
                                 [surface, fence, pixelAlpha] {
                                     QImage img(fence->size(), QImage::Format_ARGB32_Premultiplied);
                                     // T-017: in per-pixel mode the frame starts translucent so
                                     // the wallpaper shows through the panel.
                                     img.fill(pixelAlpha ? QColor(0x1F, 0x29, 0x37, 96)
                                                         : QColor(0x1F, 0x29, 0x37));
                                     {
                                         QPainter painter(&img);
                                         if (pixelAlpha)
                                             painter.setOpacity(0.85);
                                         fence->render(&painter);
                                     }
                                     surface->setFrame(img);
                                 });
            }
        }

        PaintCounter counter;
        fence->installEventFilter(&counter);

        // Show the desktop *first*: pressing Win+D after the window is up would
        // minimise a top-level control window and make the capture meaningless.
        if (parser.isSet(showDesktopOption))
            pressShowDesktop();

        // Everything below is deferred so the Win+D animation has finished
        // before the reference frame is taken. A baseline grabbed mid-animation
        // still shows the windows being minimised, which makes every later diff
        // meaningless (measured: 3.5 M "changed" pixels for a 480x320 window).
        QTimer::singleShot(1200, &app, [&] {
            // Reference frame: the desktop *without* our window. Without it a
            // before/after diff proves nothing -- the window may already be in
            // the "before" capture, in which case an unchanged screen is
            // expected and says nothing about rendering.
            const QImage baseline = grabPrimaryScreen();
            baseline.save(QStringLiteral("build/render_probe_baseline.png"));
            qCInfo(lc).noquote()
                << QStringLiteral(
                       "baseline capture %1x%2, desktop pixel at the fence centre: 0x%3")
                       .arg(baseline.width())
                       .arg(baseline.height())
                       .arg(QString::number(
                           baseline.isNull() ? 0 : baseline.pixel(geometry.center()), 16));

            if (surface) {
                if (!surface->attach(geometry)) {
                    qCCritical(lc) << "could not host the desktop surface";
                    QCoreApplication::exit(1);
                    return;
                }
                if (pixelAlpha) {
                    const bool alphaOk = surface->setPerPixelAlpha(true);
                    qCInfo(lc).noquote()
                        << QStringLiteral("T-017: per-pixel alpha (UpdateLayeredWindow) %1")
                               .arg(alphaOk ? QStringLiteral("ENABLED")
                                            : QStringLiteral("REFUSED -- opaque fallback"));
                }
            } else if (hosted && parser.isSet(layeredManualOption)) {
                // Plain opaque Qt window first, then our own hosting sequence.
                fence->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
                fence->setAttribute(Qt::WA_NativeWindow, true);
                fence->setGeometry(geometry);
                fence->show();
                QString hostingDetail;
                const bool hosted2 = hostManually(reinterpret_cast<HWND>(fence->winId()),
                                                  geometry, &hostingDetail);
                qCInfo(lc).noquote()
                    << QStringLiteral("manual hosting (Qt left opaque): %1 -- %2")
                           .arg(hosted2 ? QStringLiteral("ok") : QStringLiteral("failed"))
                           .arg(hostingDetail);
            } else if (hosted) {
                if (!host->attach(fence, geometry)) {
                    qCCritical(lc) << "could not host the fence on the desktop";
                    QCoreApplication::exit(1);
                    return;
                }
                host->syncZOrder();
            } else {
                // Control run: same widget, same rect, but a plain top-level
                // window, so the desktop hosting is the only variable left.
                fence->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
                fence->setAttribute(Qt::WA_NativeWindow, true);
                if (!opaque)
                    fence->setWindowOpacity(0.99);
                fence->setGeometry(geometry);
                fence->show();
            }
            qCInfo(lc).noquote()
                << QStringLiteral("probe mode: %1, Qt opacity %2")
                       .arg(surface      ? QStringLiteral("raw DesktopSurface (Qt offscreen)")
                            : hosted     ? QStringLiteral("hosted on the desktop")
                                         : QStringLiteral("plain top-level"))
                       .arg(opaque ? QStringLiteral("1.0") : QStringLiteral("0.99"));

            const HWND hwnd = surface ? reinterpret_cast<HWND>(surface->hwnd())
                                      : reinterpret_cast<HWND>(fence->winId());
            const QRect probeRect = geometry;

            // 1. Let Qt settle, then record what it believes about the window.
            //    `baseline` is captured *by value*: this lambda runs after the
            //    enclosing scope has returned, so a reference would dangle and
            //    silently turn every diff into "nothing changed".
            QTimer::singleShot(1500, &app, [&, baseline, hwnd, probeRect] {
            const QWindow* window = fence->windowHandle();
            qCInfo(lc).noquote()
                << QStringLiteral("qt state: visible=%1 mapped=%2 exposed=%3 size=%4x%5 "
                                  "paintEvents=%6")
                       .arg(fence->isVisible())
                       .arg(fence->testAttribute(Qt::WA_Mapped))
                       .arg(window && window->isExposed())
                       .arg(fence->width())
                       .arg(fence->height())
                       .arg(counter.count);

            // How the window is layered decides whether GDI drawing is honoured
            // at all: SetLayeredWindowAttributes mode accepts it, an
            // UpdateLayeredWindow-mode window ignores every GDI call.
            COLORREF key = 0;
            BYTE alpha = 0;
            DWORD flags = 0;
            const BOOL hasLwa = ::GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags);
            qCInfo(lc).noquote()
                << QStringLiteral("win32 state: style 0x%1 ex-style 0x%2 parent 0x%3 "
                                  "layeredAttributes=%4 flags=0x%5 alpha=%6")
                       .arg(static_cast<qulonglong>(::GetWindowLongPtrW(hwnd, GWL_STYLE)), 0, 16)
                       .arg(static_cast<qulonglong>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), 0, 16)
                       .arg(static_cast<qulonglong>(
                                reinterpret_cast<quintptr>(::GetParent(hwnd))),
                            0, 16)
                       .arg(hasLwa ? 1 : 0)
                       .arg(flags, 0, 16)
                       .arg(alpha);

            RECT windowRect{};
            ::GetWindowRect(hwnd, &windowRect);
            qCInfo(lc).noquote()
                << QStringLiteral("win32 rect %1,%2 %3x%4 visible=%5 iconic=%6 cloaked=%7")
                       .arg(windowRect.left)
                       .arg(windowRect.top)
                       .arg(windowRect.right - windowRect.left)
                       .arg(windowRect.bottom - windowRect.top)
                       .arg(::IsWindowVisible(hwnd) ? 1 : 0)
                       .arg(::IsIconic(hwnd) ? 1 : 0)
                       .arg(isDwmCloaked(hwnd) ? 1 : 0);
            starfish::platform::win::DesktopHost::dumpDesktopHierarchy();

            const QImage shown = grabPrimaryScreen();
            shown.save(QStringLiteral("build/render_probe_shown.png"));
            qint64 visiblePixels = 0;
            const QRect visibleBounds = diffBounds(baseline, shown, &visiblePixels);
            qCInfo(lc).noquote()
                << QStringLiteral("after showing the window: %1 px differ from the baseline, "
                                  "bbox %2")
                       .arg(visiblePixels)
                       .arg(visibleBounds.isNull()
                                ? QStringLiteral("none -- the window is NOT on screen")
                                : QStringLiteral("%1,%2 %3x%4")
                                      .arg(visibleBounds.x())
                                      .arg(visibleBounds.y())
                                      .arg(visibleBounds.width())
                                      .arg(visibleBounds.height()));

            const QImage before = shown;

            // Qt-rendered content, blitted by us with plain GDI.
            QImage offscreen(fence->size(), QImage::Format_ARGB32_Premultiplied);
            if (pixelAlpha) {
                // T-017: translucent base + translucent fence, so the wallpaper
                // bleeding through is visible in the captures.
                offscreen.fill(QColor(0x1F, 0x29, 0x37, 96));
                QPainter painter(&offscreen);
                painter.setOpacity(0.85);
                fence->render(&painter);
            } else {
                offscreen.fill(QColor(0xFF, 0x00, 0xFF)); // magenta corners prove our blit landed
                QPainter painter(&offscreen);
                fence->render(&painter);
            }
            offscreen.save(QStringLiteral("build/render_probe_offscreen.png"));

            QString detail;
            if (surface) {
                surface->setFrame(offscreen);
                ::UpdateWindow(hwnd);
                detail = QStringLiteral("frame pushed (%1x%2), surface paint: %3")
                             .arg(offscreen.width())
                             .arg(offscreen.height())
                             .arg(surface->paintReport());
                qCInfo(lc).noquote() << QStringLiteral("DesktopSurface: %1").arg(detail);
            } else {
                const bool blitted = parser.isSet(paintCycleOption)
                                         ? blitViaPaintCycle(hwnd, offscreen, &detail)
                                         : blitImageToHwnd(hwnd, offscreen, &detail);
                qCInfo(lc).noquote()
                    << QStringLiteral("manual GDI blit into the hosted HWND (%1): %2 -- %3")
                           .arg(parser.isSet(paintCycleOption) ? QStringLiteral("from WM_PAINT")
                                                               : QStringLiteral("between paints"))
                           .arg(blitted ? QStringLiteral("ok") : QStringLiteral("failed"))
                           .arg(detail);
            }

            // 2. What the window itself now holds, independent of the screen.
            const QImage readBack = printWindowImage(hwnd, &detail);
            qCInfo(lc).noquote() << QStringLiteral("hosted HWND read-back: %1").arg(detail);
            if (!readBack.isNull()) {
                readBack.save(QStringLiteral("build/render_probe_readback.png"));
                const QRgb centre = readBack.pixel(readBack.width() / 2, readBack.height() / 2);
                qCInfo(lc).noquote() << QStringLiteral("read-back centre pixel: 0x%1")
                                            .arg(QString::number(centre, 16));
            }

            // 3. Did the manual blit reach the screen?
            QTimer::singleShot(250, &app, [&app, probeRect, before, baseline] {
                const QImage after = grabPrimaryScreen();
                after.save(QStringLiteral("build/render_probe_blitted.png"));
                qint64 changed = 0;
                const QRect bounds = diffBounds(before, after, &changed);
                if (bounds.isNull()) {
                    qCWarning(lc) << "the manual GDI blit changed nothing on screen";
                } else {
                    const qint64 rectArea =
                        static_cast<qint64>(probeRect.width()) * probeRect.height();
                    qCInfo(lc).noquote()
                        << QStringLiteral("the manual GDI blit changed the screen: bbox %1,%2 "
                                          "%3x%4, %5 px (%6% of the fence rect)")
                               .arg(bounds.x())
                               .arg(bounds.y())
                               .arg(bounds.width())
                               .arg(bounds.height())
                               .arg(changed)
                               .arg(100.0 * static_cast<double>(changed) / rectArea, 0, 'f', 1);
                }
                qint64 fromBaseline = 0;
                const QRect baselineBounds = diffBounds(baseline, after, &fromBaseline);
                qCInfo(lc).noquote()
                    << QStringLiteral("fence rect on screen: %1,%2 %3x%4 -- %5 px differ from "
                                      "the empty desktop (bbox %6)")
                           .arg(probeRect.x())
                           .arg(probeRect.y())
                           .arg(probeRect.width())
                           .arg(probeRect.height())
                           .arg(fromBaseline)
                           .arg(baselineBounds.isNull()
                                    ? QStringLiteral("none")
                                    : QStringLiteral("%1,%2 %3x%4")
                                          .arg(baselineBounds.x())
                                          .arg(baselineBounds.y())
                                          .arg(baselineBounds.width())
                                          .arg(baselineBounds.height()));

                // 4. Qt repainting over our blit, or not, is a signal in itself.
                QTimer::singleShot(1200, &app, [&app, after] {
                    const QImage later = grabPrimaryScreen();
                    later.save(QStringLiteral("build/render_probe_later.png"));
                    qint64 changed = 0;
                    const QRect bounds = diffBounds(after, later, &changed);
                    qCInfo(lc).noquote()
                        << QStringLiteral("second capture, 1.2 s later: %1 px changed, bbox %2")
                               .arg(changed)
                               .arg(bounds.isNull()
                                        ? QStringLiteral("none")
                                        : QStringLiteral("%1,%2 %3x%4")
                                              .arg(bounds.x())
                                              .arg(bounds.y())
                                              .arg(bounds.width())
                                              .arg(bounds.height()));
                    QCoreApplication::quit();
                });
            });
        });
        });

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                         [&parser, showDesktopOption] {
                             if (parser.isSet(showDesktopOption))
                                 pressShowDesktop();
                         });
        return app.exec();
#else
        qCWarning(lc) << "--render-probe is Windows-only";
        return 1;
#endif
    }

    if (parser.isSet(demoOption)) {
        bool parsed = false;
        const int seconds = parser.value(secondsOption).toInt(&parsed);
        const int durationMs = (parsed && seconds > 0 ? seconds : 10) * 1000;

        auto* host = new starfish::platform::win::DesktopHost(&app);
        auto* fence = new starfish::ui::FenceView;
        fence->setTitle(QStringLiteral("StarFish (Phase 1)"));
        fence->setMinimumSize(160, 120);

        const QRect geometry(80, 80, 480, 320);
        if (!host->attach(fence, geometry)) {
            qCCritical(lc) << "could not host the fence on the desktop";
            delete fence;
            return 1;
        }

        // Qt's own show/resize may have re-stacked the window: re-apply the
        // placement, then report where we ended up.
        host->syncZOrder();

        if (parser.isSet(hideIconsOption))
            host->setIconsHidden(true);

        const auto verdict = host->verifyPlacement(fence);
        qCInfo(lc).noquote() << QStringLiteral("placement verdict: %1")
                                    .arg(QString::fromLatin1(
                                        starfish::core::placementVerdictText(verdict)));
        qCInfo(lc) << "a correct z-order alone proves nothing (ADR-4): confirm visually that "
                      "the panel sits above the wallpaper and under the desktop icons.";

        // A hosted window is a child of Progman and dies with it: re-attach
        // after an Explorer restart, on the next event-loop turn.
        QObject::connect(
            host, &starfish::platform::win::DesktopHost::desktopReacquired, &app,
            [host, fence, geometry] {
                qCWarning(lc) << "desktop re-acquired, re-attaching the fence";
                QTimer::singleShot(0, host, [host, fence, geometry] {
                    if (host->attach(fence, geometry))
                        host->syncZOrder();
                });
            });

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [host, fence] {
            if (host->iconsHidden())
                host->setIconsHidden(false); // never leave the user without icons
            delete fence;
        });

        QTimer::singleShot(durationMs, &app, &QCoreApplication::quit);
        return app.exec();
    }

    qCInfo(lc) << "StarFish" << STARFISH_VERSION
               << "skeleton starting (no desktop integration; use --host-demo)";

    // Keep the skeleton headless-friendly so CI can run it.
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
