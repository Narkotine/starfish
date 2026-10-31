#include "platform/win/desktop_scanner.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSettings>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shellapi.h> // SHGetFileInfoW
#include <shlguid.h>  // CLSID_ShellLink
#include <shlobj.h>   // SHGetKnownFolderPath, FOLDERID_*
#include <shobjidl.h> // IShellLinkW, IPersistFile

namespace starfish::platform::win {

namespace {

QLoggingCategory lc("starfish.desktop.scanner");

// The Shell APIs need COM. Explorer normally has it up already in the calling
// thread's apartment, in which case CoInitializeEx reports RPC_E_CHANGED_MODE:
// the APIs still work, we just must not pair it with a CoUninitialize.
class ComScope
{
public:
    ComScope() { m_result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComScope()
    {
        if (m_result == S_OK || m_result == S_FALSE)
            ::CoUninitialize();
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool usable() const { return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE; }
    HRESULT result() const { return m_result; }

private:
    HRESULT m_result = E_FAIL;
};

inline const wchar_t* wide(const QString& path)
{
    return reinterpret_cast<const wchar_t*>(path.utf16());
}

QString knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw)
        return {};
    const QString path = QString::fromWCharArray(raw);
    ::CoTaskMemFree(raw);
    return path;
}

// The name the Shell would draw, i.e. without the extension for the types it
// knows about ("Raccourci.lnk" -> "Raccourci"). Falls back to the file name.
QString shellDisplayName(const QString& nativePath)
{
    SHFILEINFOW info{};
    if (::SHGetFileInfoW(wide(nativePath), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) == 0)
        return {};
    return QString::fromWCharArray(info.szDisplayName);
}

// .lnk resolution through the Shell, so that the rules engine (F-017) can
// match on the target and not only on the shortcut name.
QString resolveShortcutTarget(const QString& nativePath)
{
    IShellLinkW* link = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&link)))
        || !link) {
        return {};
    }

    QString target;
    IPersistFile* persist = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist)))
        && persist) {
        if (SUCCEEDED(persist->Load(wide(nativePath), STGM_READ))) {
            wchar_t buffer[MAX_PATH] = {};
            if (SUCCEEDED(link->GetPath(buffer, MAX_PATH, nullptr, SLGP_RAWPATH))
                && buffer[0] != L'\0') {
                target = QString::fromWCharArray(buffer);
            }
        }
        persist->Release();
    }
    link->Release();
    return target;
}

// .url files are INI documents, so QSettings reads them without any Shell
// round-trip.
QString resolveInternetShortcutTarget(const QString& path)
{
    QSettings shortcut(path, QSettings::IniFormat);
    return shortcut.value(QStringLiteral("InternetShortcut/URL")).toString();
}

bool hasHiddenAttribute(const QString& nativePath)
{
    const DWORD attributes = ::GetFileAttributesW(wide(nativePath));
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

// Builds a DesktopItem from a single file's QFileInfo using the same Shell
// resolution as the desktop scan (display name, shortcut target, hidden
// attribute). Shared by scanFolder() and itemForPath() so a dropped file and a
// scanned desktop entry resolve identically.
core::DesktopItem itemFromFileInfo(const QFileInfo& info, bool isPublicDesktop)
{
    const QString fileName = info.fileName();
    core::DesktopItem item;
    item.fileName = fileName;
    item.path = QDir::cleanPath(info.absoluteFilePath());
    item.isDir = info.isDir();
    item.modified = info.lastModified();
    item.fromPublicDesktop = isPublicDesktop;
    item.sizeBytes = item.isDir ? -1 : info.size();

    const QString nativePath = QDir::toNativeSeparators(item.path);
    const QString displayName = shellDisplayName(nativePath);
    item.name = displayName.isEmpty() ? fileName : displayName;

    const QString suffix = info.suffix().toLower();
    item.isLink = suffix == QLatin1String("lnk") || suffix == QLatin1String("url");
    if (suffix == QLatin1String("lnk"))
        item.targetPath = resolveShortcutTarget(nativePath);
    else if (suffix == QLatin1String("url"))
        item.targetPath = resolveInternetShortcutTarget(item.path);

    item.isHidden = hasHiddenAttribute(nativePath) || core::isDotHiddenName(fileName);
    if (info.exists())
        item.fileId = fileIdOf(item.path);
    return item;
}

QList<core::DesktopItem> scanFolder(const QString& folder, bool isPublicDesktop)
{
    QList<core::DesktopItem> items;
    if (folder.isEmpty())
        return items;

    const QDir dir(folder);
    if (!dir.exists()) {
        qCWarning(lc).noquote() << QStringLiteral("desktop folder not found: %1").arg(folder);
        return items;
    }

    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);

    for (const QFileInfo& info : entries) {
        const QString fileName = info.fileName();
        // The Shell's own bookkeeping file for folder view settings.
        if (fileName.compare(QLatin1String("desktop.ini"), Qt::CaseInsensitive) == 0)
            continue;

        items.append(itemFromFileInfo(info, isPublicDesktop));
    }
    return items;
}

} // namespace

