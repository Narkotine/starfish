#include "platform/win/shell_drop_target.hpp"
#include "platform/win/shell_operations.hpp"

#include <objbase.h>
#include <shlobj.h> // CF_HDROP, DROPEFFECT_*
#include <windowsx.h>

#include <QPoint>

namespace starfish::platform::win {

namespace {
constexpr DWORD kCopyEffect = DROPEFFECT_COPY;
} // namespace

ShellDropTarget::ShellDropTarget(HWND hwnd,
                                 std::function<void(const QPoint&, const QStringList&)> handler)
    : m_hwnd(hwnd)
    , m_handler(std::move(handler))
{
}

ShellDropTarget::~ShellDropTarget() = default;

ULONG STDMETHODCALLTYPE ShellDropTarget::AddRef()
{
    return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
}

ULONG STDMETHODCALLTYPE ShellDropTarget::Release()
{
    const LONG ref = ::InterlockedDecrement(&m_ref);
    if (ref == 0)
        delete this;
    return static_cast<ULONG>(ref);
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// static
bool ShellDropTarget::dataHasHdrop(IDataObject* data)
{
    if (!data)
        return false;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SUCCEEDED(data->QueryGetData(&fmt));
}

// static
QStringList ShellDropTarget::pathsFromHdrop(IDataObject* data)
{
    QStringList paths;
    if (!data)
        return paths;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium)))
        return paths;
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
        ::ReleaseStgMedium(&medium);
        return paths;
    }
    const HDROP hdrop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = ::DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(static_cast<int>(count));
    for (UINT i = 0; i < count; ++i) {
        wchar_t buffer[MAX_PATH + 1] = {};
        const UINT len = ::DragQueryFileW(hdrop, i, buffer, MAX_PATH + 1);
        if (len > 0)
            paths.append(QString::fromWCharArray(buffer, static_cast<int>(len)));
    }
    ::ReleaseStgMedium(&medium);
    return paths;
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::DragEnter(IDataObject* pDataObj, DWORD /*grfKeyState*/,
                                                    POINTL pt, DWORD* pdwEffect)
{
    // ADR-21 lot 3: a drag of the drawn desktop's own icons may carry virtual
    // items (This PC) that no CF_HDROP can name: what is carried is known here.
    m_canDrop = internalDragActive() || dataHasHdrop(pDataObj);
    if (pdwEffect)
        *pdwEffect = m_canDrop ? kCopyEffect : DROPEFFECT_NONE;
    // Session 62: the picture follows the cursor over the fence too.
    dragImageEnter(reinterpret_cast<quintptr>(m_hwnd), pDataObj, QPoint(pt.x, pt.y),
                   m_canDrop ? kCopyEffect : DROPEFFECT_NONE);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::DragOver(DWORD /*grfKeyState*/, POINTL pt,
                                                   DWORD* pdwEffect)
{
    if (pdwEffect)
        *pdwEffect = m_canDrop ? kCopyEffect : DROPEFFECT_NONE;
    dragImageOver(QPoint(pt.x, pt.y), m_canDrop ? kCopyEffect : DROPEFFECT_NONE);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::DragLeave()
{
    m_canDrop = false;
    dragImageLeave();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::Drop(IDataObject* pDataObj, DWORD /*grfKeyState*/,
                                                POINTL pt, DWORD* pdwEffect)
{
    const QStringList paths =
        internalDragActive() ? internalDragPaths() : pathsFromHdrop(pDataObj);
    if (pdwEffect)
        *pdwEffect = paths.isEmpty() ? DROPEFFECT_NONE : kCopyEffect;
    m_canDrop = false;
    dragImageDrop(pDataObj, QPoint(static_cast<int>(pt.x), static_cast<int>(pt.y)),
                  pdwEffect ? *pdwEffect : DROPEFFECT_NONE);

    if (!paths.isEmpty() && m_handler) {
        // OLE reports screen coordinates; the handler wants client coordinates
        // of the window the target is registered on.
        QPoint client(static_cast<int>(pt.x), static_cast<int>(pt.y));
        if (m_hwnd) {
            RECT r{};
            if (::GetWindowRect(m_hwnd, &r))
                client = QPoint(static_cast<int>(pt.x) - r.left,
                                static_cast<int>(pt.y) - r.top);
        }
        m_handler(client, paths);
    }
    return S_OK;
}

} // namespace starfish::platform::win
