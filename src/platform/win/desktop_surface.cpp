#include "platform/win/desktop_surface.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>

#include "platform/win/shell_drop_target.hpp"

#include "core/icon_hiding_marker.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#include <ole2.h>     // OleInitialize / OleUninitialize (OLE drag-and-drop)

namespace starfish::platform::win {

namespace {

QLoggingCategory lc("starfish.surface");

constexpr wchar_t kWindowClass[] = L"StarFishDesktopSurface";
constexpr int kWatchdogIntervalMs = 2000;

inline HWND toHwnd(quintptr handle)
{
    return reinterpret_cast<HWND>(handle);
}

inline quintptr fromHwnd(HWND hwnd)
{
    return reinterpret_cast<quintptr>(hwnd);
}

// Progman -> SHELLDLL_DefView on recent Windows 11; on Windows 10 the DefView
// can live inside a top-level WorkerW instead. Both shapes were measured by the
// T-003 spike (ADR-4).
//
// The *list view* inside it is what matters since ADR-12, and the two windows do
// not hold the same thing: the wallpaper belongs to SHELLDLL_DefView (hiding it
// leaves Progman's flat background -- the "grey desktop" a user reported),
// while the icons belong to its child SysListView32. Hiding the child removes
// the icons and keeps the wallpaper, which is exactly what Windows' own "Show
// desktop icons" toggle does. Measured 2026-10-16: hiding the DefView changed
// 87 % of the desktop's pixels to a flat (20,20,20); hiding the list view changed
// 6 % (the icons) and left every wallpaper pixel alone.
struct DesktopHandles {
    HWND iconHost = nullptr;
    HWND defView = nullptr;
    HWND listView = nullptr; // SysListView32 under the DefView, when there is one
};

// Where a surface of ours belongs when it has to sit *directly above the
// wallpaper layer* (ADR-12). Note the Win32 convention: SetWindowPos'
// hwndInsertAfter puts the window *below* the handle it is given, so "above the
// DefView" is expressed as "below whatever already sits directly above it" --
// the lowest fence when there is one, and the top of the list otherwise.
//
// The self-hop is what makes it idempotent: when the window directly above the
// wallpaper is already this very surface, the slot is right, and the anchor
// becomes the window above *it* (again the lowest fence) rather than HWND_TOP,
// which would push the backdrop above the fences it has to stay under.
HWND anchorAboveWallpaper(HWND defView, HWND self)
{
    HWND anchor = defView ? ::GetWindow(defView, GW_HWNDPREV) : nullptr;
    if (anchor == self)
        anchor = self ? ::GetWindow(self, GW_HWNDPREV) : nullptr;
    return anchor ? anchor : HWND_TOP;
}

DesktopHandles discoverDesktop()
{
    DesktopHandles handles;
    const auto listViewOf = [](HWND defView) {
        return ::FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    };
    if (HWND progman = ::FindWindowW(L"Progman", nullptr)) {
        if (HWND defView = ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
            handles.iconHost = progman;
            handles.defView = defView;
            handles.listView = listViewOf(defView);
            return handles;
        }
    }

    HWND worker = nullptr;
    while ((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        if (HWND defView = ::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)) {
            handles.iconHost = worker;
            handles.defView = defView;
            handles.listView = listViewOf(defView);
            break;
        }
    }
    return handles;
}

// Draws a Qt frame into a window DC. StretchDIBits ignores the alpha channel,
// which is what we want here: the surface is uniformly opaque (LWA_ALPHA at
// 255), so per-pixel transparency would need UpdateLayeredWindow -- and that
// call is refused on a child window of another process (T-017).
int blitFrame(HDC dc, const QImage& frame, const QRect& area = QRect())
{
    if (frame.isNull() || frame.bytesPerLine() != frame.width() * 4)
        return -1;
    // Session 63: only the rows of the update rectangle. The DIB handed over
    // starts at its first row and is exactly as tall, so the source rectangle
    // is unambiguous whichever way GDI counts a top-down DIB's rows.
    const QRect rect = area.isNull() ? frame.rect() : area.intersected(frame.rect());
    if (rect.isEmpty())
        return 0;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width();
    info.bmiHeader.biHeight = -rect.height(); // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    ::SetStretchBltMode(dc, COLORONCOLOR);
    return ::StretchDIBits(dc, rect.left(), rect.top(), rect.width(), rect.height(), rect.left(), 0,
                           rect.width(), rect.height(), frame.constScanLine(rect.top()), &info,
                           DIB_RGB_COLORS, SRCCOPY);
}

} // namespace

// The C callback the window class is registered with. It looks the surface up
// through GWLP_USERDATA, so several surfaces can coexist.
qintptr starfishSurfaceProc(quintptr hwnd, unsigned message, quintptr wParam, quintptr lParam)
{
    auto* surface = reinterpret_cast<DesktopSurface*>(
        ::GetWindowLongPtrW(toHwnd(hwnd), GWLP_USERDATA));

    if (surface) {
        qintptr result = 0;
        if (surface->handleMessage(hwnd, message, wParam, lParam, &result))
            return result;
    }
    return ::DefWindowProcW(toHwnd(hwnd), message, static_cast<WPARAM>(wParam),
                            static_cast<LPARAM>(lParam));
}

DesktopSurface::DesktopSurface(QObject* parent)
    : QObject(parent)
{
    // A hosted window is a child of Progman and dies with it; a WS_CHILD of
    // Progman does not receive the WM_SETTINGCHANGE / WM_DISPLAYCHANGE
    // broadcasts, so the placement is re-checked on a timer (ADR-4).
    m_watchdog.setInterval(kWatchdogIntervalMs);
    connect(&m_watchdog, &QTimer::timeout, this, &DesktopSurface::watchdogTick);
}

DesktopSurface::~DesktopSurface()
{
    detach();
}

bool DesktopSurface::isDesktopAvailable()
{
    return discoverDesktop().defView != nullptr;
}

bool DesktopSurface::ensureWindowClass()
{
    static bool registered = false;
    if (registered)
        return true;

    // Deliberately mirrors the T-003 spike's class: no CS_HREDRAW/CS_VREDRAW and
    // a solid background brush. The class is part of the reference sequence that
    // is known to composite, so it is not the place to be creative. CS_DBLCLKS
    // is required so WM_LBUTTONDBLCLK is delivered and forwarded for double-click.
    static const HBRUSH background = ::CreateSolidBrush(RGB(0x1F, 0x29, 0x37));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&starfishSurfaceProc);
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = kWindowClass;

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        qCCritical(lc) << "RegisterClassExW failed, error" << ::GetLastError();
        return false;
    }
    registered = true;
    return true;
}

