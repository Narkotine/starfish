#pragma once

#include <functional>
#include <QStringList>

#include <QPoint>

#include <windows.h>
#include <oleidl.h> // IDropTarget

namespace starfish::platform::win {

// An OLE drop target (T-006-3) for a raw Win32 window -- the path the desktop
// surface takes, where Qt's own drag-and-drop never runs because the fence is
// rendered offscreen and only the Win32 window is on screen.
//
// It accepts CF_HDROP (the clipboard format Explorer drops files as), extracts
// the paths, and reports them to a handler with the drop point in *client*
// coordinates of the registered window. The application wires the handler to
// the fence's addDroppedItems().
class ShellDropTarget : public IDropTarget {
public:
    // `hwnd` is the window the target is registered on; it is used to convert
    // the screen coordinates OLE reports into client coordinates for the
    // handler. `handler` is invoked on the calling (UI) thread.
    explicit ShellDropTarget(HWND hwnd,
                             std::function<void(const QPoint&, const QStringList&)> handler);
    ~ShellDropTarget();

    ShellDropTarget(const ShellDropTarget&) = delete;
    ShellDropTarget& operator=(const ShellDropTarget&) = delete;

    // IUnknown.
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;

    // IDropTarget.
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt,
                                        DWORD* pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt,
                                   DWORD* pdwEffect) override;

private:
    static bool dataHasHdrop(IDataObject* data);
    static QStringList pathsFromHdrop(IDataObject* data);

    HWND m_hwnd;
    std::function<void(const QPoint&, const QStringList&)> m_handler;
    LONG m_ref = 1;
    bool m_canDrop = false;
};

} // namespace starfish::platform::win
