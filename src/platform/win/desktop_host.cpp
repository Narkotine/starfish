#include "platform/win/desktop_host.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QLoggingCategory>
#include <QWidget>

#include "platform/win/desktop_surface.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace starfish::platform::win {

namespace {

QLoggingCategory lc("starfish.desktop");

// Qt only maintains WS_EX_LAYERED while it believes the window needs it:
// QWindowsWindow::setWindowLayered() computes
//   needsLayered = WindowTransparentForInput || (hasAlpha && frameless) || opacity < 1
// and *removes* the flag whenever that turns false -- which it does right after
// we reparent the window to Progman. A window created with a plain opacity of
// 1.0 therefore loses WS_EX_LAYERED a few milliseconds after attach(), and on
// this build Windows refuses to add the flag back to a cross-process child
// (SetWindowLongPtr returns 0, GetLastError stays 0, the style is untouched).
// Asking Qt for a hair under full opacity keeps its own bookkeeping on the
// "needs layered" side; the real alpha is then set to 255 explicitly below.
// See ADR-4.
constexpr qreal kHostedOpacity = 0.99;

inline HWND toHwnd(quintptr handle)
{
    return reinterpret_cast<HWND>(handle);
}

inline quintptr fromHwnd(HWND hwnd)
{
    return reinterpret_cast<quintptr>(hwnd);
}

inline qulonglong hex(quintptr handle)
{
    return static_cast<qulonglong>(handle);
}

struct DesktopHandles {
    HWND iconHost = nullptr; // parent of SHELLDLL_DefView (Progman on Win11)
    HWND defView = nullptr;  // SHELLDLL_DefView: the desktop icon layer
};

// Win11 (recent builds): Progman -> SHELLDLL_DefView, and the classic Win10
// WorkerW trick (message 0x052C) no longer applies.
// Win10 fallback: SHELLDLL_DefView lives inside a top-level WorkerW.
// Both shapes were measured by the T-003 spike (ADR-4).
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

BOOL CALLBACK collectWindowProc(HWND hwnd, LPARAM param)
{
    reinterpret_cast<QList<quintptr>*>(param)->append(fromHwnd(hwnd));
    return TRUE;
}

// EnumChildWindows walks all descendants, top-first in z-order: this is the
// ordering the spike used for its placement verdict.
QList<quintptr> desktopChain(HWND iconHost)
{
    QList<quintptr> chain;
    if (iconHost)
        ::EnumChildWindows(iconHost, &collectWindowProc, reinterpret_cast<LPARAM>(&chain));
    return chain;
}

QString windowClass(HWND hwnd)
{
    wchar_t buffer[256] = {};
    ::GetClassNameW(hwnd, buffer, 256);
    return QString::fromWCharArray(buffer);
}

QString windowRectText(HWND hwnd)
{
    RECT r{};
    if (!hwnd || !::GetWindowRect(hwnd, &r))
        return QStringLiteral("(no rect)");
    return QStringLiteral("%1,%2 %3x%4")
        .arg(r.left)
        .arg(r.top)
        .arg(r.right - r.left)
        .arg(r.bottom - r.top);
}

} // namespace

DesktopHost::DesktopHost(QObject* parent)
    : QObject(parent)
{
    // Settings changes arrive in bursts (theme + work area + environment):
    // debounce before re-applying the placement.
    m_resyncTimer.setSingleShot(true);
    m_resyncTimer.setInterval(250);
    connect(&m_resyncTimer, &QTimer::timeout, this, [this] {
        emit desktopSettingsChanged();
        syncZOrder();
    });

    // Authoritative safety net: a WS_CHILD of Progman does not receive the
    // WM_SETTINGCHANGE / WM_DISPLAYCHANGE broadcasts, so we poll instead.
    m_watchdog.setInterval(2000);
    connect(&m_watchdog, &QTimer::timeout, this, &DesktopHost::watchdogTick);
}

DesktopHost::~DesktopHost()
{
    if (m_filterInstalled) {
        if (auto* app = QCoreApplication::instance())
            app->removeNativeEventFilter(this);
    }
}

bool DesktopHost::isAvailable() const
{
    return discoverDesktop().defView != nullptr;
}

