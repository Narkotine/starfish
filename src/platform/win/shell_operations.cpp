#include "platform/win/shell_operations.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#ifndef FOFX_ADDUNDORECORD
#define FOFX_ADDUNDORECORD 0x20000000
#endif
#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif

namespace starfish::platform::win {

namespace {

Q_LOGGING_CATEGORY(lc, "starfish.shellops")

// {B8C0BD9F-ED24-455C-83E6-D5390C4FE8C4}, spelled here: not every SDK's
// import library carries it.
const GUID kBhidDataObject = {0xB8C0BD9F, 0xED24, 0x455C,
                              {0x83, 0xE6, 0xD5, 0x39, 0x0C, 0x4F, 0xE8, 0xC4}};

LPCWSTR wide(const QString& s)
{
    return reinterpret_cast<LPCWSTR>(s.utf16());
}

QString nativeName(const QString& parsingName)
{
    return parsingName.startsWith(QLatin1String("::"))
               ? parsingName
               : QString(parsingName).replace(QLatin1Char('/'), QLatin1Char('\\'));
}

IShellItem* itemFor(const QString& parsingName)
{
    IShellItem* item = nullptr;
    const QString name = nativeName(parsingName);
    if (FAILED(::SHCreateItemFromParsingName(wide(name), nullptr, IID_PPV_ARGS(&item))))
        return nullptr;
    return item;
}

IFileOperation* newOperation(DWORD flags, quintptr owner)
{
    IFileOperation* op = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))
        || !op)
        return nullptr;
    op->SetOperationFlags(flags);
    if (owner)
        op->SetOwnerWindow(reinterpret_cast<HWND>(owner));
    return op;
}

bool perform(IFileOperation* op, const char* what)
{
    HRESULT hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    op->Release();
    if (FAILED(hr) || aborted) {
        qCWarning(lc).noquote() << QStringLiteral("%1: %2 (hr 0x%3)")
                                       .arg(QLatin1String(what))
                                       .arg(aborted ? QLatin1String("cancelled")
                                                    : QLatin1String("failed"))
                                       .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

struct DragState {
    bool active = false;
    QStringList paths;
    QPoint origin;
};
DragState& dragState()
{
    static DragState state;
    return state;
}

class DesktopDropTarget : public IDropTarget
{
public:
    DesktopDropTarget(HWND hwnd, InternalDrop internalDrop)
        : m_hwnd(hwnd), m_internalDrop(std::move(internalDrop))
    {
    }
    ~DesktopDropTarget()
    {
        if (m_shell)
            m_shell->Release();
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ::InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG ref = ::InterlockedDecrement(&m_ref);
        if (ref == 0)
            delete this;
        return static_cast<ULONG>(ref);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
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

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt,
                                        DWORD* effect) override
    {
        m_internal = internalDragActive();
        if (m_internal) {
            *effect = (*effect & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
            return S_OK;
        }
        if (IDropTarget* shell = shellTarget())
            return shell->DragEnter(data, keys, pt, effect);
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override
    {
        if (m_internal) {
            *effect = (*effect & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
            return S_OK;
        }
        if (m_shell)
            return m_shell->DragOver(keys, pt, effect);
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (!m_internal && m_shell)
            m_shell->DragLeave();
        m_internal = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        if (m_internal || internalDragActive()) {
            m_internal = false;
            // Ours: the icons move on the desktop, no file does. NONE tells the
            // source there is nothing for it to finish (no "delete after move").
            *effect = DROPEFFECT_NONE;
            if (m_internalDrop)
                m_internalDrop(QPoint(pt.x, pt.y), internalDragPaths(), internalDragOrigin());
            return S_OK;
        }
        if (IDropTarget* shell = shellTarget()) {
            qCInfo(lc) << "a drop from outside goes to the desktop folder";
            return shell->Drop(data, keys, pt, effect);
        }
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    IDropTarget* shellTarget()
    {
        if (m_shell)
            return m_shell;
        IShellFolder* desktop = nullptr;
        if (SUCCEEDED(::SHGetDesktopFolder(&desktop)) && desktop) {
            if (FAILED(desktop->CreateViewObject(m_hwnd, IID_PPV_ARGS(&m_shell))))
                m_shell = nullptr;
            desktop->Release();
        }
        if (!m_shell)
            qCWarning(lc) << "the desktop folder has no drop target";
        return m_shell;
    }

    HWND m_hwnd = nullptr;
    InternalDrop m_internalDrop;
    IDropTarget* m_shell = nullptr;
    bool m_internal = false;
    LONG m_ref = 1;
};

} // namespace

QString editingNameOf(const QString& parsingName)
{
    IShellItem* item = itemFor(parsingName);
    if (!item)
        return {};
    PWSTR name = nullptr;
    QString result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEEDITING, &name)) && name) {
        result = QString::fromWCharArray(name);
        ::CoTaskMemFree(name);
    }
    item->Release();
    return result;
}

bool renameShellItem(const QString& parsingName, const QString& newName, quintptr owner,
                     QString* newPath)
{
    const QString typed = newName.trimmed();
    if (typed.isEmpty())
        return false;
    QString finalName = typed;
    const bool isFile = !parsingName.startsWith(QLatin1String("::"));
    if (isFile) {
        // Extensions hidden: the edit showed "Report", the file is "Report.docx",
        // and "Summary" means "Summary.docx" -- as Explorer does it.
        const QFileInfo info(parsingName);
        const QString suffix = info.isDir() ? QString() : info.suffix();
        const QString edited = editingNameOf(parsingName);
        const bool hidden = !suffix.isEmpty()
                            && !edited.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive);
        if (hidden && !typed.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive))
            finalName += QLatin1Char('.') + suffix;
    }
    IShellItem* item = itemFor(parsingName);
    if (!item)
        return false;
    IFileOperation* op = newOperation(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD, owner);
    if (!op) {
        item->Release();
        return false;
    }
    op->RenameItem(item, wide(finalName), nullptr);
    item->Release();
    if (!perform(op, "rename"))
        return false;
    if (newPath && isFile)
        *newPath = QFileInfo(parsingName).dir().filePath(finalName);
    qCInfo(lc).noquote() << QStringLiteral("renamed \"%1\" -> \"%2\"").arg(parsingName, finalName);
    return true;
}