bool DesktopSurface::attach(const QRect& rect)
{
    detach();

    if (!ensureWindowClass())
        return false;
    if (!refreshDesktopHandles()) {
        qCWarning(lc) << "attach: desktop host not found (Progman / SHELLDLL_DefView)";
        return false;
    }

    // 1. Created *with* WS_EX_LAYERED, while it is still top-level.
    //
    //    The layered flag is MANDATORY (ADR-4): a non-layered child of Progman
    //    is not composited by DWM -- the z-order is right, IsWindowVisible() is
    //    true, and nothing is drawn. A silent failure.
    //
    //    Creating the window with the flag is also the order that survives every
    //    build of this binary. Measured in session 9 with
    //    experiments/spike_surface_min.cpp (pure Win32, no Qt, no widget), on a
    //    child of Progman, same rect, same process state:
    //
    //      CreateWindowExW(WS_EX_LAYERED), then SetParent
    //          -> flag kept, alpha accepted, 152710 / 153600 px of the test rect
    //             on screen (99.4 %)
    //      SetWindowLongW(GWL_EXSTYLE, ... | WS_EX_LAYERED) before SetParent
    //          -> flag kept, alpha accepted, 0 px on screen: the bit is set but
    //             no layered surface is ever created for the window
    //      SetWindowLongW(GWL_EXSTYLE, ... | WS_EX_LAYERED) after SetParent
    //          -> refused, and the reason is not obvious: see below
    //
    //    The last case is the interesting one. On a *manifest-less* binary the
    //    call returns 0, GetLastError() stays 0, the style reads back unchanged,
    //    and SetLayeredWindowAttributes then fails with
    //    ERROR_INVALID_PARAMETER (87). It is specific to the LAYERED bit --
    //    WS_EX_TRANSPARENT, WS_EX_TOOLWINDOW and WS_EX_NOACTIVATE all stick on
    //    the very same window -- and it is not a Qt artefact: a plain C++ process
    //    reproduces it, while the same sequence in Python succeeds.
    //
    //    Root cause: WS_EX_LAYERED on a CHILD window is only supported from
    //    Windows 8 onwards, and Windows picks the behaviour from the
    //    application's manifest. A MinGW build has no manifest by default, so it
    //    gets the legacy behaviour and the write is refused. python.exe ships a
    //    manifest, which is why the Python spike never saw this. Adding
    //    src/starfish.manifest fixed it: the same source and the same sequence
    //    then composite 99.4 % with the flag added *after* SetParent.
    //
    //    Both fixes are kept on purpose. The manifest is the correct one; the
    //    creation-time flag makes the surface work even from a binary whose
    //    manifest went missing, where the failure is completely silent.
    HWND hwnd = ::CreateWindowExW(WS_EX_LAYERED, kWindowClass, nullptr, WS_POPUP, rect.x(),
                                  rect.y(), rect.width(), rect.height(), nullptr, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        qCCritical(lc) << "CreateWindowExW failed, error" << ::GetLastError();
        return false;
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_hwnd = fromHwnd(hwnd);
    m_rect = rect;

    ::ShowWindow(hwnd, m_visible ? SW_SHOW : SW_HIDE);
    ::UpdateWindow(hwnd);

    // 2. Reparent to the icon host and become a child window.
    if (!::SetParent(hwnd, toHwnd(m_iconHost))) {
        qCWarning(lc) << "SetParent failed, error" << ::GetLastError();
        detach();
        return false;
    }
    qCInfo(lc).noquote() << QStringLiteral("after SetParent: owner thread %1 (current %2)")
                                .arg(::GetWindowThreadProcessId(hwnd, nullptr))
                                .arg(::GetCurrentThreadId());

    const LONG style = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_STYLE));
    ::SetWindowLongW(hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);

    // 3. The layered flag has to have survived the reparenting. Nothing here
    //    tries to add it: that is the write Windows refuses (see above). It is
    //    verified instead of assumed.
    //
    //    In per-pixel mode (T-017) the SetLayeredWindowAttributes call is
    //    SKIPPED on purpose: measured in session 9 (experiments/spike_ulw2.py),
    //    once LWA_ALPHA has been applied to a child window, every
    //    UpdateLayeredWindow push fails with ERROR_INVALID_PARAMETER (87).
    //    Without the attributes call the same push succeeds -- even on a
    //    cross-process child of Progman. The two layered modes are mutually
    //    exclusive, and the attributes call is a one-way door.
    const LONG exAfterHosting = static_cast<LONG>(::GetWindowLongW(hwnd, GWL_EXSTYLE));
    BOOL attributesOk = TRUE;
    DWORD attributesError = 0;
    if (!m_perPixelAlpha) {
        // ADR-15: this call IS the fence's transparency -- the material is a
        // uniform alpha for the window, and it is the only layered mode that
        // reaches the screen on a child of Progman (measured, see the header).
        // It is not a one-way door for the *blit* path as it was for the
        // per-pixel one: setUniformAlpha() can change it live, so switching a
        // material no longer means recreating the window (T-057 used to).
        ::SetLastError(0);
        attributesOk = ::SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(m_uniformAlpha),
                                                    LWA_ALPHA);
        attributesError = ::GetLastError();
    }

    qCInfo(lc).noquote()
        << QStringLiteral("layered: ex 0x%1 (expected 0x%2); alpha call %3 (err %4)%5")
               .arg(static_cast<qulonglong>(exAfterHosting), 0, 16)
               .arg(static_cast<qulonglong>(WS_EX_LAYERED), 0, 16)
               .arg(m_perPixelAlpha ? -1 : (attributesOk ? 1 : 0))
               .arg(attributesError)
               .arg(m_perPixelAlpha ? QStringLiteral(" [skipped: per-pixel push mode]")
                                    : QString());

    if (!(exAfterHosting & WS_EX_LAYERED))
        qCCritical(lc) << "WS_EX_LAYERED missing after hosting -- the surface will not be "
                          "composited by DWM (invisible, ADR-4)";
    if (!m_perPixelAlpha && !attributesOk)
        qCCritical(lc) << "SetLayeredWindowAttributes failed -- the surface will not be "
                          "composited by DWM (invisible, ADR-4)";

    // 4. Just below the icon layer.
    m_placed = place();

    // 5. OLE drag-and-drop so the Explorer can drop files onto the hosted
    //    window (T-006-3). OleInitialize must run on the thread that calls
    //    RegisterDragDrop; the Qt main thread is already an STA, so it pairs
    //    with OleUninitialize in detach(). m_oleInit is set on success (or when
    //    OLE was already initialised by somebody else) so exactly one init is
    //    balanced by one uninit.
    //
    //    Registered from a drop *handler*, not from the click-through switch
    //    (T-041). The two used to be the same thing, because the only
    //    non-click-through surfaces were fences and every fence has a handler.
    //    The backdrop broke that: it takes the mouse to answer the empty
    //    desktop's own gestures (F-001/F-022) while owning no drop behaviour at
    //    all, and registering a target with nothing behind it would swallow a
    //    drop it cannot honour -- the silent regression T-052 is about. A
    //    surface with no handler registers nothing, and the drag reaches
    //    whatever is underneath.
    if (!m_oleInit && (m_dropHandler || m_dropTargetFactory)) {
        const HRESULT hr = ::OleInitialize(nullptr);
        if (SUCCEEDED(hr)) {
            m_oleInit = true;
        } else {
            qCWarning(lc) << "OleInitialize failed, error" << hr
                          << "-- Explorer drop disabled on this surface";
        }
    }
    if (m_oleInit && m_dropTargetFactory && !m_customDropTarget) {
        m_customDropTarget = m_dropTargetFactory(m_hwnd);
        const HRESULT hr = m_customDropTarget
                               ? ::RegisterDragDrop(toHwnd(m_hwnd),
                                                    static_cast<IDropTarget*>(m_customDropTarget))
                               : E_FAIL;
        if (FAILED(hr)) {
            qCWarning(lc) << "RegisterDragDrop (custom target) failed, error" << hr;
            if (m_customDropTarget)
                static_cast<IUnknown*>(m_customDropTarget)->Release();
            m_customDropTarget = nullptr;
        }
    }
    if (m_oleInit && !m_dropTarget && !m_dropTargetFactory) {
        m_dropTarget = new starfish::platform::win::ShellDropTarget(
            toHwnd(m_hwnd), [this](const QPoint& client, const QStringList& paths) {
                if (m_dropHandler)
                    m_dropHandler(client, paths);
            });
        const HRESULT hr = ::RegisterDragDrop(toHwnd(m_hwnd), m_dropTarget);
        if (FAILED(hr)) {
            qCWarning(lc) << "RegisterDragDrop failed, error" << hr
                          << "-- Explorer drop disabled on this surface";
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
    }

    m_watchdog.start();

    qCInfo(lc).noquote() << QStringLiteral("surface 0x%1 hosted at %2,%3 %4x%5 (layered, alpha "
                                           "255, placed %6)")
                                .arg(fromHwnd(hwnd), 0, 16)
                                .arg(rect.x())
                                .arg(rect.y())
                                .arg(rect.width())
                                .arg(rect.height())
                                .arg(m_placed ? 1 : 0);
    return m_placed;
}

