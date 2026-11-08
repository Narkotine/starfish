#include "platform/win/shell_operations.hpp"

#include <QCoreApplication>
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
#include <knownfolders.h>

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

// Session 92 (t94u): a probe's file operations on its own temp files must not
// reach Explorer's undo history, which is shared with the user's session: a
// probe rename left "Undo Rename" there, and the user's later Ctrl+Z / Undo on
// the desktop tried to rename a file whose temp folder was gone ("Element
// introuvable"). In isolation: no undo record, no shell UI, ever.
bool& isolated()
{
    static bool on = false;
    return on;
}

IFileOperation* newOperation(DWORD flags, quintptr owner)
{
    if (isolated()) {
        flags &= ~DWORD(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
        flags |= FOF_NO_UI;
        owner = 0;
    }
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

// The base names (no extension) of the files a data object carries, if any.
QStringList droppedStems(IDataObject* data)
{
    QStringList stems;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&format, &medium)))
        return stems;
    auto* drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal));
    if (drop) {
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH * 2]{};
            if (::DragQueryFileW(drop, i, path, MAX_PATH * 2))
                stems << QFileInfo(QString::fromWCharArray(path)).completeBaseName();
        }
        ::GlobalUnlock(medium.hGlobal);
    }
    ::ReleaseStgMedium(&medium);
    return stems;
}

class DesktopDropTarget : public IDropTarget
{
public:
    DesktopDropTarget(HWND hwnd, InternalDrop internalDrop, ForeignDrop foreignDrop, DropHover hover)
        : m_hwnd(hwnd), m_internalDrop(std::move(internalDrop)), m_foreignDrop(std::move(foreignDrop)),
          m_hover(std::move(hover))
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
        if (m_hover)
            m_hover(QPoint(pt.x, pt.y), m_internal, true);
        if (m_internal) {
            *effect = (*effect & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
            dragImageEnter(reinterpret_cast<quintptr>(m_hwnd), data, QPoint(pt.x, pt.y), *effect);
            return S_OK;
        }
        if (IDropTarget* shell = shellTarget())
            return shell->DragEnter(data, keys, pt, effect);
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override
    {
        if (m_hover)
            m_hover(QPoint(pt.x, pt.y), m_internal, true);
        if (m_internal) {
            *effect = (*effect & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
            dragImageOver(QPoint(pt.x, pt.y), *effect);
            return S_OK;
        }
        if (m_shell)
            return m_shell->DragOver(keys, pt, effect);
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (m_hover)
            m_hover(QPoint(), m_internal, false);
        if (!m_internal && m_shell)
            m_shell->DragLeave();
        if (m_internal)
            dragImageLeave();
        m_internal = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        if (m_hover)
            m_hover(QPoint(), m_internal, false);
        if (m_internal || internalDragActive()) {
            m_internal = false;
            // Ours: the icons move on the desktop, no file does. NONE tells the
            // source there is nothing for it to finish (no "delete after move").
            *effect = DROPEFFECT_NONE;
            dragImageDrop(data, QPoint(pt.x, pt.y), DROPEFFECT_MOVE);
            if (m_internalDrop)
                m_internalDrop(QPoint(pt.x, pt.y), internalDragPaths(), internalDragOrigin());
            return S_OK;
        }
        if (IDropTarget* shell = shellTarget()) {
            qCInfo(lc) << "a drop from outside goes to the desktop folder";
            if (m_foreignDrop)
                m_foreignDrop(QPoint(pt.x, pt.y), droppedStems(data));
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
    ForeignDrop m_foreignDrop;
    DropHover m_hover;
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

namespace {

HBITMAP bitmapFrom(const QImage& source)
{
    const QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width();
    info.bmiHeader.biHeight = -image.height(); // top-down, like QImage
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!bitmap || !bits)
        return nullptr;
    const int stride = image.width() * 4;
    for (int y = 0; y < image.height(); ++y)
        memcpy(static_cast<uchar*>(bits) + y * stride, image.constScanLine(y), size_t(stride));
    return bitmap;
}

IDropTargetHelper* targetHelper()
{
    static IDropTargetHelper* helper = [] {
        IDropTargetHelper* h = nullptr;
        if (FAILED(::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&h))))
            h = nullptr;
        return h;
    }();
    return helper;
}

} // namespace

bool attachDragImage(void* dataObject, const QImage& image, const QPoint& hotspot)
{
    auto* data = static_cast<IDataObject*>(dataObject);
    if (!data || image.isNull())
        return false;
    IDragSourceHelper* helper = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))
        || !helper)
        return false;
    IDragSourceHelper2* helper2 = nullptr;
    if (SUCCEEDED(helper->QueryInterface(IID_PPV_ARGS(&helper2))) && helper2) {
        helper2->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT); // "Move to Documents"...
        helper2->Release();
    }
    SHDRAGIMAGE drag{};
    drag.sizeDragImage = {image.width(), image.height()};
    drag.ptOffset = {qBound(0, hotspot.x(), image.width() - 1), qBound(0, hotspot.y(), image.height() - 1)};
    drag.hbmpDragImage = bitmapFrom(image);
    drag.crColorKey = CLR_NONE;
    const HRESULT hr = drag.hbmpDragImage ? helper->InitializeFromBitmap(&drag, data) : E_FAIL;
    if (FAILED(hr) && drag.hbmpDragImage)
        ::DeleteObject(drag.hbmpDragImage); // owned by the helper only on success
    helper->Release();
    if (FAILED(hr))
        qCWarning(lc) << "the drag image was refused, hr" << Qt::hex << quint32(hr);
    return SUCCEEDED(hr);
}

