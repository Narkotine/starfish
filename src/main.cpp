#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QListWidget>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QRect>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include "app/product_app.hpp"
#include "core/desktop_item.hpp"
#include "core/desktop_watcher.hpp"
#include "core/icon_hiding_marker.hpp"
#include "core/jsonstore.hpp"
#include "core/layout_store.hpp"
#include "core/portal.hpp"
#include "core/rules.hpp"
#include "core/snapshot.hpp"
#include "core/zorder.hpp"
#include "platform/win/desktop_host.hpp"
#include "platform/win/desktop_icons.hpp"
#include "platform/win/desktop_scanner.hpp"
#include "platform/win/desktop_menu.hpp"
#include "platform/win/desktop_surface.hpp"
#include "platform/win/hotkey_manager.hpp"
#include "platform/win/shell_context_menu.hpp"
#include "platform/win/shell_icon_cache.hpp"
#include "ui/backdrop_layout.hpp"
#include "ui/desktop_backdrop.hpp"
#include "ui/fence_container.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/fence_material.hpp"
#include "ui/fence_menu.hpp"
#include "ui/fence_view.hpp"
#include "ui/settings_dialog.hpp"
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

// 0-based position of `target` among Progman's children, top-first -- the
// ordering core::evaluatePlacement() expects. -1 when not found.
int progmanChildIndex(HWND target)
{
    const HWND progman = ::FindWindowW(L"Progman", nullptr);
    if (!progman || !target)
        return -1;
    int index = 0;
    HWND child = nullptr;
    while ((child = ::FindWindowExW(progman, child, nullptr, nullptr)) != nullptr) {
        if (child == target)
            return index;
        ++index;
    }
    return -1;
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

// --- T-014a (F-032): monitors, DPI, and the paint scale ----------------------

// The monitor a fence is **recorded on**, read back from the document. That is
// the value that matters -- the next launch restores from it -- and it is
// derived from the rect, so reading it here also checks that the derivation is
// the one that got persisted.
QString monitorOf(const starfish::ui::FenceContainer& container, const QString& id)
{
    for (const starfish::core::Fence& fence : container.toLayout().fences) {
        if (fence.id == id)
            return fence.monitor;
    }
    return QString();
}

#if defined(_WIN32)
// The DPI awareness context the process really ended up with. Read, never
// assumed: the manifest deliberately declares no `dpiAware` entry (ADR-4), so
// whatever Windows granted -- after Qt's platform plugin asked -- is the fact
// worth reporting. Resolved through GetProcAddress because the SDK guards the
// declaration behind WINVER and the probe must build on any toolchain.
QString dpiAwarenessText()
{
    using ThreadContextFn = void* (WINAPI*)();
    using AwarenessFn = int (WINAPI*)(void*);
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (!user32)
        return QStringLiteral("unavailable (no user32)");
    const auto threadContext = reinterpret_cast<ThreadContextFn>(
        ::GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
    const auto awarenessOf = reinterpret_cast<AwarenessFn>(
        ::GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
    if (!threadContext || !awarenessOf)
        return QStringLiteral("unavailable (older than Windows 10 1607)");
    switch (awarenessOf(threadContext())) {
    case 0:
        return QStringLiteral("DPI_AWARENESS_UNAWARE (0)");
    case 1:
        return QStringLiteral("DPI_AWARENESS_SYSTEM_AWARE (1)");
    case 2:
        return QStringLiteral("DPI_AWARENESS_PER_MONITOR_AWARE (2)");
    case 3:
        return QStringLiteral("DPI_AWARENESS_PER_MONITOR_AWARE_V2 (3)");
    case 4:
        return QStringLiteral("DPI_AWARENESS_UNAWARE_GDISCALED (4)");
    default:
        return QStringLiteral("unrecognised (%1)").arg(awarenessOf(threadContext()));
    }
}
#endif

// How many **physical** pixels thick the fence's 1.0-logical-pixel top border
// covers once Qt has painted it (T-014a).
//
// This is the measurement the DPI work was missing. `FenceContainer::renderFence`
// paints the widget into an image whose devicePixelRatio is the monitor's, which
// is what keeps a fence looking the same size on a 150 % display -- and it was
// only ever *assumed*. The border is drawn with a 1.0 pen, so a painter that is
// scaled by the ratio must cover exactly `dpr` physical pixels along the top
// edge; a painter that ignored the ratio would cover exactly 1, on every
// monitor. Nothing else in the pipeline can tell those two apart.
//
// Coverage is a blue-channel fraction rather than a pixel count, so the
// half-covered pixel of a 1.5 ratio is counted instead of rounded away: the
// panel fill (#1F2937) has blue 55, the border (#3B82F6) blue 246.
double measureBorderCoverage(qreal dpr)
{
    starfish::ui::ScreenLayout layout;
    starfish::ui::ScreenInfo screen;
    screen.geometry = QRect(0, 0, 2560, 1440); // physical pixels, as everywhere
    screen.devicePixelRatio = dpr;
    screen.name = QStringLiteral("\\\\.\\MEASURED");
    screen.primary = true;
    layout.addScreen(screen);

    starfish::ui::FenceContainer container;
    container.setScreens(layout);
    const QString id =
        container.createFence(QStringLiteral("scaled"), QRect(100, 100, 400, 300));
    const QImage frame = container.frame(id);
    if (frame.isNull())
        return -1.0;

    constexpr int fillBlue = 55;    // #1F2937 panel fill
    constexpr int borderBlue = 246; // #3B82F6 panel border
    const int x = frame.width() / 2; // a column clear of the rounded corners
    double coverage = 0.0;
    for (int y = 0; y < frame.height(); ++y) {
        const int blue = frame.pixelColor(x, y).blue();
        const double covered =
            qBound(0.0, double(blue - fillBlue) / double(borderBlue - fillBlue), 1.0);
        if (covered <= 0.0)
            break; // past the border band: everything below is panel fill
        coverage += covered;
    }
    return coverage;
}

#if defined(_WIN32)
// Enumeration state for the "which desktop window is on top at this point"
// report below. EnumChildWindows needs a plain function pointer, so the state
// lives at file scope.
POINT g_zProbePoint{};
QStringList* g_zProbeLines = nullptr;

BOOL CALLBACK zProbeChildProc(HWND child, LPARAM)
{
    if (!g_zProbeLines)
        return FALSE;
    if (g_zProbeLines->size() >= 4)
        return FALSE; // the first few covering windows are the whole story
    RECT r{};
    if (!::GetWindowRect(child, &r))
        return TRUE;
    if (g_zProbePoint.x < r.left || g_zProbePoint.x >= r.right || g_zProbePoint.y < r.top
        || g_zProbePoint.y >= r.bottom)
        return TRUE; // does not cover the point: irrelevant to z-order there
    // A hidden window takes no input at all, so it is not part of the stack a
    // click would be resolved against. Without this the report cannot tell
    // "hidden the icon layer" from "did nothing".
    if (!::IsWindowVisible(child))
        return TRUE;
    wchar_t cls[128] = {0};
    ::GetClassNameW(child, cls, 128);
    const LONG ex = ::GetWindowLongW(child, GWL_EXSTYLE);
    QStringList flags;
    if (ex & WS_EX_LAYERED)
        flags << QStringLiteral("layered");
    if (ex & WS_EX_TRANSPARENT)
        flags << QStringLiteral("transparent");
    if (!(ex & WS_EX_LAYERED) && !(ex & WS_EX_TRANSPARENT))
        flags << QStringLiteral("plain");
    *g_zProbeLines << QStringLiteral("  z: %1  %2x%3  %4")
                          .arg(QString::fromWCharArray(cls))
                          .arg(r.right - r.left)
                          .arg(r.bottom - r.top)
                          .arg(flags.join(QLatin1Char('|')));
    return TRUE;
}

// Progman's children covering `pt`, topmost first (EnumChildWindows yields them
// in z-order). This is the desktop's *own* hierarchy, so a covering application
// window cannot hide the answer: it says whether the icon layer or our surface
// is picked first at that point.
QStringList zProbeAt(HWND progman, const QPoint& pt)
{
    QStringList lines;
    g_zProbePoint = POINT{pt.x(), pt.y()};
    g_zProbeLines = &lines;
    ::EnumChildWindows(progman, &zProbeChildProc, 0);
    g_zProbeLines = nullptr;
    return lines;
}

// "0x1234 Chrome_WidgetWin_1 2560x1440" -- how every report in this project
// spells a window.
QString describeWindow(HWND window)
{
    if (!window)
        return QStringLiteral("none");
    wchar_t cls[128] = {0};
    ::GetClassNameW(window, cls, 127);
    RECT r{};
    ::GetWindowRect(window, &r);
    return QStringLiteral("0x%1 %2 %3x%4")
        .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(window)), 0, 16)
        .arg(QString::fromWCharArray(cls))
        .arg(r.right - r.left)
        .arg(r.bottom - r.top);
}

// Pushes a frame of one premultiplied colour into a per-pixel layered window --
// the same call DesktopSurface makes in that mode. Black at `alpha` in
// premultiplied ARGB is (0, 0, 0, alpha), so the window's *shape* is exactly the
// question being asked.
//
// pptDst is NULL, and that is not laziness: UpdateLayeredWindow *moves* the
// window to pptDst, so passing (0, 0) parks it in the corner of its parent and
// every sample afterwards measures empty desktop. Measured the hard way in
// experiments/spike_layer_alpha_shape.cpp, where two runs agreed on a wrong
// answer because of it.
bool pushLayerFrame(HWND window, int size, int alpha)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size; // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HDC screenDc = ::GetDC(nullptr);
    const HDC memDc = ::CreateCompatibleDC(screenDc);
    const HBITMAP dib = ::CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bits || !dib || !memDc) {
        if (dib)
            ::DeleteObject(dib);
        if (memDc)
            ::DeleteDC(memDc);
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }
    const DWORD pixel = (DWORD(alpha) << 24); // premultiplied black
    for (long i = 0; i < long(size) * long(size); ++i)
        static_cast<DWORD*>(bits)[i] = pixel;
    const HGDIOBJ old = ::SelectObject(memDc, dib);

    SIZE layerSize{size, size};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::SetLastError(0);
    const BOOL ok = ::UpdateLayeredWindow(window, screenDc, nullptr, &layerSize, memDc, &src, 0,
                                          &blend, ULW_ALPHA);
    const DWORD error = ::GetLastError();
    ::SelectObject(memDc, old);
    ::DeleteObject(dib);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);
    if (!ok) {
        qCWarning(lc) << "pushLayerFrame: UpdateLayeredWindow failed, error" << error;
        return false;
    }
    return true;
}

// T-060: what a per-pixel layered window does to the mouse, measured.
//
// WHY NOT ON THE BACKDROP ITSELF. Every child of Progman sits below *every*
// top-level window, so while any application covers the sample point the system
// answers with that application: the 2026-10-16 --backdrop-demo run reported the
// user's browser even with the backdrop's frame at alpha 1. That is a reading of
// the session, not of the code, and no machine-independent check can be built on
// it.
//
// So the same pairing is built out of two windows of ours, above everything: a
// plain parent (Progman is not layered either) with a per-pixel layered child
// inside it, which is exactly the relationship the backdrop has with the
// desktop. WindowFromPoint -- the system's own hit test -- is then asked which
// of the two it would give a click to, once with an alpha-0 frame in the child
// and once with the frame floor.
//
// The measured answer, and the whole point of T-060: at alpha 0 the child is
// **skipped** and its parent answers (so a fully transparent backdrop could
// never receive the empty desktop's gestures), and at the floor the cursor finds
// the child itself.
struct LayerAlphaHit
{
    bool created = false;
    QString error;
    QPoint at;               // where the sample was taken
    QString transparentHit;  // who answers against an alpha-0 frame
    QString flooredHit;      // ...and against the floor
    bool transparentSkipped = false; // the child was not found at alpha 0
    bool flooredFound = false;       // the child was found at the floor
};