void DesktopSurface::detach()
{
    m_watchdog.stop();
    if (m_hwnd && ::IsWindow(toHwnd(m_hwnd))) {
        // Revoke and release the OLE drop target before the window goes away,
        // and before we clear USERDATA (the target's methods may still fire on
        // the calling thread until RevokeDragDrop returns).
        if (m_dropTarget) {
            ::RevokeDragDrop(toHwnd(m_hwnd));
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
        releaseCustomDropTarget();
        // Drop the user data first: WM_DESTROY must not call back into a
        // half-destroyed object.
        ::SetWindowLongPtrW(toHwnd(m_hwnd), GWLP_USERDATA, 0);
        ::DestroyWindow(toHwnd(m_hwnd));
    }
    if (m_oleInit) {
        ::OleUninitialize();
        m_oleInit = false;
    }
    m_hwnd = 0;
    m_placed = false;
}

bool DesktopSurface::isAttached() const
{
    return m_hwnd != 0 && ::IsWindow(toHwnd(m_hwnd));
}

quintptr DesktopSurface::hwnd() const
{
    return m_hwnd;
}

QRect DesktopSurface::geometry() const
{
    return m_rect;
}

void DesktopSurface::updateFrame(const QImage& frame, const QRect& dirty)
{
    const QRect rect = dirty.intersected(frame.rect());
    if (m_perPixelAlpha || m_frame.isNull() || m_frame.size() != frame.size()
        || m_frame.format() != QImage::Format_ARGB32 || frame.depth() != 32) {
        setFrame(frame);
        return;
    }
    if (rect.isEmpty())
        return;
    // Opaque frames: premultiplied and straight ARGB are the same bytes.
    const qsizetype bytes = qsizetype(rect.width()) * 4;
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        memcpy(m_frame.scanLine(y) + qsizetype(rect.left()) * 4, frame.constScanLine(y) + qsizetype(rect.left()) * 4,
               bytes);
    if (!m_hwnd || !::IsWindow(toHwnd(m_hwnd)))
        return;
    const RECT rc{rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1};
    ::InvalidateRect(toHwnd(m_hwnd), &rc, FALSE);
}

void DesktopSurface::raiseToTop()
{
    if (!m_hwnd || !::IsWindow(toHwnd(m_hwnd)))
        return;
    // WM_WINDOWPOSCHANGING keeps the rules: a panel paired with an overlay
    // still lands directly below it.
    ::SetWindowPos(toHwnd(m_hwnd), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool DesktopSurface::isAbove(const DesktopSurface* other) const
{
    if (!other || !m_hwnd || !other->m_hwnd)
        return false;
    const HWND target = toHwnd(other->m_hwnd);
    for (HWND window = ::GetWindow(toHwnd(m_hwnd), GW_HWNDNEXT); window; window = ::GetWindow(window, GW_HWNDNEXT)) {
        if (window == target)
            return true;
    }
    return false;
}

void DesktopSurface::repaintNow()
{
    if (m_hwnd && ::IsWindow(toHwnd(m_hwnd)) && !m_perPixelAlpha)
        ::UpdateWindow(toHwnd(m_hwnd));
}

void DesktopSurface::setFrame(const QImage& frame)
{
    m_frame = frame.convertToFormat(m_perPixelAlpha ? QImage::Format_ARGB32_Premultiplied
                                                    : QImage::Format_ARGB32);
    if (!m_hwnd || !::IsWindow(toHwnd(m_hwnd)))
        return;
    if (m_perPixelAlpha) {
        pushLayeredFrame();
        return;
    }
    ::InvalidateRect(toHwnd(m_hwnd), nullptr, FALSE);
}

bool DesktopSurface::setPerPixelAlpha(bool enabled)
{
    if (enabled == m_perPixelAlpha)
        return true;
    m_perPixelAlpha = enabled;
    if (!isAttached())
        return true;
    if (enabled) {
        // Switching to per-pixel mode on a live surface requires recreating
        // the window: attach() applies SetLayeredWindowAttributes(LWA_ALPHA)
        // in opaque mode, and after that call every UpdateLayeredWindow push
        // fails with ERROR_INVALID_PARAMETER (87) -- measured in session 9
        // with experiments/spike_ulw2.py. The attributes call is a one-way
        // door, so the surface is re-attached and attach() now skips it.
        const QImage frame = m_frame; // attach() -> detach() does not clear it
        if (!attach(m_rect)) {
            m_perPixelAlpha = false;
            return false;
        }
        m_frame = frame;
        if (m_frame.isNull())
            return true; // the first setFrame() will push
        m_frame = m_frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        return pushLayeredFrame();
    }
    // Back to the WM_PAINT path. The reverse switch needs a re-attach too:
    // after ULW pushes the window ignores plain GDI painting.
    const QImage frame = m_frame;
    if (!attach(m_rect))
        return false;
    m_frame = frame.convertToFormat(QImage::Format_ARGB32);
    ::InvalidateRect(toHwnd(m_hwnd), nullptr, FALSE);
    return true;
}

bool DesktopSurface::perPixelAlpha() const
{
    return m_perPixelAlpha;
}

bool DesktopSurface::pushLayeredFrame()
{
    if (!isAttached() || m_frame.isNull())
        return false;
    if (m_frame.format() != QImage::Format_ARGB32_Premultiplied)
        m_frame = m_frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // A 32 bpp top-down DIB section is the only source UpdateLayeredWindow
    // accepts for per-pixel alpha (AC_SRC_ALPHA).
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = m_frame.width();
    info.bmiHeader.biHeight = -m_frame.height(); // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    const HDC screenDc = ::GetDC(nullptr);
    const HDC memDc = ::CreateCompatibleDC(screenDc);
    void* bits = nullptr;
    const HBITMAP dib = ::CreateDIBSection(memDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        qCCritical(lc) << "UpdateLayeredWindow: CreateDIBSection failed, error"
                       << ::GetLastError();
        if (dib)
            ::DeleteObject(dib);
        ::DeleteDC(memDc);
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }

    const int bytes = m_frame.sizeInBytes();
    memcpy(bits, m_frame.constBits(), bytes);

    const HGDIOBJ previous = ::SelectObject(memDc, dib);
    POINT dst{m_rect.x(), m_rect.y()};
    SIZE size{m_frame.width(), m_frame.height()};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    ::SetLastError(0);
    const BOOL ok = ::UpdateLayeredWindow(toHwnd(m_hwnd), screenDc, &dst, &size, memDc, &src, 0,
                                          &blend, ULW_ALPHA);
    const DWORD error = ::GetLastError();

    ::SelectObject(memDc, previous);
    ::DeleteObject(dib);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);

    if (!ok) {
        // The session-7 failure signature was ERROR_INVALID_PARAMETER (87) on
        // a child window created by Qt. If this fires again, keep the opaque
        // WM_PAINT path as the fallback: invisible is worse than opaque.
        qCCritical(lc) << "UpdateLayeredWindow failed, error" << error
                       << "-- falling back to the opaque WM_PAINT path";
        m_perPixelAlpha = false;
        m_frame = m_frame.convertToFormat(QImage::Format_ARGB32);
        ::InvalidateRect(toHwnd(m_hwnd), nullptr, FALSE);
        return false;
    }
    ++m_pushCount;
    // The push sets the window's extent from the frame (T-077), so a frame that is
    // not the size of the rect the window was placed at is a defect, not a detail:
    // it would put the fence at the right position with the wrong size. Counted
    // here rather than guarded, so that any path that does it is *visible* -- the
    // one path that used to (place(), during a resize) now skips it instead.
    if (m_frame.size() != m_rect.size())
        ++m_mismatchedPushCount;
    m_lastBlitLines = m_frame.height(); // pushed in full, not painted
    return true;
}

QImage DesktopSurface::frame() const
{
    return m_frame;
}

bool DesktopSurface::setGeometry(const QRect& rect)
{
    m_rect = rect;
    if (!isAttached())
        return false;
    return place();
}

void DesktopSurface::setVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    if (!isAttached())
        return;
    if (visible) {
        // SW_SHOWNA: show without stealing focus -- the desktop must not
        // activate when a fence comes back.
        ::ShowWindow(toHwnd(m_hwnd), SW_SHOWNA);
        place();
    } else {
        ::ShowWindow(toHwnd(m_hwnd), SW_HIDE);
    }
}

void DesktopSurface::setLayer(Layer layer)
{
    if (m_layer == layer)
        return;
    m_layer = layer;
    if (isAttached())
        place();
}

void DesktopSurface::setRaisedAboveIcons(bool raised)
{
    setLayer(raised ? Layer::AboveIconLayer : Layer::BelowIconLayer);
}

void DesktopSurface::setClickThrough(bool clickThrough)
{
    if (m_clickThrough == clickThrough)
        return;
    m_clickThrough = clickThrough;
    // The window style does not change: HTTRANSPARENT is answered from
    // WM_NCHITTEST, which is a property of this WndProc and needs no repaint.
}

bool DesktopSurface::setUniformAlpha(int alpha)
{
    m_uniformAlpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    if (!isAttached() || m_perPixelAlpha)
        return true; // remembered for attach(); the per-pixel path has no uniform alpha
    if (!::SetLayeredWindowAttributes(toHwnd(m_hwnd), 0, static_cast<BYTE>(m_uniformAlpha),
                                      LWA_ALPHA)) {
        qCWarning(lc) << "setUniformAlpha:" << m_uniformAlpha << "refused, error"
                      << ::GetLastError();
        return false;
    }
    return true;
}

void DesktopSurface::setCornerRadius(int radiusPx)
{
    const int clamped = radiusPx < 0 ? 0 : radiusPx;
    if (m_cornerRadius == clamped)
        return;
    m_cornerRadius = clamped;
    applyShape();
}

// T-082. The tiles' shape: the union of the rectangles, and nothing else. The
// overlay's *content* is the very same frame the panel blits -- one render serves
// both windows, and no pixel of a tile can drift from the pixel the panel would
// have painted, because it is the same pixel. The region is what makes the two
// windows differ, and it is a function of the *size* alone, exactly like the
// rounded panel (ADR-15), so a move costs no region rebuild.
void DesktopSurface::setRegionRects(const QVector<QRect>& rects)
{
    if (m_regionRects == rects && m_shape == Shape::TileUnion)
        return;
    m_regionRects = rects;
    m_shape = Shape::TileUnion;
    applyShape();
}

void DesktopSurface::setShape(Shape shape)
{
    if (m_shape == shape)
        return;
    m_shape = shape;
    applyShape();
}

void DesktopSurface::setStackedBelow(const DesktopSurface* other)
{
    if (m_stackedBelow == other)
        return;
    m_stackedBelow = other;
    if (isAttached())
        place();
}

void DesktopSurface::applyShape()
{
    if (!isAttached())
        return;

    const HWND self = toHwnd(m_hwnd);
    RECT client{};
    ::GetClientRect(self, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;

    if (w <= 0 || h <= 0) {
        ::SetWindowRgn(self, nullptr, TRUE);
        m_shapedSize = QSize();
        return;
    }

    // T-082: the union of the tile rectangles -- the icon overlay's shape. Only
    // the pixels of a tile are this window's, so the translucent panel *below* it
    // is what shows everywhere else: that is the whole point of the pair.
    //
    // An overlay with no tile at all (a fence that holds nothing yet) gets an
    // *empty* region rather than no region: "no region" means "the whole window",
    // so it would paint a rectangle of panel over the very thing this window
    // exists to reveal. The container hides such a window as well -- two
    // mechanisms for one state, because the failure of this one is a rectangle of
    // the wrong colour on someone's desktop.
    if (m_shape == Shape::TileUnion) {
        const QRect clientRect(0, 0, w, h);
        HRGN region = nullptr;
        for (const QRect& rect : m_regionRects) {
            const QRect piece = rect.intersected(clientRect);
            if (piece.isEmpty())
                continue;
            HRGN pieceRegion = ::CreateRectRgn(piece.x(), piece.y(), piece.x() + piece.width(),
                                               piece.y() + piece.height());
            if (!pieceRegion)
                continue;
            if (!region) {
                region = pieceRegion;
            } else {
                ::CombineRgn(region, region, pieceRegion, RGN_OR);
                ::DeleteObject(pieceRegion);
            }
        }
        if (!region)
            region = ::CreateRectRgn(0, 0, 0, 0);
        if (!region || !::SetWindowRgn(self, region, TRUE)) {
            qCWarning(lc) << "SetWindowRgn (tile rects) failed, error" << ::GetLastError();
            if (region)
                ::DeleteObject(region);
            return;
        }
        m_shapedSize = QSize(w, h);
        return;
    }

    if (m_cornerRadius <= 0) {
        ::SetWindowRgn(self, nullptr, TRUE);
        m_shapedSize = QSize();
        return;
    }

    // CreateRoundRectRgn takes the full width and height of the corner ellipse,
    // so a radius of r reads 2r here; the rect is inclusive of its last pixel,
    // which is why the window is w+1 x h+1.
    HRGN region = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, m_cornerRadius * 2, m_cornerRadius * 2);
    if (!region) {
        qCWarning(lc) << "CreateRoundRectRgn failed, error" << ::GetLastError();
        return;
    }
    if (!::SetWindowRgn(self, region, TRUE)) {
        qCWarning(lc) << "SetWindowRgn failed, error" << ::GetLastError();
        ::DeleteObject(region); // on success the system owns the region
        return;
    }
    m_shapedSize = QSize(w, h);
}

void DesktopSurface::syncZOrder()
{
    if (!isAttached())
        return;
    refreshDesktopHandles();
    if (m_iconHost)
        m_placed = place();
}

void DesktopSurface::setDevicePixelRatio(qreal ratio)
{
    if (ratio <= 0.0)
        return;
    m_dpr = ratio;
}

void DesktopSurface::setInputTarget(QObject* target)
{
    m_inputTarget = target;
}

QObject* DesktopSurface::inputTarget() const
{
    return m_inputTarget;
}

void DesktopSurface::setDropHandler(
    std::function<void(const QPoint&, const QStringList&)> handler)
{
    m_dropHandler = std::move(handler);
}

const std::function<void(const QPoint&, const QStringList&)>& DesktopSurface::dropHandler() const
{
    return m_dropHandler;
}

void DesktopSurface::releaseCustomDropTarget()
{
    if (!m_customDropTarget)
        return;
    if (m_hwnd)
        ::RevokeDragDrop(toHwnd(m_hwnd));
    static_cast<IUnknown*>(m_customDropTarget)->Release();
    m_customDropTarget = nullptr;
}

bool DesktopSurface::forwardKey(quintptr wParam)
{
    if (!m_inputTarget)
        return false;
    const auto vk = static_cast<unsigned>(wParam);
    int key = 0;
    switch (vk) {
    case VK_F2: key = Qt::Key_F2; break;
    case VK_F5: key = Qt::Key_F5; break;
    case VK_F10: key = Qt::Key_F10; break;
    case VK_DELETE: key = Qt::Key_Delete; break;
    case VK_RETURN: key = Qt::Key_Return; break;
    case VK_ESCAPE: key = Qt::Key_Escape; break;
    case VK_LEFT: key = Qt::Key_Left; break;
    case VK_RIGHT: key = Qt::Key_Right; break;
    case VK_UP: key = Qt::Key_Up; break;
    case VK_DOWN: key = Qt::Key_Down; break;
    case VK_HOME: key = Qt::Key_Home; break;
    case VK_END: key = Qt::Key_End; break;
    case VK_APPS: key = Qt::Key_Menu; break;
    default:
        if (vk >= 'A' && vk <= 'Z')
            key = Qt::Key_A + static_cast<int>(vk - 'A');
        break;
    }
    if (key == 0)
        return false;
    Qt::KeyboardModifiers mods;
    if (::GetKeyState(VK_CONTROL) < 0)
        mods |= Qt::ControlModifier;
    if (::GetKeyState(VK_SHIFT) < 0)
        mods |= Qt::ShiftModifier;
    if (::GetKeyState(VK_MENU) < 0)
        mods |= Qt::AltModifier;
    QCoreApplication::postEvent(m_inputTarget, new QKeyEvent(QEvent::KeyPress, key, mods));
    return true;
}

void DesktopSurface::forwardMouse(unsigned message, quintptr lParam)
{
    if (!m_inputTarget)
        return;

    // Physical client coordinates of this surface == the frame's pixels: the
    // fence is rendered at (0,0) of the frame, and the surface has no border.
    const QPointF physical(static_cast<qreal>(GET_X_LPARAM(lParam)),
                           static_cast<qreal>(GET_Y_LPARAM(lParam)));
    // The widget paints in *logical* pixels (T-014). On a 150 % monitor the
    // surface's client area is 1.5x the fence's logical size, so a position in
    // it has to be divided by the ratio before the widget can hit-test it --
    // without this a drag would move a fence 1.5x as far as the cursor. On a
    // 100 % monitor the two spaces coincide, which is why v1 never noticed.
    const qreal dpr = m_dpr > 0.0 ? m_dpr : 1.0;
    const QPointF pos = physical / dpr;
    // Screen position stays physical on purpose: what consumes it is
    // TrackPopupMenuEx (the shell context menu), a Win32 call that works in
    // physical screen pixels. Qt's own convention for global positions is
    // logical, so this is documented rather than assumed -- see T-014 notes.
    QPointF screenPos = physical;
    if (m_hwnd) {
        RECT r{};
        if (::GetWindowRect(toHwnd(m_hwnd), &r))
            screenPos = QPointF(static_cast<qreal>(r.left) + physical.x(),
                                static_cast<qreal>(r.top) + physical.y());
    }

    // Remembered so releaseStuckGesture() can end a gesture that is taken away
    // from us (WM_CAPTURECHANGED) at the position where it was actually lost.
    m_lastPos = pos;
    m_lastScreenPos = screenPos;

    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();

    QEvent::Type type = QEvent::None;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;

    switch (message) {
    case WM_LBUTTONDOWN:
        type = QEvent::MouseButtonPress;
        button = Qt::LeftButton;
        buttons = Qt::LeftButton;
        m_leftDown = true;
        break;
    case WM_LBUTTONUP:
        type = QEvent::MouseButtonRelease;
        button = Qt::LeftButton;
        buttons = Qt::NoButton;
        m_leftDown = false;
        break;
    case WM_LBUTTONDBLCLK:
        type = QEvent::MouseButtonDblClick;
        button = Qt::LeftButton;
        buttons = Qt::LeftButton;
        m_leftDown = true;
        break;
    case WM_RBUTTONDOWN:
        type = QEvent::MouseButtonPress;
        button = Qt::RightButton;
        buttons = Qt::RightButton;
        m_rightDown = true;
        break;
    case WM_RBUTTONUP:
        type = QEvent::MouseButtonRelease;
        button = Qt::RightButton;
        buttons = Qt::NoButton;
        m_rightDown = false;
        break;
    case WM_RBUTTONDBLCLK:
        type = QEvent::MouseButtonDblClick;
        button = Qt::RightButton;
        buttons = Qt::RightButton;
        m_rightDown = true;
        break;
    case WM_MOUSEMOVE:
        type = QEvent::MouseMove;
        button = Qt::NoButton;
        buttons = (m_leftDown ? Qt::LeftButton : Qt::NoButton)
                  | (m_rightDown ? Qt::RightButton : Qt::NoButton);
        break;
    default:
        return;
    }

    // Posted, not sent: the WndProc must not re-enter the Qt event loop.
    auto* event = new QMouseEvent(type, pos, screenPos, button, buttons, mods);
    QCoreApplication::postEvent(m_inputTarget, event);
}

void DesktopSurface::releaseStuckGesture()
{
    if (!m_inputTarget || (!m_leftDown && !m_rightDown))
        return;

    // The widget is waiting for a release it will never receive: post it now, at
    // the last position we saw, so the gesture ends where it was interrupted.
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    if (m_leftDown) {
        m_leftDown = false;
        QCoreApplication::postEvent(
            m_inputTarget,
            new QMouseEvent(QEvent::MouseButtonRelease, m_lastPos, m_lastScreenPos,
                            Qt::LeftButton, m_rightDown ? Qt::RightButton : Qt::NoButton, mods));
    }
    if (m_rightDown) {
        m_rightDown = false;
        QCoreApplication::postEvent(m_inputTarget,
                                    new QMouseEvent(QEvent::MouseButtonRelease, m_lastPos,
                                                    m_lastScreenPos, Qt::RightButton,
                                                    Qt::NoButton, mods));
    }
}

bool DesktopSurface::setDesktopIconsHidden(bool hidden)
{
    const DesktopHandles handles = discoverDesktop();
    // ADR-12: the icons live in the list view, the wallpaper in the DefView. We
    // hide the *icons* -- so the list view is the window to toggle, and the user
    // keeps their wallpaper. The DefView is the fallback for a shell that has no
    // list view (and is then the only thing there is to hide).
    const HWND iconWindow = handles.listView ? handles.listView : handles.defView;
    if (!iconWindow) {
        qCWarning(lc) << "setDesktopIconsHidden: neither SysListView32 nor SHELLDLL_DefView found";
        return false;
    }

    // The safety net (T-046). Hiding the icon layer changes the whole session and
    // only this process would undo it, so the marker is written *first*: a crash
    // anywhere after this point leaves a note the next run reads, and a marker
    // that cannot be written means the layer is not hidden at all. Hiding the
    // icons without recording it is the one state nothing could repair.
    core::IconHidingMarker marker;
    if (hidden && !marker.write()) {
        qCCritical(lc) << "refusing to hide the desktop icons: the marker could not be "
                          "written, and an unrecorded hiding is one nothing can repair";
        return false;
    }

    ::ShowWindow(iconWindow, hidden ? SW_HIDE : SW_SHOW);

    // On the way out the order is the mirror image: the record must outlive the
    // state it describes, so it is removed only once the layer is really back. A
    // marker left behind by a failed removal costs one harmless restore next run.
    if (!hidden && !marker.remove())
        qCWarning(lc) << "the desktop icons are back, but the marker could not be removed";

    qCInfo(lc).noquote() << QStringLiteral("native desktop icons %1 (%2)")
                                .arg(hidden ? QStringLiteral("hidden")
                                            : QStringLiteral("restored"))
                                .arg(handles.listView ? QStringLiteral("SysListView32")
                                                      : QStringLiteral("SHELLDLL_DefView"));
    return true;
}

bool DesktopSurface::desktopIconsHidden()
{
    const DesktopHandles handles = discoverDesktop();
    const HWND iconWindow = handles.listView ? handles.listView : handles.defView;
    return iconWindow && !::IsWindowVisible(iconWindow);
}

bool DesktopSurface::refreshDesktopHandles()
{
    const DesktopHandles handles = discoverDesktop();
    const quintptr iconHost = fromHwnd(handles.iconHost);
    const quintptr defView = fromHwnd(handles.defView);
    const bool changed = (iconHost != m_iconHost) || (defView != m_defView);
    m_iconHost = iconHost;
    m_defView = defView;
    if (changed && m_iconHost) {
        qCInfo(lc).noquote() << QStringLiteral("desktop handles: icon host 0x%1, DefView 0x%2")
                                    .arg(m_iconHost, 0, 16)
                                    .arg(m_defView, 0, 16);
    }
    return m_iconHost != 0;
}

bool DesktopSurface::place()
{
    if (!m_iconHost || !isAttached())
        return false;

    const HWND self = toHwnd(m_hwnd);

    // The three roles of T-039, re-anchored by ADR-12. The DefView stays visible
    // now (it is the wallpaper), so every surface of ours has to live *above* it:
    // a fence goes to the top of Progman's child order, and the backdrop takes the
    // slot directly above the DefView -- under the fences, above the wallpaper,
    // which is what lets the wallpaper show through its transparent pixels and
    // still lets the empty desktop's gestures reach it (T-041/T-055).
    HWND anchor = HWND_TOP;
    if (m_stackedBelow && m_stackedBelow->hwnd()) {
        // T-082: the fence's *panel* takes its place directly below the overlay
        // that shows its icons (see setStackedBelow for why the pair is expressed
        // this way round).
        anchor = toHwnd(m_stackedBelow->hwnd());
    } else switch (m_layer) {
    case Layer::AboveIconLayer:
    case Layer::BelowIconLayer:
        anchor = HWND_TOP;
        break;
    case Layer::Backdrop:
        anchor = anchorAboveWallpaper(m_defView ? toHwnd(m_defView) : nullptr, self);
        break;
    }

    // The invariant every layer shares since ADR-12, and the one that says whether
    // there is anything to repair: the wallpaper's own window (the DefView) is
    // *below* us. HWND_TOP is how a fence achieves that, not what it means -- two
    // fences are both above the wallpaper and one of them is above the other, so
    // "nothing above me" would be false for every fence but the newest one. (It
    // was the first version of this check, and measured on the hosted path: every
    // fence but the last kept re-placing itself every 2 s. A check that is wrong
    // is worse than no check, and it took the counters to see it.)
    //
    // A surface that cannot state the invariant -- no DefView found on this
    // desktop -- places unconditionally rather than guessing.
    const auto wallpaperIsBelow = [self, defView = toHwnd(m_defView)] {
        if (!defView)
            return false;
        for (HWND window = ::GetWindow(self, GW_HWNDNEXT); window;
             window = ::GetWindow(window, GW_HWNDNEXT)) {
            if (window == defView)
                return true;
        }
        return false;
    };

    // ⚠️ T-050 -- ask before repairing. This is called by the 2 s watchdog for
    // every fence, and until now it did the whole job every single time: a
    // SetWindowPos and a full UpdateLayeredWindow push of an unchanged frame, for
    // a window that had not moved. The user's report ("they flicker, maybe at
    // regular intervals, all of them") names that cadence, and the counters were
    // the only thing missing to see it. What the watchdog is *for* is repairing a
    // placement something else disturbed, so the question to ask is "is anything
    // disturbed?", not "is it time?".
    //
    // GetWindowRect answers in *screen* pixels while m_rect is in the icon host's
    // client pixels (T-014), so the host's client origin is subtracted rather than
    // assumed to be the screen origin: on a monitor left of the primary one those
    // differ, and a comparison that assumed (0,0) would simply never match -- the
    // watchdog would keep its old behaviour in silence, which is exactly the kind
    // of no-op this project keeps having to measure.
    POINT hostOrigin{0, 0};
    ::ClientToScreen(toHwnd(m_iconHost), &hostOrigin);
    RECT current{};
    const bool haveRect = ::GetWindowRect(self, &current) != FALSE;
    const QRect actual(current.left - hostOrigin.x, current.top - hostOrigin.y,
                       current.right - current.left, current.bottom - current.top);
    // ⚠️ ADR-15: the rounded panel is part of "where it belongs". Without this
    // clause the guard below is satisfied by a window that is at the right rect,
    // visible and above the wallpaper -- and its region is never built, so the
    // corners stay square and the fence is a rectangle. Measured: the region check
    // in --fence-demo --host caught exactly that (region type ERROR on a fence
    // whose rect was right).
    // T-082: an icon overlay's shape is a union of tile rectangles, and the same
    // rule applies -- it is a function of the size alone, so a stale one counts as
    // "not where it belongs" and gets rebuilt.
    const bool shapeIsCurrent =
        (m_regionRects.isEmpty() && m_cornerRadius <= 0) || m_shapedSize == m_rect.size();
    // ... and a panel that belongs to an overlay is only where it belongs when the
    // overlay is **directly above** it: anything in between would hide the tiles
    // behind its own panel. Moving a fence is what breaks that (both windows are
    // re-placed, and the overlay lands at the top of the host's children), and this
    // clause is what repairs it -- on the next placement, not two seconds later.
    const bool anchorIsAbove = !m_stackedBelow || !m_stackedBelow->hwnd()
        || (::GetWindow(self, GW_HWNDPREV) == toHwnd(m_stackedBelow->hwnd()));
    if (haveRect && actual == m_rect && ::IsWindowVisible(self) == (m_visible ? TRUE : FALSE)
        && shapeIsCurrent && wallpaperIsBelow() && anchorIsAbove) {
        ++m_currentPlaceCount;
        return true; // already where it belongs: nothing to repair
    }

    // A hidden surface (quick hide, F-022) is positioned but never shown: the
    // watchdog re-places every 2 s and must not resurrect it.
    UINT flags = SWP_NOACTIVATE;
    if (m_visible)
        flags |= SWP_SHOWWINDOW;
    // Session 64: a fence already above the wallpaper keeps its place among the
    // other fences. The stacking is the container's (raiseFence): a move used to
    // send the window to the top while it was still composed *under* the
    // others -- the fence on top showed the wallpaper instead of them.
    if (!m_stackedBelow && m_layer != Layer::Backdrop && wallpaperIsBelow())
        flags |= SWP_NOZORDER;
    const BOOL ok = ::SetWindowPos(self, anchor, m_rect.x(), m_rect.y(),
                                   m_rect.width(), m_rect.height(), flags);
    if (!ok)
        qCWarning(lc) << "SetWindowPos failed, error" << ::GetLastError();
    // ADR-15: a window region is expressed in window coordinates, so it is the
    // *size* that decides whether it has to be rebuilt -- a move changes nothing
    // about it. A rolled-up fence changes height, and a resize changes both.
    if ((m_cornerRadius > 0 || !m_regionRects.isEmpty()) && m_shapedSize != m_rect.size())
        applyShape();
    if (m_perPixelAlpha) {
        // UpdateLayeredWindow carries the position *and the size*: the frame it is
        // handed becomes the window's extent.
        //
        // ⚠️ T-077 -- which is why a frame of the wrong size must never be pushed
        // here. A resize used to push the previous frame at the new rect (this
        // line), putting the fence at its new position with its old size, and then
        // the fresh frame put it right again in the same message. Measured: every
        // step of a resize gesture did exactly that twice, once wrongly, and the
        // user's report is "while the cursor drags, the fence itself looks
        // unstable, although the final result is right". The frame that fits the
        // rect we just asked for is the caller's (applyVisibleGeometry renders it
        // immediately after), so a stale one is left alone -- and counted, because
        // "nothing to do" and "forgot to do it" look identical from the outside.
        if (ok && !m_frame.isNull()) {
            if (m_frame.size() == m_rect.size())
                pushLayeredFrame();
            else
                ++m_stalePushSkips;
        }
    } else if (m_visible) {
        ::UpdateWindow(self);
    }
    return ok != FALSE;
}

void DesktopSurface::watchdogTick()
{
    if (!isAttached()) {
        m_watchdog.stop();
        m_hwnd = 0;
        m_placed = false;
        emit desktopLost();
        return;
    }
    if (!refreshDesktopHandles()) {
        qCWarning(lc) << "desktop host lost (Explorer restarting?)";
        return;
    }
    // place() is a no-op when the surface is already where it belongs (T-050);
    // the tick is what notices that the desktop itself is gone or that a handle
    // moved, and neither of those is skipped.
    m_placed = place();
}

void DesktopSurface::paintInto(quintptr dc)
{
    if (m_frame.isNull()) {
        // Nothing to show: paint the surface in the desktop background colour
        // rather than leaving whatever was there before.
        RECT client{};
        ::GetClientRect(toHwnd(m_hwnd), &client);
        const HBRUSH brush = ::CreateSolidBrush(RGB(0x1F, 0x29, 0x37));
        ::FillRect(reinterpret_cast<HDC>(dc), &client, brush);
        ::DeleteObject(brush);
        m_lastBlitLines = -2; // background fill, not a frame blit
        return;
    }
    m_lastBlitLines = blitFrame(reinterpret_cast<HDC>(dc), m_frame, m_paintRect);
}

QString DesktopSurface::paintReport() const
{
    QString report = QStringLiteral("%1 WM_PAINT, %2 WM_ERASEBKGND, last region %3,%4 %5x%6, "
                                    "clipBox %7, %8 scan lines")
        .arg(m_paintCount)
        .arg(m_eraseCount)
        .arg(m_lastUpdateRegion.x())
        .arg(m_lastUpdateRegion.y())
        .arg(m_lastUpdateRegion.width())
        .arg(m_lastUpdateRegion.height())
        .arg(m_lastClipBox)
        .arg(m_lastBlitLines);
    // T-050 / T-077: the layered path's work, and the work it did not do. Both
    // halves are reported on purpose -- a surface that pushes nothing is only
    // good news if it also says it was asked (currentPlacements).
    report += QStringLiteral(", %1 layered push(es) (%2 size-mismatched, %3 stale skipped), "
                             "%4 placement(s) already current")
                  .arg(m_pushCount)
                  .arg(m_mismatchedPushCount)
                  .arg(m_stalePushSkips)
                  .arg(m_currentPlaceCount);
    return report;
}

bool DesktopSurface::handleMessage(quintptr hwnd, unsigned message, quintptr wParam,
                                   quintptr lParam, qintptr* result)
{
    switch (message) {
    case WM_ERASEBKGND:
        // The frame covers the whole client area: erasing would only flash.
        ++m_eraseCount;
        *result = 1;
        return true;

    case WM_PAINT: {
        ++m_paintCount;
        PAINTSTRUCT ps{};
        const HDC dc = ::BeginPaint(toHwnd(hwnd), &ps);
        m_lastUpdateRegion = QRect(ps.rcPaint.left, ps.rcPaint.top,
                                   ps.rcPaint.right - ps.rcPaint.left,
                                   ps.rcPaint.bottom - ps.rcPaint.top);
        m_lastClipBox = ::GetClipBox(dc, nullptr);
        m_paintRect = m_lastUpdateRegion;
        // In per-pixel mode the content comes from UpdateLayeredWindow; a GDI
        // blit would be ignored by DWM anyway. Just validate the region.
        if (!m_perPixelAlpha)
            paintInto(reinterpret_cast<quintptr>(dc));
        m_paintRect = QRect(); // WM_PRINTCLIENT and the rest paint it all
        ::EndPaint(toHwnd(hwnd), &ps);
        *result = 0;
        return true;
    }

    case WM_PRINTCLIENT:
        // Makes PrintWindow a usable verification channel: the content can be
        // read back even when the desktop is covered.
        paintInto(wParam);
        *result = 0;
        return true;

    case WM_WINDOWPOSCHANGING: {
        // Nothing may stack a hosted surface above the icon layer -- except
        // Peek (F-021), which is the one caller allowed to ask for it. Rewriting
        // hwndInsertAfter here is what the Qt port needed a native event filter
        // for; owning the WndProc makes it trivial.
        auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
        if (pos && !(pos->flags & SWP_NOZORDER)) {
            if (m_stackedBelow && m_stackedBelow->hwnd()) {
                // T-082: a fence's panel only ever sits directly below its overlay.
                pos->hwndInsertAfter = toHwnd(m_stackedBelow->hwnd());
                return false;
            }
            switch (m_layer) {
            case Layer::AboveIconLayer:
            case Layer::BelowIconLayer:
                pos->hwndInsertAfter = HWND_TOP;
                break;
            case Layer::Backdrop:
                // Above the wallpaper (ADR-12), below the fences.
                pos->hwndInsertAfter = anchorAboveWallpaper(m_defView ? toHwnd(m_defView) : nullptr, toHwnd(m_hwnd));
                break;
            }
        }
        return false;
    }

    case WM_NCHITTEST: {
        // Hand the mouse to whatever is under this window: it is a surface of
        // ours standing over the desktop's own layers, and every click it cannot
        // honour itself belongs to them (T-042 -- a replayed click is a different
        // click).
        if (m_clickThrough) {
            *result = HTTRANSPARENT;
            return true;
        }
        // ⚠️ T-068 -- and only the *backdrop* ever steps aside. The two clauses
        // below were written for it, but they live in the WndProc every layer
        // shares, so they silently applied to the fences as well: with the left
        // button down a fence answered HTTRANSPARENT too. Windows asks this
        // question **when the button goes down**, which is exactly when a click, a
        // drag and an OLE drop happen -- so the fence could not be grabbed,
        // resized, clicked or dropped onto. Measured on the user's desktop: the
        // fence answered HTCLIENT at rest and HTTRANSPARENT the moment it was
        // pressed, and the question "why can I no longer move a fence or touch a
        // desktop icon when StarFish is running?" had this single answer.
        // A fence stands **over** the desktop's content and draws its own: what
        // the user sees there is ours, so the click there is ours too.
        if (m_layer != Layer::Backdrop) {
            *result = HTCLIENT;
            return true;
        }
        // ADR-21: the drawn desktop. The list view is hidden, so what is under
        // this window is the bare wallpaper: every click is the desktop's, and
        // the desktop is us.
        if (m_ownsAllInput) {
            *result = HTCLIENT;
            return true;
        }
        // From here on, the backdrop only: the surface that stands over the
        // desktop's own layers with nothing of its own to draw.
        //
        // ADR-13, and the reason the native icons can stay: an icon's own
        // rectangle is not ours. Its click, its drag, its menu and its tooltip
        // are the shell's, and they only stay the shell's because the mouse is
        // never taken to begin with.
        // lParam is the cursor in *screen* pixels, which is also the space the
        // icon rectangles are read in (platform/win/desktop_icons), so no
        // conversion is involved at all.
        const QPoint screen(static_cast<short>(LOWORD(lParam)),
                            static_cast<short>(HIWORD(lParam)));
        for (const QRect& icon : m_nativeIconRects) {
            if (icon.contains(screen)) {
                *result = HTTRANSPARENT;
                return true;
            }
        }
        // The left button belongs to the desktop: dragging a rectangle over the
        // bare desktop is Windows' own icon selection, and StarFish has nothing
        // to add to it. The right button is the one it does answer (F-001/
        // T-055), so the gesture is what is left of a *bare* point.
        if (::GetAsyncKeyState(VK_LBUTTON) < 0) {
            *result = HTTRANSPARENT;
            return true;
        }
        *result = HTCLIENT;
        return true;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (m_ownsAllInput && forwardKey(wParam)) {
            *result = 0;
            return true;
        }
        return false;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        // ADR-21 lot 3: a click on the drawn desktop gives it the keyboard, as
        // a click on the real one does (F2, Delete, Ctrl+A).
        if (m_ownsAllInput && ::GetFocus() != toHwnd(m_hwnd))
            ::SetFocus(toHwnd(m_hwnd));
        // Take the mouse capture. Without it Windows stops delivering
        // WM_MOUSEMOVE to this window the moment the cursor steps outside its
        // rectangle, so a title-bar drag dies at the first pixel past the edge
        // and a hosted fence can never be moved. Given back on the matching
        // button-up, dropped if something else steals it.
        ::SetCapture(toHwnd(m_hwnd));
        forwardMouse(message, lParam);
        return false;

    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MOUSEMOVE:
        // Input is owned by the surface's WndProc; it is forwarded to the Qt
        // widget rendered into this surface so selection, double-click and the
        // right-click shell context menu (T-006) work on the live desktop. The
        // message is also passed to DefWindowProc, which has nothing to do for
        // these on a layered child.
        forwardMouse(message, lParam);
        return false;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        // Forwarded first: the widget has already let go of the button by the
        // time ReleaseCapture() raises WM_CAPTURECHANGED, which would otherwise
        // look like a gesture lost mid-drag.
        forwardMouse(message, lParam);
        if (::GetCapture() == toHwnd(m_hwnd))
            ::ReleaseCapture();
        return false;

    case WM_CAPTURECHANGED:
        // Somebody else took the capture -- another window, Alt-Tab, a popup
        // menu. End the gesture the widget still thinks it is in, otherwise a
        // half-dragged fence keeps following the cursor with no button down.
        releaseStuckGesture();
        *result = 0;
        return true;

    case WM_DESTROY:
        // ⚠️ T-090: not ours. detach() clears the user data before it destroys
        // the window, so a WM_DESTROY that reaches the object is the desktop
        // taking its children with it -- Explorer restarting. The watchdog used
        // to be stopped here *silently*, so desktopLost() never fired and the
        // owner never hosted the window again. It is said now, once the
        // destruction is over.
        if (m_dropTarget) {
            ::RevokeDragDrop(toHwnd(m_hwnd));
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
        releaseCustomDropTarget();
        m_hwnd = 0;
        m_placed = false;
        m_watchdog.stop();
        QMetaObject::invokeMethod(this, [this] { emit desktopLost(); }, Qt::QueuedConnection);
        *result = 0;
        return true;

    default:
        return false;
    }
}

} // namespace starfish::platform::win
