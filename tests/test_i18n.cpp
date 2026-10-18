// F-036 / T-014b -- the interface language, as a *decision* rather than a
// translation: which language a launch speaks, what a request for a language the
// product does not have does, and the fact that the source language needs no
// catalogue at all.
//
// The compiled catalogue (`starfish_fr.qm`) is deliberately **not** what this
// suite exercises: it is a build artifact, so it is checked where it really
// matters -- next to the executable, by --product-probe, which asks the product's
// own window what language it is speaking. What is unit-tested here is the
// mapping, which is exactly the part that can be wrong without any file.

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>

#include "core/i18n.hpp"

using starfish::core::i18n::availableLanguages;
using starfish::core::i18n::catalogNameFor;
using starfish::core::i18n::fromLocale;
using starfish::core::i18n::fromSystemLocale;
using starfish::core::i18n::install;
using starfish::core::i18n::installedLanguage;
using starfish::core::i18n::resolve;
using starfish::core::i18n::uninstall;

class TestI18n : public QObject
{
    Q_OBJECT

private slots:
    void theProductSpeaksTwoLanguages()
    {
        // F-036: "at least English and French". Adding a third language is a
        // deliberate act (a catalogue to write *and* a line here), so the list is
        // pinned: this is the offer, not an implementation detail.
        QCOMPARE(availableLanguages(), QStringList({QStringLiteral("en"), QStringLiteral("fr")}));
    }

    void theSourceLanguageHasNoCatalogue()
    {
        // English is the language the code is written in: no .qm, nothing to
        // ship, and nothing that can go missing. An empty name is the honest way
        // to say it.
        QVERIFY(catalogNameFor(QStringLiteral("en")).isEmpty());
        QVERIFY(catalogNameFor(QString()).isEmpty());
        QCOMPARE(catalogNameFor(QStringLiteral("fr")), QStringLiteral("starfish_fr"));
    }

    void aFrenchMachineAsksForFrench()
    {
        QCOMPARE(fromLocale(QLocale(QLocale::French, QLocale::France)), QStringLiteral("fr"));
        // Quebec, Switzerland, Belgium: F-036 asks for a language, not a country.
        QCOMPARE(fromLocale(QLocale(QLocale::French, QLocale::Canada)), QStringLiteral("fr"));
    }

    void everyOtherMachineAsksForTheSourceLanguage()
    {
        QCOMPARE(fromLocale(QLocale(QLocale::English, QLocale::UnitedStates)),
                 QStringLiteral("en"));
        // Not French and not English: the fallback is the source language. Silently
        // picking a "close" language would be a promise the product cannot keep.
        QCOMPARE(fromLocale(QLocale(QLocale::German, QLocale::Germany)), QStringLiteral("en"));
        QCOMPARE(fromLocale(QLocale(QLocale::Spanish, QLocale::Spain)), QStringLiteral("en"));
    }

    void anEmptyRequestFollowsTheMachine()
    {
        // What a bare launch does: nothing is chosen on the user's behalf. Asserted
        // against the same call the product makes rather than against a literal,
        // so the case is about the *rule* and not about this machine's locale.
        const std::optional<QString> decided = resolve(QString());
        QVERIFY(decided.has_value());
        QCOMPARE(*decided, fromSystemLocale());
    }

    void aRequestIsAcceptedInTheFormsAUserTypesIt()
    {
        QCOMPARE(*resolve(QStringLiteral("fr")), QStringLiteral("fr"));
        QCOMPARE(*resolve(QStringLiteral("FR")), QStringLiteral("fr"));
        QCOMPARE(*resolve(QStringLiteral("fr_FR")), QStringLiteral("fr"));
        QCOMPARE(*resolve(QStringLiteral("fr-FR")), QStringLiteral("fr"));
        QCOMPARE(*resolve(QStringLiteral("en_US")), QStringLiteral("en"));
    }