bool DesktopHost::attach(QWidget* window, const QRect& rect)
{
    if (!window) {
        qCWarning(lc) << "attach: null window";
        return false;
    }

    ensureFilterInstalled();
    refreshDesktopHandles();
    if (!m_iconHost) {
        qCWarning(lc) << "attach: desktop host not found (Progman / SHELLDLL_DefView)";
        return false;
    }
    if (const int existing = indexOfHosted(window); existing >= 0) {
        if (::IsWindow(toHwnd(m_hosted.at(existing).hwnd))) {
            qCWarning(lc) << "attach: window already hosted, moving it instead";
            return setGeometry(window, rect);
        }
        // The HWND died with the shell (Explorer restart): drop the stale entry.
        qCInfo(lc) << "attach: stale entry for a window destroyed with the shell, re-hosting";
        m_hosted.removeAt(existing);
    }

    // 1. Frameless top-level window with a real HWND.
    window->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    window->setAttribute(Qt::WA_NativeWindow, true);
    // Make Qt itself request WS_EX_LAYERED, and keep requesting it (see
    // kHostedOpacity above). Must happen before show().
    window->setWindowOpacity(kHostedOpacity);
    window->setGeometry(rect);
    window->show(); // spike order: create -> show -> reparent -> place

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) {
        qCWarning(lc) << "attach: no native window handle";
        return false;
    }

    // 2. Layered -- MANDATORY (ADR-4) -- and never topmost.
    //
    //    Qt has just set WS_EX_LAYERED for us (opacity < 1); we only correct
    //    the alpha back to fully opaque, which SetLayeredWindowAttributes does
    //    fine on an already-layered window. Never try to *add* the flag here:
    //    once the window is a WS_CHILD of Progman -- a window owned by Explorer
    //    -- Windows silently refuses SetWindowLongPtr(GWL_EXSTYLE), leaving the
    //    z-order correct but the window uncomposited and invisible. That silent
    //    failure is the C++/Qt form of the session-4 false positive (ADR-4).
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST;
    if (exStyle != ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE))
        ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    if (!(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)) {
        qCCritical(lc) << "attach: WS_EX_LAYERED missing before reparenting -- the window will not "
                          "be composited by DWM (invisible, see ADR-4)";
    }
    if (!::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
        qCWarning(lc) << "attach: SetLayeredWindowAttributes failed, error"
                      << ::GetLastError() << "- the window may not be composited at all";
    }

    // 3. Reparent to the icon host and become a child window.
    if (!::SetParent(hwnd, toHwnd(m_iconHost))) {
        qCWarning(lc) << "attach: SetParent failed, error" << ::GetLastError();
        return false;
    }
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);

    // The layered flag has to survive the reparenting; check it explicitly
    // because the failure mode is completely silent (see step 2).
    const LONG_PTR exAfter = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(exAfter & WS_EX_LAYERED)) {
        qCCritical(lc) << "attach: WS_EX_LAYERED lost during reparenting -- the window will not be "
                          "composited by DWM (invisible, see ADR-4)";
    } else {
        qCInfo(lc).noquote() << QStringLiteral("layered: alpha 255, ex-style 0x%1 after "
                                               "reparenting")
                                    .arg(hex(static_cast<quintptr>(exAfter)), 0, 16);
    }

    Hosted hosted;
    hosted.widget = window;
    hosted.hwnd = fromHwnd(hwnd);
    hosted.rect = rect;
    m_hosted.append(hosted);

    // 4. Just below the icon layer.
    const bool placed = applyPlacement(m_hosted.last());
    window->update();

    // We just re-read the desktop handles: whatever changed before is consumed.
    m_handlesChanged = false;

    if (!m_watchdog.isActive())
        m_watchdog.start();

    qCInfo(lc).noquote() << QStringLiteral("attached hwnd 0x%1 at %2,%3 %4x%5")
                                .arg(hex(m_hosted.last().hwnd))
                                .arg(rect.x())
                                .arg(rect.y())
                                .arg(rect.width())
                                .arg(rect.height());
    return placed;
}

bool DesktopHost::setGeometry(QWidget* window, const QRect& rect)
{
    const int index = indexOfHosted(window);
    if (index < 0)
        return false;
    m_hosted[index].rect = rect;
    return applyPlacement(m_hosted[index]);
}

void DesktopHost::detach(QWidget* window)
{
    for (qsizetype i = 0; i < m_hosted.size(); ++i) {
        if (m_hosted.at(i).widget != window)
            continue;
        const HWND hwnd = toHwnd(m_hosted.at(i).hwnd);
        if (::IsWindow(hwnd)) {
            ::SetParent(hwnd, nullptr);
            const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
            ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_CHILD) | WS_POPUP);
        }
        m_hosted.removeAt(i);
        break;
    }
    if (m_hosted.isEmpty())
        m_watchdog.stop();
}