LayerAlphaHit measureLayerAlphaHit(const QRect& screen)
{
    LayerAlphaHit hit;

    constexpr int kParentSize = 200;
    constexpr int kChildSize = 100;
    const QPoint at = screen.topLeft() + QPoint(400, 400);
    if (at.x() + kParentSize > screen.right() || at.y() + kParentSize > screen.bottom()) {
        hit.error = QStringLiteral("no room on a %1x%2 screen for a %3 px probe window")
                        .arg(screen.width())
                        .arg(screen.height())
                        .arg(kParentSize);
        return hit;
    }

    static const wchar_t* kParentClass = L"StarFishHitProbeParent";
    static const wchar_t* kChildClass = L"StarFishHitProbeChild";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kParentClass;
        const bool parentClass = ::RegisterClassExW(&wc) != 0;
        wc.lpszClassName = kChildClass;
        const bool childClass = ::RegisterClassExW(&wc) != 0;
        registered = parentClass && childClass;
        if (!registered) {
            hit.error = QStringLiteral("RegisterClassExW failed, error %1").arg(::GetLastError());
            return hit;
        }
    }

    // TOPMOST and never activated: it has to be above every application for the
    // answer to be about this window and nothing else, and it must not steal the
    // user's focus for the few hundred milliseconds it exists.
    const HWND parent = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                          kParentClass, L"", WS_POPUP, at.x(), at.y(),
                                          kParentSize, kParentSize, nullptr, nullptr,
                                          ::GetModuleHandleW(nullptr), nullptr);
    if (!parent) {
        hit.error = QStringLiteral("parent window: CreateWindowExW, error %1").arg(::GetLastError());
        return hit;
    }
    ::ShowWindow(parent, SW_SHOWNOACTIVATE);
    ::SetWindowPos(parent, HWND_TOPMOST, at.x(), at.y(), kParentSize, kParentSize,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::UpdateWindow(parent);

    // Created as a popup with the layered flag already on -- the sequence
    // DesktopSurface uses, and the only one a binary without a manifest gets
    // (ADR-4 finding 4) -- then reparented into the probe parent.
    const HWND child = ::CreateWindowExW(WS_EX_LAYERED, kChildClass, L"", WS_POPUP,
                                         at.x() + 50, at.y() + 50, kChildSize, kChildSize, nullptr,
                                         nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!child || !::SetParent(child, parent)) {
        hit.error = QStringLiteral("child window: error %1").arg(::GetLastError());
        if (child)
            ::DestroyWindow(child);
        ::DestroyWindow(parent);
        return hit;
    }
    const LONG style = static_cast<LONG>(::GetWindowLongW(child, GWL_STYLE));
    ::SetWindowLongW(child, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);
    ::SetWindowPos(child, nullptr, 50, 50, kChildSize, kChildSize, SWP_NOACTIVATE | SWP_NOZORDER);
    ::ShowWindow(child, SW_SHOW);
    ::UpdateWindow(child);
    hit.created = true;

    const auto sample = [child, &hit](int alpha) {
        if (!pushLayerFrame(child, kChildSize, alpha))
            return QString();
        // The sample point is read *after* the push: UpdateLayeredWindow can move
        // the window (see pushLayerFrame), and a point fixed beforehand may be
        // looking next to it.
        RECT rect{};
        ::GetWindowRect(child, &rect);
        hit.at = QPoint((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        ::Sleep(100);
        const HWND under = ::WindowFromPoint(POINT{hit.at.x(), hit.at.y()});
        if (alpha == 0)
            hit.transparentSkipped = under != child;
        else
            hit.flooredFound = under == child;
        return describeWindow(under);
    };
    hit.transparentHit = sample(0);
    hit.flooredHit = sample(starfish::ui::DesktopBackdrop::kInputAlphaFloor);

    ::DestroyWindow(child);
    ::DestroyWindow(parent);
    return hit;
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
    const QCommandLineOption contextMenuVerbsOption(
        QStringLiteral("context-menu-verbs"),
        QStringLiteral("Enumerate the verbs the Windows shell would put in the right-click "
                       "context menu for this path and print them. Non-interactive: proves "
                       "the IContextMenu pipeline (T-006) without showing a popup."),
        QStringLiteral("path"));
    const QCommandLineOption fenceDemoOption(
        QStringLiteral("fence-demo"),
        QStringLiteral("T-007 diagnostic: run a scripted fence lifecycle (create, move with "
                       "snapping, resize with a minimum size, delete) and write a PASS/FAIL "
                       "report to build/fence_demo.txt."));
    const QCommandLineOption hostOption(
        QStringLiteral("host"),
        QStringLiteral("With --fence-demo: also host the fences on the desktop through "
                       "DesktopSurface instead of running headless."));
    const QCommandLineOption layoutOption(
        QStringLiteral("layout"),
        QStringLiteral("Path of the layout document (T-008). Defaults to "
                       "%APPDATA%/StarFish/layout.json; --persist-demo uses "
                       "build/layout_demo.json unless this is given."),
        QStringLiteral("file"));
    const QCommandLineOption persistDemoOption(
        QStringLiteral("persist-demo"),
        QStringLiteral("T-008 diagnostic: run the persistence round trip (build fences, "
                       "auto-save, drop the container, reload from the file, compare) and "
                       "write a PASS/FAIL report to build/persist_demo.txt."));
    const QCommandLineOption navDemoOption(
        QStringLiteral("nav-demo"),
        QStringLiteral("T-009 diagnostic: exercise roll-up (F-020), quick hide (F-022) and "
                       "Peek (F-021) through the real wiring and write a PASS/FAIL report to "
                       "build/nav_demo.txt."));
    const QCommandLineOption rulesDemoOption(
        QStringLiteral("rules-demo"),
        QStringLiteral("T-010 diagnostic: run the auto-organisation rules (F-017) and the "
                       "desktop folder watcher that feeds them (F-018) -- including a file "
                       "really created in a watched folder -- and write a PASS/FAIL report to "
                       "build/rules_demo.txt."));
    const QCommandLineOption watchOption(
        QStringLiteral("watch"),
        QStringLiteral("With --rules-demo: the folder to watch and drop a demo file into. "
                       "Defaults to build/rules_demo_watch. The files it creates are removed "
                       "again when the probe ends."),
        QStringLiteral("dir"));
    const QCommandLineOption portalDemoOption(
        QStringLiteral("portal-demo"),
        QStringLiteral("T-011 diagnostic: mirror a folder into a fence (F-024) -- the fence "
                       "shows the folder's contents, the document stores the folder path, the "
                       "mirror follows the folder -- and write a PASS/FAIL report to "
                       "build/portal_demo.txt."));
    const QCommandLineOption portalFolderOption(
        QStringLiteral("portal-folder"),
        QStringLiteral("With --portal-demo: the folder to mirror. Defaults to "
                       "build/portal_demo_folder. The files it creates inside are removed "
                       "again when the probe ends; the folder itself is never emptied."),
        QStringLiteral("dir"));

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
    parser.addOption(contextMenuVerbsOption);
    parser.addOption(fenceDemoOption);
    parser.addOption(hostOption);
    parser.addOption(layoutOption);
    parser.addOption(persistDemoOption);
    parser.addOption(navDemoOption);
    parser.addOption(rulesDemoOption);
    parser.addOption(watchOption);
    parser.addOption(portalDemoOption);
    parser.addOption(portalFolderOption);
    const QCommandLineOption snapshotDemoOption(
       QLatin1String("snapshot-demo"),
       QLatin1String("T-012 diagnostic: run the snapshot save/restore and export/import "
                       "round trips (F-027/F-028) through the real wiring and write a "
                       "PASS/FAIL report to build/snapshot_demo.txt."));
    const QCommandLineOption snapshotIdOption(
       QLatin1String("snapshot-id"),
       QLatin1String("With --snapshot-demo: the id to save under. Defaults to "
                       "\"snapshot-demo\"."),
       QLatin1String("id"));
    const QCommandLineOption snapshotNoteOption(
       QLatin1String("snapshot-note"),
       QLatin1String("With --snapshot-demo: the note attached to the snapshot."),
       QLatin1String("note"));
    parser.addOption(snapshotDemoOption);
    parser.addOption(snapshotIdOption);
    parser.addOption(snapshotNoteOption);
    const QCommandLineOption productOption(
        QLatin1String("product"),
        QLatin1String("Run the application: fences on the desktop, tray icon, settings "
                      "window, hot keys. This is also what happens when no diagnostic "
                      "option is given, so a double click lands here."));
    const QCommandLineOption productProbeOption(
        QLatin1String("product-probe"),
        QLatin1String("T-038 diagnostic: exercise the product sequence (document, settings "
                      "window, first-launch fence, rename, roll-up, delete, auto-save) "
                      "against a throwaway document, without hosting, tray or hot keys, and "
                      "write a PASS/FAIL report to build/product_probe.txt."));
    const QCommandLineOption iconsDemoOption(
        QLatin1String("icons-demo"),
        QLatin1String("T-066 diagnostic: run the product on a throwaway document whose fence "
                      "holds one of the desktop's *real* icons, then measure from outside that "
                      "the desktop's icons are still the shell's -- visible, readable and "
                      "clickable -- during the launch and after the exit. Needs a real desktop "
                      "and visible icons; report in build/icons_demo.txt."));
    const QCommandLineOption fenceMenuProbeOption(
        QLatin1String("fence-menu-probe"),
        QLatin1String("T-076 diagnostic: does the fence's own menu reach the *screen*? "
                      "Hosts one fence on the real desktop, opens the real menu with a real "
                      "right-click on its title bar, samples the screen while it is up, "
                      "dismisses it and samples again. Needs a real desktop (nothing is "
                      "hidden, and the document it writes to is thrown away); report in "
                      "build/fence_menu_probe.txt."));
    parser.addOption(fenceMenuProbeOption);
    const QCommandLineOption installMenuOption(
        QLatin1String("install-desktop-menu"),
        QLatin1String("T-071: add StarFish's entry to the desktop's *own* context menu "
                      "(HKCU\\Software\\Classes\\DesktopBackground\\Shell, per user, no "
                      "administrator needed), then quit."));
    const QCommandLineOption uninstallMenuOption(
        QLatin1String("uninstall-desktop-menu"),
        QLatin1String("T-071: remove that entry and quit."));
    const QCommandLineOption newFenceOption(
        QLatin1String("new-fence"),
        QLatin1String("T-071: what the desktop menu entry runs. Asks a running StarFish for a "
                      "fence (first free spot), or starts one."));
    const QCommandLineOption claimDesktopOption(
        QStringLiteral("claim-desktop"),
        QStringLiteral("Take the desktop at launch (T-056), which is now opt-in: the native "
                       "icon layer is hidden and StarFish draws the desktop itself. ADR-13 "
                       "made this the exception -- the product leaves the user's icons "
                       "alone -- so it is here for the development path and for the probes "
                       "that still measure the old behaviour."));
    const QCommandLineOption noHostOption(
        QLatin1String("no-host"),
        QLatin1String("With the product: do not put the fences on the desktop."));
    const QCommandLineOption noTrayOption(
        QLatin1String("no-tray"), QLatin1String("With the product: no tray icon."));
    const QCommandLineOption noHotkeysOption(
        QLatin1String("no-hotkeys"), QLatin1String("With the product: no global hot keys."));
    const QCommandLineOption noWatchOption(
        QLatin1String("no-watch"),
        QLatin1String("With the product: do not watch the desktop folder (F-018)."));
    const QCommandLineOption noSettingsOption(
        QLatin1String("no-settings"),
        QLatin1String("With the product: do not create the settings window."));
    parser.addOption(productOption);
    const QCommandLineOption screenDemoOption(
        QLatin1String("screen-demo"),
        QLatin1String("T-014a diagnostic: report the monitors Windows really reports and the "
                      "DPI awareness context the process obtained, exercise synthetic "
                      "arrangements (a monitor left of the primary, mixed DPI, one unplugged) "
                      "through a real FenceContainer, and measure that Qt scales the paint to "
                      "the monitor's ratio. Writes a PASS/FAIL report to build/screen_demo.txt."));
    parser.addOption(productProbeOption);
    parser.addOption(iconsDemoOption);
    parser.addOption(screenDemoOption);
    const QCommandLineOption backdropDemoOption(
        QLatin1String("backdrop-demo"),
        QLatin1String("T-039 diagnostic: the desktop layer StarFish draws itself (ADR-9 option "
                      "A). Checks that the icons no fence holds are computed -- never stored, "
                      "so the document does not move -- that the layout is column-major like "
                      "the Windows desktop, and that the rendered frame really loses an icon "
                      "when a fence claims it. With --host it also hosts the real full-screen "
                      "window and reads back its z-order, its layered flag and its per-pixel "
                      "mode. Writes a PASS/FAIL report to build/backdrop_demo.txt."));
    parser.addOption(backdropDemoOption);
    const QCommandLineOption crashDemoOption(
        QLatin1String("crash-demo"),
        QLatin1String("T-046 diagnostic: hide the native desktop icons through the product's "
                      "own path, kill the process that did it, and check that the next launch "
                      "gives the icons back on its own (the safety net around ADR-9). Needs a "
                      "real desktop and hides the icons for a moment. Writes a PASS/FAIL "
                      "report to build/crash_demo.txt."));
    const QCommandLineOption crashVictimOption(
        QLatin1String("crash-victim"),
        QLatin1String("Internal, the child half of --crash-demo: hide the native icons, then "
                      "wait to be killed. Nothing here restores anything -- that is the "
                      "point."));
    parser.addOption(crashDemoOption);
    parser.addOption(crashVictimOption);
    parser.addOption(installMenuOption);
    parser.addOption(uninstallMenuOption);
    parser.addOption(newFenceOption);
    parser.addOption(claimDesktopOption);
    parser.addOption(noHostOption);
    parser.addOption(noTrayOption);
    parser.addOption(noHotkeysOption);
    parser.addOption(noWatchOption);
    parser.addOption(noSettingsOption);
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

    if (parser.isSet(contextMenuVerbsOption)) {
#if defined(_WIN32)
        const QString path = parser.value(contextMenuVerbsOption);
        if (path.isEmpty()) {
            qCWarning(lc) << "--context-menu-verbs needs a path";
            return 2;
        }
        const QStringList verbs =
            starfish::platform::win::ShellContextMenu::menuVerbs(path);
        // Mirror the result to a file: this is a diagnostic, and on Windows a
        // GUI-subsystem exe's stderr is not reliably captured by every caller
        // (PowerShell `*> file`, some sandboxes). A file is the one channel
        // that always works, and it makes the diagnostic scriptable.
        const QString mirrorPath = QStringLiteral("build/context_menu_verbs.txt");
        QFile mirror(mirrorPath);
        if (mirror.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream out(&mirror);
            out << path << '\n' << verbs.size() << '\n';
            for (const QString& verb : verbs)
                out << verb << '\n';
        }
        if (verbs.isEmpty()) {
            qCWarning(lc).noquote()
                << QStringLiteral("no verbs resolved for %1 (path missing or not a file?)")
                       .arg(path);
            return 3;
        }
        qCInfo(lc).noquote()
            << QStringLiteral("%1 context menu verb(s) for %2:")
                       .arg(verbs.size())
                       .arg(path);
        for (const QString& verb : verbs)
            qCInfo(lc).noquote() << QStringLiteral("  - %1").arg(verb);
        return 0;
#else
        qCWarning(lc) << "--context-menu-verbs is Windows-only";
        return 1;
#endif
    }

    if (parser.isSet(fenceDemoOption)) {
        // T-007: a scripted, non-interactive walk through the fence lifecycle.
        // The geometry rules are unit-tested (tests/test_fence_geometry.cpp);
        // what this probe adds is the wiring -- FenceView turning a title-bar or
        // border gesture into a container call -- and the same code path being
        // hosted on the real desktop when --host is passed.
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize bounds = screen ? screen->geometry().size() : QSize(1920, 1080);
        const bool hosted = parser.isSet(hostOption);

        starfish::platform::win::ShellIconCache demoIcons;
        auto* container = new starfish::ui::FenceContainer(&app);
        container->setDesktopBounds(bounds);
        container->setHostOnDesktop(hosted);
        container->setSnapThreshold(8);
        container->setMinimumFenceSize(QSize(160, 120));
        container->setIconProvider([&demoIcons](const starfish::core::DesktopItem& item,
                                               int sizePx) {
            return demoIcons.iconFor(item.path, sizePx);
        });

        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -34)
                       .arg(detail);
        };

        // Drives the widget the way the desktop does: a press on the title bar
        // (or a border grip), a move past it, a release. FenceView does the
        // hit-testing; the container applies the delta and the snapping.
        auto dragWidget = [](QWidget* widget, const QPoint& from, const QPoint& delta) {
            const QPoint to = from + delta;
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(from),
                              QPointF(widget->mapToGlobal(from)), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(widget, &press);
            QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(widget->mapToGlobal(to)),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(widget, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to),
                                QPointF(widget->mapToGlobal(to)), Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QCoreApplication::sendEvent(widget, &release);
        };

        // The same gesture, delivered the way the desktop actually delivers it:
        // as a *sequence* of moves, each reporting the cursor relative to where
        // the fence is **right now** -- because with the mouse captured the
        // window follows the cursor and Windows reports client coordinates.
        // A single dispatched move cannot see that feedback loop, which is
        // exactly why a widget-local drag reference survived every probe until
        // session 26 while being wrong on the real desktop (the user's report:
        // "ça le fait mais pas très très bien, c'est un peu bizarre").
        auto dragTitleBarInSteps = [&container](const QString& id, const QPoint& step, int steps) {
            starfish::ui::FenceView* view = container->view(id);
            if (!view)
                return;
            // 160 px along the title bar, in screen pixels: inside the bar and
            // clear of the resize grips.
            const QPoint grab = container->fenceRect(id).topLeft() + QPoint(160, 15);
            // What Windows reports for a captured drag: client coordinates.
            const auto local = [&container, &id](const QPoint& screen) {
                return QPointF(screen - container->fenceRect(id).topLeft());
            };
            QMouseEvent press(QEvent::MouseButtonPress, local(grab), QPointF(grab),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &press);
            QPoint cursor = grab;
            for (int i = 0; i < steps; ++i) {
                cursor += step;
                QMouseEvent move(QEvent::MouseMove, local(cursor), QPointF(cursor),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(view, &move);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, local(cursor), QPointF(cursor),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &release);
        };

        log << QStringLiteral("desktop bounds %1x%2, snap threshold %3 px, hosting %4")
                   .arg(bounds.width())
                   .arg(bounds.height())
                   .arg(container->snapThreshold())
                   .arg(hosted ? QStringLiteral("yes") : QStringLiteral("no"));

        const QString idA = container->createFence(QStringLiteral("A"), QRect(80, 80, 320, 240));
        const QString idB = container->createFence(QStringLiteral("B"), QRect(520, 80, 320, 240));
        check(QStringLiteral("create two fences"), container->fenceCount() == 2,
              QStringLiteral("count=%1 ids=%2").arg(container->fenceCount()).arg(
                  container->fenceIds().join(QLatin1Char(','))));

        // A sits at x=80 w=320, so its right edge is 400. Dropping B's left edge
        // at 394 (6 px away) must snap it onto 400.
        const QRect snapped = container->setFenceRect(idB, QRect(394, 400, 320, 240));
        check(QStringLiteral("move snaps to another fence"), snapped.x() == 400,
              QStringLiteral("x=%1 (expected 400 = A.right)").arg(snapped.x()));

        // 3 px from the top-left corner: both axes snap to 0.
        const QRect corner = container->setFenceRect(idB, QRect(3, 5, 320, 240));
        check(QStringLiteral("move snaps to the screen edges"),
              corner.topLeft() == QPoint(0, 0),
              QStringLiteral("topLeft=%1,%2 (expected 0,0)").arg(corner.x()).arg(corner.y()));

        // Asking for 50x40 must stop at the 160x120 minimum, not invert.
        const QRect clamped = container->setFenceRect(idB, QRect(0, 0, 50, 40));
        check(QStringLiteral("resize clamps to the minimum"),
              clamped.size() == QSize(160, 120),
              QStringLiteral("size=%1x%2 (minimum 160x120)")
                  .arg(clamped.width())
                  .arg(clamped.height()));

        // Title-bar drag on A through the real widget: (-77,+37) targets (3,117),
        // which snaps to x=0 and to y=120 -- the bottom edge of B.
        if (starfish::ui::FenceView* viewA = container->view(idA))
            dragWidget(viewA, QPoint(160, 15), QPoint(-77, 37));
        const QRect movedA = container->fenceRect(idA);
        check(QStringLiteral("title-bar drag moves the fence"),
              movedA == QRect(0, 120, 320, 240),
              QStringLiteral("rect=%1,%2 %3x%4 (expected 0,120 320x240)")
                  .arg(movedA.x())
                  .arg(movedA.y())
                  .arg(movedA.width())
                  .arg(movedA.height()));

        // Bottom-right grip of A, dragged by (+60,+60).
        if (starfish::ui::FenceView* viewA = container->view(idA))
            dragWidget(viewA, QPoint(319, 239), QPoint(60, 60));
        const QRect resizedA = container->fenceRect(idA);
        check(QStringLiteral("border grip resizes the fence"),
              resizedA.size() == QSize(380, 300),
              QStringLiteral("size=%1x%2 (expected 380x300)")
                  .arg(resizedA.width())
                  .arg(resizedA.height()));

        const bool removed = container->removeFence(idA);
        check(QStringLiteral("delete a fence"),
              removed && container->fenceCount() == 1 && !container->contains(idA),
              QStringLiteral("removed=%1 count=%2").arg(removed ? 1 : 0).arg(
                  container->fenceCount()));
        check(QStringLiteral("deleting an unknown fence fails"),
              !container->removeFence(QStringLiteral("fence-999")),
              QStringLiteral("unknown id rejected"));

        // T-039: the tiles belong to the fence, not to the screen. A frame is
        // the zone's *own* rectangle, so rendering the same zone before and
        // after a move must produce byte-identical images -- that is what
        // "moving a fence carries its icons" means. The rect is asserted to
        // have changed too, or the check would pass on a fence that never
        // moved.
        //
        // Since T-077 a move does not re-render at all (the frame is the fence's
        // own rect, so nothing about it could change), which makes the equality
        // here hold by construction -- it stays as the definition of what the
        // tiles travelling with the fence means. The sharper claim, "one push and
        // the same buffer, not a rebuild per mouse move", is measured on the
        // hosted path at the end of this probe.
        const QList<starfish::core::DesktopItem> tiles{
            starfish::core::itemFromPathOnly(QStringLiteral("C:/tiles/one.txt")),
            starfish::core::itemFromPathOnly(QStringLiteral("C:/tiles/two.txt")),
            starfish::core::itemFromPathOnly(QStringLiteral("C:/tiles/three.txt")),
        };
        const QString idC =
            container->createFence(QStringLiteral("with tiles"), QRect(80, 600, 320, 240), tiles);
        const QImage frameBeforeMove = container->frame(idC);
        // The target is derived from the desktop rather than written down: the
        // probe used to hard-code a rect that only fits a screen taller than
        // 1140 px, so it went red on a 1920x1080 desktop for no reason at all
        // (measured: same two failures on the unmodified revision). What the
        // check is about is the *frame*, not the coordinates.
        const QSize desktop = container->desktopBounds();
        const QRect targetC(desktop.width() - 400, desktop.height() - 300, 320, 240);
        const QRect movedC = container->setFenceRect(idC, targetC, /*snap=*/false);
        const QImage frameAfterMove = container->frame(idC);
        check(QStringLiteral("moving a fence carries its tiles"),
              movedC == targetC && !frameBeforeMove.isNull() && frameBeforeMove == frameAfterMove,
              QStringLiteral("%1 tile(s), rect %2,%3 (asked %4,%5), frame %6x%7 identical after "
                             "the move: %8")
                  .arg(tiles.size())
                  .arg(movedC.x())
                  .arg(movedC.y())
                  .arg(targetC.x())
                  .arg(targetC.y())
                  .arg(frameAfterMove.width())
                  .arg(frameAfterMove.height())
                  .arg(frameAfterMove == frameBeforeMove ? QStringLiteral("yes") : QStringLiteral("no")));

        // And the same gesture over several moves, which is where a drag whose
        // reference moves with the window shows up: the fence must land exactly
        // where the cursor went, not halfway there.
        const QRect beforeSteps = container->fenceRect(idC);
        dragTitleBarInSteps(idC, QPoint(5, 4), 12);
        const QRect afterSteps = container->fenceRect(idC);
        const QPoint expectedTopLeft = beforeSteps.topLeft() + QPoint(5 * 12, 4 * 12);
        check(QStringLiteral("a multi-step drag lands where the cursor went"),
              afterSteps.topLeft() == expectedTopLeft,
              QStringLiteral("%1,%2 + 12 moves of (5,4) -> %3,%4 (expected %5,%6)")
                  .arg(beforeSteps.x())
                  .arg(beforeSteps.y())
                  .arg(afterSteps.x())
                  .arg(afterSteps.y())
                  .arg(expectedTopLeft.x())
                  .arg(expectedTopLeft.y()));

        // T-045: the icon grid must be painted at the *content* origin. Its cells
        // are content-local (the space hitTest already uses), and the painter was
        // never moved into the content area -- so the grid is drawn
        // `kTitleBarHeight + kPadding` (42 px) too high, and the clip at the top
        // of the content area then cuts the first row in half. Reported from the
        // real desktop with a screenshot: "l'icône est à moitié cachée par la
        // barre de titre" -- the cut in that image is at y = panelTop + 42.
        //
        // Colour-agnostic on purpose: the same fence is rendered with and without
        // its tiles, and only the difference between the two is inspected. And
        // the range matters: the *content* top, not the title bar, is where the
        // wrong pixels actually land -- a check bounded by the clip would pass
        // with the bug in place (it did, once: the range tested was the title
        // bar, which the clip empties whatever the painter does).
        const starfish::ui::FenceView* tiledView = container->view(idC);
        const QRect content = tiledView ? tiledView->contentRect() : QRect();
        const QString idBare = container->createFence(QStringLiteral("with tiles"),
                                                      container->fenceRect(idC));
        const QImage bareFrame = container->frame(idBare);
        const QImage tiledFrame = container->frame(idC);
        int firstDifferingRow = -1;
        int aboveContent = 0;
        if (bareFrame.size() == tiledFrame.size()) {
            for (int y = 0; y < tiledFrame.height(); ++y) {
                for (int x = 0; x < tiledFrame.width(); ++x) {
                    if (bareFrame.pixel(x, y) == tiledFrame.pixel(x, y))
                        continue;
                    if (firstDifferingRow < 0)
                        firstDifferingRow = y;
                    if (y < content.top())
                        ++aboveContent;
                }
            }
        }
        // The first tile must start *inside* the content area (a clipped top row
        // would land exactly on the content top) and near its origin (a second
        // translation would push it far below).
        const int offsetFromContent = firstDifferingRow - content.top();
        check(QStringLiteral("the tiles are painted from the content origin down"),
              !content.isEmpty() && firstDifferingRow > content.top() && offsetFromContent < 24
                  && aboveContent == 0,
              QStringLiteral("first painted row %1, content top %2 (offset %3), %4 pixel(s) "
                             "above the content")
                  .arg(firstDifferingRow)
                  .arg(content.top())
                  .arg(offsetFromContent)
                  .arg(aboveContent));
        container->removeFence(idBare);

        // -------------------------------------------------------------------
        // T-049 -- a caption is no longer cut to the width of its icon.
        //
        // Reported with a screenshot: the label band was exactly as wide as the
        // icon square, and the text was elided in the *middle* -- "Epic Games
        // Launcher" was drawn as "Epi...her". The geometry of the fix is unit
        // tested (tests/test_icon_grid.cpp: the cell is wider than the icon, a
        // long name wraps onto two lines, a too-long one is elided on the right);
        // what is measured *here* is that the pixels follow, because the frame is
        // what the desktop actually shows.
        //
        // The tiles are the probe's own, so nothing depends on the names that
        // happen to be on the user's desktop. The glyphs are read against the
        // *panel colour sampled from the frame itself* -- not a colour written
        // down here, and not a second render: an empty fence draws its own "no
        // icons yet" hint in the content area, and a difference against one would
        // have measured that hint instead of the caption. It did, on the first
        // try (124 stray pixels), which is why the reference is the panel.
        // -------------------------------------------------------------------
        {
            const QRect captionRect = container->fenceRect(idC);
            const QString idLongCaption = container->createFence(
                QStringLiteral("long caption"), captionRect,
                {starfish::core::itemFromPathOnly(
                    QStringLiteral("C:/tiles/Epic Games Launcher Deluxe Extra"))});
            const QString idShortCaption = container->createFence(
                QStringLiteral("short caption"), captionRect,
                {starfish::core::itemFromPathOnly(QStringLiteral("C:/tiles/note.txt"))});
            const QImage longFrame = container->frame(idLongCaption);
            const QImage shortFrame = container->frame(idShortCaption);
            const starfish::ui::FenceView* captionView = container->view(idLongCaption);
            const starfish::ui::FenceView* shortCaptionView = container->view(idShortCaption);
            const QRect captionContent = captionView ? captionView->contentRect() : QRect();

            // Content-local geometry of the single cell: padding 12, icon 48, and
            // the cell width read from the view itself -- since T-078 it is no
            // longer a formula to copy, it is the room the fence's longest caption
            // asked for (72 here, unless the name needs more).
            const int captionIconSize = 48;
            const int captionCellW =
                captionView ? captionView->cellWidth()
                            : starfish::ui::IconLayout::cellWidthFor(captionIconSize);
            const int captionCellFloor = starfish::ui::IconLayout::cellWidthFor(captionIconSize);
            const int captionCellCeiling =
                starfish::ui::IconLayout::maxCellWidthFor(captionIconSize);
            const int captionIconLeft = 12 + (captionCellW - captionIconSize) / 2;
            const QRect captionBand(12, 12 + captionIconSize, captionCellW, 34);

            // The frame is the fence's own rect at the screen's ratio.
            const double frameScale = captionRect.width() > 0 && !longFrame.isNull()
                                          ? double(longFrame.width()) / captionRect.width()
                                          : 1.0;
            const auto framePoint = [frameScale, &captionContent](const QPoint& local) {
                return QPoint(int((captionContent.left() + local.x()) * frameScale),
                              int((captionContent.top() + local.y()) * frameScale));
            };
            // The panel, read where no cell can be: the bottom-right corner of the
            // content area, on a fence that holds exactly one tile.
            const QPoint panelAt =
                framePoint(QPoint(captionContent.width() - 4, captionContent.height() - 4));
            const bool measurable = !longFrame.isNull() && !shortFrame.isNull()
                                    && !captionContent.isEmpty()
                                    && longFrame.size() == shortFrame.size()
                                    && panelAt.x() >= 0 && panelAt.y() >= 0
                                    && panelAt.x() < longFrame.width()
                                    && panelAt.y() < longFrame.height();
            const QRgb panel = measurable ? longFrame.pixel(panelAt) : 0;

            struct BandInk {
                bool measured = false;
                int firstRow = -1; // first row of the band that holds painted pixels
                int lastRow = -1;  // ...and the last one
                int beyondIcon = 0; // pixels right of the icon square
                int beyondCell = 0; // pixels right of the whole cell
            };
            const auto bandInk = [&](const QImage& frame) {
                BandInk ink;
                if (!measurable)
                    return ink;
                ink.measured = true;
                for (int ly = captionBand.top(); ly < captionBand.bottom(); ++ly) {
                    for (int lx = 0; lx < captionContent.width(); ++lx) {
                        const QPoint at = framePoint(QPoint(lx, ly));
                        if (at.x() >= frame.width() || at.y() >= frame.height())
                            continue;
                        // A glyph is anything clearly off the panel. The caption is
                        // light grey on a dark panel, so even an anti-aliased edge
                        // is nowhere near it, and the threshold only has to clear
                        // the panel's own noise (there is none: it is one fill).
                        const QRgb here = frame.pixel(at);
                        const int distance = qAbs(qRed(here) - qRed(panel))
                                             + qAbs(qGreen(here) - qGreen(panel))
                                             + qAbs(qBlue(here) - qBlue(panel));
                        if (distance < 24)
                            continue;
                        if (ink.firstRow < 0)
                            ink.firstRow = ly - captionBand.top();
                        ink.lastRow = ly - captionBand.top();
                        if (lx >= captionIconLeft + captionIconSize)
                            ++ink.beyondIcon;
                        if (lx >= 12 + captionCellW)
                            ++ink.beyondCell;
                    }
                }
                return ink;
            };

            const BandInk longInk = bandInk(longFrame);
            const BandInk shortInk = bandInk(shortFrame);

            check(QStringLiteral("a caption is drawn wider than its icon"),
                  longInk.measured && longInk.beyondIcon > 0,
                  QStringLiteral("%1 glyph pixel(s) right of the icon (cell %2, icon %3 -- the "
                                 "old cell was the icon, so this was impossible)")
                      .arg(longInk.beyondIcon)
                      .arg(captionCellW)
                      .arg(captionIconSize));
            check(QStringLiteral("a caption stays inside its own cell"),
                  longInk.measured && longInk.beyondIcon > 0 && longInk.beyondCell == 0,
                  QStringLiteral("%1 pixel(s) past the icon, %2 past the cell (x >= %3)")
                      .arg(longInk.beyondIcon)
                      .arg(longInk.beyondCell)
                      .arg(12 + captionCellW));
            // A long caption is two lines of text, so its ink reaches deeper into
            // the band than a caption that fits on one -- measured against the
            // short one rather than against a threshold in pixels, which keeps the
            // check honest whatever the font is.
            check(QStringLiteral("a long caption takes a second line"),
                  longInk.firstRow >= 0 && shortInk.firstRow >= 0
                      && longInk.lastRow > shortInk.lastRow + 4,
                  QStringLiteral("long caption: rows %1..%2 of %3 ; short caption: rows %4..%5 "
                                 "(the second line must reach deeper)")
                      .arg(longInk.firstRow)
                      .arg(longInk.lastRow)
                      .arg(captionBand.height())
                      .arg(shortInk.firstRow)
                      .arg(shortInk.lastRow));

            // T-078 -- the cell itself is what changed. T-049 gave every cell the
            // same 1.5x width, which is right for the average name and still cut
            // the longest ones ("manque encore un peu pour les plus longs"). Now
            // the longest caption of a fence decides, so a fence of short names
            // keeps exactly the cell it had and only the fence that needs the room
            // pays for it (up to twice the icon -- the ceiling is what stops one
            // name from thinning out the whole grid). Both numbers are read from
            // the layout, and the two checks above bound the *pixels* by the same
            // width, so geometry and paint are measured against one another.
            const int longCaptionCell = captionView ? captionView->cellWidth() : -1;
            const int shortCaptionCell = shortCaptionView ? shortCaptionView->cellWidth() : -1;
            check(QStringLiteral("only a long caption widens the cell (T-078)"),
                  longCaptionCell > captionCellFloor && longCaptionCell <= captionCellCeiling
                      && shortCaptionCell == captionCellFloor,
                  QStringLiteral("long caption cell %1, short caption cell %2 (floor %3, "
                                 "ceiling %4)")
                      .arg(longCaptionCell)
                      .arg(shortCaptionCell)
                      .arg(captionCellFloor)
                      .arg(captionCellCeiling));

            container->removeFence(idLongCaption);
            container->removeFence(idShortCaption);
        }

        // -------------------------------------------------------------------
        // T-040 / F-011 -- taking an icon *out* of a fence.
        //
        // There is no restore code to test: an icon is on the desktop exactly
        // when no fence holds it, so the whole change is "the fence stops
        // listing the tile" (T-039's model). What needs proving is the
        // *gesture*: a tile drag released outside the fence hands the tile
        // back, while the same drag released inside still reorders the grid.
        // The release point is asserted to be outside, or the check would pass
        // on a gesture that never left the fence.
        // -------------------------------------------------------------------
        const QList<starfish::core::DesktopItem> desktopWide{
            tiles.at(0), tiles.at(1), tiles.at(2),
            starfish::core::itemFromPathOnly(QStringLiteral("C:/desktop/loose-a.txt")),
            starfish::core::itemFromPathOnly(QStringLiteral("C:/desktop/loose-b.txt")),
        };
        container->setDesktopItems(desktopWide);
        const int freeBefore = container->backdrop() ? container->backdrop()->freeItems().size() : -1;
        const QList<starfish::core::DesktopItem> beforeRelease = container->fenceItems(idC);

        // The gesture as the desktop delivers it: a press on a cell, a move
        // that crosses the drag threshold while still inside the fence, a move
        // that leaves it, and the release out there.
        auto dragTileTo = [](QWidget* widget, const QPoint& from, const QPoint& to) {
            const auto send = [widget](QEvent::Type type, const QPoint& at, Qt::MouseButton button,
                                       Qt::MouseButtons buttons) {
                QMouseEvent event(type, QPointF(at), QPointF(widget->mapToGlobal(at)), button,
                                  buttons, Qt::NoModifier);
                QCoreApplication::sendEvent(widget, &event);
            };
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseMove, from + QPoint(20, 0), Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        };

        // Cell centres of the default grid (icon 48, cell 72, gap 8, padding 12):
        // the hard-coded offsets are made self-verifying by hit-testing them, so
        // a change of metrics fails the premise instead of silently testing
        // nothing.
        starfish::ui::FenceView* viewC = container->view(idC);
        const QPoint firstCell =
            viewC ? viewC->contentRect().topLeft() + QPoint(24, 24) : QPoint();
        // One column step is the *cell* + gap, read from the view rather than
        // copied: since T-078 a fence whose captions are long gets a wider cell,
        // and a hard-coded 72 would then quietly point at the wrong neighbour.
        const int columnPitch =
            (viewC ? viewC->cellWidth() : starfish::ui::IconLayout::cellWidthFor(48)) + 8;
        const QPoint secondCell = firstCell + QPoint(columnPitch, 0);
        check(QStringLiteral("the drag starts on the first tile"),
              viewC && viewC->itemAt(firstCell) == 0 && viewC->itemAt(secondCell) == 1,
              QStringLiteral("cell centres %1,%2 -> index %3 ; %4,%5 -> index %6")
                  .arg(firstCell.x())
                  .arg(firstCell.y())
                  .arg(viewC ? viewC->itemAt(firstCell) : -99)
                  .arg(secondCell.x())
                  .arg(secondCell.y())
                  .arg(viewC ? viewC->itemAt(secondCell) : -99));

        const QString releasedPath =
            beforeRelease.isEmpty() ? QString() : beforeRelease.first().path;
        const QImage frameBeforeRelease = container->frame(idC);
        // Well below the fence: 80 px under its bottom edge, in the desktop.
        const QPoint outside = QPoint(48, 240 + 80);
        const bool releaseWasOutside = viewC && !viewC->rect().contains(outside);
        if (viewC)
            dragTileTo(viewC, firstCell, outside);
        const QList<starfish::core::DesktopItem> afterRelease = container->fenceItems(idC);
        QStringList afterPaths;
        for (const starfish::core::DesktopItem& item : afterRelease)
            afterPaths.append(item.path);
        check(QStringLiteral("a tile released outside the fence goes back to the desktop"),
              releaseWasOutside && !releasedPath.isEmpty() && afterRelease.size() == beforeRelease.size() - 1
                  && !afterPaths.contains(releasedPath),
              QStringLiteral("release %1,%2 outside=%3 ; fence %4 -> %5 tile(s), %6 gone")
                  .arg(outside.x())
                  .arg(outside.y())
                  .arg(releaseWasOutside ? QStringLiteral("yes") : QStringLiteral("no"))
                  .arg(beforeRelease.size())
                  .arg(afterRelease.size())
                  .arg(afterPaths.contains(releasedPath) ? QStringLiteral("still held") : releasedPath));

        // The desktop layer is what makes F-011 free: the icon reappears there
        // because it left the occupied set, not because anything restored it.
        QStringList freePaths;
        if (container->backdrop()) {
            for (const starfish::core::DesktopItem& item : container->backdrop()->freeItems())
                freePaths.append(item.path);
        }
        const int freeAfter = container->backdrop() ? container->backdrop()->freeItems().size() : -1;
        check(QStringLiteral("the released icon is drawn on the desktop again"),
              freeAfter == freeBefore + 1 && freePaths.contains(releasedPath),
              QStringLiteral("free icons %1 -> %2 of %3 ; %4 %5")
                  .arg(freeBefore)
                  .arg(freeAfter)
                  .arg(desktopWide.size())
                  .arg(releasedPath)
                  .arg(freePaths.contains(releasedPath) ? QStringLiteral("is free")
                                                        : QStringLiteral("is NOT free")));

        // ... and the document followed: the fence lists one file less, and it
        // is that one. Nothing else about the fence moved.
        const starfish::core::Layout afterDoc = container->toLayout();
        QStringList docFiles;
        bool docLostFence = true;
        for (const starfish::core::Fence& fence : afterDoc.fences) {
            if (fence.id != idC)
                continue;
            docLostFence = false;
            for (const starfish::core::FenceItem& item : fence.items) {
                if (item.kind == QStringLiteral("file"))
                    docFiles.append(item.path);
            }
        }
        check(QStringLiteral("the document no longer holds the released icon"),
              !docLostFence && docFiles.size() == afterRelease.size()
                  && !docFiles.contains(releasedPath),
              QStringLiteral("%1 file entr(y|ies) in the document, released %2 present: %3")
                  .arg(docFiles.size())
                  .arg(releasedPath)
                  .arg(docFiles.contains(releasedPath) ? QStringLiteral("yes") : QStringLiteral("no")));

        // And the fence itself shows it. A hosted fence displays an *image*
        // (DesktopSurface blits a QImage), so a content change has to rebuild
        // that image -- the widget's own paintEvent never reaches the desktop.
        // Colour-agnostic and no tile is named: the frame simply must not be
        // the one from before the release.
        const QImage frameAfterRelease = container->frame(idC);
        check(QStringLiteral("the fence was redrawn without the released tile"),
              !frameAfterRelease.isNull() && frameAfterRelease != frameBeforeRelease,
              QStringLiteral("frame %1x%2, identical to the one before the release: %3")
                  .arg(frameAfterRelease.width())
                  .arg(frameAfterRelease.height())
                  .arg(frameAfterRelease == frameBeforeRelease ? QStringLiteral("yes")
                                                              : QStringLiteral("no")));

        // The boundary of the gesture: released *inside*, the same drag must
        // still reorder and must not lose a tile.
        const QString reorderFirst = afterPaths.isEmpty() ? QString() : afterPaths.first();
        if (viewC)
            dragTileTo(viewC, firstCell, secondCell);
        const QList<starfish::core::DesktopItem> afterReorder = container->fenceItems(idC);
        check(QStringLiteral("a tile released inside the fence still reorders"),
              afterReorder.size() == afterRelease.size() && !afterReorder.isEmpty()
                  && afterReorder.first().path != reorderFirst,
              QStringLiteral("%1 tile(s) kept, %2 -> %3")
                  .arg(afterReorder.size())
                  .arg(reorderFirst)
                  .arg(afterReorder.isEmpty() ? QStringLiteral("(none)") : afterReorder.first().path));

        // -------------------------------------------------------------------
        // T-057 / F-038 -- the fence materials.
        //
        // A material is the alpha channel of the fence's colour: the document has
        // carried it since T-008 and it could never reach a pixel, because the
        // widget dropped the alpha and the opaque GDI blit ignores whatever is
        // left (docs/technique.md section 22). What is measured here is the whole
        // chain at once -- the preset name against the document, the alpha in the
        // frame that is pushed, and the input alpha floor in the corners that
        // keeps a translucent fence reachable by the mouse (ADR-11, whose
        // corollary is exactly this window). The *look* of it -- "the wallpaper
        // really shows through" -- stays a human check: this machine has one
        // screen and the user's own windows are on top of it.
        // -------------------------------------------------------------------
        const QString idM =
            container->createFence(QStringLiteral("material"), QRect(40, 400, 200, 160));

        // QImage::pixel() returns the *stored* value, which for a premultiplied
        // frame is the premultiplied one: the alpha and the premultiplication
        // invariant are both read from it, and nothing is assumed about either.
        auto frameOf = [&container](const QString& id) { return container->frame(id); };
        auto alphaAt = [&frameOf](const QString& id, const QPoint& at) {
            const QImage frame = frameOf(id);
            if (frame.isNull() || !frame.rect().contains(at))
                return -1;
            return static_cast<int>(qAlpha(frame.pixel(at)));
        };
        // Where the panel is (its centre) and where it is not (the very corner of
        // the rectangle, outside the rounded panel).
        auto panelAlpha = [&frameOf, &alphaAt](const QString& id) {
            const QImage frame = frameOf(id);
            if (frame.isNull())
                return -1;
            return alphaAt(id, QPoint(frame.width() / 2, frame.height() / 2));
        };
        // T-075 measured the *pixels outside the rounded panel*, because the frame
        // was the shape then ("the corners are round but a right angle spills out
        // of them"). Since ADR-15 the frame is a plain rectangle on purpose and the
        // shape is the window's **region** -- so this count is no longer a defect,
        // it is the work the region does, and it is reported rather than asserted.
        // The reference shape is the one FenceView::paintEvent draws: the very
        // same rounded rect, built by the very same toolkit, into an empty image
        // (the pen and the brush are black on purpose -- only the coverage
        // matters). Reconstructing the geometry by hand was tried first and got
        // the arc's centre half a pixel wrong, which is exactly how a measurement
        // turns into an opinion: it reported 216 pixels of "spill" on a frame
        // whose corners are visibly clean.
        const auto panelMask = [](const QImage& frame, int radius) {
            QImage mask(frame.size(), QImage::Format_ARGB32_Premultiplied);
            mask.fill(Qt::transparent);
            const qreal dpr = frame.devicePixelRatio();
            mask.setDevicePixelRatio(dpr);
            QPainter painter(&mask);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(Qt::black, 1.0));
            painter.setBrush(Qt::black);
            painter.drawRoundedRect(QRectF(0, 0, frame.width() / dpr, frame.height() / dpr)
                                        .adjusted(0.5, 0.5, -0.5, -0.5),
                                    radius, radius);
            return mask;
        };
        // Pixels the panel does not cover at all (its antialiased edge is not
        // spill -- it legitimately blends into the wallpaper) and that are
        // nonetheless clearly painted. With a correct fence this is 0: everything
        // outside the panel is the under-layer, at the input alpha floor.
        const auto spillOutsideThePanel = [&panelMask](const QImage& frame, int radius) {
            if (frame.isNull() || radius <= 0)
                return -1;
            const QImage mask = panelMask(frame, radius);
            int spill = 0;
            for (int y = 0; y < frame.height(); ++y) {
                for (int x = 0; x < frame.width(); ++x) {
                    if (qAlpha(mask.pixel(x, y)) != 0)
                        continue;
                    if (qAlpha(frame.pixel(x, y)) > 128)
                        ++spill;
                }
            }
            return spill;
        };

        const int defaultFrameAlpha = panelAlpha(idM);
        const QImage defaultFrame = frameOf(idM);
        const int defaultRadius = container->fenceStyle(idM).radius;
        const int defaultOverflow = spillOutsideThePanel(defaultFrame, defaultRadius);
        check(QStringLiteral("a new fence is shown in the schema's default material"),
              container->fenceMaterial(idM) == QStringLiteral("opaque")
                  && QColor(container->fenceStyle(idM).background).alpha() == 255,
              QStringLiteral("material %1, document alpha %2 of 255 (%3)")
                  .arg(container->fenceMaterial(idM))
                  .arg(QColor(container->fenceStyle(idM).background).alpha())
                  .arg(starfish::ui::FenceMaterial::summary()));

        int transparentPixels = 0;
        int invalidPremultiplied = 0;
        for (int y = 0; y < defaultFrame.height(); ++y) {
            for (int x = 0; x < defaultFrame.width(); ++x) {
                const QRgb pixel = defaultFrame.pixel(x, y);
                const int a = qAlpha(pixel);
                if (a == 0)
                    ++transparentPixels;
                if (qRed(pixel) > a || qGreen(pixel) > a || qBlue(pixel) > a)
                    ++invalidPremultiplied;
            }
        }
        // ⚠️ ADR-15: the frame is opaque, *corner included*, and that is correct
        // now -- the shape is the window's region, so what the blit puts in the
        // corner is never seen. T-075 asserted the opposite here (the input alpha
        // floor at the corner); the check is replaced, not retuned, and the shape
        // is asserted on the region itself in the hosted half below.
        const int defaultCorner = alphaAt(idM, QPoint(0, 0));
        check(QStringLiteral("the frame handed to a window is fully opaque (the alpha is the "
                             "window's)"),
              defaultFrameAlpha == 255 && defaultCorner == 255 && transparentPixels == 0
                  && invalidPremultiplied == 0,
              QStringLiteral("centre alpha %1, corner alpha %2, %3 of %4 pixel(s) at alpha 0, %5 "
                             "pixel(s) with a channel above their own alpha")
                  .arg(defaultFrameAlpha)
                  .arg(defaultCorner)
                  .arg(transparentPixels)
                  .arg(defaultFrame.width() * defaultFrame.height())
                  .arg(invalidPremultiplied));

        log << QStringLiteral("the %1x%2 frame paints %3 pixel(s) outside the rounded panel "
                              "(radius %4): that is exactly what the window's region removes")
                   .arg(defaultFrame.width())
                   .arg(defaultFrame.height())
                   .arg(defaultOverflow)
                   .arg(defaultRadius);

        // A frame that is not valid premultiplied alpha renders as a bright
        // artefact rather than as an almost invisible pixel, and the floor is
        // exactly the case where that can happen (a large RGB under an alpha of 1).
        check(QStringLiteral("the frame is valid premultiplied alpha"),
              invalidPremultiplied == 0,
              QStringLiteral("%1 pixel(s) with a channel above their own alpha")
                  .arg(invalidPremultiplied));

        int materialChanges = 0;
        QObject::connect(container, &starfish::ui::FenceContainer::fenceContentChanged, &app,
                         [&materialChanges](const QString&) { ++materialChanges; });

        container->setFenceMaterial(idM, QStringLiteral("very-translucent"));
        const starfish::core::FenceStyle veryStyle = container->fenceStyle(idM);
        // The material is the *document's* alpha, which the window then wears as its
        // own (ADR-15). The frame's pixels are opaque either way -- that is the
        // whole point of the separation.
        const int veryFrameAlpha = panelAlpha(idM);
        check(QStringLiteral("a material rewrites only the alpha of the colour"),
              container->fenceMaterial(idM) == QStringLiteral("very-translucent")
                  && veryStyle.background == QStringLiteral("#801F2937")
                  && QColor(veryStyle.background).alpha() == 128
                  && veryFrameAlpha == 255,
              QStringLiteral("background %1 (document alpha %2), frame alpha %3")
                  .arg(veryStyle.background)
                  .arg(QColor(veryStyle.background).alpha())
                  .arg(veryFrameAlpha));

        const int beforeRepeat = materialChanges;
        container->setFenceMaterial(idM, QStringLiteral("very-translucent"));
        check(QStringLiteral("choosing the material it already has writes nothing"),
              materialChanges == beforeRepeat,
              QStringLiteral("%1 change signal(s) for a material it already had")
                  .arg(materialChanges - beforeRepeat));

        container->setFenceMaterial(idM, QStringLiteral("acrylic"));
        check(QStringLiteral("an unknown material is refused"),
              container->fenceMaterial(idM) == QStringLiteral("very-translucent"),
              QStringLiteral("material after asking for acrylic: %1 (known: %2)")
                  .arg(container->fenceMaterial(idM),
                       starfish::ui::FenceMaterial::summary()));

        // The material is a document value, not a session one: the bytes the
        // container serialises carry it, and a second container that reads them
        // shows the same thing. This is why the presets need no schema field of
        // their own (and why a document written before T-057 already has one).
        const QByteArray materialDoc = starfish::core::JsonStore::toJson(container->toLayout());
        auto* materialReader = new starfish::ui::FenceContainer(&app);
        materialReader->setDesktopBounds(bounds);
        const auto parsedMaterial = starfish::core::JsonStore::fromJson(materialDoc);
        QString materialApplyError;
        const bool materialApplied = parsedMaterial.has_value()
            && materialReader->applyLayout(*parsedMaterial, &materialApplyError);
        check(QStringLiteral("the material is a document value, not a session one"),
              materialApplied
                  && materialReader->fenceMaterial(idM) == QStringLiteral("very-translucent"),
              QStringLiteral("applied=%1, material after a round trip: %2%3")
                  .arg(materialApplied ? 1 : 0)
                  .arg(materialReader->fenceMaterial(idM))
                  .arg(materialApplyError.isEmpty()
                           ? QString()
                           : QStringLiteral(" (%1)").arg(materialApplyError)));
        delete materialReader;

        // T-074/T-079, the other half: an *opaque* fence, the case the user was
        // looking at. Its material is 255 in the document and on the window, and
        // its rounded panel is the window's region -- the pair that made it visible
        // again. Both clauses that used to live here (`corner alpha 255` in T-057,
        // then the input alpha floor in T-075) were frame-side claims about a shape
        // the frame no longer draws.
        container->setFenceMaterial(idM, QStringLiteral("opaque"));
        const int opaqueFrameAlpha = panelAlpha(idM);
        const QImage opaqueFrame = frameOf(idM);
        const int opaqueOverflow = spillOutsideThePanel(opaqueFrame, container->fenceStyle(idM).radius);
        check(QStringLiteral("an opaque fence is opaque in the document and on the window"),
              container->fenceMaterial(idM) == QStringLiteral("opaque")
                  && QColor(container->fenceStyle(idM).background).alpha() == 255
                  && opaqueFrameAlpha == 255,
              QStringLiteral("document alpha %1, frame alpha %2 at the centre of a %3x%4 frame")
                  .arg(QColor(container->fenceStyle(idM).background).alpha())
                  .arg(opaqueFrameAlpha)
                  .arg(opaqueFrame.width())
                  .arg(opaqueFrame.height()));
        log << QStringLiteral("an opaque fence: %1 pixel(s) of its %2x%3 frame fall outside the "
                              "rounded panel (radius %4); the window's region cuts them")
                   .arg(opaqueOverflow)
                   .arg(opaqueFrame.width())
                   .arg(opaqueFrame.height())
                   .arg(container->fenceStyle(idM).radius);

        if (hosted) {
            QString scanError;
            QList<starfish::core::DesktopItem> items =
                starfish::platform::win::DesktopScanner::scan(&scanError);
            if (!scanError.isEmpty())
                qCWarning(lc).noquote() << scanError;
            const QString idDemo = container->createFence(QStringLiteral("StarFish (T-007 demo)"),
                                                         QRect(420, 260, 360, 260), items);
            log << QStringLiteral("hosted fence %1 with %2 desktop item(s)")
                       .arg(idDemo)
                       .arg(items.size());

#if defined(_WIN32)
            // T-039: can the mouse actually reach a hosted fence? By design the
            // native icon layer sits *above* the fences (ADR-4), so the whole
            // manual workflow (drag a zone, drop a file on it) depends on
            // hit-testing falling through the icon layer's transparent parts to
            // us. WindowFromPoint is the test the mouse messages and the OLE
            // drop target both use -- so it is reported rather than assumed.
            if (starfish::platform::win::DesktopSurface* surface = container->surface(idDemo)) {
                const QRect visual = container->fenceVisibleRect(idDemo);
                const POINT centre{visual.center().x(), visual.center().y()};
                const HWND hit = ::WindowFromPoint(centre);
                wchar_t hitClass[128] = {0};
                if (hit)
                    ::GetClassNameW(hit, hitClass, 128);
                const bool reachesUs = hit && reinterpret_cast<quintptr>(hit) == surface->hwnd();
                log << QStringLiteral("attached=%1 hwnd=0x%2, mouse at %3,%4 reaches %5")
                           .arg(surface->isAttached() ? QStringLiteral("yes") : QStringLiteral("no"))
                           .arg(QString::number(surface->hwnd(), 16))
                           .arg(centre.x)
                           .arg(centre.y)
                           .arg(reachesUs
                                    ? QStringLiteral("the fence surface")
                                    : QStringLiteral("class \"%1\" instead")
                                          .arg(QString::fromWCharArray(hitClass)));

                // Why it does (or does not): the icon layer's own ex-style.
                const HWND defView =
                    ::FindWindowExW(::FindWindowW(L"Progman", nullptr), nullptr,
                                    L"SHELLDLL_DefView", nullptr);
                const LONG defStyle = defView ? ::GetWindowLongW(defView, GWL_EXSTYLE) : 0;
                QStringList flags;
                if (defStyle & WS_EX_LAYERED)
                    flags << QStringLiteral("LAYERED");
                if (defStyle & WS_EX_TRANSPARENT)
                    flags << QStringLiteral("TRANSPARENT (hit testing passes through)");
                log << QStringLiteral("icon layer: %3, ex-style 0x%1 %2")
                           .arg(static_cast<uint>(defStyle), 0, 16)
                           .arg(flags.isEmpty() ? QStringLiteral("(none)") : flags.join(QLatin1Char('|')))
                           .arg(defView ? QStringLiteral("found") : QStringLiteral("absent"));

                // And the desktop's own hierarchy at that point, topmost first.
                const QStringList stack = zProbeAt(::FindWindowW(L"Progman", nullptr), visual.center());
                log << QStringLiteral("desktop z-order at %1,%2 (topmost first):")
                           .arg(visual.center().x())
                           .arg(visual.center().y());
                if (stack.isEmpty())
                    log << QStringLiteral("  nothing covers the point");
                for (const QString& stackLine : stack)
                    log << stackLine;

                // What that stack means since ADR-12: our surface sits above BOTH
                // a layered DefView (the wallpaper) and a plain, full-screen
                // SysListView32 (the icons), so at that point the *fence* takes
                // the click -- with the icons shown or hidden. Hiding the native
                // icons is no longer what makes a fence reachable (it is what
                // keeps an icon from being drawn twice: native and tile); the
                // DefView stays, which is what keeps the user's wallpaper.
                // Measure both states, then put the icons back straight away.
                const bool wasHidden = starfish::platform::win::DesktopSurface::desktopIconsHidden();
                starfish::platform::win::DesktopSurface::setDesktopIconsHidden(true);
                const QStringList hiddenStack =
                    zProbeAt(::FindWindowW(L"Progman", nullptr), visual.center());
                if (!wasHidden)
                    starfish::platform::win::DesktopSurface::setDesktopIconsHidden(false);
                log << QLatin1String("with the native icon layer hidden:");
                for (const QString& stackLine : hiddenStack)
                    log << stackLine;
                check(QStringLiteral("the fence sits above the desktop's own layers, icons "
                                     "shown or not"),
                      !stack.isEmpty()
                          && stack.first().contains(QStringLiteral("StarFishDesktopSurface"))
                          && !hiddenStack.isEmpty()
                          && hiddenStack.first().contains(
                                 QStringLiteral("StarFishDesktopSurface")),
                      QStringLiteral("topmost visible at the point: %1 -> %2")
                          .arg(stack.isEmpty() ? QStringLiteral("(nothing)")
                                               : stack.first().trimmed(),
                               hiddenStack.isEmpty() ? QStringLiteral("(nothing)")
                                                     : hiddenStack.first().trimmed()));
                log << QStringLiteral("native icons restored: %1")
                           .arg(starfish::platform::win::DesktopSurface::desktopIconsHidden()
                                    ? QStringLiteral("NO -- still hidden")
                                    : QStringLiteral("yes"));

                // ⚠️ T-068 -- WindowFromPoint above is asked *at rest*. A click, a
                // drag and an OLE drop are not: Windows asks WM_NCHITTEST **when
                // the left button goes down**, and a fence used to answer
                // HTTRANSPARENT at that very moment. The "the left button belongs
                // to the desktop" clause was written for the backdrop but sits in
                // the WndProc every layer shares, so the fence could not be
                // grabbed, resized, clicked or dropped onto -- measured on the
                // user's desktop: HTCLIENT at rest, HTTRANSPARENT as soon as it
                // was pressed. Both halves are asked here.
                const auto fenceHitTest = [surface](const QPoint& point) {
                    return ::SendMessageW(reinterpret_cast<HWND>(surface->hwnd()), WM_NCHITTEST, 0,
                                          MAKELPARAM(point.x(), point.y()));
                };

                // Half one: the backdrop's own icon list is not a fence's
                // business. Planted deliberately -- the leak is what is being
                // guarded against, and a fence overlapping one of the user's icons
                // has to keep taking its own click (what the user sees there is
                // the fence, not the icon drawn twice underneath it, T-042).
                QVector<QRect> plantedRects;
                for (const starfish::platform::win::NativeDesktopIcon& icon :
                     starfish::platform::win::readNativeDesktopIcons())
                    plantedRects.append(QRect(icon.position, icon.slot));
                surface->setNativeIconRects(plantedRects);
                // Asked *on* the icon, with the fence actually lying over it: a
                // check at the fence's centre would miss the clause entirely (the
                // centre is rarely inside an icon's rectangle), and a check that
                // cannot fail measures nothing.
                const QRect onTheIcon = plantedRects.isEmpty()
                    ? visual
                    : QRect(plantedRects.first().center() - QPoint(visual.width() / 2,
                                                                   visual.height() / 2),
                            visual.size());
                container->setFenceRect(idDemo, onTheIcon, false);
                const auto overIcon = fenceHitTest(plantedRects.isEmpty() ? visual.center()
                                                                         : plantedRects.first().center());
                container->setFenceRect(idDemo, visual, false);
                surface->setNativeIconRects({});
                check(QStringLiteral("a fence steps aside for nothing -- not even the backdrop's "
                                     "icon list"),
                      overIcon == HTCLIENT,
                      QStringLiteral("fence moved onto the user's icon at %1,%2, %3 icon rect(s) "
                                     "planted on it -> %4")
                          .arg(onTheIcon.center().x())
                          .arg(onTheIcon.center().y())
                          .arg(plantedRects.size())
                          .arg(overIcon));

                // Half two, and the one the user feels: the same question with the
                // button really down. Only asked when the window under the point is
                // *this* fence -- with the user's windows in front, the press must
                // not be delivered to somebody else's window, so the clause is
                // logged as not asked rather than failed (the desktop has to be
                // visible to measure it; decision 13).
                const QPoint titlePoint(visual.x() + visual.width() / 2,
                                        visual.y() + starfish::ui::FenceGeometry::kTitleBarHeight / 2);
                const HWND underPoint =
                    ::WindowFromPoint(POINT{titlePoint.x(), titlePoint.y()});
                if (underPoint && reinterpret_cast<quintptr>(underPoint) == surface->hwnd()) {
                    POINT savedCursor{};
                    ::GetCursorPos(&savedCursor);
                    ::SetCursorPos(titlePoint.x(), titlePoint.y());
                    INPUT press[1] = {};
                    press[0].type = INPUT_MOUSE;
                    press[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    ::SendInput(1, press, sizeof(INPUT));
                    const auto answerWithButton = fenceHitTest(titlePoint);
                    INPUT release[1] = {};
                    release[0].type = INPUT_MOUSE;
                    release[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    ::SendInput(1, release, sizeof(INPUT));
                    ::SetCursorPos(savedCursor.x, savedCursor.y);
                    check(QStringLiteral("a fence answers the mouse with the left button down (the "
                                         "click, the drag and the drop all ask it then)"),
                          answerWithButton == HTCLIENT,
                          QStringLiteral("WM_NCHITTEST at the fence's title bar, button held -> %1")
                              .arg(answerWithButton));
                } else {
                    wchar_t covering[128] = {0};
                    if (underPoint)
                        ::GetClassNameW(underPoint, covering, 128);
                    log << QStringLiteral("the button-down clause was NOT asked: \"%1\" covers the "
                                          "fence's title bar -- bring the desktop to the front and "
                                          "re-run to measure it")
                               .arg(QString::fromWCharArray(covering));
                }
            }
#endif

            // ⚠️ ADR-15 (T-079), hosted half. What makes a fence visible is the
            // window's **uniform alpha** and its rounded **region** -- never the
            // per-pixel push, which does not compose on a child of Progman. Read
            // the measurement rather than the belief: six test windows on the
            // user's own desktop, same rect, same process, all layered and all
            // children of Progman; the UpdateLayeredWindow one invisible, the
            // SetLayeredWindowAttributes one beside it visible. The project had
            // measured that the push *succeeds*, never that it *appears* -- and
            // T-075 sent every fence down it, which is the report "je ne vois plus
            // les fences ... même en matériel opaque".
            container->setFenceMaterial(idM, QStringLiteral("very-translucent"));
            auto* translucentSurface = container->surface(idM);
            check(QStringLiteral("a translucent fence carries its material as the window's alpha"),
                  translucentSurface && translucentSurface->uniformAlpha() > 0
                      && translucentSurface->uniformAlpha() < 255,
                  QStringLiteral("fence %1: hosted=%2, window alpha=%3")
                      .arg(idM)
                      .arg(translucentSurface ? 1 : 0)
                      .arg(translucentSurface ? translucentSurface->uniformAlpha() : -1));

            // The icon overlay needs tiles to be about, and the material fence is
            // born empty: it gets two (a path-only item is enough -- what is
            // measured below is a window's shape, not the file behind a tile).
            {
                const QList<starfish::core::DesktopItem> tilesOfTheFence{
                    starfish::core::itemFromPathOnly(QStringLiteral("C:/fence-demo/tile-one.txt")),
                    starfish::core::itemFromPathOnly(QStringLiteral("C:/fence-demo/tile-two.txt")),
                };
                container->view(idM)->setItems(tilesOfTheFence);
            }

            // ⚠️ T-082: and the *icons* are not part of that window. F-038 asks for
            // a translucent **panel**, the user asked for the panel (« je ne pensais
            // pas que changer material s'appliquerait aussi aux icônes »), and one
            // window carries one alpha -- so the tiles are shown by a **second**
            // window, opaque, whose region is the union of the tiles. Read from the
            // windows, never from the document.
            auto* iconOverlay = container->iconOverlay(idM);
            const bool overlayIsASecondWindow =
                iconOverlay && translucentSurface && iconOverlay->hwnd() != translucentSurface->hwnd();
            check(QStringLiteral("a translucent fence shows its icons through a second, opaque "
                                 "window (the panel is what fades)"),
                  overlayIsASecondWindow && iconOverlay->uniformAlpha() == 255
                      && iconOverlay->clickThrough()
                      && translucentSurface->stackedBelow() == iconOverlay,
                  QStringLiteral("fence %1: panel 0x%2 alpha %3 | overlay 0x%4 alpha %5, "
                                 "click-through=%6, stacked above the panel=%7")
                      .arg(idM)
                      .arg(translucentSurface ? static_cast<qulonglong>(translucentSurface->hwnd()) : 0,
                           0, 16)
                      .arg(translucentSurface ? translucentSurface->uniformAlpha() : -1)
                      .arg(iconOverlay ? static_cast<qulonglong>(iconOverlay->hwnd()) : 0, 0, 16)
                      .arg(iconOverlay ? iconOverlay->uniformAlpha() : -1)
                      .arg(iconOverlay && iconOverlay->clickThrough() ? 1 : 0)
                      .arg(translucentSurface && translucentSurface->stackedBelow() == iconOverlay ? 1 : 0));

            // The pair's order as *Windows* holds it -- walking down from the overlay
            // has to reach the panel, which is the same relation place() repairs.
            const auto windowIsAbove = [](quintptr upper, quintptr lower) {
                const HWND up = reinterpret_cast<HWND>(upper);
                const HWND low = reinterpret_cast<HWND>(lower);
                if (!up || !low)
                    return false;
                for (HWND w = ::GetWindow(up, GW_HWNDNEXT); w; w = ::GetWindow(w, GW_HWNDNEXT)) {
                    if (w == low)
                        return true;
                }
                return false;
            };
            const bool overlayIsOnTop = iconOverlay && translucentSurface
                && windowIsAbove(iconOverlay->hwnd(), translucentSurface->hwnd());

            // The shape: only the pixels of a tile are the overlay's, so the panel
            // below is what shows everywhere else -- and the title bar, which belongs
            // to the panel, must not be the overlay's either.
            const QRect overlayRect = container->fenceVisibleRect(idM);
            const QVector<QRect> tiles =
                container->view(idM) ? container->view(idM)->tileRects() : QVector<QRect>();
            HRGN iconShape = ::CreateRectRgn(0, 0, 0, 0);
            const HWND overlayHwnd = iconOverlay ? reinterpret_cast<HWND>(iconOverlay->hwnd()) : nullptr;
            const int iconShapeType = overlayHwnd ? ::GetWindowRgn(overlayHwnd, iconShape) : ERROR;
            int tilesInside = 0;
            for (const QRect& tile : tiles) {
                if (::PtInRegion(iconShape, tile.center().x(), tile.center().y()))
                    ++tilesInside;
            }
            const bool barIsNotTheOverlay =
                ::PtInRegion(iconShape, overlayRect.width() / 2,
                             starfish::ui::FenceGeometry::kTitleBarHeight / 2) == FALSE;
            ::DeleteObject(iconShape);
            check(QStringLiteral("the second window exists exactly where the tiles are, and nowhere "
                                 "else"),
                  iconShapeType != ERROR && !tiles.isEmpty() && tilesInside == tiles.size()
                      && barIsNotTheOverlay && overlayIsOnTop,
                  QStringLiteral("fence %1: %2 tile raster(s), %3 of their centres inside the "
                                 "window's region (type %4), title bar inside=%5, overlay above the "
                                 "panel=%6 [tiles in widget pixels, this desktop is 100 %%]")
                      .arg(idM)
                      .arg(tiles.size())
                      .arg(tilesInside)
                      .arg(iconShapeType)
                      .arg(barIsNotTheOverlay ? 0 : 1)
                      .arg(overlayIsOnTop ? 1 : 0));

            // ...and the click still belongs to the panel: the overlay is
            // click-through on purpose, so the mouse reaches the window that owns the
            // input target *and* the OLE drop target. Only asked when the window under
            // the tile is one of ours -- with somebody else's window in front the
            // answer would be about that one (decision 13: the desktop has to be
            // visible to measure this).
            if (!tiles.isEmpty()) {
                const QPoint tileCentre = tiles.first().center() + overlayRect.topLeft();
                const HWND underTile = ::WindowFromPoint(POINT{tileCentre.x(), tileCentre.y()});
                const bool ours = underTile && translucentSurface
                    && (reinterpret_cast<quintptr>(underTile) == translucentSurface->hwnd()
                        || reinterpret_cast<quintptr>(underTile)
                               == reinterpret_cast<quintptr>(overlayHwnd));
                if (ours) {
                    check(QStringLiteral("a click on a tile still lands on the panel underneath (the "
                                         "overlay is click-through)"),
                          reinterpret_cast<quintptr>(underTile) == translucentSurface->hwnd(),
                          QStringLiteral("WindowFromPoint at the first tile's centre %1,%2 -> 0x%3 "
                                         "(panel 0x%4, overlay 0x%5)")
                              .arg(tileCentre.x())
                              .arg(tileCentre.y())
                              .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(underTile)), 0, 16)
                              .arg(translucentSurface
                                       ? static_cast<qulonglong>(translucentSurface->hwnd())
                                       : 0, 0, 16)
                              .arg(overlayHwnd
                                       ? static_cast<qulonglong>(reinterpret_cast<quintptr>(overlayHwnd))
                                       : 0, 0, 16));
                } else {
                    wchar_t covering[128] = {0};
                    if (underTile)
                        ::GetClassNameW(underTile, covering, 128);
                    log << QStringLiteral("the click-through clause was NOT asked: \"%1\" covers the "
                                          "fence -- bring the desktop to the front and re-run")
                               .arg(QString::fromWCharArray(covering));
                }
            }

            // The option, and the whole of it: `fadeIcons` hands the fence back its
            // single window -- the material on the icons as well, which is exactly
            // what it did before T-082 -- and the document carries the choice.
            container->setFenceIconsFollowMaterial(idM, true);
            const bool goneWhenAsked = container->iconOverlay(idM) == nullptr
                && container->fenceIconsFollowMaterial(idM);
            container->setFenceIconsFollowMaterial(idM, false);
            const bool backWhenUnasked = container->iconOverlay(idM) != nullptr
                && !container->fenceIconsFollowMaterial(idM);
            check(QStringLiteral("asking for the material on the icons drops the second window, and "
                                 "unasking brings it back"),
                  goneWhenAsked && backWhenUnasked,
                  QStringLiteral("fence %1: overlay after asking=%2, after unasking=%3, "
                                 "fadeIcons now=%4")
                      .arg(idM)
                      .arg(goneWhenAsked ? QStringLiteral("none") : QStringLiteral("still there"))
                      .arg(backWhenUnasked ? QStringLiteral("hosted") : QStringLiteral("missing"))
                      .arg(container->fenceIconsFollowMaterial(idM) ? 1 : 0));

            container->setFenceMaterial(idM, QStringLiteral("opaque"));
            auto* opaqueSurface = container->surface(idM);
            check(QStringLiteral("an opaque fence is a fully opaque window (the material *is* the "
                                 "alpha)"),
                  opaqueSurface && opaqueSurface->uniformAlpha() == 255,
                  QStringLiteral("fence %1: hosted=%2, window alpha=%3")
                      .arg(idM)
                      .arg(opaqueSurface ? 1 : 0)
                      .arg(opaqueSurface ? opaqueSurface->uniformAlpha() : -1));

            // The shape, and it is the *region* that has to be right: the blit
            // path has no alpha at all, so a window without a region would show a
            // square panel. The region is read back from Windows, and the two
            // points that matter are the rounded corner and the panel's middle.
            const QRect shapedRect = container->fenceVisibleRect(idM);
            HRGN shape = ::CreateRectRgn(0, 0, 0, 0);
            const HWND shapedHwnd = opaqueSurface ? reinterpret_cast<HWND>(opaqueSurface->hwnd())
                                                  : nullptr;
            const int shapeType = shapedHwnd ? ::GetWindowRgn(shapedHwnd, shape) : ERROR;
            const bool cornerIsNotTheWindows = ::PtInRegion(shape, 1, 1) == FALSE;
            const bool middleIsTheWindows =
                ::PtInRegion(shape, shapedRect.width() / 2, shapedRect.height() / 2) != FALSE;
            check(QStringLiteral("the rounded panel is the window's region, so the corners are not "
                                 "the window's pixels"),
                  shapeType != ERROR && cornerIsNotTheWindows && middleIsTheWindows,
                  QStringLiteral("fence %1 %2x%3: region type %4, point 1,1 inside=%5, centre "
                                 "inside=%6")
                      .arg(idM)
                      .arg(shapedRect.width())
                      .arg(shapedRect.height())
                      .arg(shapeType)
                      .arg(cornerIsNotTheWindows ? 0 : 1)
                      .arg(middleIsTheWindows ? 1 : 0));
            ::DeleteObject(shape);

            // ...and the frame the window was handed is *opaque*: alpha belongs to
            // the window now, and a frame carrying its own alpha would be a lie the
            // blit cannot honour. This is the check that used to encode T-075 (it
            // demanded an alpha-1 under-layer); it is replaced, not retuned.
            const QImage pushedFrame = opaqueSurface ? opaqueSurface->frame() : QImage();
            const int pushedCorner = pushedFrame.isNull()
                                         ? -1
                                         : int(qAlpha(pushedFrame.pixel(0, 0)));
            check(QStringLiteral("the frame handed to a hosted fence is opaque (the window carries "
                                 "the alpha)"),
                  pushedCorner == 255,
                  QStringLiteral("hosted fence %1: corner alpha %2 of the %3x%4 frame it was "
                                 "given")
                      .arg(idM)
                      .arg(pushedCorner)
                      .arg(pushedFrame.width())
                      .arg(pushedFrame.height()));

            // And the creation path, which is a different thing from the switch: a
            // fence restored from a document is *born* with its material's alpha
            // and its rounded region. Read from the window, not from the document
            // -- the document was never the question (T-057's lesson).
            starfish::core::Layout bornLayout;
            starfish::core::Fence born;
            born.id = QStringLiteral("fence-born");
            born.title = QStringLiteral("born translucent");
            born.rect = QRect(600, 420, 200, 160);
            born.style.background = QStringLiteral("#801F2937");
            bornLayout.fences.append(born);
            // ...and the *default* material, since T-074 the opaque preset: it must
            // be born fully opaque, with its shape on the window (ADR-15).
            starfish::core::Fence bornOpaque;
            bornOpaque.id = QStringLiteral("fence-born-opaque");
            bornOpaque.title = QStringLiteral("born opaque");
            bornOpaque.rect = QRect(600, 600, 200, 160);
            bornLayout.fences.append(bornOpaque);
            auto* bornHost = new starfish::ui::FenceContainer(&app);
            bornHost->setDesktopBounds(bounds);
            bornHost->setHostOnDesktop(true);
            QString bornError;
            const bool bornApplied = bornHost->applyLayout(bornLayout, &bornError);
            auto* bornSurface = bornHost->surface(born.id);
            auto* bornOpaqueSurface = bornHost->surface(bornOpaque.id);
            check(QStringLiteral("a fence restored from a translucent document is born with that "
                                 "alpha"),
                  bornApplied && bornSurface && bornSurface->uniformAlpha() == 128,
                  QStringLiteral("applied=%1, hosted=%2, window alpha=%3%4")
                      .arg(bornApplied ? 1 : 0)
                      .arg(bornSurface ? 1 : 0)
                      .arg(bornSurface ? bornSurface->uniformAlpha() : -1)
                      .arg(bornError.isEmpty() ? QString()
                                               : QStringLiteral(" (%1)").arg(bornError)));
            check(QStringLiteral("a fence restored from an opaque document is born opaque, and its "
                                 "panel is a region"),
                  bornApplied && bornOpaqueSurface && bornOpaqueSurface->uniformAlpha() == 255
                      && bornOpaqueSurface->cornerRadius() > 0
                      && bornHost->fenceMaterial(bornOpaque.id) == QStringLiteral("opaque"),
                  QStringLiteral("applied=%1, material=%2, hosted=%3, window alpha=%4, corner "
                                 "radius=%5")
                      .arg(bornApplied ? 1 : 0)
                      .arg(bornHost->fenceMaterial(bornOpaque.id))
                      .arg(bornOpaqueSurface ? 1 : 0)
                      .arg(bornOpaqueSurface ? bornOpaqueSurface->uniformAlpha() : -1)
                      .arg(bornOpaqueSurface ? bornOpaqueSurface->cornerRadius() : -1));
            delete bornHost;

            // ---------------------------------------------------------------
            // T-050 / T-077 -- the surface's work when nothing changed.
            //
            // Two user reports from the same session, and they are the same
            // question asked from two sides: "there are still flickerings, maybe
            // at regular intervals, of all the fences" -- the 2 s watchdog was
            // re-placing and re-pushing every fence whether anything had changed or
            // not, and a regular interval is a cadence, not a hiccup -- and "while
            // the cursor drags, the fence itself looks unstable, although the final
            // result is right", which is a resize pushing the *previous* frame at
            // the new rect: the push sets the window's extent, so the fence went to
            // the new position with its old size, once per mouse move, and the
            // fresh frame put it right again in the same message.
            //
            // Both only exist on the hosted path (no surface, no watchdog and no
            // layered push in headless mode), and both are *cadences*, so the
            // measurement has to let the event loop run -- nested ones, the same
            // instrument --persist-demo waits with.
            auto waitMs = [](int ms) {
                QEventLoop loop;
                QTimer::singleShot(ms, &loop, &QEventLoop::quit);
                loop.exec();
            };
            if (starfish::platform::win::DesktopSurface* watchful = container->surface(idM)) {
                // 1. At rest. Three watchdog periods, and both halves of the answer
                //    are required: nothing was pushed, *and* the watchdog really did
                //    ask. Without the second half this check would pass just as well
                //    on a watchdog that had stopped -- the shape of mistake this
                //    project keeps making.
                // Let the paints already queued land before the baseline: the
                // material switch just above rendered a frame, and a pending
                // WM_PAINT would otherwise be counted as work done *at rest*.
                waitMs(250);
                const int pushesBefore = watchful->layeredPushes();
                const int paintsBefore = watchful->paintCount();
                const int askedBefore = watchful->currentPlacements();
                waitMs(3 * 2000 + 500);
                const int pushesAtRest = watchful->layeredPushes() - pushesBefore;
                const int paintsAtRest = watchful->paintCount() - paintsBefore;
                const int askedAtRest = watchful->currentPlacements() - askedBefore;
                // Both halves are required, and on the blit path (ADR-15) the half
                // that matters is the paint: a fence that pushes nothing but repaints
                // itself every two seconds would flicker exactly as before.
                check(QStringLiteral("a fence at rest is asked about, and neither re-placed nor "
                                     "re-blitted (T-050)"),
                      pushesAtRest == 0 && paintsAtRest == 0 && askedAtRest >= 3
                          && watchful->mismatchedFramePushes() == 0,
                      QStringLiteral("%1 layered push(es) and %2 WM_PAINT(s) in 6.5 s at rest, %3 "
                                     "placement(s) found already current, %4 size-mismatched "
                                     "push(es) ever")
                          .arg(pushesAtRest)
                          .arg(paintsAtRest)
                          .arg(askedAtRest)
                          .arg(watchful->mismatchedFramePushes()));

                // 2. ...and the watchdog is still a watchdog. The premise of the
                //    fix: what it is *for* is repairing a placement somebody else
                //    disturbed, and a check that only watched it do nothing would be
                //    green for a broken repair too. The disturbance is applied from
                //    the outside (SetWindowPos on the hosted window from this
                //    process, which is what another program moving it would be).
                const QRect wanted = container->fenceRect(idM);
                const HWND ourWindow = reinterpret_cast<HWND>(watchful->hwnd());
                ::SetWindowPos(ourWindow, nullptr, wanted.x() + 40, wanted.y() + 40,
                               wanted.width(), wanted.height(), SWP_NOZORDER | SWP_NOACTIVATE);
                waitMs(2000 + 600);
                POINT hostOrigin{0, 0};
                const HWND host = ::FindWindowW(L"Progman", nullptr);
                if (host)
                    ::ClientToScreen(host, &hostOrigin);
                RECT repaired{};
                ::GetWindowRect(ourWindow, &repaired);
                const QRect back(repaired.left - hostOrigin.x, repaired.top - hostOrigin.y,
                                 repaired.right - repaired.left, repaired.bottom - repaired.top);
                check(QStringLiteral("the watchdog still repairs a fence moved behind its back"),
                      back == wanted,
                      QStringLiteral("moved +40,+40 from outside, one watchdog period later: %1,%2 %3x%4 "
                                     "(wanted %5,%6 %7x%8)")
                          .arg(back.x()).arg(back.y()).arg(back.width()).arg(back.height())
                          .arg(wanted.x()).arg(wanted.y()).arg(wanted.width()).arg(wanted.height()));

                // The other clause of the guard is the z-order one: "the wallpaper
                // is below me". ⚠️ It cannot be disturbed through the fence --
                // measurement, not reasoning: SetWindowPos(fence, HWND_BOTTOM) is
                // rewritten to HWND_TOP by the fence's *own* WM_WINDOWPOSCHANGING
                // guard (ADR-4, the same guard that stops anything stacking a
                // surface above the icon layer), so the fence never leaves the top.
                // The only way in is the wallpaper's own window, which is what a
                // second desktop tool would move. The relation walked here is the
                // one place() uses: walking down from our window has to reach the
                // DefView.
                const auto aboveTheWallpaper = [](HWND window, HWND defView) {
                    if (!window || !defView)
                        return false;
                    for (HWND w = ::GetWindow(window, GW_HWNDNEXT); w; w = ::GetWindow(w, GW_HWNDNEXT)) {
                        if (w == defView)
                            return true;
                    }
                    return false;
                };
                const HWND defView = host ? ::FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr)
                                          : nullptr;
                if (defView) {
                    ::SetWindowPos(defView, HWND_TOP, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    const bool buriedOutside = !aboveTheWallpaper(ourWindow, defView);
                    waitMs(2000 + 600);
                    const bool buriedAfter = !aboveTheWallpaper(ourWindow, defView);
                    if (buriedOutside) {
                        check(QStringLiteral("the watchdog still lifts a fence the wallpaper was "
                                             "stacked over"),
                              !buriedAfter,
                              QStringLiteral("the DefView put above the fence from outside; one "
                                             "watchdog period later the wallpaper is still above it: "
                                             "%1")
                                  .arg(buriedAfter ? QStringLiteral("yes") : QStringLiteral("no")));
                    } else {
                        log << QStringLiteral("the z-order clause was NOT asked: the shell put the "
                                              "DefView back above the fence on its own, so the "
                                              "disturbed state does not last long enough to measure");
                    }
                } else {
                    log << QStringLiteral("the z-order clause was NOT asked: no SHELLDLL_DefView "
                                          "found under Progman");
                }
            }

            if (starfish::platform::win::DesktopSurface* resizing = container->surface(idM)) {
                // 3. A resize gesture, step by step, driven through the *widget*:
                //    a press on the bottom-right grip and a move per step, each
                //    reporting the cursor relative to where the fence is right now
                //    (what Windows reports under capture, and the shape of gesture
                //    that caught T-043). FenceView turns it into
                //    geometryDragDelta, and the container applies it -- the same
                //    path the mouse takes, rather than a call to the container.
                //
                //    Each step used to push twice -- the previous frame at the new
                //    rect (a frame of the wrong size, which is also what resized the
                //    window back) and then the right one. The claim now is one push
                //    per step whose rect changed, no mismatched push at all, and the
                //    stale frame left to the render that fits -- the skip is counted
                //    because "did not need to" and "forgot to" look identical from
                //    outside.
                const int rendersBefore = container->frameRenderCount();
                const int mismatchedBefore = resizing->mismatchedFramePushes();
                auto resizeInSteps = [&container](const QString& id, const QPoint& step, int count) {
                    auto* view = container->view(id);
                    if (!view)
                        return 0;
                    // Three pixels inside the corner: within the 6 px grip band, and
                    // on the corner, which wins over the edges in hitTest.
                    const QPoint grab = container->fenceRect(id).bottomRight() - QPoint(2, 2);
                    const auto local = [&container, &id](const QPoint& screen) {
                        return QPointF(screen - container->fenceRect(id).topLeft());
                    };
                    QMouseEvent press(QEvent::MouseButtonPress, local(grab), QPointF(grab),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(view, &press);
                    int changed = 0;
                    QPoint cursor = grab;
                    QRect previous = container->fenceRect(id);
                    for (int i = 0; i < count; ++i) {
                        cursor += step;
                        QMouseEvent move(QEvent::MouseMove, local(cursor), QPointF(cursor),
                                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(view, &move);
                        const QRect now = container->fenceRect(id);
                        if (now != previous)
                            ++changed;
                        previous = now;
                    }
                    QMouseEvent release(QEvent::MouseButtonRelease, local(cursor), QPointF(cursor),
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(view, &release);
                    return changed;
                };
                // The steps grow the fence towards the bottom right, so the grip
                // stays inside the desktop: each move is 6 px further than the last.
                const QSize desktopSize = container->desktopBounds();
                const QRect beforeGesture = container->fenceRect(idM);
                const int room = qMin(desktopSize.width() - beforeGesture.right(),
                                      desktopSize.height() - beforeGesture.bottom());
                const int steps = resizeInSteps(idM, QPoint(6, 6), qMax(1, qMin(12, room / 6 - 1)));
                const int renders = container->frameRenderCount() - rendersBefore;
                const int mismatched = resizing->mismatchedFramePushes() - mismatchedBefore;
                // The claim is unchanged since T-077 -- one image per changed size,
                // none of the wrong size -- but it is measured where the work is now:
                // the *render*, since a fence no longer pushes anything (ADR-15). The
                // frame's size is checked too, because that is what used to be wrong.
                check(QStringLiteral("a resize re-renders once per changed size, and nothing of the "
                                     "wrong size (T-077)"),
                      steps > 0 && renders == steps && mismatched == 0
                          && container->frame(idM).size() == container->fenceVisibleRect(idM).size(),
                      QStringLiteral("%1 step(s) with a changed rect -> %2 render(s), %3 "
                                     "size-mismatched push(es), frame %4x%5 for a %6x%7 panel")
                          .arg(steps)
                          .arg(renders)
                          .arg(mismatched)
                          .arg(container->frame(idM).width())
                          .arg(container->frame(idM).height())
                          .arg(container->fenceVisibleRect(idM).width())
                          .arg(container->fenceVisibleRect(idM).height()));

                // 4. And what a *move* should do: re-push the image it already has,
                //    not rebuild it. The frame is the fence's own rectangle, so a
                //    move cannot change a single byte of it -- and rebuilding it
                //    re-laid out the icon grid and repainted every tile ~60 times a
                //    second under the cursor. "Not rebuilt" is measured as the same
                //    buffer, not as equal bytes: an identical redraw would be equal
                //    too, and that is exactly the work being removed.
                const QImage beforeMove = container->frame(idM);
                const int rendersBeforeMove = container->frameRenderCount();
                const QRect from = container->fenceRect(idM);
                const QRect movedTo =
                    container->setFenceRect(idM, from.translated(-24, -18), /*snap=*/false);
                const QImage afterMove = container->frame(idM);
                const int rendersForTheMove = container->frameRenderCount() - rendersBeforeMove;
                check(QStringLiteral("moving a fence shows the image it already has instead of "
                                     "redrawing it"),
                      movedTo != from && !beforeMove.isNull() && beforeMove == afterMove
                          && beforeMove.constBits() == afterMove.constBits()
                          && rendersForTheMove == 0,
                      QStringLiteral("moved %1,%2 -> %3,%4: %5 re-render(s), frame %6x%7 %8")
                          .arg(from.x()).arg(from.y()).arg(movedTo.x()).arg(movedTo.y())
                          .arg(rendersForTheMove)
                          .arg(afterMove.width())
                          .arg(afterMove.height())
                          .arg(beforeMove.constBits() == afterMove.constBits()
                                   ? QStringLiteral("reused (not redrawn)")
                                   : QStringLiteral("rebuilt")));
            }
        }

        // -------------------------------------------------------------------
        // T-076 / T-064 -- the fence's **own** menu.
        //
        // What is measured is the half the probe can measure: a right-click
        // inside a fence *reaches the product* as a menu request for the right
        // fence, at the right screen point, while a left press asks for nothing --
        // and the content the product will show follows the fence's own state
        // (the radio mark on the material in force, the roll entry naming the
        // gesture). The popup itself is Qt's, and the actions it triggers are the
        // same calls the tray and the settings window already make, so they are
        // covered by the checks above rather than duplicated here.
        // -------------------------------------------------------------------
        {
            const QString idMenu =
                container->createFence(QStringLiteral("menu"), QRect(680, 80, 320, 240));

            QString askedFor;
            QPoint askedAt;
            int menuRequests = 0;
            const QMetaObject::Connection menuConnection = QObject::connect(
                container, &starfish::ui::FenceContainer::fenceMenuRequested,
                [&askedFor, &askedAt, &menuRequests](const QString& fenceId, const QPoint& at) {
                    askedFor = fenceId;
                    askedAt = at;
                    ++menuRequests;
                });

            // A right-click is delivered the way the desktop delivers it: a press
            // with the button, in screen coordinates (the fence's own rect is what
            // the surface translates from).
            auto rightClickAt = [&container](const QString& fenceId, const QPoint& local) {
                auto* view = container->view(fenceId);
                const QPoint screen = container->fenceRect(fenceId).topLeft() + local;
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(screen),
                                  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
                QCoreApplication::sendEvent(view, &press);
                return screen;
            };

            const QPoint onTitleBar(160, 15);
            const QPoint expected = rightClickAt(idMenu, onTitleBar);
            check(QStringLiteral("a right-click on a fence asks for its own menu (T-076)"),
                  menuRequests == 1 && askedFor == idMenu && askedAt == expected,
                  QStringLiteral("%1 request(s) for %2 at %3,%4 (expected one for %5 at %6,%7)")
                      .arg(menuRequests)
                      .arg(askedFor.isEmpty() ? QStringLiteral("<none>") : askedFor)
                      .arg(askedAt.x())
                      .arg(askedAt.y())
                      .arg(idMenu)
                      .arg(expected.x())
                      .arg(expected.y()));

            // Empty panel too -- the title bar is not the only way in, and it is
            // what makes this usable on a fence whose bar is out of reach.
            const QPoint onPanel(160, 150);
            const QPoint expectedPanel = rightClickAt(idMenu, onPanel);
            check(QStringLiteral("the empty part of the panel asks for it as well"),
                  menuRequests == 2 && askedAt == expectedPanel,
                  QStringLiteral("%1 request(s), last at %2,%3 (expected %4,%5)")
                      .arg(menuRequests)
                      .arg(askedAt.x())
                      .arg(askedAt.y())
                      .arg(expectedPanel.x())
                      .arg(expectedPanel.y()));

            // The gesture that must NOT open a menu: a left press on the same
            // spot starts a geometry drag. (A right-click on a *tile* keeps the
            // Shell's menu, T-006 -- that path opens a blocking popup and is
            // therefore not driven here.)
            auto* menuView = container->view(idMenu);
            const QPoint local(onTitleBar);
            QMouseEvent leftPress(QEvent::MouseButtonPress, QPointF(local),
                                  QPointF(container->fenceRect(idMenu).topLeft() + local),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(menuView, &leftPress);
            QMouseEvent leftRelease(QEvent::MouseButtonRelease, QPointF(local),
                                    QPointF(container->fenceRect(idMenu).topLeft() + local),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(menuView, &leftRelease);
            check(QStringLiteral("a left press asks for no menu at all"), menuRequests == 2,
                  QStringLiteral("%1 request(s) after a left press on the title bar")
                      .arg(menuRequests));

            // The content follows the fence, not the code: a menu built from the
            // fence's own material marks that material, and only it.
            starfish::ui::FenceMenu::State menuState;
            menuState.rolledUp = container->fenceRolledUp(idMenu);
            menuState.material = container->fenceMaterial(idMenu);
            int marks = 0;
            QString marked;
            for (const starfish::ui::FenceMenu::Entry& preset :
                 starfish::ui::FenceMenu::materialEntries(menuState)) {
                if (preset.checked) {
                    ++marks;
                    marked = preset.material;
                }
            }
            check(QStringLiteral("the menu marks the material the fence is shown in"),
                  marks == 1 && marked == menuState.material,
                  QStringLiteral("fence %1 is \"%2\", the menu marks %3 preset(s) (%4)")
                      .arg(idMenu)
                      .arg(menuState.material)
                      .arg(marks)
                      .arg(marked.isEmpty() ? QStringLiteral("none") : marked));

            container->setFenceMaterial(idMenu, QStringLiteral("very-translucent"));
            menuState.material = container->fenceMaterial(idMenu);
            bool marksTheNewOne = false;
            for (const starfish::ui::FenceMenu::Entry& preset :
                 starfish::ui::FenceMenu::materialEntries(menuState)) {
                if (preset.checked)
                    marksTheNewOne = preset.material == QStringLiteral("very-translucent");
            }
            check(QStringLiteral("and it follows a material the fence just took"),
                  menuState.material == QStringLiteral("very-translucent") && marksTheNewOne,
                  QStringLiteral("fence now \"%1\", the mark %2")
                      .arg(menuState.material.isEmpty() ? QStringLiteral("<custom>")
                                                        : menuState.material)
                      .arg(marksTheNewOne ? QStringLiteral("moved with it")
                                          : QStringLiteral("did NOT move")));

            // The roll entry names the gesture, and the fence's state is what
            // changes it -- on an expanded fence and on a rolled-up one alike (a
            // rolled-up fence is all title bar, and that is exactly where a menu
            // has to stay reachable).
            const auto rollLabel = [&container](const QString& fenceId) {
                starfish::ui::FenceMenu::State state;
                state.rolledUp = container->fenceRolledUp(fenceId);
                for (const starfish::ui::FenceMenu::Entry& entry :
                     starfish::ui::FenceMenu::entries(state)) {
                    if (entry.action == starfish::ui::FenceMenu::Action::ToggleRoll)
                        return entry.label;
                }
                return QString();
            };
            const QString expandedLabel = rollLabel(idMenu);
            container->setFenceRolledUp(idMenu, true);
            const QString rolledLabel = rollLabel(idMenu);
            const int requestsBefore = menuRequests;
            rightClickAt(idMenu, onTitleBar);
            check(QStringLiteral("a rolled-up fence still offers its menu, and the roll entry "
                                 "names the way back"),
                  !expandedLabel.isEmpty() && !rolledLabel.isEmpty()
                      && expandedLabel != rolledLabel && menuRequests == requestsBefore + 1
                      && container->fenceRolledUp(idMenu),
                  QStringLiteral("expanded: \"%1\", rolled up: \"%2\", %3 request(s) from the bar")
                      .arg(expandedLabel)
                      .arg(rolledLabel)
                      .arg(menuRequests - requestsBefore));

            // The whole menu, as the product will build it: one entry per action,
            // and the destructive one last.
            starfish::ui::FenceMenu::State finalState;
            finalState.rolledUp = container->fenceRolledUp(idMenu);
            finalState.material = container->fenceMaterial(idMenu);
            const QList<starfish::ui::FenceMenu::Entry> menuEntries =
                starfish::ui::FenceMenu::entries(finalState);
            int rollEntries = 0;
            int renameEntries = 0;
            int removeEntries = 0;
            int submenus = 0;
            for (const starfish::ui::FenceMenu::Entry& entry : menuEntries) {
                if (entry.kind == starfish::ui::FenceMenu::Kind::Submenu)
                    ++submenus;
                if (entry.action == starfish::ui::FenceMenu::Action::ToggleRoll)
                    ++rollEntries;
                if (entry.action == starfish::ui::FenceMenu::Action::Rename)
                    ++renameEntries;
                if (entry.action == starfish::ui::FenceMenu::Action::Remove)
                    ++removeEntries;
            }
            check(QStringLiteral("the menu holds one material submenu, one roll, one rename and "
                                 "one remove, remove last"),
                  submenus == 1 && rollEntries == 1 && renameEntries == 1 && removeEntries == 1
                      && menuEntries.last().action == starfish::ui::FenceMenu::Action::Remove,
                  QStringLiteral("%1 entry(ies): %2 submenu, %3 roll, %4 rename, %5 remove -- "
                                 "last: %6")
                      .arg(menuEntries.size())
                      .arg(submenus)
                      .arg(rollEntries)
                      .arg(renameEntries)
                      .arg(removeEntries)
                      .arg(menuEntries.isEmpty() ? QStringLiteral("<none>")
                                                 : menuEntries.last().label));

            QObject::disconnect(menuConnection);
            container->removeFence(idMenu);
        }

        const QString reportPath = QStringLiteral("build/fence_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n").arg(failures));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        if (!hosted) {
            QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
            return app.exec();
        }
        bool parsedSeconds = false;
        const int seconds = parser.value(secondsOption).toInt(&parsedSeconds);
        QTimer::singleShot((parsedSeconds && seconds > 0 ? seconds : 8) * 1000, &app,
                           [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(persistDemoOption)) {
        // T-008: the persistence round trip. It goes through the wiring the
        // product uses -- a container change updates the LayoutStore, which
        // debounces the write -- rather than calling the serialiser by hand, so
        // the probe also covers the auto-save path. What it proves is the exit
        // criterion of Phase 3: "restart the app and the layout comes back
        // identical" (docs/plan.md).
        const QString layoutPath = parser.isSet(layoutOption)
                                       ? parser.value(layoutOption)
                                       : QStringLiteral("build/layout_demo.json");
        QFile::remove(layoutPath);

        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize bounds = screen ? screen->geometry().size() : QSize(1920, 1080);
        const bool hosted = parser.isSet(hostOption);

        starfish::core::LayoutStore store;
        store.setFilePath(layoutPath);
        store.setDebounceInterval(60);
        int writes = 0;
        QObject::connect(&store, &starfish::core::LayoutStore::saved, &app,
                         [&writes](const QString&) { ++writes; });

        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -48)
                       .arg(detail);
        };
        auto waitMs = [](int ms) {
            QEventLoop loop;
            QTimer::singleShot(ms, &loop, &QEventLoop::quit);
            loop.exec();
        };

        // `persist` wires the container to the store exactly like the product
        // will: every lifecycle signal updates the document, and the store
        // decides whether anything really changed and when to write.
        auto makeContainer = [&store, &bounds, hosted](bool persist) {
            auto* container = new starfish::ui::FenceContainer;
            container->setDesktopBounds(bounds);
            container->setHostOnDesktop(hosted);
            container->setSnapThreshold(8);
            container->setMinimumFenceSize(QSize(160, 120));
            if (persist) {
                auto sync = [container, &store] {
                    store.setLayout(container->toLayout());
                    store.scheduleSave();
                };
                QObject::connect(container, &starfish::ui::FenceContainer::fenceCreated, container,
                                 [sync](const QString&) { sync(); });
                QObject::connect(container, &starfish::ui::FenceContainer::fenceRemoved, container,
                                 [sync](const QString&) { sync(); });
                QObject::connect(container, &starfish::ui::FenceContainer::fenceRectChanged,
                                 container, [sync](const QString&, const QRect&) { sync(); });
                QObject::connect(container, &starfish::ui::FenceContainer::fenceContentChanged,
                                 container, [sync](const QString&) { sync(); });
            }
            return container;
        };

        // Items are described by path only: the probe must not depend on what
        // happens to be on this machine's desktop.
        const QString itemDir = QStringLiteral("build/persist_demo_items");
        auto itemFor = [&itemDir](const QString& name) {
            return starfish::core::itemFromPathOnly(itemDir + QLatin1Char('/') + name);
        };

        auto* first = makeContainer(/*persist=*/true);
        const QString idA = first->createFence(QStringLiteral("Travail"), QRect(120, 90, 320, 240),
                                              {itemFor(QStringLiteral("a.pdf")),
                                               itemFor(QStringLiteral("b.txt")),
                                               itemFor(QStringLiteral("c.png"))});
        const QString idB = first->createFence(QStringLiteral("Perso"), QRect(600, 120, 280, 200),
                                              {itemFor(QStringLiteral("notes.md"))});
        const QString idC = first->createFence(QStringLiteral("Temporaire"),
                                              QRect(120, 400, 240, 180));
        first->setFenceSort(idA, QStringLiteral("size"), false);
        check(QStringLiteral("three fences created"), first->fenceCount() == 3,
              QStringLiteral("count=%1 ids=%2")
                  .arg(first->fenceCount())
                  .arg(first->fenceIds().join(QLatin1Char(','))));

        // 434 is 6 px from A's right edge (440): what has to survive the restart
        // is the *snapped* rect, not the one that was asked for.
        const QRect snappedB = first->setFenceRect(idB, QRect(434, 400, 280, 200));
        check(QStringLiteral("move snaps to A's right edge"), snappedB.x() == 440,
              QStringLiteral("x=%1 (expected 440)").arg(snappedB.x()));

        const bool removedC = first->removeFence(idC);
        check(QStringLiteral("delete a fence"), removedC && first->fenceCount() == 2,
              QStringLiteral("removed=%1 count=%2").arg(removedC ? 1 : 0).arg(
                  first->fenceCount()));
        const QStringList savedIds = first->fenceIds();
        const QRect savedRectB = first->fenceRect(idB);

        // Six lifecycle signals have fired by now (three creations, a sort, a
        // snapped move, a deletion). They all happened inside one event-loop
        // turn, so the debounce must collapse them into a single write.
        waitMs(250); // longer than the debounce interval
        check(QStringLiteral("the change burst auto-saved exactly once"), writes == 1,
              QStringLiteral("writes=%1 (expected 1 for the whole burst)").arg(writes));
        QString saveError;
        const bool savedNow = store.saveNow(&saveError);
        check(QStringLiteral("saveNow writes the document"),
              savedNow && QFileInfo::exists(layoutPath),
              savedNow ? layoutPath : saveError);
        const int writesAfterSave = writes;
        check(QStringLiteral("a save with no change writes nothing"),
              store.saveNow() && writes == writesAfterSave,
              QStringLiteral("writes=%1 (expected %2)").arg(writes).arg(writesAfterSave));

        QString readError;
        const auto document = starfish::core::JsonStore::load(layoutPath, &readError);
        check(QStringLiteral("the file parses as schema 1"),
              document.has_value() && document->schema == 1,
              document.has_value() ? QStringLiteral("schema=%1").arg(document->schema) : readError);
        if (document) {
            check(QStringLiteral("the file holds the two surviving fences"),
                  document->fences.size() == 2,
                  QStringLiteral("fences=%1").arg(document->fences.size()));
            check(QStringLiteral("the snapped rect is in the file"),
                  document->fences.at(1).rect == snappedB,
                  QStringLiteral("%1,%2 %3x%4")
                      .arg(document->fences.at(1).rect.x())
                      .arg(document->fences.at(1).rect.y())
                      .arg(document->fences.at(1).rect.width())
                      .arg(document->fences.at(1).rect.height()));
            check(QStringLiteral("the three items are in the file"),
                  document->fences.at(0).items.size() == 3,
                  QStringLiteral("items=%1").arg(document->fences.at(0).items.size()));
            check(QStringLiteral("the sort order is in the file"),
                  document->fences.at(0).sortBy == QLatin1String("size")
                      && !document->fences.at(0).sortAscending,
                  QStringLiteral("%1/%2")
                      .arg(document->fences.at(0).sortBy)
                      .arg(document->fences.at(0).sortAscending ? QStringLiteral("asc")
                                                                : QStringLiteral("desc")));
            check(QStringLiteral("the style is in the file"),
                  document->fences.at(0).style.background
                          == starfish::core::FenceStyle().background
                      && document->fences.at(0).style.radius == 10,
                  QStringLiteral("bg=%1 (the default is %2) radius=%3")
                      .arg(document->fences.at(0).style.background)
                      .arg(starfish::core::FenceStyle().background)
                      .arg(document->fences.at(0).style.radius));
            check(QStringLiteral("the deleted fence is gone from the file"),
                  document->fences.at(0).id == idA && document->fences.at(1).id == idB,
                  QStringLiteral("ids=%1,%2").arg(document->fences.at(0).id,
                                                  document->fences.at(1).id));
        }

        // A click that only moves the selection must not rewrite the file: the
        // selection is not part of the document, so the document is unchanged.
        const int writesBeforeClick = writes;
        if (starfish::ui::FenceView* viewB = first->view(idB)) {
            const QPoint empty(200, 150); // inside the content area, on no cell
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(empty),
                              QPointF(viewB->mapToGlobal(empty)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewB, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(empty),
                                QPointF(viewB->mapToGlobal(empty)), Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QCoreApplication::sendEvent(viewB, &release);
        }
        waitMs(250); // longer than the debounce interval
        check(QStringLiteral("a selection change does not rewrite the file"),
              writes == writesBeforeClick,
              QStringLiteral("writes=%1 (expected %2)").arg(writes).arg(writesBeforeClick));

        // --- the restart ------------------------------------------------------
        delete first;

        starfish::core::LayoutStore reopened;
        reopened.setFilePath(layoutPath);
        QString loadError;
        const bool loaded = reopened.load(&loadError);
        check(QStringLiteral("restart: the document loads"), loaded,
              loaded ? QStringLiteral("fences=%1").arg(reopened.layout().fences.size())
                     : loadError);

        auto* second = makeContainer(/*persist=*/false);
        QString applyError;
        const bool applied = second->applyLayout(reopened.layout(), &applyError);
        check(QStringLiteral("restart: the layout applies"), applied,
              applied ? QStringLiteral("count=%1").arg(second->fenceCount()) : applyError);
        check(QStringLiteral("restart: the fence ids are stable"),
              second->fenceIds() == savedIds,
              second->fenceIds().join(QLatin1Char(',')));
        check(QStringLiteral("restart: the snapped rect comes back"),
              second->fenceRect(idB) == savedRectB,
              QStringLiteral("rect=%1,%2 %3x%4 (expected %5,%6 %7x%8)")
                  .arg(second->fenceRect(idB).x())
                  .arg(second->fenceRect(idB).y())
                  .arg(second->fenceRect(idB).width())
                  .arg(second->fenceRect(idB).height())
                  .arg(savedRectB.x())
                  .arg(savedRectB.y())
                  .arg(savedRectB.width())
                  .arg(savedRectB.height()));
        check(QStringLiteral("restart: the items come back"),
              second->fenceItems(idA).size() == 3,
              QStringLiteral("items=%1").arg(second->fenceItems(idA).size()));
        check(QStringLiteral("restart: the sort order comes back"),
              second->view(idA) && second->view(idA)->sortBy() == QLatin1String("size")
                  && !second->view(idA)->sortAscending(),
              second->view(idA)
                  ? QStringLiteral("%1/%2")
                        .arg(second->view(idA)->sortBy())
                        .arg(second->view(idA)->sortAscending() ? QStringLiteral("asc")
                                                                : QStringLiteral("desc"))
                  : QStringLiteral("no view"));
        check(QStringLiteral("restart: the style comes back"),
              second->fenceStyle(idA).radius == 10
                  && second->fenceStyle(idA).background
                         == starfish::core::FenceStyle().background,
              QStringLiteral("radius=%1 bg=%2 (the default is %3)")
                  .arg(second->fenceStyle(idA).radius)
                  .arg(second->fenceStyle(idA).background)
                  .arg(starfish::core::FenceStyle().background));
        check(QStringLiteral("restart: the document round-trips byte for byte"),
              starfish::core::JsonStore::toJson(second->toLayout())
                  == starfish::core::JsonStore::toJson(reopened.layout()),
              QStringLiteral("toLayout() == the loaded document"));

        // A restored id must not be handed out again by the id generator.
        const QString idNew = second->createFence(QStringLiteral("Neuf"),
                                                 QRect(600, 620, 200, 160));
        check(QStringLiteral("restart: a new fence gets a fresh id"),
              !savedIds.contains(idNew) && second->fenceCount() == 3,
              QStringLiteral("new id=%1").arg(idNew));
        second->removeFence(idNew);

        // --- deleting after a restart shrinks the document ---------------------
        second->removeFence(idA);
        reopened.setLayout(second->toLayout());
        QString secondSaveError;
        const bool secondSaved = reopened.saveNow(&secondSaveError);
        const auto afterDelete = starfish::core::JsonStore::load(layoutPath, &readError);
        check(QStringLiteral("delete + save shrinks the document"),
              secondSaved && afterDelete.has_value() && afterDelete->fences.size() == 1
                  && afterDelete->fences.at(0).id == idB,
              secondSaved ? QStringLiteral("fences=%1")
                                .arg(afterDelete ? afterDelete->fences.size() : -1)
                          : secondSaveError);

        // --- robustness -------------------------------------------------------
        const QString corruptPath = QStringLiteral("build/persist_demo_corrupt.json");
        {
            QFile corrupt(corruptPath);
            if (corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate))
                corrupt.write("{ this is not a layout");
        }
        starfish::core::LayoutStore guarded;
        guarded.setFilePath(corruptPath);
        guarded.setLayout(reopened.layout());
        QString corruptError;
        const bool corruptLoaded = guarded.load(&corruptError);
        check(QStringLiteral("a corrupted file is refused, not applied"),
              !corruptLoaded && !corruptError.isEmpty()
                  && guarded.layout().fences.size() == 1,
              corruptLoaded ? QStringLiteral("accepted a corrupt file!")
                            : QStringLiteral("kept %1 fence(s): %2")
                                  .arg(guarded.layout().fences.size())
                                  .arg(corruptError));

        starfish::core::LayoutStore fresh;
        fresh.setFilePath(QStringLiteral("build/persist_demo_absent.json"));
        QFile::remove(fresh.filePath());
        check(QStringLiteral("a missing file is a first run, not an error"),
              fresh.load() && fresh.layout().fences.isEmpty(),
              QStringLiteral("fences=%1").arg(fresh.layout().fences.size()));

        if (hosted) {
#if defined(_WIN32)
            // The same restart, but on the real desktop: the user can see the
            // restored fences, plus one filled with the actual desktop entries.
            auto* hosted = makeContainer(/*persist=*/false);
            hosted->setItemResolver([](const QString& path) {
                return starfish::platform::win::DesktopScanner::itemForPath(path);
            });
            hosted->applyLayout(reopened.layout());
            QString scanError;
            const QList<starfish::core::DesktopItem> desktopItems =
                starfish::platform::win::DesktopScanner::scan(&scanError);
            if (!scanError.isEmpty())
                qCWarning(lc).noquote() << scanError;
            const QString idDemo = hosted->createFence(QStringLiteral("StarFish (T-008 demo)"),
                                                      QRect(200, 300, 360, 260), desktopItems);
            log << QStringLiteral("hosted %1 restored fence(s) + %2 with %3 desktop item(s)")
                       .arg(hosted->fenceCount() - 1)
                       .arg(idDemo)
                       .arg(desktopItems.size());
#endif
        }

        const QString reportPath = QStringLiteral("build/persist_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QStringLiteral("layout file: %1\n").arg(layoutPath);
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n").arg(failures));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        if (!hosted)
            return exitCode;

        bool parsedSeconds = false;
        const int seconds = parser.value(secondsOption).toInt(&parsedSeconds);
        QTimer::singleShot((parsedSeconds && seconds > 0 ? seconds : 10) * 1000, &app,
                           [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(navDemoOption)) {
        // T-009: roll-up (F-020), quick hide (F-022) and Peek (F-021). The
        // container owns every piece of state and the widget only reports
        // intent, so the probe drives the real chain -- a double-click on the
        // title bar goes through FenceView's own event handling, not through a
        // direct container call.
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize bounds = screen ? screen->geometry().size() : QSize(1920, 1080);
        const bool hosted = parser.isSet(hostOption);

        starfish::platform::win::ShellIconCache demoIcons;
        auto* container = new starfish::ui::FenceContainer(&app);
        container->setDesktopBounds(bounds);
        container->setHostOnDesktop(hosted);
        container->setSnapThreshold(8);
        container->setMinimumFenceSize(QSize(160, 120));
        container->setIconProvider([&demoIcons](const starfish::core::DesktopItem& item,
                                               int sizePx) {
            return demoIcons.iconFor(item.path, sizePx);
        });

        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -46)
                       .arg(detail);
        };

        // Drives the widget exactly like the desktop does.
        auto sendClick = [](QWidget* widget, const QPoint& pos, QEvent::Type type) {
            QMouseEvent event(type, QPointF(pos), QPointF(widget->mapToGlobal(pos)),
                              Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                                                : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(widget, &event);
        };
        auto sendWheel = [](QWidget* widget, const QPoint& pos, int notches) {
            QWheelEvent event(QPointF(pos), QPointF(widget->mapToGlobal(pos)), QPoint(0, 0),
                              QPoint(0, notches * 120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(widget, &event);
        };

        // Signal bookkeeping: the probe checks that the navigation changes are
        // announced, because that is what the product wires to the auto-save.
        int rolledUpSignals = 0;
        int visibilitySignals = 0;
        int peekSignals = 0;
        QObject::connect(container, &starfish::ui::FenceContainer::fenceRolledUpChanged, &app,
                         [&rolledUpSignals](const QString&, bool) { ++rolledUpSignals; });
        QObject::connect(container, &starfish::ui::FenceContainer::fencesVisibilityChanged, &app,
                         [&visibilitySignals](bool) { ++visibilitySignals; });
        QObject::connect(container, &starfish::ui::FenceContainer::peekChanged, &app,
                         [&peekSignals](bool) { ++peekSignals; });

        log << QStringLiteral("desktop bounds %1x%2, title bar %3 px, hosting %4")
                   .arg(bounds.width())
                   .arg(bounds.height())
                   .arg(starfish::ui::FenceGeometry::kTitleBarHeight)
                   .arg(hosted ? QStringLiteral("yes") : QStringLiteral("no"));

        const QString idA = container->createFence(QStringLiteral("A"), QRect(80, 80, 320, 240));
        const QString idB = container->createFence(QStringLiteral("B"), QRect(520, 80, 320, 240));
        const QRect rectA = container->fenceRect(idA);

        // --- F-020 roll-up ---------------------------------------------------
        check(QStringLiteral("a fresh fence is not rolled up"),
              !container->fenceRolledUp(idA) && container->fenceVisibleRect(idA) == rectA,
              QStringLiteral("visible=%1x%2")
                  .arg(container->fenceVisibleRect(idA).width())
                  .arg(container->fenceVisibleRect(idA).height()));

        // Each mutation is a statement of its own: the detail string is built as
        // another argument of check(), and argument evaluation order is
        // unspecified, so it must not be able to read the state before the call.
        const bool rolledA = container->setFenceRolledUp(idA, true);
        check(QStringLiteral("roll up shrinks the visible rect to the title bar"),
              rolledA
                  && container->fenceVisibleRect(idA).height()
                         == starfish::ui::FenceGeometry::kTitleBarHeight,
              QStringLiteral("visible height=%1 (title bar %2)")
                  .arg(container->fenceVisibleRect(idA).height())
                  .arg(starfish::ui::FenceGeometry::kTitleBarHeight));

        check(QStringLiteral("roll up keeps the stored rect (the document's)"),
              container->fenceRect(idA) == rectA,
              QStringLiteral("stored=%1,%2 %3x%4")
                  .arg(container->fenceRect(idA).x())
                  .arg(container->fenceRect(idA).y())
                  .arg(container->fenceRect(idA).width())
                  .arg(container->fenceRect(idA).height()));

        // A rolled-up fence has no visible body: a resize must not grow it.
        const QRect stretched = container->setFenceRect(idA, QRect(80, 80, 320, 600), false);
        check(QStringLiteral("resizing a rolled-up fence cannot change its height"),
              stretched.height() == rectA.height() && stretched.width() == 320,
              QStringLiteral("stored=%1x%2 (expected 320x%3)")
                  .arg(stretched.width())
                  .arg(stretched.height())
                  .arg(rectA.height()));

        // The title bar -- not the hidden body -- is what snaps to the edge.
        const QRect rolledSnap = container->setFenceRect(
            idA, QRect(80, bounds.height() - starfish::ui::FenceGeometry::kTitleBarHeight - 5, 320,
                       rectA.height()));
        check(QStringLiteral("a rolled-up fence snaps by its title bar"),
              rolledSnap.y() + starfish::ui::FenceGeometry::kTitleBarHeight == bounds.height(),
              QStringLiteral("title bar bottom=%1 (screen %2)")
                  .arg(rolledSnap.y() + starfish::ui::FenceGeometry::kTitleBarHeight)
                  .arg(bounds.height()));

        const bool unrolledA = container->setFenceRolledUp(idA, false);
        check(QStringLiteral("rolling back down restores the fence exactly"),
              unrolledA && container->fenceVisibleRect(idA).height() == rectA.height()
                  && container->fenceRect(idA).height() == rectA.height(),
              QStringLiteral("visible=%1x%2")
                  .arg(container->fenceVisibleRect(idA).width())
                  .arg(container->fenceVisibleRect(idA).height()));

        // --- F-020 through the widget's own gestures -------------------------
        const QString idC = container->createFence(QStringLiteral("C"), QRect(80, 500, 320, 240));
        if (starfish::ui::FenceView* viewC = container->view(idC)) {
            const QPoint titleBar(160, 15);
            sendClick(viewC, titleBar, QEvent::MouseButtonPress);
            sendClick(viewC, titleBar, QEvent::MouseButtonRelease);
            sendClick(viewC, titleBar, QEvent::MouseButtonDblClick);
            sendClick(viewC, titleBar, QEvent::MouseButtonRelease);
            check(QStringLiteral("double-click on the title bar rolls the fence up"),
                  container->fenceRolledUp(idC),
                  QStringLiteral("rolledUp=%1").arg(container->fenceRolledUp(idC) ? 1 : 0));

            sendWheel(viewC, titleBar, -1);
            check(QStringLiteral("a wheel notch down rolls the fence back down"),
                  !container->fenceRolledUp(idC),
                  QStringLiteral("rolledUp=%1").arg(container->fenceRolledUp(idC) ? 1 : 0));

            sendWheel(viewC, titleBar, 1);
            check(QStringLiteral("a wheel notch up rolls the fence up"),
                  container->fenceRolledUp(idC),
                  QStringLiteral("rolledUp=%1").arg(container->fenceRolledUp(idC) ? 1 : 0));

            // The icon grid keeps its own clicks: a double-click far from the
            // title bar must not roll the fence up or down.
            const bool wasRolled = container->fenceRolledUp(idC);
            const QPoint content(160, 140);
            sendClick(viewC, content, QEvent::MouseButtonPress);
            sendClick(viewC, content, QEvent::MouseButtonRelease);
            sendClick(viewC, content, QEvent::MouseButtonDblClick);
            sendClick(viewC, content, QEvent::MouseButtonRelease);
            check(QStringLiteral("a double-click in the content area does not roll up"),
                  container->fenceRolledUp(idC) == wasRolled,
                  QStringLiteral("rolledUp=%1 (unchanged)").arg(wasRolled ? 1 : 0));
        }
        check(QStringLiteral("roll-up changes were announced"),
              rolledUpSignals >= 4,
              QStringLiteral("fenceRolledUpChanged x%1 (expected >= 4)").arg(rolledUpSignals));

        // --- F-020 roll-up survives a restart --------------------------------
        {
            starfish::core::LayoutStore store;
            const QString layoutPath = QStringLiteral("build/nav_demo_layout.json");
            store.setFilePath(layoutPath);
            store.setLayout(container->toLayout());
            QString saveError;
            const bool saved = store.saveNow(&saveError);
            const auto document = starfish::core::JsonStore::load(layoutPath, &saveError);
            check(QStringLiteral("the roll-up state is written to the document"),
                  saved && document.has_value() && document->fences.at(2).rolledUp,
                  saved ? QStringLiteral("fences=%1 rolledUp=%2")
                              .arg(document ? document->fences.size() : -1)
                              .arg(document && document->fences.at(2).rolledUp ? 1 : 0)
                        : saveError);

            auto* restored = new starfish::ui::FenceContainer(&app);
            restored->setDesktopBounds(bounds);
            restored->setMinimumFenceSize(QSize(160, 120));
            const bool applied = document && restored->applyLayout(*document);
            check(QStringLiteral("a restored fence comes back rolled up"),
                  applied && restored->fenceRolledUp(idC)
                      && restored->fenceVisibleRect(idC).height()
                             == starfish::ui::FenceGeometry::kTitleBarHeight,
                  applied ? QStringLiteral("rolledUp=%1 visible height=%2")
                                .arg(restored->fenceRolledUp(idC) ? 1 : 0)
                                .arg(restored->fenceVisibleRect(idC).height())
                          : QStringLiteral("applyLayout failed"));
            delete restored;
        }

        // --- F-022 quick hide -------------------------------------------------
        const int countBeforeHide = container->fenceCount();
        const bool hidden = container->setFencesVisible(false);
        check(QStringLiteral("quick hide takes every fence off screen"),
              hidden && !container->fencesVisible(),
              QStringLiteral("fencesVisible=%1").arg(container->fencesVisible() ? 1 : 0));
        check(QStringLiteral("quick hide destroys nothing"),
              container->fenceCount() == countBeforeHide,
              QStringLiteral("count=%1 (was %2)").arg(container->fenceCount()).arg(countBeforeHide));
        const bool shown = container->setFencesVisible(true);
        check(QStringLiteral("quick hide shows the fences again"),
              shown && container->fencesVisible(),
              QStringLiteral("fencesVisible=%1").arg(container->fencesVisible() ? 1 : 0));
        check(QStringLiteral("visibility changes were announced (2, not 3)"),
              visibilitySignals == 2,
              QStringLiteral("fencesVisibilityChanged x%1").arg(visibilitySignals));

        // --- F-021 Peek -------------------------------------------------------
        const bool peeked = container->setPeekActive(true);
        check(QStringLiteral("Peek raises every fence above the icon layer"),
              peeked && container->peekActive(),
              QStringLiteral("peekActive=%1").arg(container->peekActive() ? 1 : 0));
        const bool peekedOff = container->setPeekActive(false);
        check(QStringLiteral("Peek is reversible"),
              peekedOff && !container->peekActive(),
              QStringLiteral("peekActive=%1").arg(container->peekActive() ? 1 : 0));
        check(QStringLiteral("Peek changes were announced (2, not 3)"),
              peekSignals == 2, QStringLiteral("peekChanged x%1").arg(peekSignals));

        // --- the hotkey plumbing ---------------------------------------------
        starfish::platform::win::HotkeyManager hotkeys;
        const bool peekKey = hotkeys.registerHotkey(1, Qt::MetaModifier, Qt::Key_Space);
        // Win+Space is the documented default (F-021) but Windows itself owns it
        // on most systems, so a refusal is expected and reported, not hidden.
        log << QStringLiteral("hotkey %1 (Peek): %2")
                   .arg(starfish::platform::win::HotkeyManager::describe(Qt::MetaModifier,
                                                                        Qt::Key_Space),
                        peekKey ? QStringLiteral("registered")
                                : QStringLiteral("refused by Windows (already owned)"));
        const bool hideKey =
            hotkeys.registerHotkey(2, Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_H);
        check(QStringLiteral("the quick-hide hotkey registers"),
              hideKey,
              QStringLiteral("%1 (Win+Shift+H)")
                  .arg(hideKey ? QStringLiteral("registered") : QStringLiteral("refused")));
        check(QStringLiteral("a second manager cannot steal the same hotkey"),
              !hotkeys.registerHotkey(3, Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_H),
              QStringLiteral("re-registering Win+Shift+H under a second id is refused"));

        int hotkeyEvents = 0;
        QObject::connect(&hotkeys, &starfish::platform::win::HotkeyManager::triggered, &app,
                         [&hotkeyEvents, container](int id) {
                             ++hotkeyEvents;
                             if (id == 1)
                                 container->togglePeek();
                             else if (id == 2)
                                 container->toggleFencesVisible();
                         });
        // The wiring is exercised by emitting through the manager's own signal
        // path: WM_HOTKEY itself needs a real key press, which is a human
        // validation (STATE.md decision 13).
        hotkeys.unregisterHotkey(2);

        if (hosted) {
#if defined(_WIN32)
            // On the real desktop: the geometry of the hosted windows is what
            // the user sees, so it is checked directly.
            const QString idHosted =
                container->createFence(QStringLiteral("StarFish (T-009 demo)"),
                                       QRect(420, 260, 360, 260));
            auto* surface = container->surface(idHosted);
            check(QStringLiteral("hosted: the surface exists"), surface != nullptr,
                  surface ? QStringLiteral("hwnd=0x%1")
                                .arg(surface->hwnd(), 0, 16)
                          : QStringLiteral("no surface (desktop unavailable?)"));
            if (surface) {
                container->setFenceRolledUp(idHosted, true);
                check(QStringLiteral("hosted: rolling up shrinks the window"),
                      surface->geometry().height() == starfish::ui::FenceGeometry::kTitleBarHeight,
                      QStringLiteral("window height=%1 (title bar %2)")
                          .arg(surface->geometry().height())
                          .arg(starfish::ui::FenceGeometry::kTitleBarHeight));

                container->setFencesVisible(false);
                check(QStringLiteral("hosted: quick hide hides the window"),
                      !surface->isVisible(),
                      QStringLiteral("isVisible=%1").arg(surface->isVisible() ? 1 : 0));
                container->setFencesVisible(true);
                check(QStringLiteral("hosted: showing brings the window back"),
                      surface->isVisible(),
                      QStringLiteral("isVisible=%1").arg(surface->isVisible() ? 1 : 0));

                // The z-order is what Peek actually changes: read it back from
                // Progman's child list rather than trusting the flag we set.
                const HWND progman = ::FindWindowW(L"Progman", nullptr);
                const HWND defView =
                    progman ? ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)
                            : nullptr;
                const HWND surfaceHwnd = reinterpret_cast<HWND>(surface->hwnd());
                const auto verdict = [&] {
                    return starfish::core::evaluatePlacement(progmanChildIndex(surfaceHwnd),
                                                             progmanChildIndex(defView));
                };
                check(QStringLiteral("hosted: the surface sits above the desktop's own layers"),
                      verdict() == starfish::core::PlacementVerdict::AboveIconLayer,
                      QStringLiteral("z-order: surface [%1] vs DefView [%2]")
                          .arg(progmanChildIndex(surfaceHwnd))
                          .arg(progmanChildIndex(defView)));

                container->setPeekActive(true);
                // T-053: a fence is already above everything the desktop paints,
                // so Peek has nothing left to change. The check now asserts that
                // (rather than a raise that no longer exists), and the flag is
                // still honoured by place() for the probes that set it.
                check(QStringLiteral("hosted: Peek no longer changes anything (T-053)"),
                      surface->raisedAboveIcons()
                          && verdict() == starfish::core::PlacementVerdict::AboveIconLayer,
                      QStringLiteral("raisedAboveIcons=%1, z-order: surface [%2] vs DefView [%3]")
                          .arg(surface->raisedAboveIcons() ? 1 : 0)
                          .arg(progmanChildIndex(surfaceHwnd))
                          .arg(progmanChildIndex(defView)));
                container->setPeekActive(false);
                check(QStringLiteral("hosted: leaving Peek keeps it above the layers too"),
                      !surface->raisedAboveIcons()
                          && verdict() == starfish::core::PlacementVerdict::AboveIconLayer,
                      QStringLiteral("raisedAboveIcons=%1, z-order: surface [%2] vs DefView [%3]")
                          .arg(surface->raisedAboveIcons() ? 1 : 0)
                          .arg(progmanChildIndex(surfaceHwnd))
                          .arg(progmanChildIndex(defView)));
            }
            log << QStringLiteral("hosted fence %1, %2 fence(s) live, %3 hotkey event(s)")
                       .arg(idHosted)
                       .arg(container->fenceCount())
                       .arg(hotkeyEvents);
#endif
        }

        const QString reportPath = QStringLiteral("build/nav_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n").arg(failures));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        if (!hosted) {
            QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
            return app.exec();
        }
        bool parsedSeconds = false;
        const int seconds = parser.value(secondsOption).toInt(&parsedSeconds);
        QTimer::singleShot((parsedSeconds && seconds > 0 ? seconds : 8) * 1000, &app,
                           [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(rulesDemoOption)) {
        // T-010: auto-organisation (F-017) and the watcher that feeds it
        // (F-018). This is also the exit criterion of Phase 4, checked for
        // real: a file created in a watched folder ends up inside a fence --
        // and the file itself never moves.
        const bool hosted = parser.isSet(hostOption);
        const QString layoutPath = parser.isSet(layoutOption)
                                       ? parser.value(layoutOption)
                                       : QStringLiteral("build/rules_demo_layout.json");
        const bool customWatch = parser.isSet(watchOption);
        const QString watchDir = customWatch ? parser.value(watchOption)
                                             : QStringLiteral("build/rules_demo_watch");
        const QString itemDir = QStringLiteral("build/rules_demo_items");

        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize bounds = screen ? screen->geometry().size() : QSize(1920, 1080);

        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -52)
                       .arg(detail);
        };
        auto waitMs = [](int ms) {
            QEventLoop loop;
            QTimer::singleShot(ms, &loop, &QEventLoop::quit);
            loop.exec();
        };

        // The document is wired exactly like the product will: every
        // document-level signal updates the layout and re-arms the debounced
        // write. Filing a file by rule is a change to the document, so it has
        // to be persisted like any other.
        starfish::core::LayoutStore store;
        store.setFilePath(layoutPath);
        store.setDebounceInterval(60);
        QFile::remove(layoutPath);
        int writes = 0;
        QObject::connect(&store, &starfish::core::LayoutStore::saved, &app,
                         [&writes](const QString&) { ++writes; });

        auto* container = new starfish::ui::FenceContainer;
        container->setDesktopBounds(bounds);
        container->setHostOnDesktop(hosted);
        container->setMinimumFenceSize(QSize(160, 120));
        {
            auto sync = [container, &store] {
                store.setLayout(container->toLayout());
                store.scheduleSave();
            };
            QObject::connect(container, &starfish::ui::FenceContainer::fenceCreated, container,
                             [sync](const QString&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::fenceRemoved, container,
                             [sync](const QString&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::fenceRectChanged, container,
                             [sync](const QString&, const QRect&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::fenceContentChanged,
                             container, [sync](const QString&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::rulesChanged, container,
                             [sync] { sync(); });
        }

        const QString idShots = container->createFence(QStringLiteral("Captures"),
                                                      QRect(80, 80, 320, 240));
        const QString idDocs = container->createFence(QStringLiteral("Factures"),
                                                     QRect(440, 80, 320, 240));

        QList<starfish::core::Rule> rules;
        {
            // Priority order: the first match wins, so the extension rule is
            // deliberately placed before the name pattern.
            starfish::core::Rule shots;
            shots.id = QStringLiteral("rule-shots");
            shots.name = QStringLiteral("Captures d'écran");
            shots.extensions = {QStringLiteral("png")};
            shots.targetFenceId = idShots;
            rules.append(shots);

            starfish::core::Rule docs;
            docs.id = QStringLiteral("rule-docs");
            docs.name = QStringLiteral("Factures");
            docs.namePattern = QStringLiteral("facture*");
            docs.targetFenceId = idDocs;
            rules.append(docs);

            // A deliberately narrow catch-all: .txt files *older than ten
            // years*. Nothing fresh can match it, which is what proves the
            // date criteria really gate the rule.
            starfish::core::Rule ancient;
            ancient.id = QStringLiteral("rule-ancient");
            ancient.name = QStringLiteral("Vieux textes");
            ancient.extensions = {QStringLiteral("txt")};
            ancient.olderThanDays = 3650;
            ancient.targetFenceId = idShots;
            rules.append(ancient);
        }
        container->setRules(rules);

        // --- A. the engine, no I/O -------------------------------------------
        //
        // A fixed clock, so the date criteria are checked against a known
        // reference instead of whatever the wall clock says when this runs.
        const QDateTime clock = QDateTime::fromString(QStringLiteral("2026-10-14T12:00:00"),
                                                      Qt::ISODate);
        auto meta = [&clock](const QString& name, int daysAgo) {
            starfish::core::FileMeta file;
            file.name = name;
            file.path = QStringLiteral("C:/Desktop/") + name;
            file.modified = clock.addDays(-daysAgo);
            return file;
        };
        auto targetOf = [&rules, &clock](const starfish::core::FileMeta& file) {
            return starfish::core::RuleEngine::classify(file, rules, clock)
                .value_or(QStringLiteral("(none)"));
        };

        const QString t1 = targetOf(meta(QStringLiteral("shot-1.png"), 0));
        check(QStringLiteral("an extension rule matches"), t1 == idShots,
              QStringLiteral("\"shot-1.png\" -> %1 (expected %2)").arg(t1, idShots));

        const QString t2 = targetOf(meta(QStringLiteral("facture-2026.pdf"), 0));
        check(QStringLiteral("a name pattern rule matches"), t2 == idDocs,
              QStringLiteral("\"facture-2026.pdf\" -> %1 (expected %2)").arg(t2, idDocs));

        const QString t3 = targetOf(meta(QStringLiteral("facture-1.png"), 0));
        check(QStringLiteral("the first matching rule wins"), t3 == idShots,
              QStringLiteral("both rules match, %1 chosen (expected %2 = the first)")
                  .arg(t3, idShots));

        const QString t4 = targetOf(meta(QStringLiteral("notes.txt"), 0));
        check(QStringLiteral("the age window rejects a fresh file"), t4 == QLatin1String("(none)"),
              QStringLiteral("\"notes.txt\" written today -> %1 (expected no rule: older than "
                             "3650 days only)")
                  .arg(t4));

        const QString t5 = targetOf(meta(QStringLiteral("notes.txt"), 4000));
        check(QStringLiteral("the age window accepts an old file"), t5 == idShots,
              QStringLiteral("\"notes.txt\" 4000 days old -> %1 (expected %2)").arg(t5, idShots));

        const QString t6 = targetOf(meta(QStringLiteral("readme.md"), 0));
        check(QStringLiteral("an item no rule wants is left alone"),
              t6 == QLatin1String("(none)"),
              QStringLiteral("\"readme.md\" -> %1").arg(t6));

        // --- B. the document --------------------------------------------------
        const QByteArray before = starfish::core::JsonStore::toJson(container->toLayout());
        check(QStringLiteral("the rules are in the document"),
              before.contains(QByteArrayLiteral("\"rules\""))
                  && before.contains(QByteArrayLiteral("rule-shots")),
              QStringLiteral("%1 byte(s), %2 rule(s)")
                  .arg(before.size())
                  .arg(container->rules().size()));

        waitMs(250); // longer than the debounce interval
        check(QStringLiteral("setting the rules was auto-saved"), writes == 1,
              QStringLiteral("writes=%1 (expected 1 for the whole burst)").arg(writes));

        starfish::core::LayoutStore reopened;
        reopened.setFilePath(layoutPath);
        QString loadError;
        const bool loaded = reopened.load(&loadError);
        check(QStringLiteral("reload: the document loads"),
              loaded && reopened.layout().rules.size() == 3,
              loaded ? QStringLiteral("rules=%1").arg(reopened.layout().rules.size()) : loadError);

        auto* restored = new starfish::ui::FenceContainer;
        restored->setDesktopBounds(bounds);
        restored->setItemResolver([](const QString& path) {
            return starfish::core::itemFromPathOnly(path);
        });
        QString applyError;
        const bool applied = restored->applyLayout(reopened.layout(), &applyError);
        check(QStringLiteral("reload: the rules come back"), applied && restored->rules().size() == 3,
              applied ? QStringLiteral("rules=%1").arg(restored->rules().size()) : applyError);
        if (applied && restored->rules().size() == 3) {
            const starfish::core::Rule& first = restored->rules().at(0);
            const starfish::core::Rule& third = restored->rules().at(2);
            check(QStringLiteral("reload: the criteria survive intact"),
                  first.extensions == QStringList({QStringLiteral("png")})
                      && first.targetFenceId == idShots
                      && restored->rules().at(1).namePattern == QStringLiteral("facture*")
                      && third.olderThanDays.value_or(-1) == 3650,
                  QStringLiteral("%1 ext, pattern \"%2\", %3 days")
                      .arg(first.extensions.join(QLatin1Char('/')))
                      .arg(restored->rules().at(1).namePattern)
                      .arg(third.olderThanDays.value_or(-1)));
            // A restored rule must classify exactly like the live one: the same
            // clock, the same answer.
            const QString t7 = starfish::core::RuleEngine::classify(
                                   meta(QStringLiteral("shot-1.png"), 0), restored->rules(), clock)
                                   .value_or(QStringLiteral("(none)"));
            check(QStringLiteral("reload: a restored rule still matches"), t7 == idShots,
                  QStringLiteral("\"shot-1.png\" -> %1 (expected %2)").arg(t7, idShots));
        }
        check(QStringLiteral("reload: the document round-trips byte for byte"),
              starfish::core::JsonStore::toJson(restored->toLayout())
                  == starfish::core::JsonStore::toJson(reopened.layout()),
              QStringLiteral("toLayout() == the loaded document"));

        {
            // What T-008 wrote: no `rules` key at all. It must keep loading.
            const QByteArray legacy = QByteArrayLiteral(
                "{\n  \"schema\": 1,\n  \"fences\": []\n}");
            QString legacyError;
            const auto parsedLegacy = starfish::core::JsonStore::fromJson(legacy, &legacyError);
            check(QStringLiteral("a document without rules still loads"),
                  parsedLegacy.has_value() && parsedLegacy->rules.isEmpty(),
                  parsedLegacy ? QStringLiteral("rules=%1").arg(parsedLegacy->rules.size())
                               : legacyError);
        }
        delete restored;

        // --- C. the wiring: an item is filed, nothing moves -------------------
        QDir().mkpath(itemDir);
        const QString realName = QStringLiteral("starfish-rules-demo-shot.png");
        const QString realPath = QDir(itemDir).absoluteFilePath(realName);
        {
            QFile file(realPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                file.write("not really a png");
        }
        const starfish::core::DesktopItem realItem = container->itemForPath(realPath);

        const std::optional<QString> target = container->classify(realItem);
        check(QStringLiteral("the container classifies a real file"),
              target.has_value() && *target == idShots,
              QStringLiteral("%1 -> %2").arg(realName, target.value_or(QStringLiteral("(none)"))));

        const int placed = container->applyRulesTo({realItem});
        check(QStringLiteral("the rule files it into the fence"), placed == 1,
              QStringLiteral("placed=%1").arg(placed));

        bool inFence = false;
        for (const starfish::core::DesktopItem& item : container->fenceItems(idShots)) {
            if (item.path.compare(realPath, Qt::CaseInsensitive) == 0)
                inFence = true;
        }
        check(QStringLiteral("the fence really holds it"), inFence,
              QStringLiteral("%1 item(s) in the fence").arg(container->fenceItems(idShots).size()));

        check(QStringLiteral("filing it twice does nothing the second time"),
              container->applyRulesTo({realItem}) == 0,
              QStringLiteral("the item is already there"));

        const starfish::core::DesktopItem stranger =
            container->itemForPath(QDir(itemDir).absoluteFilePath(QStringLiteral("readme.md")));
        check(QStringLiteral("an item no rule wants is not filed"),
              container->applyRulesTo({stranger}) == 0,
              QStringLiteral("placed=0"));

        {
            // A rule naming a fence that no longer exists: stale, not fatal.
            starfish::core::Rule stale;
            stale.id = QStringLiteral("rule-stale");
            stale.extensions = {QStringLiteral("png")};
            stale.targetFenceId = QStringLiteral("fence-999");
            starfish::ui::FenceContainer throwaway;
            throwaway.setRules({stale});
            const int stalePlaced = throwaway.applyRulesTo({realItem});
            check(QStringLiteral("a rule pointing at a dead fence is refused"),
                  stalePlaced == 0, QStringLiteral("placed=%1 (expected 0)").arg(stalePlaced));
        }

        check(QStringLiteral("the file itself was not moved"),
              QFileInfo::exists(realPath),
              QStringLiteral("still at %1").arg(realPath));

        waitMs(250);
        const auto afterFiling = starfish::core::JsonStore::load(layoutPath, &loadError);
        bool persistedItem = false;
        if (afterFiling) {
            for (const starfish::core::Fence& fence : afterFiling->fences) {
                for (const starfish::core::FenceItem& entry : fence.items) {
                    if (entry.path.compare(realPath, Qt::CaseInsensitive) == 0)
                        persistedItem = true;
                }
            }
        }
        check(QStringLiteral("filing by rule is persisted"), persistedItem,
              afterFiling ? QStringLiteral("the item is in %1").arg(layoutPath) : loadError);

        // --- D. F-018 for real: a file appears in a watched folder ------------
        //
        // The files are named after this process, so the probe can never
        // overwrite something that was already there -- and it removes exactly
        // what it created, wherever it was pointed.
        QDir().mkpath(watchDir);
        const QString stamp = QString::number(QCoreApplication::applicationPid());
        const QString shotPath =
            QDir(watchDir).absoluteFilePath(QStringLiteral("starfish-rules-demo-%1-shot.png")
                                                .arg(stamp));
        const QString notesPath =
            QDir(watchDir).absoluteFilePath(QStringLiteral("starfish-rules-demo-%1-notes.txt")
                                                .arg(stamp));
        QFile::remove(shotPath);
        QFile::remove(notesPath);

        starfish::core::DesktopWatcher watcher;
        watcher.setFolders({QDir(watchDir).absolutePath()});
        watcher.setDebounceInterval(200);
        watcher.start();
        check(QStringLiteral("the watcher is watching the folder"), watcher.isWatching(),
              QStringLiteral("watching %1").arg(QDir(watchDir).absolutePath()));

        QStringList reported;
        int filedByWatcher = 0;
        QObject::connect(&watcher, &starfish::core::DesktopWatcher::itemsAdded, container,
                         [container, &reported, &filedByWatcher](const QStringList& paths) {
                             reported += paths;
                             QList<starfish::core::DesktopItem> items;
                             items.reserve(paths.size());
                             for (const QString& path : paths)
                                 items.append(container->itemForPath(path));
                             filedByWatcher += container->applyRulesTo(items);
                         });

        for (const QString& path : {shotPath, notesPath}) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                file.write("dropped by --rules-demo");
        }

        // Spin the event loop until the watcher has reported both files (the
        // debounce plus the round trip), or give up and say so.
        QElapsedTimer elapsed;
        elapsed.start();
        while (reported.size() < 2 && elapsed.elapsed() < 8000)
            waitMs(100);

        check(QStringLiteral("a file created in the watched folder is reported"),
              reported.size() == 2,
              QStringLiteral("%1 of 2 reported in %2 ms").arg(reported.size()).arg(elapsed.elapsed()));

        bool shotFiled = false;
        bool notesFiled = false;
        for (const starfish::core::DesktopItem& item : container->fenceItems(idShots)) {
            if (item.path.compare(shotPath, Qt::CaseInsensitive) == 0)
                shotFiled = true;
            if (item.path.compare(notesPath, Qt::CaseInsensitive) == 0)
                notesFiled = true;
        }
        check(QStringLiteral("the dropped file landed in the target fence"), shotFiled,
              QStringLiteral("%1 -> %2").arg(QFileInfo(shotPath).fileName(), idShots));
        check(QStringLiteral("only the matching file was filed"), filedByWatcher == 1 && !notesFiled,
              QStringLiteral("filed=%1, the .txt (too fresh for the age rule) is untouched")
                  .arg(filedByWatcher));
        check(QStringLiteral("the dropped files were not moved"),
              QFileInfo::exists(shotPath) && QFileInfo::exists(notesPath),
              QStringLiteral("both still in %1").arg(QDir(watchDir).absolutePath()));

        watcher.stop();
        const bool cleaned = QFile::remove(shotPath) && QFile::remove(notesPath);
        check(QStringLiteral("the probe removed the files it created"), cleaned,
              QStringLiteral("%1 and %2 are gone").arg(QFileInfo(shotPath).fileName(),
                                                       QFileInfo(notesPath).fileName()));

        const QString reportPath = QStringLiteral("build/rules_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QStringLiteral("layout file: %1\n").arg(layoutPath);
                stream << QStringLiteral("watched folder: %1%2\n")
                              .arg(QDir(watchDir).absolutePath(),
                                   customWatch ? QStringLiteral(" (--watch)")
                                               : QStringLiteral(" (scratch)"));
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n").arg(failures));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        if (!hosted) {
            QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
            return app.exec();
        }
        bool parsedSeconds = false;
        const int seconds = parser.value(secondsOption).toInt(&parsedSeconds);
        QTimer::singleShot((parsedSeconds && seconds > 0 ? seconds : 10) * 1000, &app,
                           [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(portalDemoOption)) {
        // T-011: folder portals (F-024). A portal mirrors a folder into a fence
        // -- the fence shows the folder's *contents*, the document stores the
        // folder *path*, and the mirror follows the folder. The folder itself is
        // never drawn as a tile, and nothing is ever moved into it: this is the
        // read half of F-024, the write half is T-035.
        const bool hosted = parser.isSet(hostOption);
        const QString layoutPath = parser.isSet(layoutOption)
                                       ? parser.value(layoutOption)
                                       : QStringLiteral("build/portal_demo_layout.json");
        const bool customFolder = parser.isSet(portalFolderOption);
        const QString folder = customFolder ? parser.value(portalFolderOption)
                                            : QStringLiteral("build/portal_demo_folder");

        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize bounds = screen ? screen->geometry().size() : QSize(1920, 1080);

        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -52)
                       .arg(detail);
        };
        auto waitMs = [](int ms) {
            QEventLoop loop;
            QTimer::singleShot(ms, &loop, &QEventLoop::quit);
            loop.exec();
        };
        auto waitUntil = [&waitMs](const std::function<bool()>& done, int timeoutMs) {
            QElapsedTimer timer;
            timer.start();
            while (!done() && timer.elapsed() < timeoutMs)
                waitMs(50);
            return done();
        };

        // Everything the probe creates is named after its own process, so it can
        // never overwrite something that was already there -- and it removes
        // exactly what it created, wherever it was pointed.
        const QString stamp = QString::number(QCoreApplication::applicationPid());
        QDir().mkpath(folder);
        const QString alphaName = QStringLiteral("starfish-portal-%1-alpha.txt").arg(stamp);
        const QString betaName = QStringLiteral("starfish-portal-%1-beta.png").arg(stamp);
        const QString gammaName = QStringLiteral("starfish-portal-%1-gamma.md").arg(stamp);
        const QString dotName = QStringLiteral(".starfish-portal-%1-hidden").arg(stamp);
        const QString subName = QStringLiteral("starfish-portal-%1-sub").arg(stamp);
        const QString alphaPath = QDir(folder).absoluteFilePath(alphaName);
        const QString betaPath = QDir(folder).absoluteFilePath(betaName);
        const QString gammaPath = QDir(folder).absoluteFilePath(gammaName);
        const QString dotPath = QDir(folder).absoluteFilePath(dotName);
        const QString subPath = QDir(folder).absoluteFilePath(subName);

        // A previous run may have been killed before its cleanup: start from a
        // known state instead of asserting on leftovers.
        for (const QString& path : {alphaPath, betaPath, gammaPath, dotPath})
            QFile::remove(path);
        QDir(folder).rmdir(subName);
        // What the folder held before the probe touched it, so the checks below
        // are about the probe's own entries -- --portal-folder may well point at
        // a folder that is already full.
        const int baseline =
            QDir(folder)
                .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                   | QDir::System,
                               QDir::NoSort)
                .size();

        auto writeFile = [](const QString& path, const QByteArray& payload) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            return file.write(payload) == payload.size();
        };
        const bool seeded = writeFile(alphaPath, "alpha") && writeFile(betaPath, "beta")
                            && writeFile(gammaPath, "gamma") && writeFile(dotPath, "dot")
                            && QDir(folder).mkpath(subName);

        // --- A. what a portal lists -------------------------------------------
        const starfish::core::Portal listing = starfish::core::listPortal(folder);
        check(QStringLiteral("the folder is listed"), seeded && listing.error.isEmpty(),
              listing.error.isEmpty() ? QStringLiteral("%1 item(s) in %2")
                                            .arg(listing.items.size())
                                            .arg(QDir(folder).absolutePath())
                                      : listing.error);

        QStringList listed;
        for (const starfish::core::DesktopItem& item : listing.items)
            listed.append(item.fileName);
        check(QStringLiteral("the listing is the folder's own entries"), listed.size() == 4,
              QStringLiteral("%1 (the dot entry is skipped by convention)").arg(listed.join(
                  QStringLiteral(", "))));
        check(QStringLiteral("the listing is sorted naturally"),
              listed == QStringList({alphaName, betaName, gammaName, subName}),
              QStringLiteral("%1").arg(listed.join(QStringLiteral(" < "))));

        const starfish::core::Portal missing =
            starfish::core::listPortal(QDir(folder).absoluteFilePath(QStringLiteral("unplugged")));
        check(QStringLiteral("a folder that is not there is reported, not fatal"),
              !missing.error.isEmpty() && missing.items.isEmpty(),
              QStringLiteral("\"%1\", 0 item(s)").arg(missing.error));

        check(QStringLiteral("the document entry kind is recognised"),
              starfish::core::isPortalKind(QStringLiteral("portal"))
                  && !starfish::core::isPortalKind(QStringLiteral("file")),
              QStringLiteral("\"portal\" -> portal, \"file\" -> file, \"\" -> file"));

        // --- B. the wiring: a fence mirrors the folder -------------------------
        starfish::core::LayoutStore store;
        store.setFilePath(layoutPath);
        store.setDebounceInterval(60);
        QFile::remove(layoutPath);
        int writes = 0;
        QObject::connect(&store, &starfish::core::LayoutStore::saved, &app,
                         [&writes](const QString&) { ++writes; });

        auto* container = new starfish::ui::FenceContainer;
        container->setDesktopBounds(bounds);
        container->setHostOnDesktop(hosted);
        container->setMinimumFenceSize(QSize(160, 120));
        {
            auto sync = [container, &store] {
                store.setLayout(container->toLayout());
                store.scheduleSave();
            };
            QObject::connect(container, &starfish::ui::FenceContainer::fenceCreated, container,
                             [sync](const QString&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::fenceRemoved, container,
                             [sync](const QString&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::fenceRectChanged, container,
                             [sync](const QString&, const QRect&) { sync(); });
            QObject::connect(container, &starfish::ui::FenceContainer::fenceContentChanged,
                             container, [sync](const QString&) { sync(); });
        }

        const QString idInbox = container->createFence(QStringLiteral("Boîte de réception"),
                                                       QRect(120, 120, 360, 300));

        // The product answers fenceContentChanged() by reading toLayout(), so the
        // document has to be consistent *when the signal fires*, not once the
        // call that emitted it has returned. This records what the auto-save
        // would have seen. (It did catch a real bug: the portal used to be
        // appended after its tiles, so the first read saw the folder's files as
        // plain items and persisted a frozen listing.)
        QByteArray documentWhenAnnounced;
        QObject::connect(container, &starfish::ui::FenceContainer::fenceContentChanged, container,
                         [container, &documentWhenAnnounced](const QString&) {
                             if (documentWhenAnnounced.isEmpty())
                                 documentWhenAnnounced =
                                     starfish::core::JsonStore::toJson(container->toLayout());
                         });

        const bool mirrored = container->addPortal(idInbox, folder);
        check(QStringLiteral("the fence mirrors the folder"), mirrored,
              QStringLiteral("addPortal(%1, %2)").arg(idInbox, QDir(folder).absolutePath()));

        const QList<starfish::core::Portal> portals = container->portals(idInbox);
        check(QStringLiteral("the portal is recorded with the path as declared"),
              portals.size() == 1 && portals.at(0).path == folder,
              portals.isEmpty() ? QStringLiteral("no portal")
                                : QStringLiteral("path=\"%1\"").arg(portals.at(0).path));

        QList<starfish::core::DesktopItem> tiles = container->fenceItems(idInbox);
        bool folderItselfShown = false;
        bool betaShown = false;
        for (const starfish::core::DesktopItem& item : tiles) {
            if (starfish::core::sameFolder(item.path, folder))
                folderItselfShown = true;
            if (item.path.compare(betaPath, Qt::CaseInsensitive) == 0)
                betaShown = true;
        }
        check(QStringLiteral("the fence shows the folder's contents"), tiles.size() == 4,
              QStringLiteral("%1 tile(s)").arg(tiles.size()));
        check(QStringLiteral("the folder itself is not drawn as a tile"), !folderItselfShown,
              QStringLiteral("no tile points at %1").arg(QDir(folder).absolutePath()));
        check(QStringLiteral("a file of the folder is really there"), betaShown,
              QStringLiteral("%1 is in the fence").arg(betaName));

        check(QStringLiteral("mirroring the same folder twice is refused"),
              !container->addPortal(idInbox, folder)
                  && container->portals(idInbox).size() == 1
                  && container->fenceItems(idInbox).size() == 4,
              QStringLiteral("still 1 portal, %1 tile(s)")
                  .arg(container->fenceItems(idInbox).size()));

        check(QStringLiteral("an unknown fence cannot mirror anything"),
              !container->addPortal(QStringLiteral("fence-999"), folder),
              QStringLiteral("addPortal(fence-999) refused"));

        // T-040: a portal's tiles are the *folder's* icons, not the fence's, so
        // they are the one thing a fence must not give back -- releasing one
        // would take it off the desktop only until the next refresh put it
        // straight back. Refused, and the fence keeps it.
        const int releasedMirror = container->releaseItems(idInbox, QStringList{betaPath});
        bool mirrorStillShown = false;
        for (const starfish::core::DesktopItem& item : container->fenceItems(idInbox)) {
            if (item.path.compare(betaPath, Qt::CaseInsensitive) == 0)
                mirrorStillShown = true;
        }
        check(QStringLiteral("a folder's own icon is not the fence's to give back"),
              releasedMirror == 0 && mirrorStillShown
                  && container->fenceItems(idInbox).size() == 4,
              QStringLiteral("%1 released, still listed: %2, %3 tile(s)")
                  .arg(releasedMirror)
                  .arg(mirrorStillShown ? QStringLiteral("yes") : QStringLiteral("no"))
                  .arg(container->fenceItems(idInbox).size()));

        check(QStringLiteral("the folders to watch are reported"),
              container->portalFolders() == QStringList{folder},
              QStringLiteral("%1").arg(container->portalFolders().join(QStringLiteral(" | "))));

        if (hosted) {
            check(QStringLiteral("the fence is hosted on the desktop"),
                  container->surface(idInbox) != nullptr,
                  container->surface(idInbox) ? QStringLiteral("surface attached")
                                              : QStringLiteral("no surface"));
            const QImage frame = container->frame(idInbox);
            check(QStringLiteral("the hosted fence rendered its frame"),
                  !frame.isNull() && frame.width() > 0,
                  QStringLiteral("frame %1x%2").arg(frame.width()).arg(frame.height()));
        }

        // --- C. the document --------------------------------------------------
        //
        // The document is read back through the parser rather than grepped as
        // bytes: what matters is the entry the format describes, not the
        // escaping a path happens to need.
        const starfish::core::Layout liveLayout = container->toLayout();
        const QByteArray document = starfish::core::JsonStore::toJson(liveLayout);
        int portalEntries = 0;
        int fileEntries = 0;
        bool portalPathMatches = false;
        for (const starfish::core::Fence& fence : liveLayout.fences) {
            for (const starfish::core::FenceItem& entry : fence.items) {
                if (starfish::core::isPortalKind(entry.kind)) {
                    ++portalEntries;
                    if (entry.path == folder)
                        portalPathMatches = true;
                } else {
                    ++fileEntries;
                }
            }
        }
        check(QStringLiteral("the document holds a portal entry"),
              portalEntries == 1 && fileEntries == 0 && portalPathMatches,
              QStringLiteral("%1 portal entry, %2 file entry, path as declared")
                  .arg(portalEntries)
                  .arg(fileEntries));
        check(QStringLiteral("the listing is NOT frozen into the document"),
              !document.contains(betaName.toUtf8()) && !document.contains(alphaName.toUtf8()),
              QStringLiteral("no file of the folder is persisted as an item"));

        {
            // What the auto-save read at the instant the change was announced.
            int announcedPortals = 0;
            int announcedFiles = 0;
            if (const auto announced = starfish::core::JsonStore::fromJson(documentWhenAnnounced)) {
                for (const starfish::core::Fence& fence : announced->fences) {
                    for (const starfish::core::FenceItem& entry : fence.items) {
                        if (starfish::core::isPortalKind(entry.kind))
                            ++announcedPortals;
                        else
                            ++announcedFiles;
                    }
                }
            }
            check(QStringLiteral("the document is consistent when the signal fires"),
                  announcedPortals == 1 && announcedFiles == 0,
                  QStringLiteral("%1 portal entry, %2 file entry as the auto-save saw it")
                      .arg(announcedPortals)
                      .arg(announcedFiles));
        }

        waitMs(250); // longer than the debounce interval
        check(QStringLiteral("the portal was auto-saved"), writes == 1,
              QStringLiteral("writes=%1 (expected 1 for the whole burst)").arg(writes));

        starfish::core::LayoutStore reopened;
        reopened.setFilePath(layoutPath);
        QString loadError;
        const bool loaded = reopened.load(&loadError);
        check(QStringLiteral("reload: the document loads"), loaded,
              loaded ? QStringLiteral("%1 fence(s)").arg(reopened.layout().fences.size())
                     : loadError);

        auto* restored = new starfish::ui::FenceContainer;
        restored->setDesktopBounds(bounds);
        restored->setHostOnDesktop(hosted);
        restored->setItemResolver([](const QString& path) {
            return starfish::core::itemFromPathOnly(path);
        });
        QString applyError;
        const bool applied = restored->applyLayout(reopened.layout(), &applyError);
        const QList<starfish::core::Portal> restoredPortals =
            applied ? restored->portals(idInbox) : QList<starfish::core::Portal>{};
        check(QStringLiteral("reload: the portal comes back"), applied && restoredPortals.size() == 1,
              applied ? QStringLiteral("%1 portal(s)").arg(restoredPortals.size()) : applyError);
        check(QStringLiteral("reload: the folder is listed again, from disk"),
              applied && restored->fenceItems(idInbox).size() == 4,
              QStringLiteral("%1 tile(s)").arg(restored->fenceItems(idInbox).size()));
        check(QStringLiteral("reload: the document round-trips byte for byte"),
              applied
                  && starfish::core::JsonStore::toJson(restored->toLayout())
                         == starfish::core::JsonStore::toJson(reopened.layout()),
              QStringLiteral("toLayout() == the loaded document"));

        {
            // A portal pointing at a folder that is not reachable (an unplugged
            // drive, an offline share) must survive a round trip: dropping it
            // would silently delete the user's portal.
            starfish::ui::FenceContainer offline;
            const QString idOffline =
                offline.createFence(QStringLiteral("Hors ligne"), QRect(0, 0, 320, 240));
            const QString goneFolder = QDir(folder).absoluteFilePath(QStringLiteral("unplugged"));
            const bool accepted = offline.addPortal(idOffline, goneFolder);
            const QByteArray offlineDoc =
                starfish::core::JsonStore::toJson(offline.toLayout());
            starfish::ui::FenceContainer reloaded;
            const bool offlineApplied =
                reloaded.applyLayout(starfish::core::JsonStore::fromJson(offlineDoc).value_or(
                                         starfish::core::Layout{}),
                                     nullptr);
            check(QStringLiteral("a portal to a missing folder is kept, not dropped"),
                  accepted && offlineApplied && reloaded.portals(idOffline).size() == 1
                      && reloaded.fenceItems(idOffline).isEmpty(),
                  QStringLiteral("accepted=%1, %2 portal(s), %3 tile(s)")
                      .arg(accepted ? QStringLiteral("yes") : QStringLiteral("no"))
                      .arg(reloaded.portals(idOffline).size())
                      .arg(reloaded.fenceItems(idOffline).size()));
        }
        delete restored;

        // --- D. the mirror follows the folder (F-018) --------------------------
        starfish::core::DesktopWatcher watcher;
        watcher.setFolders(container->portalFolders());
        watcher.setDebounceInterval(200);
        watcher.start();
        check(QStringLiteral("the portal's folder is watched"),
              watcher.isWatching()
                  && watcher.folders() == QStringList{QDir::cleanPath(QDir(folder).absolutePath())},
              QStringLiteral("watching %1").arg(watcher.folders().join(QStringLiteral(" | "))));

        int refreshed = 0;
        QObject::connect(&watcher, &starfish::core::DesktopWatcher::folderChanged, container,
                         [container, &refreshed](const QString& changed) {
                             refreshed += container->refreshPortalsFor(changed);
                         });

        const QString deltaPath = QDir(folder).absoluteFilePath(
            QStringLiteral("starfish-portal-%1-delta.txt").arg(stamp));
        QFile::remove(deltaPath);
        const int writesBeforeFolderChange = writes;
        const bool deltaWritten = writeFile(deltaPath, "delta");
        const bool sawDelta = waitUntil([&container, &idInbox] {
                                            return container->fenceItems(idInbox).size() == 5;
                                        },
                                        8000);
        bool deltaShown = false;
        for (const starfish::core::DesktopItem& item : container->fenceItems(idInbox)) {
            if (item.path.compare(deltaPath, Qt::CaseInsensitive) == 0)
                deltaShown = true;
        }
        check(QStringLiteral("a file added to the folder appears in the fence"),
              deltaWritten && sawDelta && deltaShown,
              QStringLiteral("%1 tile(s) after the change (refreshed %2 time(s))")
                  .arg(container->fenceItems(idInbox).size())
                  .arg(refreshed));

        const bool deltaRemoved = QFile::remove(deltaPath);
        const bool sawRemoval = waitUntil([&container, &idInbox] {
                                              return container->fenceItems(idInbox).size() == 4;
                                          },
                                          8000);
        check(QStringLiteral("a file removed from the folder leaves the fence"),
              deltaRemoved && sawRemoval,
              QStringLiteral("%1 tile(s) after the deletion")
                  .arg(container->fenceItems(idInbox).size()));

        waitMs(250);
        check(QStringLiteral("a folder change does not rewrite the document"),
              writes == writesBeforeFolderChange,
              QStringLiteral("writes=%1 (unchanged: only the folder path is persisted)")
                  .arg(writes));

        watcher.stop();

        // --- E. nothing was moved ---------------------------------------------
        const QFileInfoList after = QDir(folder).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
        check(QStringLiteral("the folder still holds exactly what the probe left"),
              after.size() == baseline + 5 && QFileInfo(subPath).isDir(),
              QStringLiteral("%1 entries (baseline %2 + 4 files + 1 subfolder)")
                  .arg(after.size())
                  .arg(baseline));

        // Every tile a portal contributes *is* the file, not a copy of it: the
        // mirror has no second copy of anything, anywhere.
        const QString folderAbs = QDir(folder).absolutePath();
        bool allInside = !container->fenceItems(idInbox).isEmpty();
        for (const starfish::core::DesktopItem& item : container->fenceItems(idInbox)) {
            if (QFileInfo(item.path).absolutePath().compare(folderAbs, Qt::CaseInsensitive) != 0)
                allInside = false;
        }
        check(QStringLiteral("the tiles point into the folder, nothing is copied"), allInside,
              QStringLiteral("%1 tile(s), all under %2")
                  .arg(container->fenceItems(idInbox).size())
                  .arg(folderAbs));

        // --- cleanup ----------------------------------------------------------
        const bool cleaned = QFile::remove(alphaPath) && QFile::remove(betaPath)
                             && QFile::remove(gammaPath) && QFile::remove(dotPath)
                             && QDir(folder).rmdir(subName);
        check(QStringLiteral("the probe removed what it created"), cleaned,
              QStringLiteral("%1 entries removed").arg(4));

        if (!customFolder) {
            // Only a scratch folder the probe made itself is removed, and only
            // if it is empty: a folder the user pointed at is never touched.
            const bool gone = QDir().rmdir(folder);
            log << QStringLiteral("%1  %2  %3")
                       .arg(QStringLiteral("INFO"), -6)
                       .arg(QStringLiteral("scratch folder"), -52)
                       .arg(gone ? QStringLiteral("removed")
                                 : QStringLiteral("kept (not empty)"));
        }

        const QString reportPath = QStringLiteral("build/portal_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QStringLiteral("layout file: %1\n").arg(layoutPath);
                stream << QStringLiteral("mirrored folder: %1%2\n")
                              .arg(QDir(folder).absolutePath(),
                                   customFolder ? QStringLiteral(" (--portal-folder)")
                                                : QStringLiteral(" (scratch)"));
                stream << QStringLiteral("hosted: %1\n")
                              .arg(hosted ? QStringLiteral("yes") : QStringLiteral("no"));
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n").arg(failures));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        if (!hosted) {
            QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
            return app.exec();
        }
        bool parsedSeconds = false;
        const int seconds = parser.value(secondsOption).toInt(&parsedSeconds);
        QTimer::singleShot((parsedSeconds && seconds > 0 ? seconds : 10) * 1000, &app,
                           [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(snapshotDemoOption)) {
        // T-012: snapshots (F-027) and export/import (F-028). A snapshot is a
        // named copy of the layout document; restore is applying that layout.
        // Export/import is the plain schema-1 round trip. Nothing here touches
        // the disk beyond the layout document and its snapshots directory.
        const QString layoutPath = parser.isSet(layoutOption)
                                       ? parser.value(layoutOption)
                                       : QString();
        const QString id = parser.isSet(snapshotIdOption)
                                ? parser.value(snapshotIdOption)
                                :QLatin1String("snapshot-demo");
        const QString note = parser.value(snapshotNoteOption);

        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize bounds = screen ? screen->geometry().size() : QSize(1920, 1080);

        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log <<QLatin1String("%1  %2  %3")
                       .arg(ok ?QLatin1String("PASS") :QLatin1String("FAIL"))
                       .arg(step, -52)
                       .arg(detail);
        };

        starfish::core::SnapshotManager m;
        starfish::core::Layout layout;
        layout.schema = 1;
        starfish::core::Fence f;
        f.id =QLatin1String("f-main");
        f.title =QLatin1String("Main");
        f.rect = QRect(0, 0, bounds.width() / 3, bounds.height() / 2);
        f.items.append({QLatin1String("file"),QLatin1String("C:/Desktop/a.txt")});
        layout.fences.append(f);

        const QString dir = layoutPath.isEmpty()
                                ? m.defaultSnapshotsDir()
                                : m.snapshotsDirFor(layoutPath);
        if (!QDir().mkpath(dir)) {
            log <<QLatin1String("FAIL  could not create snapshots directory  %1").arg(dir);
            ++failures;
        }

        const bool saved = m.saveSnapshot(dir, layout, id, note);
        check(QLatin1String("snapshot saved"), saved,
             QLatin1String("%1 -> %2").arg(id, m.snapshotFilePath(dir, id)));

        const QStringList listed = m.listSnapshots(dir);
        check(QLatin1String("snapshot is listed"), listed.contains(id),
             QLatin1String("%1 id(s)").arg(QString::number(listed.size())));

        starfish::core::Snapshot loaded;
        const bool loadedOk = m.loadSnapshot(dir, id, loaded);
        check(QLatin1String("snapshot loads"), loadedOk, {});
        if (loadedOk) {
            check(QLatin1String("loaded layout matches"), loaded.layout.fences.size() == 1
                  && loaded.layout.fences.at(0).id ==QLatin1String("f-main"),
                 QLatin1String("%1 fence(s)").arg(QString::number(loaded.layout.fences.size())));
            check(QLatin1String("loaded note is preserved"),
                  note.isEmpty() ? loaded.note.isEmpty() : loaded.note == note,
                 QLatin1String("note=%1").arg(loaded.note));

            // F-027's "restore" is applying the snapshot's layout to the live
            // fences -- that is what the product does, and nothing else. Prove it
            // rebuilds the fence with the same id and the same rect.
            starfish::ui::FenceContainer restored;
            QString restoreError;
            const bool restoreOk = restored.applyLayout(loaded.layout, &restoreError);
            check(QLatin1String("restore rebuilds the fence"),
                  restoreOk && restored.fenceCount() == 1
                      && restored.fenceIds().value(0) ==QLatin1String("f-main")
                      && restored.fenceRect(QLatin1String("f-main")) == layout.fences.at(0).rect,
                 restoreOk ?QLatin1String("%1 fence(s), %2")
                                 .arg(QString::number(restored.fenceCount()))
                                 .arg(restored.fenceIds().join(QLatin1Char(',')))
                           : restoreError);
        }

        const QString exportPath = dir +QLatin1String("/snapshot-demo-export.json");
        const bool exported = m.exportLayout(layout, exportPath);
        check(QLatin1String("layout exported"), exported,
             QLatin1String("-> %1").arg(exportPath));
        if (exported) {
            const auto imported = m.importLayout(exportPath);
            check(QLatin1String("exported layout re-imports"),
                  imported.has_value() && imported->fences.size() == 1,
                 QLatin1String("%1 fence(s)").arg(QString::number(
                     imported.has_value() ? imported->fences.size() : qsizetype(-1))));
        }

        const bool removed = m.removeSnapshot(dir, id);
        check(QLatin1String("snapshot removed"), removed, {});

        // Leave nothing behind: the probe wrote a snapshot (now removed) and an
        // exported file in the snapshots directory -- which, with no --layout, is
        // the user's real %APPDATA%/StarFish/snapshots. A probe must not leave
        // junk in a real profile.
        const bool exportCleaned = !QFileInfo::exists(exportPath) || QFile::remove(exportPath);
        check(QLatin1String("export file cleaned up"), exportCleaned, {});
        check(QLatin1String("snapshot file is gone"),
              !QFileInfo::exists(m.snapshotFilePath(dir, id)), {});

        const QString reportPath =QLatin1String("build/snapshot_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream <<QLatin1String("layout document: %1\n").arg(layoutPath.isEmpty()
                                  ?QLatin1String("(default, not written)")
                                  : layoutPath);
                stream <<QLatin1String("snapshots directory: %1\n").arg(dir);
                stream <<QLatin1String("id: %1\n").arg(id);
                stream <<QLatin1String("note: %1\n").arg(note.isEmpty()
                                  ?QLatin1String("(none)") : note);
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ?QLatin1String("RESULT: all checks passed\n")
                               :QLatin1String("RESULT: %1 check(s) FAILED\n").arg(QString::number(failures)));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() <<QLatin1String("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
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
                // T-006-3: accept files dropped from the Explorer onto the hosted
                // surface and append them to the fence. The OLE drop target reports
                // the drop point in surface client coordinates, which match the
                // fence's local coordinates (rendered at (0,0) of the frame).
                surface->setDropHandler([fence](const QPoint&, const QStringList& paths) {
                    QList<starfish::core::DesktopItem> items;
                    items.reserve(paths.size());
                    for (const QString& p : paths) {
                        const starfish::core::DesktopItem item =
                            starfish::platform::win::DesktopScanner::itemForPath(p);
                        if (!item.path.isEmpty())
                            items.append(item);
                    }
                    if (!items.isEmpty())
                        fence->addDroppedItems(items);
                });
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

    if (parser.isSet(productProbeOption)) {
        // T-038: the product sequence, headless. Hosting, tray and hot keys are
        // off on purpose so the probe runs anywhere; what it exercises is exactly
        // what a real launch does -- read the document, apply it to the fences,
        // materialise a first-launch fence, auto-save, rename, roll up, delete,
        // then start again and restore.
        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QLatin1String("%1  %2  %3")
                       .arg(ok ? QLatin1String("PASS") : QLatin1String("FAIL"))
                       .arg(step, -54)
                       .arg(detail);
        };

        const QString probeDir = QLatin1String("build/product_probe");
        QDir().mkpath(probeDir);
        const QString layoutPath = probeDir + QLatin1String("/layout.json");
        QFile::remove(layoutPath);

        starfish::app::ProductApp::Options options;
        options.layoutPath = layoutPath;
        options.hostOnDesktop = false;
        options.tray = false;
        options.settingsWindow = true;
        options.showSettingsOnStart = false;
        options.hotkeys = false;
        options.watchDesktop = false;
        options.starterFence = true;

        {
            starfish::app::ProductApp product;
            QString startError;
            const bool started = product.start(options, &startError);
            check(QLatin1String("the product starts"), started,
                  started ? product.statusText() : startError);

            if (started) {
                check(QLatin1String("a first launch materialises one fence"),
                      product.container()->fenceCount() == 1,
                      QLatin1String("count=%1")
                          .arg(QString::number(product.container()->fenceCount())));
                // ADR-13: the launch leaves the desktop alone -- nothing is
                // hidden, and the fences are clickable anyway (they sit above the
                // wallpaper, and the surface steps the mouse aside over the
                // user's icons). T-056's "the launch claims the desktop" is gone
                // with the behaviour it described; the dev path that still does it
                // is checked just below.
                check(QLatin1String("the launch leaves the desktop's icons alone"),
                      !product.desktopIconsHidden(), product.statusText());
                check(QLatin1String("the settings window exists"),
                      product.settingsDialog() != nullptr, {});
                check(QLatin1String("no tray was created (none was asked for)"),
                      product.tray() == nullptr, {});

                const QString id = product.container()->fenceIds().value(0);
                product.container()->setFenceTitle(id, QLatin1String("Renamed"));
                check(QLatin1String("renaming a fence takes"),
                      product.container()->fenceTitle(id) == QLatin1String("Renamed"),
                      product.container()->fenceTitle(id));

                product.container()->setFenceRolledUp(id, true);
                check(QLatin1String("rolling a fence up takes"),
                      product.container()->fenceRolledUp(id), {});
                product.container()->setFenceRolledUp(id, false);

                // The icon toggle is tracked even without hosting, and never
                // claims a state the desktop is not in.
                product.setDesktopIconsHidden(true);
                check(QLatin1String("the desktop icon toggle is remembered"),
                      product.desktopIconsHidden(), product.statusText());
                product.setDesktopIconsHidden(false);
                check(QLatin1String("the desktop icon toggle is reversible"),
                      !product.desktopIconsHidden(), {});

                const QString secondId = product.createFenceWithDefaultPlacement();
                check(QLatin1String("a new fence does not land under another one"),
                      !secondId.isEmpty()
                          && !product.container()->fenceRect(secondId).intersects(
                                 product.container()->fenceRect(id)),
                      QLatin1String("new fence at x=%1")
                          .arg(QString::number(product.container()->fenceRect(secondId).x())));

                // T-066: and it keeps working once the first row is full. This used
                // to be "to the right of everything, wrapping to the same row start",
                // and the wrap put every later fence on the same point -- measured on
                // the user's own document: three fences at (80, 1200), stacked on
                // each other at the bottom of the screen and looking like one fence.
                QList<QRect> placed;
                placed.append(product.container()->fenceRect(id));
                placed.append(product.container()->fenceRect(secondId));
                QStringList spawned;
                for (int i = 0; i < 4; ++i) {
                    const QString made = product.createFenceWithDefaultPlacement();
                    if (made.isEmpty())
                        continue;
                    spawned.append(made);
                    placed.append(product.container()->fenceRect(made));
                }
                int overlapping = 0;
                int outside = 0;
                bool duplicated = false;
                const QRect desktop(QPoint(0, 0), product.container()->desktopBounds());
                for (int i = 0; i < placed.size(); ++i) {
                    if (!desktop.contains(placed.at(i)))
                        ++outside;
                    for (int j = i + 1; j < placed.size(); ++j) {
                        if (placed.at(i) == placed.at(j))
                            duplicated = true;
                        if (placed.at(i).intersects(placed.at(j)))
                            ++overlapping;
                    }
                }
                check(QLatin1String("every fence created lands somewhere of its own"),
                      placed.size() == 6 && !duplicated && overlapping == 0 && outside == 0,
                      QLatin1String("%1 fence(s): %2 duplicate(s), %3 overlapping pair(s), "
                                    "%4 outside the desktop")
                          .arg(QString::number(placed.size()))
                          .arg(duplicated ? 1 : 0)
                          .arg(QString::number(overlapping))
                          .arg(QString::number(outside)));

                // Back to one fence: the checks below describe the document.
                for (const QString& extra : spawned)
                    product.container()->removeFence(extra);

                check(QLatin1String("deleting a fence works"),
                      product.container()->removeFence(secondId)
                          && product.container()->fenceCount() == 1, {});

                QString saveError;
                const bool saved = product.store()->saveNow(&saveError);
                check(QLatin1String("the document is written"),
                      saved && QFileInfo::exists(layoutPath), saved ? layoutPath : saveError);

                const auto document = starfish::core::JsonStore::load(layoutPath, nullptr);
                check(QLatin1String("the document holds exactly one fence"),
                      document.has_value() && document->fences.size() == 1,
                      document.has_value()
                          ? QLatin1String("fences=%1")
                                .arg(QString::number(document->fences.size()))
                          : QLatin1String("unreadable"));
                check(QLatin1String("the rename reached the document"),
                      document.has_value()
                          && document->fences.at(0).title == QLatin1String("Renamed"),
                      document.has_value() ? document->fences.at(0).title : QString());

                // F-038 / T-057: the material as the *product* applies it. The
                // tray's "apply to every fence" and the settings window's
                // per-fence choice both end up in these two calls, and what they
                // do is a style change -- so it is the document that carries the
                // material, not the session, and the second launch below is what
                // proves it.
                product.applyMaterialToAllFences(QLatin1String("very-translucent"));
                check(QLatin1String("a material reaches every fence"),
                      product.sharedFenceMaterial() == QLatin1String("very-translucent")
                          && product.container()->fenceStyle(id).background
                                 == QLatin1String("#801F2937"),
                      QLatin1String("material %1, colour %2")
                          .arg(product.sharedFenceMaterial(),
                               product.container()->fenceStyle(id).background));

                // Two fences that disagree must not have the tray claim one of
                // them: the product reports "mixed" instead of guessing.
                const QString mixedId = product.createFenceWithDefaultPlacement();
                product.applyMaterialToFence(mixedId, QLatin1String("opaque"));
                check(QLatin1String("a mixed desktop is reported as mixed"),
                      product.sharedFenceMaterial().isEmpty(),
                      QLatin1String("shared material [%1] with %2 fences")
                          .arg(product.sharedFenceMaterial())
                          .arg(QString::number(product.container()->fenceCount())));
                product.container()->removeFence(mixedId);

                // ... and the settings window, which is the per-fence choice. Its
                // two widgets are driven the way a user drives them (selecting a
                // row, picking an entry), so what is measured is the window's
                // *wiring* -- including the part that must **not** travel: putting
                // the document's state on screen is not a choice, and a refresh
                // that looked like one would rewrite the document on every change.
                int contentChanges = 0;
                QObject::connect(product.container(),
                                 &starfish::ui::FenceContainer::fenceContentChanged, &product,
                                 [&contentChanges](const QString&) { ++contentChanges; });
                auto* fenceList = product.settingsDialog()->findChild<QListWidget*>(
                    QLatin1String("fenceList"));
                auto* materialCombo = product.settingsDialog()->findChild<QComboBox*>(
                    QLatin1String("fenceMaterial"));

                const int changesBefore = contentChanges;
                product.settingsDialog()->refresh();
                if (fenceList && fenceList->count() > 0)
                    fenceList->setCurrentRow(0); // the user picking a fence
                check(QLatin1String("the settings window shows the fence's material"),
                      materialCombo && materialCombo->count() > 0
                          && materialCombo->currentData().toString()
                                 == QLatin1String("very-translucent"),
                      QLatin1String("combo=%1 of %2, row selected=%3, shows %4")
                          .arg(QString::number(materialCombo ? materialCombo->currentIndex() : -9))
                          .arg(QString::number(materialCombo ? materialCombo->count() : -1))
                          .arg(QString::number(fenceList ? fenceList->currentRow() : -9))
                          .arg(materialCombo ? materialCombo->currentText()
                                             : QLatin1String("(no combo)")));
                check(QLatin1String("showing the state is not a choice"),
                      contentChanges == changesBefore,
                      QLatin1String("%1 document change(s) for a refresh and a selection")
                          .arg(QString::number(contentChanges - changesBefore)));

                const int opaqueIndex =
                    materialCombo ? materialCombo->findData(QLatin1String("opaque")) : -1;
                if (materialCombo && opaqueIndex >= 0)
                    materialCombo->setCurrentIndex(opaqueIndex); // exactly what a click does
                check(QLatin1String("a material chosen in the settings window takes"),
                      opaqueIndex >= 0
                          && product.sharedFenceMaterial() == QLatin1String("opaque"),
                      product.statusText());
                product.applyMaterialToAllFences(QLatin1String("very-translucent"));
                product.store()->saveNow(nullptr);
            }

            product.shutdown();
        }

        // Starting again must restore what was there, not start over.
        {
            options.starterFence = false;
            starfish::app::ProductApp second;
            QString startError;
            const bool started = second.start(options, &startError);
            check(QLatin1String("an existing document is restored"), started,
                  started ? second.statusText() : startError);
            check(QLatin1String("the restored fence keeps its title"),
                  started && second.container()->fenceCount() == 1
                      && second.container()->fenceTitle(
                             second.container()->fenceIds().value(0))
                             == QLatin1String("Renamed"),
                  started ? second.container()->fenceIds().join(QLatin1Char(',')) : startError);
            // F-038 / T-057, the other end of the material: it was written to the
            // document, so it comes back with it -- and the status line, which is
            // the only place the product describes itself in words, says so.
            check(QLatin1String("the restored fence keeps its material"),
                  started && second.sharedFenceMaterial() == QLatin1String("very-translucent")
                      && second.statusText().contains(QLatin1String("Very translucent")),
                  started ? second.statusText() : startError);
            second.shutdown();
        }

        // Leave nothing behind: the probe owns this directory and only removes it.
        QDir(probeDir).removeRecursively();
        check(QLatin1String("the probe cleaned up after itself"),
              !QFileInfo::exists(layoutPath), probeDir);

        const QString reportPath = QLatin1String("build/product_probe.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QLatin1String("product probe (T-038)\n");
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QLatin1String("RESULT: all checks passed\n")
                               : QLatin1String("RESULT: %1 check(s) FAILED\n")
                                     .arg(QString::number(failures)));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QLatin1String("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(fenceMenuProbeOption)) {
        // T-076: the one claim no other probe makes -- that the fence's own menu is
        // **on the screen**.
        //
        // Session 46 paid for the lesson (T-079): a window can exist, be visible to
        // the window manager, print its own content back through PrintWindow and
        // still not be composed. So nothing here is read from Qt's state alone. One
        // fence is hosted on the real desktop, a real right-click is posted to its
        // surface (the message the mouse sends), and while the menu is up the
        // *screen* is asked -- pixels, and the system's own hit test -- before the
        // menu is dismissed and the same questions are asked again.
        //
        // The document is a throwaway (`build/fence_menu_probe/layout.json`): a
        // probe must not write to the user's own fences.
        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -52)
                       .arg(detail);
        };

        const QString probeDir = QStringLiteral("build/fence_menu_probe");
        QDir().mkpath(probeDir);
        const QString layoutPath = probeDir + QStringLiteral("/layout.json");
        QFile::remove(layoutPath);

#if !defined(_WIN32)
        check(QStringLiteral("this probe needs a real Windows desktop"), false,
              QStringLiteral("not Windows"));
#else
        starfish::app::ProductApp::Options options;
        options.layoutPath = layoutPath;
        options.hostOnDesktop = true;
        options.tray = false;
        options.settingsWindow = false;
        options.showSettingsOnStart = false;
        options.hotkeys = false;
        options.watchDesktop = false;
        options.starterFence = true;

        starfish::app::ProductApp product;
        QString startError;
        const bool started = product.start(options, &startError);
        check(QStringLiteral("the product runs with a fence on the real desktop"),
              started && product.container()->fenceCount() >= 1,
              started ? product.statusText() : startError);

        if (!started || product.container()->fenceCount() < 1) {
            log << QStringLiteral("the probe stops here: it needs one hosted fence");
        } else {
            const QString id = product.container()->fenceIds().value(0);
            // A fixed spot on the primary screen: the probe compares screen captures,
            // and the primary screen is the one they cover. Moved without snapping so
            // the rect is the one asked for.
            const QRect desktopBounds = starfish::app::ProductApp::screenLayout().bounds();
            const QRect spot(200, 200, 320, 240);
            product.container()->setFenceRect(id, spot, /*snap=*/false);
            check(QStringLiteral("the fence sits where the probe can see it"),
                  desktopBounds.contains(spot),
                  QStringLiteral("fence at %1,%2 %3x%4, desktop %5x%6")
                      .arg(spot.x())
                      .arg(spot.y())
                      .arg(spot.width())
                      .arg(spot.height())
                      .arg(desktopBounds.width())
                      .arg(desktopBounds.height()));

            starfish::platform::win::DesktopSurface* surface = product.container()->surface(id);
            const HWND fenceHwnd = surface ? reinterpret_cast<HWND>(surface->hwnd()) : nullptr;
            check(QStringLiteral("the fence is hosted -- it has a window of its own (the popup's "
                                 "owner)"),
                  fenceHwnd != nullptr,
                  fenceHwnd ? QStringLiteral("hwnd %1")
                                  .arg(static_cast<qulonglong>(
                                      reinterpret_cast<quintptr>(fenceHwnd)),
                                       0, 16)
                            : QStringLiteral("no surface: hosting failed"));

            if (fenceHwnd) {
                // The right-click is delivered as a *message*, like the mouse does:
                // WM_RBUTTONDOWN at the fence's own client coordinates (160,15 --
                // inside the title bar, clear of the resize grips), then the release,
                // so the surface's capture bookkeeping is symmetric.
                constexpr int kClickX = 160;
                constexpr int kClickY = 15;
                const LPARAM clickPos = MAKELPARAM(kClickX, kClickY);

                QImage before;
                QImage during;
                QImage after;
                HWND popup = nullptr;
                QRect popupRect;
                bool popupVisible = false;
                bool popupIsMenu = false;
                qint64 pixelsThatAppeared = 0;
                qint64 pixelsLeftBehind = 0;
                bool menusPennyDropped = false; // WindowFromPoint answered with the popup

                QTimer::singleShot(1500, &app, [&] {
                    // The fence has painted by now (and the native icon pass has
                    // settled): this is the baseline the menu must stand out from.
                    before = grabPrimaryScreen();
                    const POINT at{spot.x() + kClickX, spot.y() + kClickY};
                    const HWND beforeHit = ::WindowFromPoint(at);
                    log << QStringLiteral("before the click: %1x%2 capture, WindowFromPoint %3 "
                                          "at %4,%5")
                               .arg(before.width())
                               .arg(before.height())
                               .arg(static_cast<qulonglong>(
                                        reinterpret_cast<quintptr>(beforeHit)),
                                    0, 16)
                               .arg(at.x)
                               .arg(at.y);
                    ::PostMessageW(fenceHwnd, WM_RBUTTONDOWN, MK_RBUTTON, clickPos);
                    ::PostMessageW(fenceHwnd, WM_RBUTTONUP, 0, clickPos);

                    // Fires *inside* the menu's own event loop: that is the only
                    // moment the popup exists, and the only way to see it.
                    QTimer::singleShot(900, &app, [&] {
                        during = grabPrimaryScreen();
                        if (QWidget* active = QApplication::activePopupWidget()) {
                            popupVisible = active->isVisible();
                            popupIsMenu = active->inherits("QMenu");
                            popup = reinterpret_cast<HWND>(active->winId());
                            RECT rect{};
                            if (popup && ::GetWindowRect(popup, &rect)) {
                                popupRect = QRect(QPoint(rect.left, rect.top),
                                                  QPoint(rect.right - 1, rect.bottom - 1));
                            }
                            log << QStringLiteral("with the menu up: %1 (%2), hwnd %3, rect "
                                                  "%4,%5 %6x%7, win32-visible %8")
                                       .arg(QString::fromLatin1(active->metaObject()->className()))
                                       .arg(popupIsMenu ? QStringLiteral("a QMenu")
                                                        : QStringLiteral("not a QMenu"))
                                       .arg(popup ? QLatin1String("yes") : QLatin1String("no"))
                                       .arg(popupRect.x())
                                       .arg(popupRect.y())
                                       .arg(popupRect.width())
                                       .arg(popupRect.height())
                                       .arg(popup && ::IsWindowVisible(popup) ? 1 : 0);
                            if (!popupRect.isEmpty() && popup) {
                                const POINT centre{popupRect.x() + popupRect.width() / 2,
                                                   popupRect.y() + popupRect.height() / 2};
                                menusPennyDropped =
                                    ::WindowFromPoint(centre) == popup;
                                const QRect clipped = popupRect.intersected(
                                    QRect(QPoint(0, 0), during.size()));
                                if (!clipped.isEmpty() && !before.isNull()
                                    && before.size() == during.size()) {
                                    diffBounds(before.copy(clipped), during.copy(clipped),
                                               &pixelsThatAppeared);
                                }
                                during.save(probeDir + QStringLiteral("/menu_up.png"));
                            }
                            // Dismissed the way a click elsewhere would.
                            active->close();
                        } else {
                            log << QStringLiteral("with the menu up: no popup widget at all -- "
                                                  "StarFish's menu never opened");
                        }

                        QTimer::singleShot(700, &app, [&] {
                            after = grabPrimaryScreen();
                            if (!popupRect.isEmpty() && !before.isNull()
                                && before.size() == after.size()) {
                                const QRect clipped =
                                    popupRect.intersected(QRect(QPoint(0, 0), after.size()));
                                if (!clipped.isEmpty()) {
                                    diffBounds(before.copy(clipped), after.copy(clipped),
                                               &pixelsLeftBehind);
                                }
                            }
                            after.save(probeDir + QStringLiteral("/menu_gone.png"));

                            const qint64 area = qint64(popupRect.width()) * popupRect.height();
                            check(QStringLiteral("a right-click on a fence opens StarFish's own "
                                                 "menu"),
                                  popupVisible && popupIsMenu,
                                  popupIsMenu
                                      ? QStringLiteral("a visible QMenu, hwnd %1")
                                            .arg(static_cast<qulonglong>(
                                                     reinterpret_cast<quintptr>(popup)),
                                                 0, 16)
                                      : QStringLiteral("no visible QMenu came up (the click "
                                                       "never reached a menu)"));
                            check(QStringLiteral("the menu's pixels are on the screen (not just a "
                                                 "widget that exists)"),
                                  !popupRect.isEmpty() && pixelsThatAppeared > 0
                                      && pixelsThatAppeared * 5 > area * 1,
                                  QStringLiteral("%1 of the %2 pixel(s) of the menu's rect changed "
                                                 "(%3%)")
                                      .arg(pixelsThatAppeared)
                                      .arg(area)
                                      .arg(area > 0 ? (pixelsThatAppeared * 100 / area) : 0));
                            check(QStringLiteral("the point inside the menu belongs to the menu"),
                                  menusPennyDropped,
                                  menusPennyDropped
                                      ? QStringLiteral("WindowFromPoint answers our own popup -- the "
                                                       "menu owns the mouse, as a menu must")
                                      : QStringLiteral("WindowFromPoint answers someone else: the "
                                                       "menu is under something"));
                            check(QStringLiteral("dismissing it gives the pixels back"),
                                  !popupRect.isEmpty() && pixelsLeftBehind == 0,
                                  QStringLiteral("%1 of the menu's %2 pixel(s) still differ from the "
                                                 "baseline after the menu was closed")
                                      .arg(pixelsLeftBehind)
                                      .arg(area));
                            check(QStringLiteral("the fence is still there when the menu is gone"),
                                  product.container()->contains(id)
                                      && product.container()->fenceCount() >= 1,
                                  QStringLiteral("%1 fence(s) left")
                                      .arg(product.container()->fenceCount()));

                            const QString reportPath =
                                QStringLiteral("build/fence_menu_probe.txt");
                            QFile report(reportPath);
                            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate
                                            | QIODevice::Text)) {
                                QTextStream stream(&report);
                                for (const QString& line : log)
                                    stream << line << '\n';
                                stream << (failures == 0
                                               ? QStringLiteral("RESULT: all checks passed\n")
                                               : QStringLiteral("RESULT: %1 check(s) FAILED\n")
                                                     .arg(failures));
                            }
                            for (const QString& line : log)
                                qCInfo(lc).noquote() << line;
                            qCInfo(lc).noquote()
                                << QStringLiteral("report written to %1").arg(reportPath);
                            qCInfo(lc).noquote()
                                << QStringLiteral("the menu was opened and closed by the probe; "
                                                  "the fence it lived on is a throwaway document "
                                                  "(%1)")
                                       .arg(layoutPath);
                            QCoreApplication::exit(failures == 0 ? 0 : 1);
                        });
                    });
                });
            }
        }
#endif
        return app.exec();
    }

    if (parser.isSet(iconsDemoOption)) {
        // T-066: the product must not take the desktop's icons away -- not at
        // launch, not when a fence holds one of them, and not ever.
        //
        // The case that used to trigger it is the *first icon a fence holds*: the
        // product hid SysListView32 and drew the icons itself, and the icons it
        // drew could not be clicked, dragged or right-clicked (T-042 is unwritten),
        // so the desktop stopped answering the mouse. The user's report:
        // "starfish ne masque pas les icônes du bureau, affiche les fences sur le
        // bureau normal, et que ce soit comme ça dès le démarrage".
        //
        // Everything below is measured from *outside* the product: the shell's own
        // list view, the marker file, and the hit test Windows itself would use.
        // "The icons are still there" is a claim about the desktop, not about our
        // model, so the model is not asked.
        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -58)
                       .arg(detail);
        };

#if !defined(_WIN32)
        check(QStringLiteral("this probe needs a real Windows desktop"), false,
              QStringLiteral("not Windows"));
#else
        // The premise, checked first: the desktop's icons are the shell's, and they
        // can be read. A probe that measured "the icons are still visible" on a
        // desktop whose icons were already hidden would pass while measuring
        // nothing.
        const bool visibleBefore = starfish::platform::win::nativeDesktopIconsVisible();
        const QVector<starfish::platform::win::NativeDesktopIcon> iconsBefore =
            visibleBefore ? starfish::platform::win::readNativeDesktopIcons()
                          : QVector<starfish::platform::win::NativeDesktopIcon>{};
        const starfish::core::IconHidingMarker marker;
        check(QStringLiteral("the desktop's icons are the shell's to begin with"),
              visibleBefore && iconsBefore.size() > 1 && !marker.isPresent(),
              QStringLiteral("SysListView32 visible=%1, %2 icon(s) readable, marker %3")
                  .arg(visibleBefore ? 1 : 0)
                  .arg(iconsBefore.size())
                  .arg(marker.isPresent() ? marker.description() : QStringLiteral("absent")));

        if (!visibleBefore || iconsBefore.isEmpty()) {
            log << QStringLiteral("the probe stops here: it will not measure a desktop it cannot "
                                  "read (hide nothing, and re-run with the icons shown)");
        } else {
            const QString probeDir = QStringLiteral("build/icons_demo");
            QDir().mkpath(probeDir);
            const QString layoutPath = probeDir + QStringLiteral("/layout.json");
            QFile::remove(layoutPath);

            // A fence holding one of the desktop's *real* items: that is what makes
            // the fence "hold" an icon, and it is the exact state that used to take
            // the desktop over. The fence itself goes to a free spot, found from the
            // icons' own rectangles, so it covers none of them.
            QString scanError;
            const QList<starfish::core::DesktopItem> desktopItems =
                starfish::platform::win::DesktopScanner::scan(&scanError);
            QString heldPath;
            QVector<QRect> iconRects;
            iconRects.reserve(iconsBefore.size());
            for (const starfish::platform::win::NativeDesktopIcon& icon : iconsBefore)
                iconRects.append(QRect(icon.position, icon.slot));
            for (const starfish::core::DesktopItem& item : desktopItems) {
                if (item.path.isEmpty())
                    continue;
                heldPath = item.path; // the first real file/folder on the desktop
                break;
            }

            const QRect desktopBounds = starfish::app::ProductApp::screenLayout().bounds();
            const QRect fenceRect = starfish::ui::FenceGeometry::freeSpot(
                iconRects, QSize(320, 240),
                desktopBounds.adjusted(80, 80, -80, -80));

            starfish::core::Layout seed;
            starfish::core::Fence fence;
            fence.id = QStringLiteral("fence-demo");
            fence.title = QStringLiteral("holds one of your icons");
            fence.rect = fenceRect;
            starfish::core::FenceItem held;
            held.kind = QStringLiteral("file");
            held.path = heldPath;
            fence.items.append(held);
            seed.fences.append(fence);
            QString seedError;
            const bool seeded = !heldPath.isEmpty()
                && starfish::core::JsonStore::save(seed, layoutPath, &seedError);
            check(QStringLiteral("a throwaway document holds one real desktop icon"),
                  seeded,
                  seeded ? QStringLiteral("%1 at %2,%3 %4x%5")
                               .arg(heldPath)
                               .arg(fenceRect.x())
                               .arg(fenceRect.y())
                               .arg(fenceRect.width())
                               .arg(fenceRect.height())
                         : seedError);

            if (!seeded) {
                log << QStringLiteral("the probe stops here: it needs a document whose fence holds "
                                      "a real desktop item");
            } else {
                starfish::app::ProductApp::Options options;
                options.layoutPath = layoutPath;
                options.hostOnDesktop = true;
                options.tray = false;
                options.settingsWindow = false;
                options.hotkeys = false;
                options.watchDesktop = false;
                options.starterFence = false;

                {
                    starfish::app::ProductApp product;
                    QString startError;
                    const bool started = product.start(options, &startError);
                    check(QStringLiteral("the product starts with a fence that holds an icon"),
                          started && product.container()->fenceCount() == 1,
                          started ? product.statusText() : startError);

                    if (started) {
                        const QString id = product.container()->fenceIds().value(0);
                        auto* surface = product.container()->surface(id);
                        const QImage frame = product.container()->frame(id);
                        check(QStringLiteral("the fence is hosted and drawn on the normal desktop"),
                              surface && surface->isAttached() && !frame.isNull(),
                              QStringLiteral("hosted=%1, frame %2x%3, at %4,%5")
                                  .arg(surface && surface->isAttached() ? 1 : 0)
                                  .arg(frame.width())
                                  .arg(frame.height())
                                  .arg(fenceRect.x())
                                  .arg(fenceRect.y()));

                        // The fence answers for itself where it is: the mouse finds
                        // *it* there, so a fence on a normal desktop is usable even
                        // though the icons underneath are still the shell's.
                        if (surface) {
                            const QPoint centre = fenceRect.center() - fenceRect.topLeft();
                            const auto answer =
                                ::SendMessageW(reinterpret_cast<HWND>(surface->hwnd()),
                                               WM_NCHITTEST, 0,
                                               MAKELPARAM(centre.x(), centre.y()));
                            check(QStringLiteral("the fence itself answers the mouse"),
                                  answer == HTCLIENT,
                                  QStringLiteral("WM_NCHITTEST at the fence's centre -> %1")
                                      .arg(answer));
                        }

                        // ⚠️ T-071 / ADR-14 -- and the claim is now a structural
                        // one, not a hit-test answer: the product hosts **no window
                        // of ours over the desktop at all**. Measured from outside:
                        // of Progman's children, only the fences in the document are
                        // ours, and none of them covers the screen. That is what
                        // gives the icons their clicks *and* their drags (a drag is
                        // a drop, and a drop needs a target under the cursor: our
                        // window registered none on purpose, T-052, so an icon could
                        // be picked up and never put down), the shell's own menu and
                        // its rubber band.
                        starfish::ui::DesktopBackdrop* backdrop = product.container()->backdrop();
                        const bool backdropOff = !backdrop || !backdrop->enabled();
                        const HWND progman = ::FindWindowW(L"Progman", nullptr);
                        int surfacesOfOurs = 0;
                        int fullScreenOnes = 0;
                        const QRect virtualScreen(
                            QPoint(::GetSystemMetrics(SM_XVIRTUALSCREEN),
                                   ::GetSystemMetrics(SM_YVIRTUALSCREEN)),
                            QSize(::GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                  ::GetSystemMetrics(SM_CYVIRTUALSCREEN)));
                        HWND child = progman ? ::FindWindowExW(progman, nullptr, nullptr, nullptr)
                                             : nullptr;
                        while (child) {
                            wchar_t klass[128] = {0};
                            ::GetClassNameW(child, klass, 128);
                            if (QString::fromWCharArray(klass) == QLatin1String("StarFishDesktopSurface")) {
                                ++surfacesOfOurs;
                                RECT box{};
                                ::GetWindowRect(child, &box);
                                if (box.left <= virtualScreen.left() && box.top <= virtualScreen.top()
                                    && box.right >= virtualScreen.right()
                                    && box.bottom >= virtualScreen.bottom())
                                    ++fullScreenOnes;
                            }
                            child = ::FindWindowExW(progman, child, nullptr, nullptr);
                        }
                        check(QStringLiteral("the product puts no window of ours on the desktop"),
                              backdropOff && surfacesOfOurs == product.container()->fenceCount()
                                  && fullScreenOnes == 0,
                              QStringLiteral("backdrop hosted=%1, our surface(s) under Progman=%2 "
                                             "for %3 fence(s), full-screen=%4")
                                  .arg(backdropOff ? 0 : 1)
                                  .arg(surfacesOfOurs)
                                  .arg(product.container()->fenceCount())
                                  .arg(fullScreenOnes));

                        // And the icons are therefore the shell's -- including for a
                        // click, which is asked *there*: who does the system say is
                        // under the point? Not asked (never failed) when one of the
                        // user's own windows covers it: that would be a verdict about
                        // their browser, not about StarFish (decision 13).
                        const QRect iconBox(iconsBefore.first().position, iconsBefore.first().slot);
                        const POINT iconCentre{iconBox.center().x(), iconBox.center().y()};
                        const HWND underIcon = ::WindowFromPoint(iconCentre);
                        wchar_t underClass[128] = {0};
                        if (underIcon)
                            ::GetClassNameW(underIcon, underClass, 128);
                        const QString underName = QString::fromWCharArray(underClass);
                        if (underName == QLatin1String("SysListView32")) {
                            check(QStringLiteral("the desktop's own icon is what the mouse finds "
                                                 "at an icon"),
                                  true,
                                  QStringLiteral("%1,%2 -> the shell's list view")
                                      .arg(iconCentre.x)
                                      .arg(iconCentre.y));

                            // The delivery half -- the one that would have caught T-069 --
                            // and the instrument is the *shell's own* selection count: a
                            // left click on an icon selects it, so the count moving is
                            // proof that the click arrived, and nothing about menu classes
                            // is assumed (Windows 11's modern menus are not the classic
                            // #32768 any more -- measured: no such window appears even
                            // without StarFish running). The point has to be the list view
                            // (checked above), and the selection is cleared afterwards by
                            // clicking empty desktop, which is also now the shell's.
                            const HWND defView = ::FindWindowExW(progman, nullptr,
                                                                 L"SHELLDLL_DefView", nullptr);
                            const HWND listView = ::FindWindowExW(defView, nullptr,
                                                                  L"SysListView32", nullptr);
                            // LVM_GETSELECTEDCOUNT, spelled out: commctrl.h is not part of
                            // this translation unit's diet and the value is stable.
                            constexpr UINT kLvmGetSelectedCount = 0x1000 + 50;
                            const LRESULT selectedBefore =
                                listView
                                    ? ::SendMessageW(listView, kLvmGetSelectedCount, 0, 0)
                                    : -1;
                            POINT savedCursor{};
                            ::GetCursorPos(&savedCursor);
                            ::SetCursorPos(iconCentre.x, iconCentre.y);
                            auto mouse = [](DWORD flags) {
                                INPUT in{};
                                in.type = INPUT_MOUSE;
                                in.mi.dwFlags = flags;
                                return in;
                            };
                            INPUT click[2] = {mouse(MOUSEEVENTF_LEFTDOWN),
                                              mouse(MOUSEEVENTF_LEFTUP)};
                            int refused = 0;
                            for (const INPUT& in : click) {
                                if (::SendInput(1, const_cast<INPUT*>(&in), sizeof(INPUT)) == 0)
                                    ++refused;
                                ::Sleep(60);
                            }
                            ::Sleep(200);
                            const LRESULT selectedAfter = listView
                                ? ::SendMessageW(listView, kLvmGetSelectedCount, 0, 0)
                                : -1;

                            // Give the selection back, and **only one way is safe**:
                            // Ctrl+click on the very same icon, which toggles it off
                            // (measured by hand: 1 -> 0). Two traps, both paid for:
                            //  - guessing a "bare" point from the icon rectangles does not
                            //    work -- the shell's own hit area includes the label, so the
                            //    click lands on another icon and the count stays at one;
                            //  - `LVM_HITTEST` cannot be asked point-blank: it takes a
                            //    *pointer* to an LVHITTESTINFO, and passing coordinates in
                            //    lParam hands explorer a bogus pointer. It **crashes and
                            //    restarts** (measured here: the desktop came back with the
                            //    icons hidden for a few seconds, and this probe's own
                            //    "the desktop is exactly the one it started with" check
                            //    went red because the list view was rebuilding). Never
                            //    send a list-view message whose argument is a pointer we
                            //    have not allocated *in the shell's* process.
                            // The cleanup is best effort: it is logged, not asserted --
                            // the point of this check is the delivery above.
                            auto key = [](WORD vk, bool up) {
                                INPUT in{};
                                in.type = INPUT_KEYBOARD;
                                in.ki.wVk = vk;
                                in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
                                return in;
                            };
                            INPUT control[1] = {key(VK_CONTROL, false)};
                            ::SendInput(1, control, sizeof(INPUT));
                            ::Sleep(300);
                            ::SetCursorPos(iconCentre.x, iconCentre.y);
                            ::Sleep(150);
                            for (const INPUT& in : click) {
                                ::SendInput(1, const_cast<INPUT*>(&in), sizeof(INPUT));
                                ::Sleep(80);
                            }
                            ::Sleep(150);
                            INPUT controlUp[1] = {key(VK_CONTROL, true)};
                            ::SendInput(1, controlUp, sizeof(INPUT));
                            ::Sleep(300);
                            const LRESULT selectedCleared = listView
                                ? ::SendMessageW(listView, kLvmGetSelectedCount, 0, 0)
                                : -1;
                            ::SetCursorPos(savedCursor.x, savedCursor.y);

                            if (selectedCleared == selectedAfter)
                                log << QStringLiteral("(one icon of the desktop is left selected: "
                                                      "\"%1\" -- Ctrl+click it to clear it, the "
                                                      "click reaches the shell)")
                                           .arg(iconsBefore.first().name);
                            check(QStringLiteral("a click on one of the desktop's icons still "
                                                 "reaches the shell"),
                                  refused == 0 && selectedBefore >= 0
                                      && selectedAfter != selectedBefore,
                                  QStringLiteral("selection %1 -> %2 (a click on the icon at "
                                                 "%5,%6) -> %3 (Ctrl+click puts it back), %4 "
                                                 "SendInput refused")
                                      .arg(selectedBefore)
                                      .arg(selectedAfter)
                                      .arg(selectedCleared)
                                      .arg(refused)
                                      .arg(iconCentre.x)
                                      .arg(iconCentre.y));
                        } else {
                            log << QStringLiteral("the icons' click was NOT asked: \"%1\" covers "
                                                  "the icon at %2,%3 -- bring the desktop to the "
                                                  "front and re-run to measure it")
                                       .arg(underName)
                                       .arg(iconCentre.x)
                                       .arg(iconCentre.y);
                        }

                        // The two claims of the requirement, from outside: the shell
                        // is still the one drawing the desktop, and nothing was left
                        // on disk that says otherwise.
                        check(QStringLiteral("the launch leaves the desktop's icons alone, even "
                                             "when a fence holds one"),
                              starfish::platform::win::nativeDesktopIconsVisible(),
                              product.statusText());
                        check(QStringLiteral("no icon-hiding marker was written"),
                              !marker.isPresent(), marker.description());
                    }

                    product.shutdown();
                }

                // And after the exit the desktop is exactly the one it started with:
                // same icons, still the shell's.
                QStringList namesBefore;
                QStringList namesAfter;
                for (const starfish::platform::win::NativeDesktopIcon& icon : iconsBefore)
                    namesBefore << icon.name;
                const QVector<starfish::platform::win::NativeDesktopIcon> iconsAfter =
                    starfish::platform::win::readNativeDesktopIcons();
                for (const starfish::platform::win::NativeDesktopIcon& icon : iconsAfter)
                    namesAfter << icon.name;
                namesBefore.sort();
                namesAfter.sort();
                check(QStringLiteral("the desktop is exactly the one it started with"),
                      starfish::platform::win::nativeDesktopIconsVisible()
                          && !marker.isPresent() && namesBefore == namesAfter,
                      QStringLiteral("%1 icon(s) before, %2 after, visible=%3, marker %4")
                          .arg(namesBefore.size())
                          .arg(namesAfter.size())
                          .arg(starfish::platform::win::nativeDesktopIconsVisible() ? 1 : 0)
                          .arg(marker.isPresent() ? marker.description()
                                                  : QStringLiteral("absent")));
            }

            // Leave nothing behind: the probe owns this directory and removes it.
            QDir(probeDir).removeRecursively();
            check(QStringLiteral("the probe cleaned up after itself"),
                  !QFileInfo::exists(layoutPath), probeDir);
        }
#endif

        const QString reportPath = QStringLiteral("build/icons_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QStringLiteral("icons probe (T-066): the desktop's icons stay the "
                                         "shell's\n");
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n").arg(failures));
            }
        }
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(backdropDemoOption)) {
        // T-039 (ADR-9 option A): the layer StarFish draws the desktop's own
        // icons on while it owns the icon layer. Headless by default -- the free
        // set, the layout, the frame and the document check need no desktop;
        // --host additionally hosts the real full-screen window and measures it.
        //
        // Nothing here hides the native icons: the backdrop is proved *before*
        // it is asked to stand in for them (the product only hides the layer
        // once this succeeds -- see ProductApp::enableBackdrop).
        QStringList header;
        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QStringLiteral("%1  %2  %3")
                       .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                       .arg(step, -58)
                       .arg(detail);
        };

        // --- the desktop, as the Shell reports it ---------------------------
        header << QStringLiteral("-- the desktop, as the Shell reports it --");
        QString scanError;
        const QList<starfish::core::DesktopItem> scanned =
            starfish::platform::win::DesktopScanner::scan(&scanError);
        QList<starfish::core::DesktopItem> visible;
        for (const starfish::core::DesktopItem& item : scanned) {
            if (!item.isHidden)
                visible.append(item);
        }
        header << QStringLiteral("  %1 entries, %2 of them visible to the Shell")
                      .arg(scanned.size())
                      .arg(visible.size());
        if (!scanError.isEmpty())
            header << QStringLiteral("  scan error: %1").arg(scanError);

        starfish::ui::FenceContainer container;
        // The real monitors, from the product's own definition of them: a probe
        // that built its own could measure a different desktop than the product
        // works on.
        container.setScreens(starfish::app::ProductApp::screenLayout());
        // A real desktop that has icons on it is the case this feature exists
        // for; a probe on an empty one would measure nothing.
        const bool enoughIcons = visible.size() >= 8;
        check(QStringLiteral("the desktop has icons to draw"), enoughIcons,
              QStringLiteral("%1 visible entries").arg(visible.size()));
        if (!enoughIcons) {
            header << QStringLiteral("  (stopping here: nothing to measure)");
        }

        const QRect area = container.screens().bounds();
        header << QStringLiteral("-- the virtual screen --");
        header << QStringLiteral("  %1,%2 %3x%4 (primary dpr %5)")
                      .arg(area.x())
                      .arg(area.y())
                      .arg(area.width())
                      .arg(area.height())
                      .arg(container.screens().primary()
                               ? container.screens().primary()->devicePixelRatio
                               : 1.0);

        container.setDesktopItems(visible);
        starfish::ui::DesktopBackdrop* backdrop = container.backdrop();
        check(QStringLiteral("the container owns a backdrop"), backdrop != nullptr, {});
        if (!backdrop || !enoughIcons) {
            // Nothing else can be measured; report what we have.
            const QString reportPath = QStringLiteral("build/backdrop_demo.txt");
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QStringLiteral("backdrop probe (T-039)\n");
                for (const QString& line : header)
                    stream << line << '\n';
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0 ? QStringLiteral("RESULT: all checks passed\n")
                                         : QStringLiteral("RESULT: %1 check(s) FAILED\n")
                                               .arg(QString::number(failures)));
            }
            for (const QString& line : log)
                qCInfo(lc).noquote() << line;
            const int exitCode = failures == 0 ? 0 : 1;
            QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
            return app.exec();
        }

        // --- the free set ---------------------------------------------------
        //
        // The desktop's icons are what no fence holds. That set is *computed*
        // here and stored nowhere: the document keeps describing fences (ADR-7),
        // and F-007/F-011 come out of it for free.
        log << QStringLiteral("-- the free set --");
        check(QStringLiteral("with no fence, every icon is on the desktop"),
              backdrop->freeCount() == visible.size(),
              QStringLiteral("free=%1 of %2")
                  .arg(backdrop->freeCount())
                  .arg(visible.size()));

        // ADR-7: the desktop layer is a *view*. Drawing it must not put a single
        // byte in the document -- that is what keeps the schema still, and what
        // makes F-007/F-011 fall out of the model instead of needing restore
        // code.
        const QByteArray emptyDocument = starfish::core::JsonStore::toJson(container.toLayout());
        backdrop->renderNow();
        backdrop->renderNow();
        check(QStringLiteral("drawing the desktop never touches the document"),
              starfish::core::JsonStore::toJson(container.toLayout()) == emptyDocument,
              QStringLiteral("%1 bytes, unchanged after two full renders")
                  .arg(emptyDocument.size()));

        const QList<starfish::core::DesktopItem> claimed = visible.mid(0, 3);
        const QString fenceId = container.createFence(
            QStringLiteral("Backdrop probe"), QRect(200, 200, 320, 240), claimed);
        check(QStringLiteral("a fence takes exactly its icons off the desktop"),
              backdrop->freeCount() == visible.size() - claimed.size(),
              QStringLiteral("free=%1, fence holds %2")
                  .arg(backdrop->freeCount())
                  .arg(claimed.size()));

        const QSet<QString> freeKeys = [&backdrop] {
            QSet<QString> keys;
            for (const starfish::core::DesktopItem& item : backdrop->freeItems())
                keys.insert(starfish::ui::BackdropLayout::pathKey(item.path));
            return keys;
        }();
        bool noneClaimed = true;
        for (const starfish::core::DesktopItem& item : claimed)
            noneClaimed = noneClaimed && !freeKeys.contains(
                                             starfish::ui::BackdropLayout::pathKey(item.path));
        check(QStringLiteral("an icon a fence holds is not among the free ones"), noneClaimed,
              QStringLiteral("%1 claimed, %2 free").arg(claimed.size()).arg(freeKeys.size()));

        // The same file spelled differently is the same file: a fence can hold
        // the path a Shell lookup returned while the scan returned the other
        // spelling, and drawing it twice would be the visible bug.
        const starfish::core::DesktopItem upper = [&visible] {
            starfish::core::DesktopItem item = visible.at(3);
            item.path = item.path.toUpper();
            return item;
        }();
        const QString secondId =
            container.createFence(QStringLiteral("Backdrop probe 2"), QRect(560, 200, 320, 240),
                                  {upper});
        check(QStringLiteral("the comparison ignores case and separators"),
              backdrop->freeCount() == visible.size() - claimed.size() - 1,
              QStringLiteral("free=%1 after claiming \"%2\"")
                  .arg(backdrop->freeCount())
                  .arg(upper.fileName));
        container.removeFence(secondId);

        // ADR-7, the other half: what the document holds is exactly the fences'
        // icons -- never the desktop's. 80-odd icons are on screen and three
        // paths are written.
        const auto parsed =
            starfish::core::JsonStore::fromJson(starfish::core::JsonStore::toJson(
                                                    container.toLayout()),
                                                nullptr);
        int documentedItems = 0;
        if (parsed) {
            for (const starfish::core::Fence& fence : parsed->fences)
                documentedItems += fence.items.size();
        }
        check(QStringLiteral("the document holds the fence's icons and none of the desktop's"),
              parsed.has_value() && parsed->fences.size() == 1
                  && documentedItems == claimed.size(),
              QStringLiteral("%1 path(s) in the document, %2 icons on the desktop")
                  .arg(documentedItems)
                  .arg(visible.size()));

        // F-007 / F-011, at the model level: deleting a fence is the whole of
        // "give the icons back". No restore code exists, and none is needed.
        container.removeFence(fenceId);
        check(QStringLiteral("deleting the fence puts the icons back"),
              backdrop->freeCount() == visible.size(),
              QStringLiteral("free=%1 of %2")
                  .arg(backdrop->freeCount())
                  .arg(visible.size()));

        // --- where it lands -------------------------------------------------
        log << QStringLiteral("-- where it lands --");
        const starfish::ui::BackdropLayout::Metrics physical = backdrop->physicalMetrics();
        const int rows = starfish::ui::BackdropLayout::rowsForHeight(area.height(), physical);
        const QVector<starfish::ui::BackdropCell> cells = backdrop->cells();
        const bool enoughForColumns = cells.size() > rows + 1;
        check(QStringLiteral("the desktop has enough icons for a column check"), enoughForColumns,
              QStringLiteral("%1 cells, %2 rows").arg(cells.size()).arg(rows));
        if (enoughForColumns) {
            bool sameColumn = true;
            for (int i = 1; i < rows; ++i)
                sameColumn = sameColumn && cells.at(i).icon.x() == cells.at(0).icon.x();
            const bool stepsRight = cells.at(rows).icon.x() > cells.at(0).icon.x();
            check(QStringLiteral("the cells fill columns, like the Windows desktop"),
                  sameColumn && stepsRight,
                  QStringLiteral("first %1 share x=%2, cell %3 starts the next column at x=%4")
                      .arg(rows)
                      .arg(cells.at(0).icon.x())
                      .arg(rows)
                      .arg(cells.at(rows).icon.x()));
        }

        // --- the frame ------------------------------------------------------
        log << QStringLiteral("-- the frame --");
        // ADR-13: the product does not draw the fence-less icons any more -- the
        // desktop's own are on screen. This section measures the layer that still
        // exists for the case a caller wants StarFish to own them, so it asks for
        // it explicitly; the default is what the next check is about.
        backdrop->setDrawFreeIcons(true);
        // "Painted" now means *above the input floor*. Since T-060 every frame
        // carries one alpha level across the whole screen
        // (DesktopBackdrop::kInputAlphaFloor), so counting every pixel whose alpha
        // is not zero would call the whole screen painted. These counts are about
        // the icons -- what the floor is invisible for.
        const auto paintedPixels = [](const QImage& image) {
            qint64 count = 0;
            for (int y = 0; y < image.height(); ++y) {
                const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    if (qAlpha(line[x]) > starfish::ui::DesktopBackdrop::kInputAlphaFloor)
                        ++count;
                }
            }
            return count;
        };

        backdrop->renderNow();
        const QImage full = backdrop->frame();
        check(QStringLiteral("the frame is the size of the virtual screen"),
              full.size() == area.size(),
              QStringLiteral("%1x%2").arg(full.width()).arg(full.height()));
        const qint64 paintedFull = paintedPixels(full);
        const qint64 totalPixels = qint64(full.width()) * full.height();
        // The budget comes from the cells, not from a magic fraction: "mostly
        // transparent" means the painted pixels are accounted for by the icons
        // and by nothing else, and that has to hold on any screen size. The
        // former `painted < 10 % of the screen` went red on a 1920x1080 desktop
        // while the pixel count stayed identical (231738) -- the denominator had
        // moved, not the frame. A budget also cannot be satisfied by a big
        // screen, which the fraction could.
        qint64 cellBudget = 0;
        for (const starfish::ui::BackdropCell& cell : backdrop->cells()) {
            const QRect box = cell.icon.united(cell.label);
            cellBudget += qint64(box.width()) * box.height();
        }
        check(QStringLiteral("the frame is mostly transparent (the wallpaper shows)"),
              full.hasAlphaChannel() && paintedFull > 0 && paintedFull <= cellBudget,
              QStringLiteral("%1 of %2 pixels painted, the %3 cells account for at most %4")
                  .arg(paintedFull)
                  .arg(totalPixels)
                  .arg(backdrop->cells().size())
                  .arg(cellBudget));

        // T-060, and the invariant that makes the gestures reachable at all: no
        // pixel of the frame is a hole the cursor falls through. A per-pixel
        // layered window is hit-tested against its own alpha, so a zero-alpha
        // pixel is a pixel Windows answers with whatever is *behind* the
        // backdrop -- which is how a fully transparent frame made the empty
        // desktop's own gestures unreachable (the fix is the floor; this is what
        // would catch it if the floor were ever dropped again). The floor itself
        // is invisible: the check above counts only what is *above* it.
        qint64 transparentPixels = 0;
        for (int y = 0; y < full.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(full.constScanLine(y));
            for (int x = 0; x < full.width(); ++x) {
                if (qAlpha(line[x]) == 0)
                    ++transparentPixels;
            }
        }
        check(QStringLiteral("the frame has nowhere the cursor falls through"),
              full.hasAlphaChannel() && transparentPixels == 0,
              QStringLiteral("%1 of %2 pixels at alpha 0, the floor is %3")
                  .arg(transparentPixels)
                  .arg(totalPixels)
                  .arg(starfish::ui::DesktopBackdrop::kInputAlphaFloor));

        // ADR-13's default, measured rather than described: with the drawing
        // switched off the frame paints nothing at all -- the icons on screen are
        // the desktop's own, where the user put them, and all this layer is left
        // with is the input floor that makes the bare desktop ours to answer.
        backdrop->setDrawFreeIcons(false);
        backdrop->renderNow();
        const qint64 paintedDefault = paintedPixels(backdrop->frame());
        check(QStringLiteral("by default the frame paints nothing and leaves the icons alone"),
              paintedDefault == 0 && !backdrop->freeItems().isEmpty(),
              QStringLiteral("%1 painted pixel(s), %2 icon(s) left to the desktop")
                  .arg(paintedDefault)
                  .arg(backdrop->freeItems().size()));
        backdrop->setDrawFreeIcons(true);
        backdrop->renderNow();

        // ⚠️ T-069 -- and the holes. With the drawing off -- the product's state --
        // the frame must carry one alpha-0 hole per icon of the desktop, and none
        // anywhere else. The answer to WM_NCHITTEST does not cross into another
        // process: measured with the backdrop hosted, the shell's own list view
        // received **0 of 4** clicks aimed at real icons (against 1 of 4 without
        // StarFish), so the desktop's icons were visible and untouchable. The alpha
        // is the step-aside Windows honours, and this is the check that keeps it.
        // The premise is measured too: a desktop whose icons are hidden would
        // otherwise prove nothing and pass.
        QVector<QRect> iconRects;
        for (const starfish::platform::win::NativeDesktopIcon& icon :
             starfish::platform::win::readNativeDesktopIcons())
            iconRects.append(QRect(icon.position, icon.slot));
        backdrop->setDrawFreeIcons(false);
        backdrop->setNativeIconRects(iconRects);
        backdrop->renderNow();
        const QImage holed = backdrop->frame();
        const QPoint frameOrigin = area.topLeft();
        qint64 holesInIcons = 0;
        qint64 holesElsewhere = 0;
        for (int y = 0; y < holed.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(holed.constScanLine(y));
            for (int x = 0; x < holed.width(); ++x) {
                if (qAlpha(line[x]) != 0)
                    continue;
                const QPoint inFrame(x, y);
                bool onAnIcon = false;
                for (const QRect& rect : iconRects) {
                    if (rect.translated(-frameOrigin).contains(inFrame)) {
                        onAnIcon = true;
                        break;
                    }
                }
                if (onAnIcon)
                    ++holesInIcons;
                else
                    ++holesElsewhere;
            }
        }
        check(QStringLiteral("an icon's rectangle is cut out of the layer, so the shell gets "
                             "its click"),
              iconRects.size() > 0 && holesInIcons > 0 && holesElsewhere == 0,
              QStringLiteral("%1 alpha-0 pixel(s) over %2 icon rect(s), %3 elsewhere -- the icons "
                             "have to be visible for this to measure anything")
                  .arg(holesInIcons)
                  .arg(iconRects.size())
                  .arg(holesElsewhere));
        backdrop->setNativeIconRects({});
        backdrop->setDrawFreeIcons(true);
        backdrop->renderNow();
        const QString claimedId = container.createFence(
            QStringLiteral("Backdrop probe 3"), QRect(200, 200, 320, 240), claimed);
        backdrop->renderNow();
        const qint64 paintedClaimed = paintedPixels(backdrop->frame());
        check(QStringLiteral("the frame loses an icon when a fence claims it"),
              paintedClaimed < paintedFull,
              QStringLiteral("%1 painted pixels, %2 after claiming %3 icon(s)")
                  .arg(paintedFull)
                  .arg(paintedClaimed)
                  .arg(claimed.size()));
        container.removeFence(claimedId);

        // --- the empty desktop's gestures (T-041 / T-055) ---------------------
        //
        // F-001 (a right-drag on the bare desktop draws a fence, and the gesture
        // ends on the desktop's own context menu) and F-022 (a left double-click
        // hides or shows them all). Both were always specified for the *empty*
        // desktop, which belongs to SysListView32 -- another process's window --
        // so they had always looked like they needed a global mouse hook (T-028).
        // They do not: the backdrop is a window of ours standing in for the
        // desktop, so the gesture starts inside it. What this section measures is
        // the half that matters, which is *whether* a gesture happened at all --
        // a click, a wobble, a press on an icon, a left-drag and a draw too small
        // to be a fence must all create nothing.
        //
        // The events are real QMouseEvents sent to the backdrop, the same events
        // DesktopSurface::forwardMouse posts from the hosted window's WndProc.
        log << QStringLiteral("-- the empty desktop's gestures --");
        backdrop->setGestureEnabled(true);
        check(QStringLiteral("the backdrop takes the gestures when asked"),
              backdrop->gestureEnabled(), backdrop->statusText());

        const QVector<starfish::ui::BackdropCell> gestureCells = backdrop->cells();
        // A point on the icons we draw, and a point on bare wallpaper. Both are
        // premises: if the desktop is too small to have bare room beside the
        // icons, say so instead of measuring nothing.
        const QPoint iconPoint = gestureCells.isEmpty() ? QPoint()
                                                        : gestureCells.at(0).icon.center();
        int rightMost = 0;
        for (const starfish::ui::BackdropCell& cell : gestureCells)
            rightMost = qMax(rightMost, cell.label.right());
        const QPoint barePoint(qMin(area.right() - 20, rightMost + 40), area.top() + 40);
        const bool hasBare = !gestureCells.isEmpty()
                             && starfish::ui::BackdropGesture::iconAt(gestureCells, barePoint) < 0
                             && starfish::ui::BackdropGesture::iconAt(gestureCells, iconPoint) >= 0;
        check(QStringLiteral("there is both bare desktop and an icon to aim at"), hasBare,
              QStringLiteral("bare %1,%2 (icon %3), icon %4,%5 (icon %6)")
                  .arg(barePoint.x())
                  .arg(barePoint.y())
                  .arg(starfish::ui::BackdropGesture::iconAt(gestureCells, barePoint))
                  .arg(iconPoint.x())
                  .arg(iconPoint.y())
                  .arg(starfish::ui::BackdropGesture::iconAt(gestureCells, iconPoint)));

        if (hasBare) {
            // The product's wiring, reproduced: the backdrop says what happened,
            // the product decides what to make of it (ProductApp::onDesktopMenu).
            // The menu itself is deliberately *not* opened here -- it is a
            // blocking popup and there is nobody to click it -- so the probe takes
            // the two halves apart: the gesture asks for a menu with this
            // rectangle, and the menu is really built with our entry (below).
            int menuRequests = 0;
            int hideRequests = 0;
            QRect offeredRect;
            QPoint offeredPoint;
            QObject::connect(backdrop, &starfish::ui::DesktopBackdrop::desktopMenuRequested,
                             &container, [&](const QRect& rect, const QPoint& point) {
                                 ++menuRequests;
                                 offeredRect = rect;
                                 offeredPoint = point;
                             });
            QObject::connect(backdrop, &starfish::ui::DesktopBackdrop::quickHideRequested,
                             &container, [&container, &hideRequests] {
                                 ++hideRequests;
                                 container.toggleFencesVisible();
                             });

            const auto sendMouse = [&backdrop](QEvent::Type type, const QPoint& screen,
                                               Qt::MouseButton button, Qt::MouseButtons buttons) {
                QMouseEvent event(type, QPointF(0, 0), QPointF(screen), button, buttons,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(backdrop, &event);
            };
            const auto drag = [&](Qt::MouseButton button, const QPoint& from, const QPoint& to) {
                sendMouse(QEvent::MouseButtonPress, from, button, button);
                sendMouse(QEvent::MouseMove, to, Qt::NoButton, button);
                sendMouse(QEvent::MouseButtonRelease, to, button, Qt::NoButton);
            };

            // The rectangle a drag describes is the rectangle offered, in
            // virtual-screen pixels -- the space a stored fence rect uses, so no
            // conversion sits between the gesture and the document (T-014).
            const QPoint farCorner(barePoint.x() - 300, barePoint.y() + 200);
            // Normalised: the drag runs left and down from the bare point, so the
            // rectangle starts at the far corner.
            const QRect expectedRect(farCorner.x(), barePoint.y(), 300, 200);
            const int fencesBefore = container.fenceCount();

            // The baseline is taken *before* the press: the rubber band is
            // painted into the frame, so the frame with it has to be the frame
            // without it plus something.
            backdrop->renderNow();
            const qint64 paintedWithout = paintedPixels(backdrop->frame());

            sendMouse(QEvent::MouseButtonPress, barePoint, Qt::RightButton, Qt::RightButton);
            sendMouse(QEvent::MouseMove, farCorner, Qt::NoButton, Qt::RightButton);
            check(QStringLiteral("the rubber band follows the drag"),
                  backdrop->rubberBand() == expectedRect,
                  QStringLiteral("%1,%2 %3x%4")
                      .arg(backdrop->rubberBand().x())
                      .arg(backdrop->rubberBand().y())
                      .arg(backdrop->rubberBand().width())
                      .arg(backdrop->rubberBand().height()));

            // ...and it is really on screen: the preview is painted, not merely
            // remembered.
            backdrop->renderNow();
            const qint64 paintedDuring = paintedPixels(backdrop->frame());
            check(QStringLiteral("the frame shows the fence being drawn"),
                  paintedDuring > paintedWithout,
                  QStringLiteral("%1 painted pixels while drawing, %2 before")
                      .arg(paintedDuring)
                      .arg(paintedWithout));

            sendMouse(QEvent::MouseButtonRelease, farCorner, Qt::RightButton, Qt::NoButton);
            check(QStringLiteral("a right-drag on the bare desktop asks for the desktop menu"),
                  menuRequests == 1,
                  QStringLiteral("%1 request(s)") .arg(menuRequests));
            check(QStringLiteral("the rectangle offered is the one that was drawn"),
                  offeredRect == expectedRect && offeredPoint == farCorner,
                  QStringLiteral("%1,%2 %3x%4 at %5,%6")
                      .arg(offeredRect.x())
                      .arg(offeredRect.y())
                      .arg(offeredRect.width())
                      .arg(offeredRect.height())
                      .arg(offeredPoint.x())
                      .arg(offeredPoint.y()));
            check(QStringLiteral("the rubber band is gone once the button is up"),
                  backdrop->rubberBand().isNull(), {});

            // And choosing our entry creates the fence where it was drawn: no
            // conversion, no default placement, no snap.
            const QString drawnId = container.createFence(QStringLiteral("Drawn"), offeredRect);
            check(QStringLiteral("the offered fence is created where it was drawn"),
                  container.fenceCount() == fencesBefore + 1 && !drawnId.isEmpty()
                      && container.fenceRect(drawnId) == expectedRect,
                  QStringLiteral("%1 -> %2,%3 %4x%5")
                      .arg(drawnId)
                      .arg(container.fenceRect(drawnId).x())
                      .arg(container.fenceRect(drawnId).y())
                      .arg(container.fenceRect(drawnId).width())
                      .arg(container.fenceRect(drawnId).height()));

            // The golden rule, from the gesture's side: drawing a fence claims no
            // file. The document gains a fence and not one path.
            const auto drawnDoc =
                starfish::core::JsonStore::fromJson(starfish::core::JsonStore::toJson(
                                                        container.toLayout()),
                                                    nullptr);
            int drawnItems = 0;
            if (drawnDoc) {
                for (const starfish::core::Fence& fence : drawnDoc->fences)
                    drawnItems += fence.items.size();
            }
            check(QStringLiteral("drawing a fence claims no file"),
                  drawnDoc.has_value() && drawnItems == 0,
                  QStringLiteral("%1 path(s) in the document, %2 fence(s)")
                      .arg(drawnItems)
                      .arg(drawnDoc ? drawnDoc->fences.size() : -1));
            container.removeFence(drawnId);

            // What must create nothing, and what must still open the menu. Each of
            // these is a way to make a fence the user did not ask for.
            const int quietBefore = container.fenceCount();
            drag(Qt::RightButton, barePoint, barePoint);
            check(QStringLiteral("a plain right-click opens the menu with no rectangle"),
                  menuRequests == 2 && offeredRect.isNull() && offeredPoint == barePoint
                      && container.fenceCount() == quietBefore,
                  QStringLiteral("the menu T-039 had taken away (T-054)"));

            drag(Qt::RightButton, barePoint, QPoint(barePoint.x() + 3, barePoint.y() + 2));
            check(QStringLiteral("a wobble offers no rectangle either"),
                  menuRequests == 3 && offeredRect.isNull()
                      && container.fenceCount() == quietBefore,
                  QStringLiteral("a 3x2 px drag offered nothing"));

            drag(Qt::RightButton, barePoint, QPoint(barePoint.x() + 40, barePoint.y() + 40));
            check(QStringLiteral("a right-draw too small to be a fence offers no rectangle"),
                  menuRequests == 4 && offeredRect.isNull()
                      && container.fenceCount() == quietBefore,
                  QStringLiteral("40x40 px, the minimum is %1x%2")
                      .arg(backdrop->gestureMetrics().minimumFence.width())
                      .arg(backdrop->gestureMetrics().minimumFence.height()));

            drag(Qt::LeftButton, barePoint, farCorner);
            check(QStringLiteral("the left button draws nothing"),
                  menuRequests == 4 && container.fenceCount() == quietBefore,
                  QStringLiteral("F-001 is the right button; the left one is T-042's"));

            drag(Qt::RightButton, iconPoint, QPoint(iconPoint.x() + 300, iconPoint.y() + 200));
            check(QStringLiteral("a press on one of the icons starts nothing"),
                  menuRequests == 4 && container.fenceCount() == quietBefore,
                  QStringLiteral("the icons the backdrop draws are not ours to drag yet"));

            // F-022, the gesture the feature was always specified with.
            const bool visibleBefore = container.fencesVisible();
            sendMouse(QEvent::MouseButtonDblClick, barePoint, Qt::LeftButton, Qt::LeftButton);
            const bool hiddenAfter = container.fencesVisible();
            sendMouse(QEvent::MouseButtonDblClick, barePoint, Qt::LeftButton, Qt::LeftButton);
            check(QStringLiteral("a double-click on the bare desktop hides and shows the fences"),
                  hideRequests == 2 && hiddenAfter != visibleBefore
                      && container.fencesVisible() == visibleBefore,
                  QStringLiteral("%1 request(s), visible %2 -> %3 -> %4")
                      .arg(hideRequests)
                      .arg(visibleBefore ? QStringLiteral("yes") : QStringLiteral("no"))
                      .arg(hiddenAfter ? QStringLiteral("yes") : QStringLiteral("no"))
                      .arg(container.fencesVisible() ? QStringLiteral("yes")
                                                     : QStringLiteral("no")));

            sendMouse(QEvent::MouseButtonDblClick, iconPoint, Qt::LeftButton, Qt::LeftButton);
            check(QStringLiteral("a double-click on an icon is not ours"), hideRequests == 2,
                  QStringLiteral("%1 request(s) in total").arg(hideRequests));
        }

        // --- the desktop's own menu (T-055, and T-054 with it) ---------------
        //
        // The gesture ends on the desktop's context menu, so the menu itself has
        // to be real: the desktop namespace folder's view object, plus our entry
        // on top of it. Built without ever showing it -- a popup would block on a
        // user who is not there -- which is the same trick --context-menu-verbs
        // uses for an item.
        log << QStringLiteral("-- the desktop's own menu --");
        QString starfishEntry;
        const QStringList desktopVerbs =
            starfish::platform::win::ShellContextMenu::desktopBackgroundVerbs(&starfishEntry);
        check(QStringLiteral("our entry sits on top of the desktop's menu"),
              starfishEntry.contains(QStringLiteral("Create a fence here")),
              QStringLiteral("\"%1\"").arg(starfishEntry));
        check(QStringLiteral("the shell's own entries are still there"),
              desktopVerbs.size() >= 2,
              QStringLiteral("%1 verb(s): %2")
                  .arg(desktopVerbs.size())
                  .arg(desktopVerbs.join(QStringLiteral(", "))));

        backdrop->setGestureEnabled(false);
        check(QStringLiteral("turning the gestures off leaves the backdrop inert"),
              !backdrop->gestureEnabled() && backdrop->rubberBand().isNull(), {});

        // --- the real window (--host) ---------------------------------------
        const bool hosted = parser.isSet(hostOption);
        if (hosted) {
            log << QStringLiteral("-- the real window (--host) --");
            const bool attached = container.setBackdropEnabled(true);
            check(QStringLiteral("the backdrop is hosted"), attached && backdrop->isAttached(),
                  backdrop->statusText());

            if (attached && backdrop->surface()) {
                starfish::platform::win::DesktopSurface* surface = backdrop->surface();
                const HWND hwnd = reinterpret_cast<HWND>(surface->hwnd());
                const LONG exStyle = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_EXSTYLE));
                check(QStringLiteral("the window is layered (ADR-4)"),
                      (exStyle & WS_EX_LAYERED) != 0,
                      QStringLiteral("ex 0x%1")
                          .arg(static_cast<qulonglong>(exStyle), 0, 16));
                check(QStringLiteral("the per-pixel path took (no opaque fallback)"),
                      surface->perPixelAlpha(),
                      QStringLiteral("UpdateLayeredWindow succeeded, alpha honoured"));
                check(QStringLiteral("the window covers the whole virtual screen"),
                      surface->geometry() == area,
                      QStringLiteral("%1,%2 %3x%4")
                          .arg(surface->geometry().x())
                          .arg(surface->geometry().y())
                          .arg(surface->geometry().width())
                          .arg(surface->geometry().height()));
                check(QStringLiteral("without the gestures the backdrop stays out of the way"),
                      surface->clickThrough() && surface->inputTarget() == nullptr,
                      QStringLiteral("WM_NCHITTEST answers HTTRANSPARENT"));

                // T-041: the empty desktop's gestures are what turn the backdrop
                // from a decoration into the desktop's input surface. It takes
                // the mouse only because it has something to answer with.
                backdrop->setGestureEnabled(true);
                check(QStringLiteral("the gestures take the mouse"),
                      !surface->clickThrough() && surface->inputTarget() == backdrop,
                      QStringLiteral("the hosted window routes its mouse to the backdrop"));

                // Taking the mouse has two halves and only the second one is ours.
                // Posting a QMouseEvent straight at the backdrop -- all of the
                // section above does -- proves the *machine*; it says nothing about
                // whether Windows ever delivers a real click. That delivery is what
                // T-060 was about: a per-pixel layered window is hit-tested against
                // its own alpha (MSDN, "Layered Windows": the areas whose alpha
                // value is zero let the mouse messages through), and a backdrop
                // whose frame was transparent nearly everywhere was a window the
                // cursor could never find. The gestures existed in the probes, which
                // post their events at this object, and nowhere a mouse could reach.
                //
                // It is measured on a pairing of our own windows rather than on the
                // backdrop itself, for a reason the first attempt produced: every
                // child of Progman sits below *every* top-level window, so while an
                // application covers the sample point the system answers with the
                // application -- that run reported the user's browser even with the
                // frame at alpha 1, which is a reading of the session and not of the
                // code. measureLayerAlphaHit() builds the same parent/child
                // relationship above everything instead, and asks the system's own
                // hit test who would get the click.
                const LayerAlphaHit layerHit = measureLayerAlphaHit(area);
                check(QStringLiteral("a transparent layered child lets the mouse through"),
                      layerHit.created && layerHit.transparentSkipped,
                      layerHit.created
                          ? QStringLiteral("at %1,%2 an alpha-0 frame is skipped: the cursor "
                                           "finds %3")
                                .arg(layerHit.at.x())
                                .arg(layerHit.at.y())
                                .arg(layerHit.transparentHit)
                          : layerHit.error);
                check(QStringLiteral("one alpha level is enough for the cursor to find it"),
                      layerHit.created && layerHit.flooredFound,
                      layerHit.created
                          ? QStringLiteral("at %1,%2 the cursor finds %3 with the frame at "
                                           "alpha %4")
                                .arg(layerHit.at.x())
                                .arg(layerHit.at.y())
                                .arg(layerHit.flooredHit)
                                .arg(starfish::ui::DesktopBackdrop::kInputAlphaFloor)
                          : layerHit.error);

                // Context, deliberately not a check: the cursor's path to the
                // backdrop is asked here, on a grid of bare-desktop points, so the
                // reading is "the backdrop answers N of M" rather than a single
                // point that may be the one under a window -- and so a human who
                // clears the desktop sees the backdrop named below.
                //
                // Since ADR-12 no artefact is needed: the backdrop already sits
                // above the wallpaper layer, so at a bare point the cursor finds
                // *it* even while this probe leaves the native icons visible.
                const QVector<starfish::ui::BackdropCell>& bareCells = backdrop->cells();
                int samples = 0;
                int answeredByTheBackdrop = 0;
                QString firstHit;
                QString firstMiss;
                for (int y = area.top() + 30; y < area.bottom() - 30; y += 45) {
                    for (int x = area.left() + 30; x < area.right() - 30; x += 45) {
                        const QPoint at(x, y);
                        if (starfish::ui::BackdropGesture::iconAt(bareCells, at) >= 0)
                            continue; // an icon is not bare desktop: its behaviour is T-042
                        ++samples;
                        const HWND under = ::WindowFromPoint(POINT{at.x(), at.y()});
                        if (under == hwnd) {
                            ++answeredByTheBackdrop;
                            if (firstHit.isEmpty())
                                firstHit = QStringLiteral("%1,%2").arg(at.x()).arg(at.y());
                        } else if (firstMiss.isEmpty()) {
                            firstMiss = QStringLiteral("%1,%2 -> %3")
                                            .arg(at.x())
                                            .arg(at.y())
                                            .arg(describeWindow(under));
                        }
                    }
                }
                log << QStringLiteral("-- who the cursor finds on the bare desktop --");
                log << QStringLiteral("  the backdrop answers %1 of %2 bare point(s)%3")
                           .arg(answeredByTheBackdrop)
                           .arg(samples)
                           .arg(answeredByTheBackdrop > 0
                                    ? QStringLiteral("  (first at %1 -- the desktop the user "
                                                     "right-drags on is ours)")
                                          .arg(firstHit)
                                    : QStringLiteral("  -- none answered, so either an "
                                                     "application covers every sampled point "
                                                     "(the reading below) or the frame is a shape "
                                                     "again instead of a rectangle"));
                if (!firstMiss.isEmpty())
                    log << QStringLiteral("  the first point that answers otherwise: %1 -- what "
                                          "covers the desktop is a reading of this session and not "
                                          "of the code")
                               .arg(firstMiss);
                // T-061, and the reason ADR-12 exists: hiding the icons must not
                // touch the wallpaper. Measured, not argued -- the desktop is
                // captured twice, with the native icons shown and with them hidden,
                // and the pixels our own layout does not claim are compared.
                //
                // Hiding SHELLDLL_DefView (what StarFish did until ADR-12) replaces
                // the entire background with Progman's flat colour: measured on this
                // machine, 87 % of the screen changed to a uniform (20,20,20) -- the
                // "grey desktop" the user reported. Hiding the list view inside it
                // changes only the icons (6 %). Only a fraction of the sampled
                // points can be compared, because the native icons that go away sit
                // on points our layout may leave bare; the threshold is generous on
                // purpose, and the two behaviours are nowhere near it.
                const QImage beforeHiding = grabPrimaryScreen();
                const bool hiddenNow = starfish::platform::win::DesktopSurface::setDesktopIconsHidden(true);
                ::Sleep(500);
                const QImage afterHiding = grabPrimaryScreen();
                if (hiddenNow)
                    starfish::platform::win::DesktopSurface::setDesktopIconsHidden(false);

                qint64 compared = 0;
                qint64 unchanged = 0;
                if (hiddenNow && !beforeHiding.isNull() && beforeHiding.size() == afterHiding.size()) {
                    QVector<QRect> ours = backdrop->cells().isEmpty() ? QVector<QRect>() : [&backdrop] {
                        QVector<QRect> boxes;
                        for (const starfish::ui::BackdropCell& cell : backdrop->cells())
                            boxes.append(cell.icon.united(cell.label));
                        return boxes;
                    }();
                    for (const QString& id : container.fenceIds())
                        ours.append(container.fenceRect(id));
                    const auto claimed = [&ours](const QPoint& p) {
                        for (const QRect& box : ours)
                            if (box.contains(p))
                                return true;
                        return false;
                    };
                    for (int y = 8; y < beforeHiding.height() - 8; y += 24) {
                        for (int x = 8; x < beforeHiding.width() - 8; x += 24) {
                            const QPoint p(x, y);
                            if (claimed(p))
                                continue;
                            ++compared;
                            const QColor before = beforeHiding.pixelColor(p);
                            const QColor after = afterHiding.pixelColor(p);
                            if (qAbs(before.red() - after.red()) <= 12
                                && qAbs(before.green() - after.green()) <= 12
                                && qAbs(before.blue() - after.blue()) <= 12)
                                ++unchanged;
                        }
                    }
                }
                check(QStringLiteral("hiding the icons leaves the wallpaper alone"),
                      hiddenNow && compared > 0 && unchanged * 100 >= compared * 80,
                      QStringLiteral("%1 of %2 sampled pixel(s) outside our own layout are "
                                     "unchanged (a flat background would change them all)")
                          .arg(unchanged)
                          .arg(compared));

                // T-052 is untouched by that: taking the mouse is not the same as
                // swallowing a drop. Dropping onto the desktop *moves a file*, so
                // the backdrop registers no OLE drop target -- it has no handler
                // to honour one with, and a drag still reaches what is under it.
                check(QStringLiteral("the backdrop still swallows no drop"),
                      !surface->dropHandler(),
                      QStringLiteral("no drop handler, so no drop target is registered"));
                backdrop->setGestureEnabled(false);

                const HWND progman = ::FindWindowW(L"Progman", nullptr);
                const HWND defView =
                    progman ? ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)
                            : nullptr;
                // ADR-13, and the half the pixels cannot prove: with the user's
                // icons left in place, the mouse still has to *reach* them. The
                // surface is asked the question Windows asks it (WM_NCHITTEST) at
                // a point on one of the native icons and at a bare point. Taken
                // and replayed would be a different click (T-042); stepped aside is
                // the same click. The rectangles come from the shell's own list
                // view -- the same read the product runs on its timer.
                backdrop->setGestureEnabled(true);
                const QVector<starfish::platform::win::NativeDesktopIcon> nativeIcons =
                    starfish::platform::win::readNativeDesktopIcons();
                QVector<QRect> nativeRects;
                nativeRects.reserve(nativeIcons.size());
                for (const starfish::platform::win::NativeDesktopIcon& icon : nativeIcons)
                    nativeRects.append(QRect(icon.position, icon.slot));
                backdrop->setNativeIconRects(nativeRects);

                const auto hitTest = [hwnd](const QPoint& point) {
                    return ::SendMessageW(reinterpret_cast<HWND>(hwnd), WM_NCHITTEST, 0,
                                          MAKELPARAM(point.x(), point.y()));
                };
                const auto insideANativeIcon = [&nativeRects](const QPoint& point) {
                    for (const QRect& rect : nativeRects) {
                        if (rect.contains(point))
                            return true;
                    }
                    return false;
                };

                if (nativeIcons.isEmpty()) {
                    check(QStringLiteral("the shell's own icons could be read"), false,
                          QStringLiteral("no item in the desktop list view"));
                } else {
                    const QRect iconBox(nativeIcons.first().position, nativeIcons.first().slot);
                    check(QStringLiteral("a point on one of the user's icons is not ours"),
                          hitTest(iconBox.center()) == HTTRANSPARENT,
                          QStringLiteral("WM_NCHITTEST at %1,%2 (%3) -> %4")
                              .arg(iconBox.center().x())
                              .arg(iconBox.center().y())
                              .arg(nativeIcons.first().name)
                              .arg(hitTest(iconBox.center())));
                }

                // A bare point: the first sample of a coarse grid that no native
                // icon claims, so the answer is about the wallpaper and not about
                // whatever happens to sit there.
                QPoint barePoint(-1, -1);
                for (int y = 40; y < area.height() - 40 && barePoint.x() < 0; y += 37) {
                    for (int x = 40; x < area.width() - 40; x += 37) {
                        const QPoint sample(x, y);
                        if (!insideANativeIcon(sample)) {
                            barePoint = sample;
                            break;
                        }
                    }
                }
                check(QStringLiteral("a bare point is ours (that is the draw-a-fence gesture)"),
                      barePoint.x() >= 0 && hitTest(barePoint) == HTCLIENT,
                      QStringLiteral("WM_NCHITTEST at %1,%2 -> %3, %4 native icon(s) stepped "
                                     "around")
                          .arg(barePoint.x())
                          .arg(barePoint.y())
                          .arg(barePoint.x() >= 0 ? hitTest(barePoint) : -1)
                          .arg(nativeRects.size()));

                const int backdropIndex = progmanChildIndex(hwnd);
                const int defViewIndex = progmanChildIndex(defView);
                // ADR-12: above the wallpaper (the DefView), below the fences --
                // that is what lets the wallpaper show through the backdrop's
                // transparent pixels while the empty desktop still reaches it.
                check(QStringLiteral("the backdrop sits above the wallpaper and under the fences"),
                      backdropIndex >= 0 && defViewIndex >= 0 && backdropIndex < defViewIndex,
                      QStringLiteral("z-order: backdrop [%1] vs DefView [%2] (0 is topmost)")
                          .arg(backdropIndex)
                          .arg(defViewIndex));

                container.setBackdropEnabled(false);
                check(QStringLiteral("turning it off leaves no window behind"),
                      !backdrop->isAttached() && !::IsWindow(hwnd), {});
            }
        } else {
            log << QStringLiteral("-- the real window -- skipped (pass --host to host it) --");
        }

        // The probe never hides the native icons, so the desktop is untouched by
        // construction -- but say so, because that is the property the product
        // depends on.
        check(QStringLiteral("the probe left the native icons alone"),
              !starfish::platform::win::DesktopSurface::desktopIconsHidden(),
              QStringLiteral("the desktop's icon layer is %1")
                  .arg(starfish::platform::win::DesktopSurface::desktopIconsHidden()
                           ? QStringLiteral("hidden")
                           : QStringLiteral("visible")));

        const QString reportPath = QStringLiteral("build/backdrop_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QStringLiteral("backdrop probe (T-039, ADR-9 option A)\n");
                for (const QString& line : header)
                    stream << line << '\n';
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QStringLiteral("RESULT: all checks passed\n")
                               : QStringLiteral("RESULT: %1 check(s) FAILED\n")
                                     .arg(QString::number(failures)));
            }
        }
        for (const QString& line : header)
            qCInfo(lc).noquote() << line;
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QStringLiteral("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(screenDemoOption)) {
        // T-014a (F-032): what Windows really reports, then the multi-monitor
        // rules driven through a real FenceContainer, then the one thing that had
        // only ever been *assumed* -- that Qt scales the paint to the monitor's
        // ratio. Headless throughout: no desktop, nothing written outside build/.
        QStringList header;
        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QLatin1String("%1  %2  %3")
                       .arg(ok ? QLatin1String("PASS") : QLatin1String("FAIL"))
                       .arg(step, -58)
                       .arg(detail);
        };

        // --- what this machine really reports -------------------------------
        header << QLatin1String("-- the monitors Qt sees --");
        const QList<QScreen*> realScreens = QGuiApplication::screens();
        for (QScreen* screen : realScreens) {
            const qreal dpr = screen->devicePixelRatio();
            const QRect logical = screen->geometry();
            const QString role = (screen == QGuiApplication::primaryScreen())
                                     ? QStringLiteral(" (primary)")
                                     : QString();
            header << QStringLiteral(
                          "display %1  logical %2,%3 %4x%5  dpr %6  physical %7,%8 %9x%10%11")
                          .arg(screen->name())
                          .arg(logical.x())
                          .arg(logical.y())
                          .arg(logical.width())
                          .arg(logical.height())
                          .arg(QString::number(dpr))
                          .arg(qRound(logical.x() * dpr))
                          .arg(qRound(logical.y() * dpr))
                          .arg(qRound(logical.width() * dpr))
                          .arg(qRound(logical.height() * dpr))
                          .arg(role);
        }
        check(QLatin1String("at least one monitor is reported"), !realScreens.isEmpty(),
              QStringLiteral("%1 screen(s)").arg(realScreens.size()));
