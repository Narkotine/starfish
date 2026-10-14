#include "platform/win/desktop_surface.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPointF>

#include "platform/win/shell_drop_target.hpp"

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
struct DesktopHandles {
    HWND iconHost = nullptr;
    HWND defView = nullptr;
};

DesktopHandles discoverDesktop()
{
    DesktopHandles handles;
    if (HWND progman = ::FindWindowW(L"Progman", nullptr)) {
        if (HWND defView = ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) {
            handles.iconHost = progman;
            handles.defView = defView;
            return handles;
        }
    }

    HWND worker = nullptr;
    while ((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        if (HWND defView = ::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)) {
            handles.iconHost = worker;
            handles.defView = defView;
            break;
        }
    }
    return handles;
}

// Draws a Qt frame into a window DC. StretchDIBits ignores the alpha channel,
// which is what we want here: the surface is uniformly opaque (LWA_ALPHA at
// 255), so per-pixel transparency would need UpdateLayeredWindow -- and that
// call is refused on a child window of another process (T-017).
int blitFrame(HDC dc, const QImage& frame)
{
    if (frame.isNull() || frame.bytesPerLine() != frame.width() * 4)
        return -1;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width();
    info.bmiHeader.biHeight = -frame.height(); // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    ::SetStretchBltMode(dc, COLORONCOLOR);
    return ::StretchDIBits(dc, 0, 0, frame.width(), frame.height(), 0, 0, frame.width(),
                           frame.height(), frame.constBits(), &info, DIB_RGB_COLORS, SRCCOPY);
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
        ::SetLastError(0);
        attributesOk = ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
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
    if (!m_oleInit) {
        const HRESULT hr = ::OleInitialize(nullptr);
        if (SUCCEEDED(hr)) {
            m_oleInit = true;
        } else {
            qCWarning(lc) << "OleInitialize failed, error" << hr
                          << "-- Explorer drop disabled on this surface";
        }
    }
    if (m_oleInit && !m_dropTarget) {
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

void DesktopSurface::setRaisedAboveIcons(bool raised)
{
    if (m_raisedAboveIcons == raised)
        return;
    m_raisedAboveIcons = raised;
    if (isAttached())
        place();
}

void DesktopSurface::syncZOrder()
{
    if (!isAttached())
        return;
    refreshDesktopHandles();
    if (m_iconHost)
        m_placed = place();
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

void DesktopSurface::forwardMouse(unsigned message, quintptr lParam)
{
    if (!m_inputTarget)
        return;

    // Client coordinates of this surface == the fence's local coordinates: the
    // fence is rendered at (0,0) of the frame, and the surface has no border.
    const QPointF pos(static_cast<qreal>(GET_X_LPARAM(lParam)),
                      static_cast<qreal>(GET_Y_LPARAM(lParam)));
    // Translate the client position to screen coordinates. Posted QMouseEvents
    // need a real screen position so widgets that pop up a menu (the shell
    // context menu on right-click) can place it under the actual cursor; using
    // `pos, pos` would put the menu at the top-left of the primary monitor.
    QPointF screenPos = pos;
    if (m_hwnd) {
        RECT r{};
        if (::GetWindowRect(toHwnd(m_hwnd), &r))
            screenPos = QPointF(static_cast<qreal>(r.left) + pos.x(),
                                static_cast<qreal>(r.top) + pos.y());
    }

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

    // Peek (F-021) puts the surface at the top of Progman's child z-order, i.e.
    // above the icon layer; the normal position is just below SHELLDLL_DefView.
    const HWND anchor = m_raisedAboveIcons
                            ? HWND_TOP
                            : (m_defView ? toHwnd(m_defView) : HWND_BOTTOM);
    // A hidden surface (quick hide, F-022) is positioned but never shown: the
    // watchdog re-places every 2 s and must not resurrect it.
    UINT flags = SWP_NOACTIVATE;
    if (m_visible)
        flags |= SWP_SHOWWINDOW;
    const BOOL ok = ::SetWindowPos(toHwnd(m_hwnd), anchor, m_rect.x(), m_rect.y(),
                                   m_rect.width(), m_rect.height(), flags);
    if (!ok)
        qCWarning(lc) << "SetWindowPos failed, error" << ::GetLastError();
    if (m_perPixelAlpha) {
        // UpdateLayeredWindow carries the position: after a move/resize the
        // frame has to be pushed again, no WM_PAINT will refresh it.
        if (ok && !m_frame.isNull())
            pushLayeredFrame();
    } else if (m_visible) {
        ::UpdateWindow(toHwnd(m_hwnd));
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
    m_lastBlitLines = blitFrame(reinterpret_cast<HDC>(dc), m_frame);
}

QString DesktopSurface::paintReport() const
{
    return QStringLiteral("%1 WM_PAINT, %2 WM_ERASEBKGND, last region %3,%4 %5x%6, clipBox %7, "
                          "%8 scan lines")
        .arg(m_paintCount)
        .arg(m_eraseCount)
        .arg(m_lastUpdateRegion.x())
        .arg(m_lastUpdateRegion.y())
        .arg(m_lastUpdateRegion.width())
        .arg(m_lastUpdateRegion.height())
        .arg(m_lastClipBox)
        .arg(m_lastBlitLines);
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
        // In per-pixel mode the content comes from UpdateLayeredWindow; a GDI
        // blit would be ignored by DWM anyway. Just validate the region.
        if (!m_perPixelAlpha)
            paintInto(reinterpret_cast<quintptr>(dc));
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
            if (m_raisedAboveIcons)
                pos->hwndInsertAfter = HWND_TOP;
            else if (m_defView)
                pos->hwndInsertAfter = toHwnd(m_defView);
        }
        return false;
    }

    case WM_NCHITTEST:
        // Empty fence areas should let the click through to the desktop icon
        // layer in Phase 2 (T-006); for now the whole surface is a client area.
        *result = HTCLIENT;
        return true;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MOUSEMOVE:
        // Input is owned by the surface's WndProc; it is forwarded to the Qt
        // widget rendered into this surface so selection, double-click and the
        // right-click shell context menu (T-006) work on the live desktop. The
        // message is also passed to DefWindowProc, which has nothing to do for
        // these on a layered child.
        forwardMouse(message, lParam);
        return false;

    case WM_DESTROY:
        m_hwnd = 0;
        m_placed = false;
        m_watchdog.stop();
        *result = 0;
        return true;

    default:
        return false;
    }
}

} // namespace starfish::platform::win