void DesktopHost::syncZOrder()
{
    if (m_hosted.isEmpty() || m_resyncing)
        return;

    m_resyncing = true;
    refreshDesktopHandles();
    if (m_iconHost) {
        for (Hosted& hosted : m_hosted) {
            if (::IsWindow(toHwnd(hosted.hwnd)))
                applyPlacement(hosted);
        }
    }
    m_resyncing = false;

    if (m_handlesChanged) {
        m_handlesChanged = false;
        // Deferred on purpose: the owner re-creates its windows here, and we
        // may be inside a message handler or a timer callback.
        emit desktopReacquired();
    }
}

bool DesktopHost::setIconsHidden(bool hidden)
{
    // One implementation of the hiding, and it owns the crash-repair marker
    // (T-046): the hide and its record must never be able to drift apart. This
    // host is no longer the render path, but it is still asked to hide the icons
    // by the older probes -- those must leave the same net behind.
    return DesktopSurface::setDesktopIconsHidden(hidden);
}

bool DesktopHost::iconsHidden() const
{
    return DesktopSurface::desktopIconsHidden();
}

bool DesktopHost::setWindowAlpha(QWidget* window, int alpha)
{
    const int index = indexOfHosted(window);
    if (index < 0 || !::IsWindow(toHwnd(m_hosted.at(index).hwnd)))
        return false;

    const int clamped = qBound(0, alpha, 255);
    if (!::SetLayeredWindowAttributes(toHwnd(m_hosted.at(index).hwnd), 0,
                                      static_cast<BYTE>(clamped), LWA_ALPHA))
        return false;
    m_hosted[index].alpha = clamped;
    return true;
}

core::PlacementVerdict DesktopHost::verifyPlacement(const QWidget* window)
{
    const int index = indexOfHosted(window);
    if (index < 0)
        return core::PlacementVerdict::Unknown;

    // Uses the handles cached by the last attach()/syncZOrder().
    int defViewIndex = -1;
    const int ourIndex = indexInDesktopChain(m_hosted.at(index).hwnd, &defViewIndex);
    return core::evaluatePlacement(ourIndex, defViewIndex);
}

void DesktopHost::dumpDesktopHierarchy()
{
    const DesktopHandles handles = discoverDesktop();
    const HWND progman = ::FindWindowW(L"Progman", nullptr);

    qCInfo(lc).noquote() << QStringLiteral("Progman 0x%1 %2")
                                .arg(hex(fromHwnd(progman)))
                                .arg(windowRectText(progman));
    if (!handles.defView) {
        qCWarning(lc) << "SHELLDLL_DefView not found";
    } else {
        qCInfo(lc).noquote() << QStringLiteral("icon host 0x%1 / SHELLDLL_DefView 0x%2 %3")
                                    .arg(hex(fromHwnd(handles.iconHost)))
                                    .arg(hex(fromHwnd(handles.defView)))
                                    .arg(windowRectText(handles.defView));
    }

    const QList<quintptr> chain = desktopChain(handles.iconHost);
    qCInfo(lc).noquote() << QStringLiteral("desktop chain (top first): %1 windows")
                                .arg(chain.size());
    for (int i = 0; i < chain.size(); ++i) {
        const HWND hwnd = toHwnd(chain.at(i));
        qCInfo(lc).noquote() << QStringLiteral("  [%1] 0x%2 %3 %4")
                                    .arg(i, 2)
                                    .arg(hex(chain.at(i)))
                                    .arg(windowClass(hwnd), -22)
                                    .arg(windowRectText(hwnd));
    }
}

bool DesktopHost::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result);
    if (eventType != QByteArrayLiteral("windows_generic_MSG")
        && eventType != QByteArrayLiteral("windows_dispatcher_MSG")) {
        return false;
    }

    auto* msg = static_cast<MSG*>(message);
    if (!msg)
        return false;

    switch (msg->message) {
    case WM_WINDOWPOSCHANGING: {
        // Nothing -- Qt's own show/resize included -- may stack a hosted
        // window above the icon layer: rewrite the z-order anchor on the fly.
        if (!m_defView)
            return false;
        for (const Hosted& hosted : m_hosted) {
            if (hosted.hwnd != fromHwnd(msg->hwnd))
                continue;
            auto* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam);
            if (pos && !(pos->flags & SWP_NOZORDER))
                pos->hwndInsertAfter = toHwnd(m_defView);
            return false;
        }
        return false;
    }
    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
        // Best effort only: those broadcasts target top-level windows, and a
        // hosted window is a child of Progman. The watchdog is what actually
        // guarantees the placement.
        scheduleResync();
        return false;
    default:
        break;
    }
    return false;
}