    void aLanguageTheProductDoesNotHaveIsRefused()
    {
        // Not silently downgraded to English: an interface that claims German and
        // shows English is a promise (the rule the fence menu is written under).
        QString error;
        const std::optional<QString> decided = resolve(QStringLiteral("de"), &error);
        QVERIFY(!decided.has_value());
        QVERIFY(error.contains(QStringLiteral("de")));
        QVERIFY(error.contains(QStringLiteral("en,fr")));
    }

    void theSourceLanguageInstallsWithNothingAtAll()
    {
        // No application, no .qm, no file to find: asking for English is asking for
        // the strings as they are written. This is also what makes the failure of
        // a *missing* catalogue repairable -- the fallback is a real language.
        QString error;
        QVERIFY2(install(QStringLiteral("en"), &error), qPrintable(error));
        QCOMPARE(installedLanguage(), QStringLiteral("en"));
        uninstall();
        QVERIFY(installedLanguage().isEmpty());
    }

    void aRequestForAnUnknownLanguageFailsBeforeTouchingTheApplication()
    {
        QString error;
        QVERIFY(!install(QStringLiteral("de"), &error));
        QVERIFY(error.contains(QStringLiteral("unknown language")));
        QVERIFY(installedLanguage().isEmpty());
    }

    void theShippedCatalogueResolvesOurOwnStrings()
    {
        // The artifact F-036 promises: `starfish_fr.qm`, built from
        // src/i18n/starfish_fr.ts and placed next to the executable -- which is
        // also this binary's directory, since both live in the build tree.
        //
        // ⚠️ The premise first, and it is the whole point of the case: without the
        // file this test would pass while measuring nothing.
        const QString catalogue =
            QCoreApplication::applicationDirPath() + QStringLiteral("/starfish_fr.qm");
        QVERIFY2(QFileInfo::exists(catalogue), qPrintable(catalogue));

        QString error;
        QVERIFY2(install(QStringLiteral("fr"), &error), qPrintable(error));
        QCOMPARE(installedLanguage(), QStringLiteral("fr"));

        // Two different contexts, and the *qualified* one is the one that matters:
        // moc records a namespaced class as `starfish::ui::SettingsDialog`, and a
        // catalogue whose context says `SettingsDialog` translates nothing at all,
        // silently. `FenceMenu` is the other kind of context -- an explicit name
        // passed to QCoreApplication::translate by a static function.
        QCOMPARE(QCoreApplication::translate("starfish::ui::SettingsDialog", "Close"),
                 QStringLiteral("Fermer"));
        QCOMPARE(QCoreApplication::translate("FenceMenu", "Roll up"), QStringLiteral("Enrouler"));

        // ... and uninstalling really goes back to the source language: that is
        // what makes "no catalogue" a working interface rather than a broken one.
        uninstall();
        QCOMPARE(QCoreApplication::translate("starfish::ui::SettingsDialog", "Close"),
                 QStringLiteral("Close"));
    }

    void aCatalogueThatIsNotThereIsReportedRatherThanGuessed()
    {
        // ⚠️ This case needs the *absence* of a catalogue next to the binary, and
        // the build tree puts one there -- the test shares the product's directory.
        // Skipped rather than faked: the half that can be measured here is the one
        // above, and --product-probe covers the same load from the product's own
        // window. What the skipped half would assert is the *warning* path, which
        // is only reachable in a build that did not ship the catalogue.
        const QString appDir = QCoreApplication::applicationDirPath();
        if (QFileInfo::exists(appDir + QStringLiteral("/starfish_fr.qm")))
            QSKIP("a catalogue is next to this binary: the missing-file path is unreachable here");

        QString error;
        QVERIFY(!install(QStringLiteral("fr"), &error));
        QVERIFY(error.contains(QStringLiteral("starfish_fr")));
        QVERIFY(installedLanguage().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestI18n)

#include "test_i18n.moc"
