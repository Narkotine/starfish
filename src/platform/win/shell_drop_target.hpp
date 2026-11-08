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

    // Session 80: told where a drop it could take hovers (client coordinates,
    // as for the handler), and when it leaves or lands (`active` false).
    void setHoverHandler(std::function<void(const QPoint&, bool)> hover) { m_hover = std::move(hover); }
    // Session 94: asked at each drag whether anything may land at all (a locked
    // fence says no: the cursor shows "no drop", no bar, no handler call).
    void setAcceptHandler(std::function<bool()> accept) { m_accept = std::move(accept); }
    // Session 98 (T-160): Ctrl copies, Alt makes a shortcut, a right-button
    // drag asks (Move here / Copy here / Create shortcuts here / Cancel).
    // Copies and shortcuts go to `folder` (empty: the user's Desktop) and the
    // fence gets them. `chooser` answers the menu instead of the user (probes).
    enum class Action { Insert = 0, Copy = 1, Link = 2, Cancel = 3 };
    void setFileFolder(const QString& folder) { m_fileFolder = folder; }
    void setMenuChooser(std::function<int()> chooser) { m_menuChooser = std::move(chooser); }
    Action lastAction() const { return m_lastAction; }

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
    Action actionFor(DWORD keys) const;
    DWORD effectFor(DWORD keys, DWORD allowed) const;
    void describe(IDataObject* data, DWORD keys);
    Action askOnDrop(const QPoint& screen);
    Action m_described = Action::Cancel;
    Action m_lastAction = Action::Cancel;
    static bool dataHasHdrop(IDataObject* data);
    static QStringList pathsFromHdrop(IDataObject* data);

    HWND m_hwnd;
    std::function<void(const QPoint&, const QStringList&)> m_handler;
    std::function<void(const QPoint&, bool)> m_hover;
    std::function<bool()> m_accept;
    QPoint clientOf(const POINTL& pt) const;
    LONG m_ref = 1;
    bool m_canDrop = false;
    DWORD m_keys = 0;    // session 98: the keys and buttons while dragging
    DWORD m_allowed = 0; // what the source allows
    QString m_fileFolder;
    std::function<int()> m_menuChooser;
    IDataObject* m_data = nullptr; // session 97: held from DragEnter to DragLeave / Drop
};

} // namespace starfish::platform::win
