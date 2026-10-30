#include "platform/win/packaged_commands.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QLoggingCategory>
#include <QMap>
#include <QSettings>
#include <QStandardPaths>
#include <QVersionNumber>
#include <QXmlStreamReader>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace starfish::platform::win {

namespace {

Q_LOGGING_CATEGORY(lc, "starfish.packaged")

struct Declaration {
    QString family;   // "Microsoft.WindowsTerminal_8wekyb3d8bbwe"
    QString itemType; // "*", ".png", "Directory", "Directory\Background"
    QString verbId;
    QString clsid;    // "{...}"
};

const QString kRepository = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\"
    "CurrentVersion\\AppModel\\Repository\\Packages");

// "Name_Version_Arch_ResourceId_Publisher" -> family "Name_Publisher", version.
bool splitFullName(const QString& full, QString* family, QVersionNumber* version)
{
    const QStringList parts = full.split(QLatin1Char('_'));
    if (parts.size() < 5)
        return false;
    *family = parts.first() + QLatin1Char('_') + parts.last();
    *version = QVersionNumber::fromString(parts.at(1));
    return true;
}

void readManifest(const QString& path, const QString& family, std::vector<Declaration>& out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader xml(&file);
    bool inMenus = false;
    QString itemType;
    while (!xml.atEnd()) {
        xml.readNext();
        const QStringView name = xml.name();
        if (xml.isStartElement()) {
            if (name == QLatin1String("FileExplorerContextMenus"))
                inMenus = true;
            else if (inMenus && name == QLatin1String("ItemType"))
                itemType = xml.attributes().value(QLatin1String("Type")).toString();
            else if (inMenus && name == QLatin1String("Verb") && !itemType.isEmpty()) {
                Declaration d;
                d.family = family;
                d.itemType = itemType;
                d.verbId = xml.attributes().value(QLatin1String("Id")).toString();
                QString clsid = xml.attributes().value(QLatin1String("Clsid")).toString();
                if (!clsid.startsWith(QLatin1Char('{')))
                    clsid = QLatin1Char('{') + clsid + QLatin1Char('}');
                d.clsid = clsid;
                if (!d.verbId.isEmpty())
                    out.push_back(d);
            }
        } else if (xml.isEndElement()) {
            if (name == QLatin1String("FileExplorerContextMenus"))
                inMenus = false;
            else if (name == QLatin1String("ItemType"))
                itemType.clear();
        }
    }
}

// The declarations of the newest version of each package, read again at most
// once a minute (an app installed meanwhile shows up on the next menu).
const std::vector<Declaration>& declarations()
{
    static std::vector<Declaration> cached;
    static qint64 readAt = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (readAt && now - readAt < 60000)
        return cached;
    readAt = now;
    cached.clear();
    QSettings repo(kRepository, QSettings::NativeFormat);
    QMap<QString, QPair<QVersionNumber, QString>> newest; // family -> (version, root)
    for (const QString& full : repo.childGroups()) {
        QString family;
        QVersionNumber version;
        if (!splitFullName(full, &family, &version))
            continue;
        const QString root = repo.value(full + QStringLiteral("/PackageRootFolder")).toString();
        if (root.isEmpty() || !QFileInfo::exists(root + QStringLiteral("/AppxManifest.xml")))
            continue;
        if (!newest.contains(family) || newest.value(family).first < version)
            newest.insert(family, {version, root});
    }
    for (auto it = newest.cbegin(); it != newest.cend(); ++it)
        readManifest(it.value().second + QStringLiteral("/AppxManifest.xml"), it.key(), cached);
    qCInfo(lc) << cached.size() << "packaged menu verb(s) declared in" << newest.size() << "package(s)";
    return cached;
}

struct Live {
    IExplorerCommand* command = nullptr;
    IShellItemArray* items = nullptr;
};
std::vector<Live> g_live;
// One object per class for the session: activating a packaged server costs.
QHash<QString, IExplorerCommand*> g_objects;

IExplorerCommand* objectFor(const QString& clsidText)
{
    if (g_objects.contains(clsidText))
        return g_objects.value(clsidText);
    CLSID clsid{};
    IExplorerCommand* command = nullptr;
    if (SUCCEEDED(::CLSIDFromString(reinterpret_cast<LPCWSTR>(clsidText.utf16()), &clsid))) {
        const HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&command));
        if (FAILED(hr)) {
            qCInfo(lc).noquote() << QStringLiteral("%1 not activatable: 0x%2")
                                        .arg(clsidText)
                                        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
            command = nullptr;
        }
    }
    g_objects.insert(clsidText, command); // a failure is remembered too
    return command;
}

