#include "platform/win/shell_drop_target.hpp"
#include "platform/win/shell_operations.hpp"

#include <objbase.h>
#include <QCoreApplication>
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

ShellDropTarget::~ShellDropTarget()
{
    if (m_data)
        m_data->Release();
}

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

// Session 98 (T-160): what letting go would do, by Windows' rules -- Alt (or
// Ctrl+Shift) makes a shortcut, Ctrl copies, otherwise the item goes into the
// fence as it always did. A right-button drag asks on drop (the menu).
ShellDropTarget::Action ShellDropTarget::actionFor(DWORD keys) const
{
    if ((keys & MK_ALT) || ((keys & MK_CONTROL) && (keys & MK_SHIFT)))
        return Action::Link;
    if (keys & MK_CONTROL)
        return Action::Copy;
    return Action::Insert;
}

void ShellDropTarget::describe(IDataObject* data, DWORD keys)
{
    if (!data || !m_canDrop)
        return;
    const Action action = (keys & MK_RBUTTON) ? Action::Insert : actionFor(keys);
    if (action == m_described)
        return;
    m_described = action;
    switch (action) {
    case Action::Copy:
        setDropDescription(data, QCoreApplication::translate("ShellDropTarget", "Copy"), DROPIMAGE_COPY);
        break;
    case Action::Link:
        setDropDescription(data, QCoreApplication::translate("ShellDropTarget", "Create shortcut"), DROPIMAGE_LINK);
        break;
    default:
        // Session 97: "Insert", not the shell's "Copy".
        setDropDescription(data, QCoreApplication::translate("ShellDropTarget", "Insert"));
        break;
    }
}