bool hasDragImage(void* dataObject)
{
    auto* data = static_cast<IDataObject*>(dataObject);
    if (!data)
        return false;
    FORMATETC format{static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(L"DragImageBits")), nullptr,
                     DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data->QueryGetData(&format) == S_OK;
}

void dragImageEnter(quintptr hwnd, void* dataObject, const QPoint& screen, unsigned long effect)
{
    if (IDropTargetHelper* h = targetHelper()) {
        POINT pt{screen.x(), screen.y()};
        h->DragEnter(reinterpret_cast<HWND>(hwnd), static_cast<IDataObject*>(dataObject), &pt, effect);
    }
}

void dragImageOver(const QPoint& screen, unsigned long effect)
{
    if (IDropTargetHelper* h = targetHelper()) {
        POINT pt{screen.x(), screen.y()};
        h->DragOver(&pt, effect);
    }
}

void dragImageLeave()
{
    if (IDropTargetHelper* h = targetHelper())
        h->DragLeave();
}

void dragImageDrop(void* dataObject, const QPoint& screen, unsigned long effect)
{
    if (IDropTargetHelper* h = targetHelper()) {
        POINT pt{screen.x(), screen.y()};
        h->Drop(static_cast<IDataObject*>(dataObject), &pt, effect);
    }
}

void setDropDescription(void* dataObject, const QString& message, int image)
{
    auto* data = static_cast<IDataObject*>(dataObject);
    if (!data)
        return;
    HGLOBAL global = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPDESCRIPTION));
    if (!global)
        return;
    auto* description = static_cast<DROPDESCRIPTION*>(::GlobalLock(global));
    if (!description) {
        ::GlobalFree(global);
        return;
    }
    // DROPIMAGE_INVALID with no text: the shell's own label comes back.
    description->type = message.isEmpty() ? DROPIMAGE_INVALID
                        : image < 0       ? DROPIMAGE_MOVE
                                          : static_cast<DROPIMAGETYPE>(image);
    const std::wstring text = message.left(MAX_PATH - 1).toStdWString();
    wcsncpy_s(description->szMessage, MAX_PATH, text.c_str(), _TRUNCATE);
    ::GlobalUnlock(global);
    FORMATETC format{static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION)), nullptr,
                     DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = global;
    if (FAILED(data->SetData(&format, &medium, TRUE)))
        ::GlobalFree(global); // not taken: ours to free
}

namespace {

// Session 98: what a copy or a move created, as the shell reports it.
class CreatedItems : public IFileOperationProgressSink {
public:
    QStringList created;
    ULONG STDMETHODCALLTYPE AddRef() override { return ::InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { return ::InterlockedDecrement(&m_ref); } // on the stack
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* made) override
    {
        note(hr, made);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* made) override
    {
        note(hr, made);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    void note(HRESULT hr, IShellItem* made)
    {
        if (FAILED(hr) || !made)
            return;
        PWSTR name = nullptr;
        if (SUCCEEDED(made->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
            created << QString::fromWCharArray(name);
            ::CoTaskMemFree(name);
        }
    }
    LONG m_ref = 1;
};

// Copies (or moves) every item into `folder`, the shell's undo included.
QStringList transfer(const QStringList& parsingNames, const QString& folder, quintptr owner, bool copy)
{
    IShellItem* destination = itemFor(folder);
    if (!destination)
        return {};
    IFileOperation* op = newOperation(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOF_RENAMEONCOLLISION, owner);
    if (!op) {
        destination->Release();
        return {};
    }
    int queued = 0;
    for (const QString& name : parsingNames) {
        if (IShellItem* item = itemFor(name)) {
            const HRESULT hr = copy ? op->CopyItem(item, destination, nullptr, nullptr)
                                    : op->MoveItem(item, destination, nullptr, nullptr);
            if (SUCCEEDED(hr))
                ++queued;
            item->Release();
        }
    }
    destination->Release();
    if (queued == 0) {
        op->Release();
        return {};
    }
    CreatedItems sink;
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(op->Advise(&sink, &cookie));
    const HRESULT hr = op->PerformOperations();
    if (advised)
        op->Unadvise(cookie);
    op->Release();
    if (FAILED(hr))
        qCWarning(lc).noquote() << QStringLiteral("%1 into %2 failed (hr 0x%3)")
                                       .arg(copy ? QLatin1String("copy") : QLatin1String("move"), folder)
                                       .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
    return sink.created;
}

} // namespace

QString userDesktopFolder()
{
    PWSTR path = nullptr;
    QString folder;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) && path)
        folder = QString::fromWCharArray(path);
    ::CoTaskMemFree(path);
    return folder;
}