QList<core::DesktopItem> DesktopScanner::scan(QString* error)
{
    if (error)
        error->clear();

    const ComScope com;
    if (!com.usable()) {
        if (error) {
            *error = QStringLiteral("COM could not be initialised (HRESULT 0x%1)")
                         .arg(static_cast<quint32>(com.result()), 8, 16, QLatin1Char('0'));
        }
        return {};
    }

    const QStringList folders = desktopFolders();
    if (folders.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "SHGetKnownFolderPath returned neither FOLDERID_Desktop nor "
                "FOLDERID_PublicDesktop");
        }
        return {};
    }

    QList<core::DesktopItem> items;
    items += scanFolder(folders.at(0), false);
    if (folders.size() > 1)
        items += scanFolder(folders.at(1), true);

    const QList<core::DesktopItem> merged = core::mergeDesktopItems(items);
    qCInfo(lc).noquote() << QStringLiteral("desktop: %1 entries (%2 before merging the "
                                           "per-user and public folders)")
                                .arg(merged.size())
                                .arg(items.size());
    return merged;
}

QStringList DesktopScanner::desktopFolders()
{
    QStringList folders;
    const QString user = knownFolder(FOLDERID_Desktop);
    if (!user.isEmpty())
        folders.append(QDir::cleanPath(user));
    const QString publicDesktop = knownFolder(FOLDERID_PublicDesktop);
    if (!publicDesktop.isEmpty())
        folders.append(QDir::cleanPath(publicDesktop));
    return folders;
}

core::DesktopItem DesktopScanner::itemForPath(const QString& path)
{
    if (path.isEmpty())
        return {};
    const ComScope com;
    if (!com.usable())
        return {};
    // ADR-21: a virtual item (This PC, the Recycle Bin) is named by its shell
    // item, not by a file: the tile shows what the desktop shows.
    if (path.startsWith(QLatin1String("::"))) {
        core::DesktopItem item;
        item.path = path;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                           &pidl, 0, nullptr))
            && pidl) {
            PWSTR name = nullptr;
            if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &name)) && name) {
                item.name = QString::fromWCharArray(name);
                ::CoTaskMemFree(name);
            }
            ::CoTaskMemFree(pidl);
        }
        if (item.name.isEmpty())
            item.name = path;
        item.fileName = item.name;
        item.isDir = true;
        return item;
    }
    // Even a path that does not (yet) exist becomes a valid tile: the Shell
    // display name then falls back to the file name, and the tile is still
    // useful once the file appears (a drop can race with a copy completing).
    const QFileInfo info(path);
    return itemFromFileInfo(info, false);
}

QString fileIdOf(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1String("::")))
        return {};
    const QString native = QDir::toNativeSeparators(path);
    // Attributes only, every sharing mode: never in the way of anyone.
    HANDLE handle = ::CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION info{};
    QString id;
    if (::GetFileInformationByHandle(handle, &info)) {
        const quint64 index = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        id = QStringLiteral("%1-%2").arg(quint32(info.dwVolumeSerialNumber), 8, 16, QLatin1Char('0'))
                 .arg(index, 16, 16, QLatin1Char('0'));
    }
    ::CloseHandle(handle);
    return id;
}

bool openShellItem(const QString& path)
{
    if (path.isEmpty())
        return false;
    const ComScope com;
    PIDLIST_ABSOLUTE pidl = nullptr;
    const QString native = QDir::toNativeSeparators(path);
    if (FAILED(::SHParseDisplayName(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr, &pidl, 0,
                                    nullptr))
        || !pidl) {
        qCWarning(lc).noquote() << QStringLiteral("\"%1\": no shell item to open").arg(path);
        return false;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpIDList = pidl;
    info.nShow = SW_SHOWNORMAL;
    const bool ok = ::ShellExecuteExW(&info) != FALSE;
    ::CoTaskMemFree(pidl);
    if (!ok)
        qCWarning(lc).noquote() << QStringLiteral("\"%1\" could not be opened (error %2)")
                                       .arg(path)
                                       .arg(::GetLastError());
    return ok;
}
} // namespace starfish::platform::win