bool deleteShellItems(const QStringList& parsingNames, DeleteMode mode, quintptr owner)
{
    if (parsingNames.isEmpty())
        return false;
    DWORD flags = 0;
    switch (mode) {
    case DeleteMode::RecycleBin:
        flags = FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD;
        break;
    case DeleteMode::Permanent:
        flags = 0;
        break;
    case DeleteMode::PermanentSilent:
        flags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        break;
    }
    IFileOperation* op = newOperation(flags, owner);
    if (!op)
        return false;
    int queued = 0;
    for (const QString& name : parsingNames) {
        if (IShellItem* item = itemFor(name)) {
            if (SUCCEEDED(op->DeleteItem(item, nullptr)))
                ++queued;
            item->Release();
        }
    }
    if (queued == 0) {
        op->Release();
        return false;
    }
    return perform(op, "delete");
}

void* createShellDataObject(const QStringList& parsingNames)
{
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const QString& name : parsingNames) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(wide(nativeName(name)), nullptr, &pidl, 0, nullptr))
            && pidl)
            pidls.push_back(pidl);
    }
    IDataObject* data = nullptr;
    if (!pidls.empty()) {
        IShellItemArray* array = nullptr;
        if (SUCCEEDED(::SHCreateShellItemArrayFromIDLists(
                static_cast<UINT>(pidls.size()),
                const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array))
            && array) {
            if (FAILED(array->BindToHandler(nullptr, kBhidDataObject, IID_PPV_ARGS(&data))))
                data = nullptr;
            array->Release();
        }
    }
    for (PIDLIST_ABSOLUTE pidl : pidls)
        ::CoTaskMemFree(pidl);
    if (!data)
        qCWarning(lc) << "no data object for" << parsingNames;
    return data;
}

void releaseComObject(void* object)
{
    if (object)
        static_cast<IUnknown*>(object)->Release();
}

bool shellDataObjectFormats(void* dataObject, bool* hdrop, bool* idList)
{
    auto* data = static_cast<IDataObject*>(dataObject);
    if (!data)
        return false;
    FORMATETC drop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    FORMATETC ids{static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr,
                  DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (hdrop)
        *hdrop = data->QueryGetData(&drop) == S_OK;
    if (idList)
        *idList = data->QueryGetData(&ids) == S_OK;
    return true;
}

int dragShellItems(const QStringList& parsingNames, const QPoint& origin, quintptr owner)
{
    auto* data = static_cast<IDataObject*>(createShellDataObject(parsingNames));
    if (!data)
        return DROPEFFECT_NONE;
    beginInternalDrag(parsingNames, origin);
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = ::SHDoDragDrop(reinterpret_cast<HWND>(owner), data, nullptr,
                                      DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    endInternalDrag();
    data->Release();
    qCInfo(lc).noquote() << QStringLiteral("drag of %1 item(s) ended: hr 0x%2, effect %3")
                                .arg(parsingNames.size())
                                .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))
                                .arg(effect);
    return static_cast<int>(effect);
}

bool internalDragActive()
{
    return dragState().active;
}

QStringList internalDragPaths()
{
    return dragState().paths;
}

QPoint internalDragOrigin()
{
    return dragState().origin;
}

void beginInternalDrag(const QStringList& parsingNames, const QPoint& origin)
{
    dragState() = DragState{true, parsingNames, origin};
}

void endInternalDrag()
{
    dragState() = DragState{};
}

void* createDesktopDropTarget(quintptr hwnd, InternalDrop internalDrop)
{
    return static_cast<IDropTarget*>(
        new DesktopDropTarget(reinterpret_cast<HWND>(hwnd), std::move(internalDrop)));
}

} // namespace starfish::platform::win
namespace starfish::platform::win {

void bringToForeground(quintptr hwnd)
{
    if (hwnd)
        ::SetForegroundWindow(reinterpret_cast<HWND>(hwnd));
}

} // namespace starfish::platform::win
