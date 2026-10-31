#include "core/i18n.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLoggingCategory>
#include <QTranslator>

namespace starfish::core::i18n {

namespace {

// A function-local category: a file-scope one is an external symbol, and the day
// a second translation unit defines `lc` the *link* fails somewhere else
// (docs/technique.md, "un `QLoggingCategory` de portée fichier est un symbole
// externe").
QLoggingCategory& log()
{
    static QLoggingCategory category("starfish.i18n");
    return category;
}

// Owned by the application, kept here so `uninstall()` (and a second `install()`)
// can hand them back. Never more than the translators of one language: leaving
// the previous ones installed would make the interface a mixture of two.
QList<QTranslator*>& translators()
{
    static QList<QTranslator*> list;
    return list;
}

QString& currentLanguage()
{
    static QString language;
    return language;
}

// The directories a catalogue is looked for in, in order: next to the executable
// (the deployed layout, where `starfish_fr.qm` and Qt's own `qtbase_fr.qm` are
// copied), the usual `translations/` subdirectory, then the Qt installation
// itself -- which is what a development build has and a deployed one does not.
QStringList catalogueDirectories(const QString& applicationDir)
{
    QStringList dirs;
    dirs << applicationDir;
    dirs << applicationDir + QStringLiteral("/translations");
    const QString qtTranslations = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (!qtTranslations.isEmpty() && !dirs.contains(qtTranslations))
        dirs << qtTranslations;
    return dirs;
}

// Loads `name` from the first directory that has it. Returns false when no
// directory does.
bool loadFrom(QTranslator* translator, const QString& name, const QStringList& directories)
{
    for (const QString& dir : directories) {
        if (translator->load(name, dir))
            return true;
    }
    return false;
}

} // namespace

QStringList availableLanguages()
{
    // Session 68: the offer is what ships -- the source language plus every
    // `starfish_<lang>.qm` found where install() looks (next to the executable,
    // then translations/). Adding a language is adding src/i18n/starfish_<lang>.ts:
    // CMake compiles it, this lists it, Settings offers it.
    QStringList languages{QLatin1String(kSourceLanguage)};
    if (QCoreApplication* app = QCoreApplication::instance()) {
        const QString base = app->applicationDirPath();
        for (const QString& dir : {base, base + QStringLiteral("/translations")}) {
            const QStringList files =
                QDir(dir).entryList({QStringLiteral("starfish_*.qm")}, QDir::Files, QDir::Name);
            for (const QString& file : files) {
                const QString code = file.mid(9, file.size() - 9 - 3).toLower(); // starfish_<code>.qm
                if (!code.isEmpty() && !languages.contains(code))
                    languages << code;
            }
        }
    }
    return languages;
}

QString displayName(const QString& language)
{
    if (language == QLatin1String(kSourceLanguage))
        return QStringLiteral("English");
    QString name = QLocale(language).nativeLanguageName();
    if (name.isEmpty())
        return language;
    name[0] = name.at(0).toUpper();
    return name;
}

QString catalogNameFor(const QString& language)
{
    if (language.isEmpty() || language == QLatin1String(kSourceLanguage))
        return QString();
    return QStringLiteral("starfish_") + language;
}

QString fromLocale(const QLocale& locale)
{
    // Any language that ships, not only French (session 68).
    const QString code = locale.name().section(QLatin1Char('_'), 0, 0).toLower();
    if (availableLanguages().contains(code))
        return code;
    return QLatin1String(kSourceLanguage);
}

QString fromSystemLocale()
{
    return fromLocale(QLocale::system());
}

std::optional<QString> resolve(const QString& requested, QString* error)
{
    if (requested.isEmpty())
        return fromSystemLocale();

    // Accept what a user actually types: a bare code ("fr"), a full locale
    // ("fr_FR"), either case. The point is that the *offer* is
    // availableLanguages(): anything else is refused rather than approximated.
    const QString normalized = requested.toLower().section(QLatin1Char('_'), 0, 0)
                                   .section(QLatin1Char('-'), 0, 0);
    if (availableLanguages().contains(normalized))
        return normalized;

    if (error) {
        *error = QStringLiteral("unknown language \"%1\" (available: %2)")
                     .arg(requested, availableLanguages().join(QLatin1Char(',')));
    }
    return std::nullopt;
}

bool install(const QString& language, QString* error)
{
    uninstall();

    const std::optional<QString> wanted = resolve(language, error);
    if (!wanted)
        return false;
    const QString lang = *wanted;

    currentLanguage() = lang;
    // The source language needs no catalogue and no application: "English" here
    // means "the strings as they are written in the code".
    if (lang == QLatin1String(kSourceLanguage))
        return true;

    QCoreApplication* const application = QCoreApplication::instance();
    if (!application) {
        currentLanguage().clear();
        if (error)
            *error = QStringLiteral("no application instance to install a translator on");
        return false;
    }

    const QStringList directories = catalogueDirectories(application->applicationDirPath());

    // Our own strings first: this is the one catalogue whose absence is reported,
    // because it is the promise F-036 makes.
    auto* ours = new QTranslator(application);
    if (!loadFrom(ours, catalogNameFor(lang), directories)) {
        delete ours;
        currentLanguage().clear();
        if (error) {
            *error = QStringLiteral("no catalogue %1.qm for \"%2\" (looked in %3); "
                                    "the interface stays in English")
                         .arg(catalogNameFor(lang), lang, directories.join(QLatin1Char(';')));
        }
        return false;
    }
    application->installTranslator(ours);
    translators().append(ours);

    // ... then Qt's own, which is what makes the toolkit's dialogs (the rename
    // box, the colour picker, its OK/Cancel) speak the same language as the rest.
    // Optional: a machine without them still gets a fully French application.
    for (const QString& name : {QStringLiteral("qtbase_") + lang, QStringLiteral("qt_") + lang}) {
        auto* qt = new QTranslator(application);
        if (loadFrom(qt, name, directories)) {
            application->installTranslator(qt);
            translators().append(qt);
        } else {
            delete qt;
        }
    }

    qCInfo(log()).noquote() << QStringLiteral("interface language: %1").arg(lang);
    return true;
}

void uninstall()
{
    QCoreApplication* const application = QCoreApplication::instance();
    for (QTranslator* translator : translators()) {
        if (application)
            application->removeTranslator(translator);
        delete translator;
    }
    translators().clear();
    currentLanguage().clear();
}

QString installedLanguage()
{
    return currentLanguage();
}

} // namespace starfish::core::i18n