#if defined(_WIN32)
        const HDC screenDc = ::GetDC(nullptr);
        const int systemDpi = screenDc ? ::GetDeviceCaps(screenDc, LOGPIXELSX) : 0;
        if (screenDc)
            ::ReleaseDC(nullptr, screenDc);
        header << QLatin1String("DPI awareness context obtained: %1").arg(dpiAwarenessText());
        header << QStringLiteral("system DPI: %1 (%2 %)")
                      .arg(systemDpi)
                      .arg(qRound(systemDpi * 100.0 / 96.0));
#else
        header << QLatin1String("DPI awareness context: Windows only");
#endif

        // --- the multi-monitor rules, on a real container --------------------
        //
        // Synthetic on purpose. The dev machine has one monitor (measured), and
        // the arrangements that actually break are the ones it cannot show: a
        // monitor to the left of the primary (negative coordinates), monitors of
        // different sizes (a dead zone that no monitor draws), mixed DPI. They
        // are built here and driven through the same FenceContainer the product
        // uses, so what is exercised is the real code, not a model of it.
        starfish::ui::ScreenLayout mixed;
        {
            starfish::ui::ScreenInfo left;
            left.geometry = QRect(-1920, 0, 1280, 1024); // left of the primary
            left.devicePixelRatio = 1.5;
            left.name = QLatin1String("\\\\.\\DISPLAY2");
            mixed.addScreen(left);

            starfish::ui::ScreenInfo right;
            right.geometry = QRect(0, 0, 1920, 1080);
            right.devicePixelRatio = 1.0;
            right.name = QLatin1String("\\\\.\\DISPLAY1");
            right.primary = true;
            mixed.addScreen(right);
        }
        const QRect primaryRect(0, 0, 1920, 1080);
        const QRect leftRect(-1920, 0, 1280, 1024);

        starfish::ui::ScreenLayout primaryOnly;
        {
            starfish::ui::ScreenInfo only;
            only.geometry = primaryRect;
            only.devicePixelRatio = 1.0;
            only.name = QLatin1String("\\\\.\\DISPLAY1");
            only.primary = true;
            primaryOnly.addScreen(only);
        }

        starfish::ui::FenceContainer container;
        container.setHostOnDesktop(false);
        container.setScreens(mixed);
        int rectChanges = 0;
        QObject::connect(&container, &starfish::ui::FenceContainer::fenceRectChanged, &container,
                         [&rectChanges](const QString&, const QRect&) { ++rectChanges; });

        const QRect virtualDesktop = container.screens().bounds();
        check(QLatin1String("the virtual desktop spans both monitors"),
              virtualDesktop.left() == -1920 && virtualDesktop.right() == 1919
                  && virtualDesktop.top() == 0 && virtualDesktop.bottom() == 1079,
              QStringLiteral("%1,%2 %3x%4")
                  .arg(virtualDesktop.x())
                  .arg(virtualDesktop.y())
                  .arg(virtualDesktop.width())
                  .arg(virtualDesktop.height()));

        // -1000,500 is on the left monitor; a point at x = -100 would fall in the
        // gap between the two (the left one only reaches x = -641).
        const starfish::ui::ScreenInfo* leftHit = container.screens().screenAt(QPoint(-1000, 500));
        check(QLatin1String("a negative coordinate resolves to the left monitor"),
              leftHit && leftHit->name == QLatin1String("\\\\.\\DISPLAY2"),
              leftHit ? leftHit->name : QLatin1String("none"));

        // The hole is real: the left monitor is shorter than the primary, so
        // there are coordinates inside the bounding box that no monitor draws.
        // Without this, the clamp check below would be vacuous.
        check(QLatin1String("the gap between the two monitors belongs to no screen"),
              container.screens().screenAt(QPoint(-100, 1050)) == nullptr,
              QLatin1String("x -100, y 1050: below the left monitor, left of the primary"));

        const starfish::ui::ScreenInfo* primary = container.screens().primary();
        check(QLatin1String("the primary monitor is still anchored at (0,0)"),
              primary && primary->geometry.topLeft() == QPoint(0, 0),
              primary ? QStringLiteral("%1 at %2,%3")
                            .arg(primary->name)
                            .arg(primary->geometry.x())
                            .arg(primary->geometry.y())
                      : QLatin1String("none"));

        const QString first =
            container.createFence(QStringLiteral("primary"), QRect(100, 100, 320, 240));
        check(QLatin1String("a fence records the monitor it actually sits on"),
              monitorOf(container, first) == QLatin1String("\\\\.\\DISPLAY1"),
              QStringLiteral("recorded \"%1\"").arg(monitorOf(container, first)));

        const QRect docked = container.setFenceRect(first, QRect(1570, 200, 344, 240), true);
        check(QLatin1String("snapping docks to the inner edge of the neighbouring monitor"),
              docked.x() + docked.width() == 1920,
              QStringLiteral("right edge landed on %1 (intended 1920)")
                  .arg(docked.x() + docked.width()));

        const QRect deadZoned = container.setFenceRect(first, QRect(-1900, 1000, 500, 200), false);
        check(QLatin1String("a fence in the dead zone between two monitors is pushed back on one"),
              leftRect.contains(deadZoned.topLeft()) && leftRect.contains(deadZoned.bottomRight()),
              QStringLiteral("%1,%2 %3x%4, dead zone was y 1024..1079")
                  .arg(deadZoned.x())
                  .arg(deadZoned.y())
                  .arg(deadZoned.width())
                  .arg(deadZoned.height()));

        const QRect onLeft = container.fenceRect(first);
        const qreal leftDpr = container.screens().devicePixelRatioFor(onLeft);
        const starfish::ui::FenceView* leftView = container.view(first);
        const QImage scaledFrame = container.frame(first);
        check(QLatin1String("a fence on the 150 % monitor is rendered at its physical size"),
              qFuzzyCompare(leftDpr, 1.5) && leftView && leftView->size() == QSize(333, 133)
                  && scaledFrame.size() == QSize(500, 200)
                  && qFuzzyCompare(scaledFrame.devicePixelRatio(), 1.5),
              QStringLiteral("dpr %1, widget %2x%3, frame %4x%5 at dpr %6")
                  .arg(QString::number(leftDpr))
                  .arg(leftView ? leftView->width() : -1)
                  .arg(leftView ? leftView->height() : -1)
                  .arg(scaledFrame.width())
                  .arg(scaledFrame.height())
                  .arg(QString::number(scaledFrame.devicePixelRatio())));

        // --- a monitor goes away --------------------------------------------
        const QString second =
            container.createFence(QStringLiteral("second"), QRect(600, 600, 320, 240));
        const QRect secondBefore = container.fenceRect(second);

        rectChanges = 0;
        container.setScreens(primaryOnly);
        const QRect broughtBack = container.fenceRect(first);
        check(QLatin1String("unplugging a monitor brings its fence back onto the primary"),
              primaryRect.contains(broughtBack.topLeft())
                  && primaryRect.contains(broughtBack.bottomRight())
                  && monitorOf(container, first) == QLatin1String("\\\\.\\DISPLAY1"),
              QStringLiteral("%1,%2 %3x%4 on %5")
                  .arg(broughtBack.x())
                  .arg(broughtBack.y())
                  .arg(broughtBack.width())
                  .arg(broughtBack.height())
                  .arg(monitorOf(container, first)));
        check(QLatin1String("only the fence that moved announced a change"), rectChanges == 1,
              QStringLiteral("%1 signal(s)").arg(rectChanges));
        check(QLatin1String("a fence that did not move keeps its rect"),
              container.fenceRect(second) == secondBefore,
              QStringLiteral("%1,%2 %3x%4")
                  .arg(container.fenceRect(second).x())
                  .arg(container.fenceRect(second).y())
                  .arg(container.fenceRect(second).width())
                  .arg(container.fenceRect(second).height()));

        rectChanges = 0;
        container.setScreens(primaryOnly);
        check(QLatin1String("re-applying the same arrangement moves nothing"), rectChanges == 0,
              QStringLiteral("%1 signal(s) -- the document must not be rewritten")
                  .arg(rectChanges));

        // --- a document written on another desk ------------------------------
        starfish::core::Layout document;
        {
            starfish::core::Fence stored;
            stored.id = QLatin1String("fence-900");
            stored.title = QLatin1String("From the document");
            stored.rect = QRect(-1500, 200, 320, 240); // off every screen here
            stored.monitor = QLatin1String("\\\\.\\DISPLAY2");
            document.fences.append(stored);
        }

        starfish::ui::FenceContainer restoredContainer;
        restoredContainer.setHostOnDesktop(false);
        restoredContainer.setScreens(primaryOnly);
        const bool applied = restoredContainer.applyLayout(document);
        const QRect restoredRect = restoredContainer.fenceRect(QLatin1String("fence-900"));
        check(QLatin1String("a document naming a monitor that is gone restores onto one that is here"),
              applied && primaryRect.contains(restoredRect.topLeft())
                  && primaryRect.contains(restoredRect.bottomRight())
                  && monitorOf(restoredContainer, QLatin1String("fence-900"))
                         == QLatin1String("\\\\.\\DISPLAY1"),
              QStringLiteral("%1,%2 %3x%4 on %5")
                  .arg(restoredRect.x())
                  .arg(restoredRect.y())
                  .arg(restoredRect.width())
                  .arg(restoredRect.height())
                  .arg(monitorOf(restoredContainer, QLatin1String("fence-900"))));

        starfish::ui::FenceContainer twoScreens;
        twoScreens.setHostOnDesktop(false);
        twoScreens.setScreens(mixed);
        const bool appliedBack = twoScreens.applyLayout(document);
        const QRect backOnSecond = twoScreens.fenceRect(QLatin1String("fence-900"));
        check(QLatin1String("with the monitor back, the fence returns to it unchanged"),
              appliedBack && backOnSecond == QRect(-1500, 200, 320, 240)
                  && monitorOf(twoScreens, QLatin1String("fence-900"))
                         == QLatin1String("\\\\.\\DISPLAY2"),
              QStringLiteral("%1,%2 %3x%4 on %5")
                  .arg(backOnSecond.x())
                  .arg(backOnSecond.y())
                  .arg(backOnSecond.width())
                  .arg(backOnSecond.height())
                  .arg(monitorOf(twoScreens, QLatin1String("fence-900"))));

        // --- the paint scale, measured ---------------------------------------
        //
        // The whole DPI story rests on Qt scaling the painter by the monitor's
        // ratio. Measure it, do not believe it: a 1.0-logical-pixel border must
        // cover `dpr` physical pixels once painted, and would cover exactly 1 at
        // every ratio if the scaling were not happening.
        header << QLatin1String("-- the paint scale, measured --");
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            const double coverage = measureBorderCoverage(dpr);
            header << QStringLiteral("1.0-logical-pixel border at dpr %1 measured %2 physical px")
                          .arg(QString::number(dpr))
                          .arg(QString::number(coverage, 'f', 2));
            check(QStringLiteral("Qt scales the paint to the monitor's ratio (border, dpr %1)")
                      .arg(QString::number(dpr)),
                  qAbs(coverage - dpr) <= 0.25,
                  QStringLiteral("measured %1 px, expected about %2")
                      .arg(QString::number(coverage, 'f', 2))
                      .arg(QString::number(dpr, 'f', 1)));
        }
        check(QLatin1String("the measurement can tell a scaled paint from an unscaled one"),
              measureBorderCoverage(2.0) > 1.5,
              QLatin1String("a painter ignoring the ratio would measure 1.00 at every dpr"));

        const QString reportPath = QLatin1String("build/screen_demo.txt");
        {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream stream(&report);
                stream << QLatin1String("screen probe (T-014a, F-032)\n");
                for (const QString& line : header)
                    stream << line << '\n';
                stream << '\n';
                for (const QString& line : log)
                    stream << line << '\n';
                stream << (failures == 0
                               ? QLatin1String("RESULT: all checks passed\n")
                               : QLatin1String("RESULT: %1 check(s) FAILED\n")
                                     .arg(QString::number(failures)));
            }
        }
        for (const QString& line : header)
            qCInfo(lc).noquote() << line;
        for (const QString& line : log)
            qCInfo(lc).noquote() << line;
        qCInfo(lc).noquote() << QLatin1String("report written to %1").arg(reportPath);

        const int exitCode = failures == 0 ? 0 : 1;
        QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
        return app.exec();
    }

    if (parser.isSet(crashVictimOption)) {
        // The child half of --crash-demo (T-046). It hides the native icons
        // through the product's own path -- the very call the tray toggle runs --
        // and then waits to be killed. Nothing here restores anything and no
        // clean end is arranged on purpose: everything the product would have
        // undone stays undone, which is exactly the state the parent measures.
        starfish::app::ProductApp::Options options;
        options.layoutPath = QLatin1String("build/crash_probe/victim.json");
        options.hostOnDesktop = true; // the hiding has to be real
        options.tray = false;
        options.settingsWindow = false;
        options.showSettingsOnStart = false;
        options.hotkeys = false;
        options.watchDesktop = false;
        options.starterFence = false; // nothing appears on the user's desk
        // The tray path is what this victim is here to exercise, so the launch must
        // not have claimed the desktop already (T-056).
        options.claimDesktop = false;

        starfish::app::ProductApp product;
        QString startError;
        if (!product.start(options, &startError)) {
            qCCritical(lc).noquote() << QStringLiteral("crash victim: %1").arg(startError);
            return 2;
        }

        product.setDesktopIconsHidden(true);
        if (!product.desktopIconsHidden()) {
            qCCritical(lc) << "crash victim: the native icons could not be hidden";
            return 3;
        }

        qCInfo(lc).noquote()
            << QStringLiteral("crash victim: the native icons are hidden and recorded (%1); "
                              "waiting to be killed")
                   .arg(starfish::core::IconHidingMarker().description());
        // Long enough that only the parent ends this process in practice.
        QTimer::singleShot(10 * 60 * 1000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    if (parser.isSet(crashDemoOption)) {
        // T-046: the safety net around ADR-9. Hiding the native icon layer is a
        // change to the whole session and only the process that made it would
        // undo it, so a hard kill would leave the desktop with no icons at all.
        // The net is a marker on disk: written before the layer is hidden, read
        // by the next launch, which restores the icons without needing anything
        // from the dead process.
        //
        // This probe hides the *real* desktop icons for a moment and kills the
        // process that did it. It needs a real desktop, and it expects the icons
        // to be visible when it starts.
        QStringList header;
        QStringList log;
        int failures = 0;
        auto check = [&log, &failures](const QString& step, bool ok, const QString& detail) {
            if (!ok)
                ++failures;
            log << QLatin1String("%1  %2  %3")
                       .arg(ok ? QLatin1String("PASS") : QLatin1String("FAIL"))
                       .arg(step, -58)
                       .arg(detail);
        };
        auto waitMs = [](int ms) {
            QEventLoop loop;
            QTimer::singleShot(ms, &loop, &QEventLoop::quit);
            loop.exec();
        };

        const QString probeDir = QLatin1String("build/crash_probe");
        QDir().mkpath(probeDir);
        const QString layoutPath = probeDir + QLatin1String("/layout.json");
        QFile::remove(layoutPath);

        starfish::core::IconHidingMarker marker;
        auto iconsVisible = [] {
            return !starfish::platform::win::DesktopSurface::desktopIconsHidden();
        };
        // Read once, before anything touches the layer: the probe promises to
        // leave the desktop the way it found it.
        const bool iconsVisibleAtStart = iconsVisible();

        QProcess victim;
        auto finish = [&](int exitCode) {
            // Whatever happened above, give back what is ours -- and only that.
            // A marker on disk is the proof that a hidden layer came from a
            // StarFish run; the user's own hidden icons are none of our business.
            // The victim goes first, so it cannot touch the layer mid-cleanup.
            if (victim.state() != QProcess::NotRunning) {
                victim.kill();
                victim.waitForFinished(3000);
            }
            if (!iconsVisible() && marker.isPresent())
                starfish::platform::win::DesktopSurface::setDesktopIconsHidden(false);
            if (marker.isPresent())
                marker.remove();
            QDir(probeDir).removeRecursively();

            check(QLatin1String("the probe left no marker behind"), !marker.isPresent(),
                  marker.path());
            check(QLatin1String("the probe left the desktop as it found it"),
                  iconsVisible() == iconsVisibleAtStart,
                  QStringLiteral("icons visible at the start: %1, at the end: %2")
                      .arg(iconsVisibleAtStart ? QLatin1String("yes") : QLatin1String("no"),
                           iconsVisible() ? QLatin1String("yes") : QLatin1String("no")));
            check(QLatin1String("the probe cleaned up after itself"),
                  !QFileInfo::exists(probeDir), probeDir);

            const QString reportPath = QLatin1String("build/crash_demo.txt");
            {
                QFile report(reportPath);
                if (report.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                    QTextStream stream(&report);
                    stream << QLatin1String("crash-repair probe (T-046)\n");
                    for (const QString& line : header)
                        stream << line << '\n';
                    for (const QString& line : log)
                        stream << line << '\n';
                    stream << (failures == 0
                                   ? QLatin1String("RESULT: all checks passed\n")
                                   : QLatin1String("RESULT: %1 check(s) FAILED\n")
                                         .arg(QString::number(failures)));
                }
            }
            for (const QString& line : header)
                qCInfo(lc).noquote() << line;
            for (const QString& line : log)
                qCInfo(lc).noquote() << line;
            qCInfo(lc).noquote() << QLatin1String("report written to %1").arg(reportPath);

            QTimer::singleShot(0, &app, [&app, exitCode] { QCoreApplication::exit(exitCode); });
        };

        // --- the premise -------------------------------------------------------
        const bool desktopUp = starfish::platform::win::DesktopSurface::isDesktopAvailable();
        check(QLatin1String("a real desktop is available (Explorer is running)"), desktopUp,
              desktopUp ? QLatin1String("Progman / SHELLDLL_DefView found")
                        : QLatin1String("no desktop: this probe cannot run headless"));
        if (!desktopUp) {
            finish(1);
            return app.exec();
        }

        // A marker already on disk is a real leftover from an earlier bad end --
        // repaired here the way any launch would, before anything is measured.
        if (marker.isPresent()) {
            const QString leftover = marker.description();
            const bool repaired =
                starfish::platform::win::DesktopSurface::setDesktopIconsHidden(false);
            check(QLatin1String("a marker already on disk is repaired the way a launch would"),
                  repaired && !marker.isPresent(), leftover);
        }

        check(QLatin1String("the native icons are visible to begin with"), iconsVisibleAtStart,
              QLatin1String("the measurement needs to see them hidden and shown again"));
        if (!iconsVisibleAtStart) {
            finish(1);
            return app.exec();
        }

        header << QStringLiteral("victim: %1 --crash-victim")
                      .arg(QCoreApplication::applicationFilePath());
        header << QStringLiteral("marker: %1").arg(marker.path());

        // --- the victim: hide through the product's own path, then die hard ----
        victim.setProgram(QCoreApplication::applicationFilePath());
        victim.setArguments({QLatin1String("--crash-victim")});
        victim.setProcessChannelMode(QProcess::MergedChannels);
        victim.start();
        check(QLatin1String("the victim starts"),
              victim.waitForStarted(10000),
              QStringLiteral("pid %1").arg(QString::number(victim.processId())));

        // Wait for the two facts that are visible from *outside* the victim: the
        // marker on disk, and the layer really hidden inside Explorer's window.
        // Both are read by this process, so neither can be a self-report.
        bool markerSeen = false;
        bool hiddenSeen = false;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 20000 && victim.state() != QProcess::NotRunning) {
            if (marker.isPresent())
                markerSeen = true;
            if (!iconsVisible())
                hiddenSeen = true;
            if (markerSeen && hiddenSeen)
                break;
            waitMs(50);
        }
        const QString victimOutput = QString::fromLocal8Bit(victim.readAll()).trimmed();
        if (!victimOutput.isEmpty())
            log << QLatin1String("victim said: ") + victimOutput;

        check(QLatin1String("the victim recorded the hiding (marker on disk)"), markerSeen,
              marker.description());
        check(QLatin1String("the hiding is real (read from outside the victim)"), hiddenSeen,
              QLatin1String("the shell reports its icon list hidden"));
        if (!markerSeen || !hiddenSeen) {
            finish(1);
            return app.exec();
        }

        // --- the brutal end ----------------------------------------------------
        const bool wasRunning = victim.state() != QProcess::NotRunning;
        if (wasRunning)
            victim.kill(); // TerminateProcess: no teardown, no handlers, nothing
        const bool victimGone = victim.waitForFinished(5000);
        check(QLatin1String("the victim died hard (no clean shutdown ran)"),
              wasRunning && victimGone && victim.exitStatus() == QProcess::CrashExit,
              QStringLiteral("exit code %1, %2")
                  .arg(victim.exitCode())
                  .arg(victim.exitStatus() == QProcess::CrashExit ? QLatin1String("crash exit")
                                                                  : QLatin1String("normal exit")));

        // The crash must really be one. Had any teardown run, the marker would be
        // gone and the icons back -- which would make the next check vacuous.
        check(QLatin1String("the crash left the marker on disk"), marker.isPresent(),
              marker.description());
        check(QLatin1String("the crash left the native icons hidden"), !iconsVisible(), {});

        // --- the next launch repairs it ----------------------------------------
        {
            starfish::app::ProductApp::Options options;
            options.layoutPath = layoutPath;
            options.hostOnDesktop = false;
            options.tray = false;
            options.settingsWindow = false;
            options.showSettingsOnStart = false;
            options.hotkeys = false;
            options.watchDesktop = false;
            options.starterFence = false;

            starfish::app::ProductApp next;
            QString startError;
            const bool started = next.start(options, &startError);
            check(QLatin1String("the next launch starts"), started,
                  started ? next.statusText() : startError);
            check(QLatin1String("the next launch gives the icons back, with no memory of the crash"),
                  started && iconsVisible(), QLatin1String("restored from the marker alone"));
            check(QLatin1String("...and removes the marker"), !marker.isPresent(),
                  marker.path());
            next.shutdown();
        }

        // --- the emergency exit, in a state the ordinary toggle cannot reach ---
        {
            starfish::app::ProductApp::Options options;
            options.layoutPath = probeDir + QLatin1String("/emergency.json");
            options.hostOnDesktop = false; // the exit must work even here
            options.tray = false;
            options.settingsWindow = false;
            options.showSettingsOnStart = false;
            options.hotkeys = false;
            options.watchDesktop = false;
            options.starterFence = false;

            starfish::app::ProductApp product;
            QString startError;
            const bool started = product.start(options, &startError);
            check(QLatin1String("a product instance starts again"), started,
                  started ? product.statusText() : startError);

            // Put the desktop in exactly the state the emergency exit exists for:
            // really hidden, while this instance remembers nothing about it (it
            // never asked for the hiding -- which is what a crash leaves behind).
            const bool hid =
                starfish::platform::win::DesktopSurface::setDesktopIconsHidden(true);
            check(QLatin1String("the layer can be hidden behind the product's back"),
                  hid && !iconsVisible(),
                  hid ? QLatin1String("hidden") : QLatin1String("refused"));
            check(QLatin1String("...and the marker records it"), marker.isPresent(),
                  marker.description());

            // The premise of the exit: the ordinary toggle is a no-op here -- it
            // only acts when the requested state differs from the remembered one.
            product.setDesktopIconsHidden(false);
            check(QLatin1String("the ordinary toggle cannot reach a state it does not know"),
                  !iconsVisible(),
                  QLatin1String("still hidden after an ordinary \"show icons\" request"));

            product.restoreDesktopIcons();
            check(QLatin1String("the emergency exit gives the icons back anyway"),
                  iconsVisible(), {});
            check(QLatin1String("the emergency exit clears the marker"), !marker.isPresent(),
                  marker.path());

            product.shutdown();
        }

        finish(failures == 0 ? 0 : 1);
        return app.exec();
    }

    // ---- the desktop's own menu entry (T-071) ------------------------------
    //
    // F-001 lives here now: a static verb Explorer draws in its own desktop menu,
    // and a command line that asks the *running* product for a fence. Nothing of
    // ours is on the desktop, so the icons keep their clicks, their drags and
    // their menu, and the shell keeps its rubber band.
    if (parser.isSet(installMenuOption)) {
        QString installError;
        const bool done = starfish::platform::win::installDesktopMenu(&installError);
        qCInfo(lc).noquote() << (done
                                     ? QStringLiteral("desktop menu entry installed: \"%1\" -> %2")
                                           .arg(starfish::platform::win::desktopMenuLabel(),
                                                starfish::platform::win::desktopMenuCommand())
                                     : QStringLiteral("desktop menu entry NOT installed: %1")
                                           .arg(installError));
        return done ? 0 : 1;
    }
    if (parser.isSet(uninstallMenuOption)) {
        QString removeError;
        const bool done = starfish::platform::win::uninstallDesktopMenu(&removeError);
        qCInfo(lc).noquote() << (done
                                     ? QStringLiteral("desktop menu entry removed")
                                     : QStringLiteral("desktop menu entry NOT removed: %1")
                                           .arg(removeError));
        return done ? 0 : 1;
    }
    if (parser.isSet(newFenceOption)) {
        if (starfish::platform::win::requestFenceFromRunningInstance()) {
            // The running product owns the document, so *it* has to be the one that
            // makes the fence -- and it will, within a quarter of a second.
            qCInfo(lc).noquote() << QStringLiteral("asked the running StarFish for a new fence");
            return 0;
        }
        // Nothing running: fall through and start the product, which honours the
        // request on its way up (the event is already set).
        qCInfo(lc).noquote() << QStringLiteral("no StarFish running -- starting one, with a fence");
    }

    // ---- product mode ------------------------------------------------------
    //
    // Reaching this point means no diagnostic probe was asked for: this is the
    // application itself, the thing a double click launches. It reads the
    // document, puts the fences on the desktop, shows the tray icon and the
    // settings window, and writes everything back on the way out. Suspending the
    // machine is not the only way to leave: closing the settings window only
    // hides it, and quitting goes through the tray.
    {
        // One instance at a time (session 26). Two copies fight over exactly the
        // two things that make the product look broken: the layout document
        // (last write wins, so fences disappear) and the state of the native
        // icon layer (each copy restores icons the other expected to stay
        // hidden). Measured on the real desktop: two instances started a minute
        // apart, and the confusion reads as a bug in the fences themselves.
        //
        // Scoped to the product on purpose: the diagnostic probes must stay
        // runnable while the application is open, or they stop being useful.
        // The handle is deliberately *not* closed -- holding it is the lock, and
        // the OS releases it when this process ends.
#if defined(_WIN32)
        HANDLE singleInstance = ::CreateMutexW(nullptr, TRUE, L"Local\\StarFish.Desktop");
        if (singleInstance && ::GetLastError() == ERROR_ALREADY_EXISTS) {
            ::CloseHandle(singleInstance);
            qCWarning(lc).noquote()
                << "StarFish is already running -- not starting a second copy "
                   "(its icon is in the notification area)";
            return 0;
        }
#endif

        starfish::app::ProductApp product;
        starfish::app::ProductApp::Options options;
        if (parser.isSet(layoutOption))
            options.layoutPath = parser.value(layoutOption);
        options.hostOnDesktop = !parser.isSet(noHostOption);
        // T-071: the desktop's own menu entry (`--new-fence`) created this launch.
        options.newFence = parser.isSet(newFenceOption);
        options.tray = !parser.isSet(noTrayOption);
        options.settingsWindow = !parser.isSet(noSettingsOption);
        // The settings window is scaffolding, not the product (T-059): it lists the
        // fences and creates them, which is what the desktop's own gesture is for.
        // It stays reachable from the tray; it does not open itself at launch.
        options.showSettingsOnStart = false;
        // ADR-13: nothing is taken from the desktop unless it is asked for. The
        // ordinary launch leaves the wallpaper and the user's icons exactly as
        // they are; --claim-desktop brings T-056's behaviour back, for the dev
        // path and for the probes that measure it.
        options.claimDesktop = parser.isSet(claimDesktopOption);
        options.hotkeys = !parser.isSet(noHotkeysOption);
        options.watchDesktop = !parser.isSet(noWatchOption);

        QString startError;
        if (!product.start(options, &startError)) {
            qCCritical(lc).noquote() << startError;
            return 1;
        }

        QObject::connect(&product, &starfish::app::ProductApp::quitRequested, &app,
                         &QCoreApplication::quit);

        // `--seconds N` closes the application after N seconds, like every other
        // demo. It is how the product mode is exercised without a hand on the
        // tray, and the only path that runs a *clean* shutdown (document written,
        // native icons restored).
        if (parser.isSet(secondsOption)) {
            bool parsedSeconds = false;
            const int seconds = parser.value(secondsOption).toInt(&parsedSeconds);
            if (parsedSeconds && seconds > 0)
                QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
        }

        qCInfo(lc).noquote() << QStringLiteral("StarFish %1").arg(STARFISH_VERSION);
        qCInfo(lc).noquote() << product.statusText();

        const int exitCode = app.exec();
        // Restore the native icons and write the last change before handing back
        // to main: the destructor would do it too, but only after Qt starts
        // tearing itself down.
        product.shutdown();
        return exitCode;
    }
}
