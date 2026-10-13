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

        items.append(item);
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

} // namespace starfish::platform::win