QStringList copyShellItemsInto(const QStringList& parsingNames, const QString& folder, quintptr owner)
{
    const QStringList made = transfer(parsingNames, folder, owner, true);
    qCInfo(lc).noquote() << QStringLiteral("copied %1 item(s) into %2").arg(made.size()).arg(folder);
    return made;
}

QStringList createShortcutsIn(const QStringList& parsingNames, const QString& folder, quintptr owner)
{
    // Each shortcut is written in a folder of our own first, then moved in by
    // the shell: the move is what the desktop's Undo takes back.
    const QString staging = QDir::temp().filePath(QStringLiteral("starfish-links-%1")
                                                      .arg(QCoreApplication::applicationPid()));
    QDir().mkpath(staging);
    QStringList written;
    for (const QString& name : parsingNames) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(::SHParseDisplayName(wide(nativeName(name)), nullptr, &pidl, 0, nullptr)) || !pidl)
            continue;
        QString display;
        PWSTR shown = nullptr;
        if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &shown)) && shown) {
            display = QString::fromWCharArray(shown);
            ::CoTaskMemFree(shown);
        }
        if (display.isEmpty())
            display = QFileInfo(name).completeBaseName();
        for (const QChar bad : QStringLiteral("\\/:*?\"<>|"))
            display.replace(bad, QLatin1Char('_'));
        IShellLinkW* link = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))
            && link) {
            link->SetIDList(pidl);
            const QFileInfo info(nativeName(name));
            if (info.exists())
                link->SetWorkingDirectory(wide(QDir::toNativeSeparators(info.absolutePath())));
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
                const QString target = QDir::toNativeSeparators(QDir(staging).filePath(
                    QCoreApplication::translate("ShellOperations", "%1 - Shortcut").arg(display)
                    + QStringLiteral(".lnk")));
                if (SUCCEEDED(file->Save(wide(target), TRUE)))
                    written << target;
                file->Release();
            }
            link->Release();
        }
        ::CoTaskMemFree(pidl);
    }
    const QStringList made = written.isEmpty() ? QStringList() : transfer(written, folder, owner, false);
    QDir(staging).removeRecursively();
    qCInfo(lc).noquote() << QStringLiteral("%1 shortcut(s) created in %2").arg(made.size()).arg(folder);
    return made;
}

int dragShellItems(const QStringList& parsingNames, const QPoint& origin, quintptr owner,
                   const QImage& image, const QPoint& hotspot)
{
    auto* data = static_cast<IDataObject*>(createShellDataObject(parsingNames));
    if (!data)
        return DROPEFFECT_NONE;
    // Session 62: our picture, or the shell's default when there is none.
    if (!image.isNull())
        attachDragImage(data, image, hotspot);
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

void* createDesktopDropTarget(quintptr hwnd, InternalDrop internalDrop, ForeignDrop foreignDrop,
                              DropHover hover)
{
    return static_cast<IDropTarget*>(new DesktopDropTarget(reinterpret_cast<HWND>(hwnd), std::move(internalDrop),
                                                           std::move(foreignDrop), std::move(hover)));
}

} // namespace starfish::platform::win
namespace starfish::platform::win {

void bringToForeground(quintptr hwnd)
{
    if (hwnd)
        ::SetForegroundWindow(reinterpret_cast<HWND>(hwnd));
}

void setShellOperationIsolation(bool on)
{
    isolated() = on;
}

bool shellOperationIsolation()
{
    return isolated();
}

} // namespace starfish::platform::win