IShellItemArray* itemArray(const QStringList& paths)
{
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const QString& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(p).utf16()),
                                           nullptr, &pidl, 0, nullptr)) && pidl)
            pidls.push_back(pidl);
    }
    IShellItemArray* array = nullptr;
    if (!pidls.empty())
        ::SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                            const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
    for (PIDLIST_ABSOLUTE pidl : pidls)
        ::CoTaskMemFree(pidl);
    return array;
}

QString iconPng(IExplorerCommand* command, IShellItemArray* items, const QString& clsid)
{
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                            .filePath(QStringLiteral("StarFish/cache"));
    const QString png = QDir(dir).filePath(QStringLiteral("pkg-%1.png").arg(QString(clsid).remove(QLatin1Char('{')).remove(QLatin1Char('}'))));
    if (QFileInfo::exists(png))
        return png;
    LPWSTR resource = nullptr;
    if (FAILED(command->GetIcon(items, &resource)) || !resource)
        return {};
    QString spec = QString::fromWCharArray(resource);
    ::CoTaskMemFree(resource);
    int index = 0;
    const int comma = spec.lastIndexOf(QLatin1Char(','));
    if (comma > 0) {
        index = spec.mid(comma + 1).toInt();
        spec = spec.left(comma);
    }
    spec.remove(QLatin1Char('"'));
    HICON icon = nullptr;
    ::ExtractIconExW(reinterpret_cast<LPCWSTR>(spec.utf16()), index, nullptr, &icon, 1);
    if (!icon)
        return {};
    const QImage image = QImage::fromHICON(icon);
    ::DestroyIcon(icon);
    QDir().mkpath(dir);
    return !image.isNull() && image.save(png) ? png : QString();
}

} // namespace

std::vector<PackagedCommand> packagedCommandsFor(const QStringList& paths, bool background,
                                                 const QString& desktopFolder)
{
    releasePackagedCommands();
    QStringList types;
    QStringList targets = paths;
    if (background) {
        types << QStringLiteral("Directory\\Background");
        targets = {desktopFolder};
    } else {
        bool dirs = true;
        bool files = true;
        QStringList exts;
        for (const QString& p : paths) {
            if (p.startsWith(QLatin1String("::")))
                return {}; // This PC & co.: no file type to match
            const QFileInfo info(p);
            if (info.isDir()) {
                files = false;
            } else {
                dirs = false;
                exts << QLatin1Char('.') + info.suffix().toLower();
            }
        }
        if (dirs)
            types << QStringLiteral("Directory");
        if (files) {
            types << QStringLiteral("*");
            exts.removeDuplicates();
            if (exts.size() == 1)
                types << exts.first();
        }
    }
    std::vector<PackagedCommand> out;
    QStringList seen;
    for (const Declaration& d : declarations()) {
        if (!types.contains(d.itemType, Qt::CaseInsensitive))
            continue;
        const QString key = QStringLiteral("app:%1/%2").arg(d.family.section(QLatin1Char('_'), 0, 0), d.verbId);
        if (seen.contains(key))
            continue;
        seen << key;
        IExplorerCommand* command = objectFor(d.clsid);
        if (!command)
            continue;
        IShellItemArray* items = itemArray(targets);
        if (!items)
            continue;
        EXPCMDSTATE state = ECS_ENABLED;
        if (FAILED(command->GetState(items, FALSE, &state)) || (state & ECS_HIDDEN)) {
            items->Release();
            continue;
        }
        LPWSTR title = nullptr;
        if (FAILED(command->GetTitle(items, &title)) || !title) {
            items->Release();
            continue;
        }
        PackagedCommand entry;
        entry.key = key;
        entry.title = QString::fromWCharArray(title).remove(QLatin1Char('&'));
        ::CoTaskMemFree(title);
        entry.enabled = !(state & ECS_DISABLED);
        entry.iconFile = iconPng(command, items, d.clsid);
        command->AddRef();
        g_live.push_back({command, items});
        entry.index = static_cast<int>(g_live.size()) - 1;
        out.push_back(entry);
    }
    return out;
}

bool invokePackagedCommand(int index)
{
    if (index < 0 || index >= static_cast<int>(g_live.size()))
        return false;
    const Live& live = g_live.at(static_cast<size_t>(index));
    const HRESULT hr = live.command->Invoke(live.items, nullptr);
    qCInfo(lc).noquote() << QStringLiteral("Invoke: 0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
    return SUCCEEDED(hr);
}

void releasePackagedCommands()
{
    for (Live& live : g_live) {
        live.command->Release();
        live.items->Release();
    }
    g_live.clear();
}

QStringList packagedCommandDeclarations()
{
    QStringList out;
    for (const Declaration& d : declarations())
        out << QStringLiteral("%1: %2 -> %3 %4").arg(d.family, d.itemType, d.verbId, d.clsid);
    return out;
}

} // namespace starfish::platform::win