DWORD ShellDropTarget::effectFor(DWORD keys, DWORD allowed) const
{
    if (!m_canDrop)
        return DROPEFFECT_NONE;
    if (!(keys & MK_RBUTTON) && actionFor(keys) == Action::Link && (allowed & DROPEFFECT_LINK))
        return DROPEFFECT_LINK;
    return kCopyEffect; // never MOVE: the source must not delete what the fence now shows
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::DragEnter(IDataObject* pDataObj, DWORD grfKeyState,
                                                    POINTL pt, DWORD* pdwEffect)
{
    // ADR-21 lot 3: a drag of the drawn desktop's own icons may carry virtual
    // items (This PC) that no CF_HDROP can name: what is carried is known here.
    m_canDrop = (!m_accept || m_accept()) && (internalDragActive() || dataHasHdrop(pDataObj));
    m_keys = grfKeyState;
    m_allowed = pdwEffect ? *pdwEffect : DROPEFFECT_COPY;
    if (pdwEffect)
        *pdwEffect = effectFor(m_keys, m_allowed);
    if (m_data)
        m_data->Release();
    m_data = pDataObj;
    if (m_data)
        m_data->AddRef();
    m_described = Action::Cancel; // nothing said yet
    describe(pDataObj, m_keys);
    // Session 62: the picture follows the cursor over the fence too.
    dragImageEnter(reinterpret_cast<quintptr>(m_hwnd), pDataObj, QPoint(pt.x, pt.y),
                   pdwEffect ? *pdwEffect : DROPEFFECT_NONE);
    if (m_canDrop && m_hover)
        m_hover(clientOf(pt), true);
    return S_OK;
}

QPoint ShellDropTarget::clientOf(const POINTL& pt) const
{
    QPoint client(static_cast<int>(pt.x), static_cast<int>(pt.y));
    RECT r{};
    if (m_hwnd && ::GetWindowRect(m_hwnd, &r))
        client = QPoint(static_cast<int>(pt.x) - r.left, static_cast<int>(pt.y) - r.top);
    return client;
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::DragOver(DWORD grfKeyState, POINTL pt,
                                                   DWORD* pdwEffect)
{
    m_keys = grfKeyState;
    if (pdwEffect) {
        m_allowed = *pdwEffect;
        *pdwEffect = effectFor(m_keys, m_allowed);
    }
    describe(m_data, m_keys); // Ctrl / Alt may change while the drag runs
    dragImageOver(QPoint(pt.x, pt.y), pdwEffect ? *pdwEffect : DROPEFFECT_NONE);
    if (m_canDrop && m_hover)
        m_hover(clientOf(pt), true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::DragLeave()
{
    m_canDrop = false;
    if (m_data) { // the label is the shell's again outside the fence
        setDropDescription(m_data, QString());
        m_data->Release();
        m_data = nullptr;
    }
    dragImageLeave();
    if (m_hover)
        m_hover(QPoint(), false);
    return S_OK;
}

ShellDropTarget::Action ShellDropTarget::askOnDrop(const QPoint& screen)
{
    if (m_menuChooser) // the probes: no popup
        return static_cast<Action>(m_menuChooser());
    HMENU menu = ::CreatePopupMenu();
    if (!menu)
        return Action::Cancel;
    const auto add = [menu](Action action, const QString& text) {
        ::AppendMenuW(menu, MF_STRING, UINT_PTR(int(action) + 1), reinterpret_cast<LPCWSTR>(text.utf16()));
    };
    add(Action::Insert, QCoreApplication::translate("ShellDropTarget", "Move here"));
    add(Action::Copy, QCoreApplication::translate("ShellDropTarget", "Copy here"));
    add(Action::Link, QCoreApplication::translate("ShellDropTarget", "Create shortcuts here"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(Action::Cancel, QCoreApplication::translate("ShellDropTarget", "Cancel"));
    ::SetMenuDefaultItem(menu, UINT(int(Action::Insert) + 1), FALSE);
    HWND owner = m_hwnd;
    if (owner)
        ::SetForegroundWindow(owner);
    const int chosen = int(::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                              screen.x(), screen.y(), owner, nullptr));
    ::DestroyMenu(menu);
    return chosen <= 0 ? Action::Cancel : static_cast<Action>(chosen - 1);
}

HRESULT STDMETHODCALLTYPE ShellDropTarget::Drop(IDataObject* pDataObj, DWORD grfKeyState,
                                                POINTL pt, DWORD* pdwEffect)
{
    const bool accepted = !m_accept || m_accept();
    QStringList paths = !accepted ? QStringList()
                        : internalDragActive() ? internalDragPaths()
                                               : pathsFromHdrop(pDataObj);
    // The keys as they were while dragging: on drop the button is already up.
    const DWORD keys = m_keys | (grfKeyState & (MK_CONTROL | MK_SHIFT | MK_ALT));
    const bool rightDrag = (m_keys & MK_RBUTTON) != 0;
    m_canDrop = false;
    setDropDescription(pDataObj, QString());
    if (m_data) {
        m_data->Release();
        m_data = nullptr;
    }
    if (m_hover)
        m_hover(QPoint(), false);
    Action action = paths.isEmpty() ? Action::Cancel
                    : rightDrag     ? askOnDrop(QPoint(static_cast<int>(pt.x), static_cast<int>(pt.y)))
                                    : actionFor(keys);
    DWORD effect = DROPEFFECT_NONE;
    switch (action) {
    case Action::Insert:
        effect = kCopyEffect; // as before: nothing for the source to delete
        break;
    case Action::Copy:
        paths = copyShellItemsInto(paths, m_fileFolder.isEmpty() ? userDesktopFolder() : m_fileFolder,
                                   reinterpret_cast<quintptr>(m_hwnd));
        effect = paths.isEmpty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        break;
    case Action::Link:
        paths = createShortcutsIn(paths, m_fileFolder.isEmpty() ? userDesktopFolder() : m_fileFolder,
                                  reinterpret_cast<quintptr>(m_hwnd));
        effect = paths.isEmpty() ? DROPEFFECT_NONE : DROPEFFECT_LINK;
        break;
    case Action::Cancel:
        paths.clear();
        break;
    }
    m_lastAction = action;
    if (pdwEffect)
        *pdwEffect = effect;
    dragImageDrop(pDataObj, QPoint(static_cast<int>(pt.x), static_cast<int>(pt.y)), effect);

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
