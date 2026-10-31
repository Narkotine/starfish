#pragma once

#include <QLocale>
#include <QString>
#include <QStringList>
#include <optional>

namespace starfish::core {

// F-036 / T-014b -- the interface speaks at least English and French.
//
// The strings in the code are written in **English**: that is the source
// language, so it needs no catalogue and it is always available. French is the
// translation that ships with the product, and its only artifact is
// `src/i18n/starfish_fr.ts` (compiled to `starfish_fr.qm` next to the
// executable).
//
// Two properties are deliberate:
//
//  * **A launch speaks the system's language.** Nothing is chosen by the
//    application on the user's behalf: a French Windows gets the French
//    interface, anything else gets the source language. `--lang` overrides it,
//    which is what makes the feature measurable -- the diagnostic probes read
//    labels, so they must be able to say which language they expect instead of
//    inheriting the machine's.
//  * **A missing catalogue degrades in the log, never in the window.** If
//    `starfish_fr.qm` is not next to the executable, `install()` reports it and
//    the interface stays English: an application that refuses to start because a
//    translation is missing would be trading a working program for a broken one.
//    The probe checks the catalogue loads, so a build that ships it is verified
//    rather than assumed.
//
// Nothing here is persisted: the language is a property of the *machine* (the
// locale) and of the *invocation* (`--lang`), not of the document -- a layout
// exported to another machine must not carry someone else's language.
namespace i18n {

// The source language: every string in the code is written in it, and it stays
// the fallback for every locale the product does not speak.
inline constexpr const char* kSourceLanguage = "en";

// The languages the product can actually speak. A request for anything else is
// answered with an error, never with a silent fallback: an interface claiming a
// language it does not have is a promise (the rule the fence menu is written
// under, T-076).
QStringList availableLanguages();

// The language's own name for itself, capitalised ("Français", "Deutsch"):
// what the language list in Settings shows.
QString displayName(const QString& language);

// The catalogue file name for a language, without extension: `starfish_fr`.
// Empty for the source language, which has no catalogue.
QString catalogNameFor(const QString& language);

// The language a machine's locale asks for, when a catalogue for it ships;
// anything else gets the source language.
QString fromLocale(const QLocale& locale);
QString fromSystemLocale();

// Resolves what to speak. `requested` is `--lang` (empty = decide from the
// locale); the returned code is always in availableLanguages(). Returns nullopt
// with a message when `requested` names a language the product does not have.
std::optional<QString> resolve(const QString& requested, QString* error = nullptr);

// Installs (or replaces) the translators for `language`, from
// `QLibraryInfo::path(...)` fallback order. Idempotent: installing another
// language drops the previous translators first, which is how the probes check
// two languages in one run. Returns false with a message when the application's
// own catalogue could not be found.
bool install(const QString& language, QString* error = nullptr);

// Removes every translator this module installed.
void uninstall();

// The language currently installed, or an empty string when none is.
QString installedLanguage();

} // namespace i18n

} // namespace starfish::core