bool DesktopHost::refreshDesktopHandles()
{
    const DesktopHandles handles = discoverDesktop();
    const quintptr iconHost = fromHwnd(handles.iconHost);
    const quintptr defView = fromHwnd(handles.defView);

    m_handlesChanged = (iconHost != m_iconHost) || (defView != m_defView);
    const bool first = (m_iconHost == 0);
    m_iconHost = iconHost;
    m_defView = defView;

    if (!m_iconHost)
        return false;

    if (m_handlesChanged) {
        qCInfo(lc).noquote() << QStringLiteral("desktop handles %1: icon host 0x%2, DefView 0x%3")
                                    .arg(first ? QStringLiteral("acquired")
                                               : QStringLiteral("changed"))
                                    .arg(hex(m_iconHost))
                                    .arg(hex(m_defView));
    }
    return true;
}

void DesktopHost::ensureFilterInstalled()
{
    if (m_filterInstalled)
        return;
    if (auto* app = QCoreApplication::instance()) {
        app->installNativeEventFilter(this);
        m_filterInstalled = true;
    }
}

void DesktopHost::scheduleResync()
{
    if (!m_resyncTimer.isActive())
        m_resyncTimer.start();
}

void DesktopHost::watchdogTick()
{
    if (m_hosted.isEmpty()) {
        m_watchdog.stop();
        return;
    }

    refreshDesktopHandles();
    if (!m_iconHost) {
        qCWarning(lc) << "desktop host lost (Explorer restarting?); waiting for it to come back";
        return;
    }

    // A child of Progman is destroyed along with its parent: drop the entries
    // that died with the shell before re-applying anything.
    bool dropped = false;
    for (qsizetype i = m_hosted.size() - 1; i >= 0; --i) {
        if (!::IsWindow(toHwnd(m_hosted.at(i).hwnd))) {
            qCWarning(lc) << "hosted window died with the desktop (Explorer restart)";
            m_hosted.removeAt(i);
            dropped = true;
        }
    }
    if (m_hosted.isEmpty()) {
        m_watchdog.stop();
        if (dropped || m_handlesChanged) {
            m_handlesChanged = false;
            emit desktopReacquired();
        }
        return;
    }
    if (dropped) {
        // The windows are gone: let the owner re-create them rather than
        // re-placing dead handles.
        m_handlesChanged = false;
        emit desktopReacquired();
        return;
    }

    syncZOrder();
}

bool DesktopHost::applyPlacement(Hosted& hosted)
{
    const HWND hwnd = toHwnd(hosted.hwnd);
    if (!m_iconHost || !::IsWindow(hwnd))
        return false;

    const HWND anchor = m_defView ? toHwnd(m_defView) : HWND_BOTTOM;
    const BOOL ok = ::SetWindowPos(hwnd, anchor, hosted.rect.x(), hosted.rect.y(),
                                   hosted.rect.width(), hosted.rect.height(),
                                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!ok)
        qCWarning(lc) << "SetWindowPos failed, error" << ::GetLastError();
    ::UpdateWindow(hwnd);
    return ok != FALSE;
}

int DesktopHost::indexInDesktopChain(quintptr hwnd, int* defViewIndex) const
{
    if (defViewIndex)
        *defViewIndex = -1;
    if (!m_iconHost)
        return -1;

    const QList<quintptr> chain = desktopChain(toHwnd(m_iconHost));
    int ourIndex = -1;
    for (int i = 0; i < chain.size(); ++i) {
        if (chain.at(i) == hwnd)
            ourIndex = i;
        if (defViewIndex && chain.at(i) == m_defView)
            *defViewIndex = i;
    }
    return ourIndex;
}

int DesktopHost::indexOfHosted(const QWidget* window) const
{
    for (int i = 0; i < m_hosted.size(); ++i) {
        if (m_hosted.at(i).widget == window)
            return i;
    }
    return -1;
}

} // namespace starfish::platform::win
