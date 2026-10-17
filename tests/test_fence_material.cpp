#include <QtTest>

#include <QColor>
#include <QString>
#include <QStringList>

#include "ui/fence_material.hpp"

// T-074: the schema's default style is where the *document* says what a new fence
// looks like, so the material tests read it from there rather than copying the
// string -- the day the default moves, this is what has to move with it.
#include "core/jsonstore.hpp"

using namespace starfish::ui;

// T-057 (F-038): the pure half of the fence materials. What it has to get right
// is not the alpha arithmetic but two promises the rest of the product leans on:
//
//  * a preset name means **one** alpha, everywhere (the settings window, the tray
//    and the probes all read it from here), and
//  * applying a preset rewrites **only** the alpha of the document's colour, so
//    the string the user or an older document wrote survives byte for byte --
//    which is what keeps `--persist-demo`'s "identical to the byte" comparison
//    honest and avoids a schema change (ADR-7).
//
// The last section checks the parser against QColor itself: the schema's colour
// form is defined by what the toolkit reads, not by what we believe about it.
class TestFenceMaterial : public QObject
{
    Q_OBJECT

private slots:
    void thePresetsAreTheThreeOfF038()
    {
        const QStringList ids = FenceMaterial::presetIds();
        QCOMPARE(ids.size(), 3);
        QCOMPARE(ids.at(0), QStringLiteral("opaque"));
        QCOMPARE(ids.at(1), QStringLiteral("translucent"));
        QCOMPARE(ids.at(2), QStringLiteral("very-translucent"));
        // The first preset is the opaque one on purpose: it is what a fence looks
        // like today, so an interface that defaults to the first entry is not a
        // change of behaviour.
        QCOMPARE(FenceMaterial::alphaFor(ids.at(0)), 255);
        QCOMPARE(FenceMaterial::alphaFor(ids.at(1)), 204);
        QCOMPARE(FenceMaterial::alphaFor(ids.at(2)), 128);
    }

    void anUnknownPresetHasNoAlpha()
    {
        QCOMPARE(FenceMaterial::alphaFor(QStringLiteral("acrylic")), -1);
        QCOMPARE(FenceMaterial::alphaFor(QString()), -1);
        QCOMPARE(FenceMaterial::alphaFor(QStringLiteral("Opaque")), -1); // exact, not folded
    }

    void everyPresetAlphaMapsBackToItsName()
    {
        QCOMPARE(FenceMaterial::presetFor(255), QStringLiteral("opaque"));
        QCOMPARE(FenceMaterial::presetFor(204), QStringLiteral("translucent"));
        QCOMPARE(FenceMaterial::presetFor(128), QStringLiteral("very-translucent"));
    }

    void aCustomAlphaIsNotAPreset()
    {
        // A document may hold any alpha at all; the interface has to say so
        // instead of rounding it to the nearest preset and lying.
        QCOMPARE(FenceMaterial::presetFor(200), QString());
        QCOMPARE(FenceMaterial::presetFor(0), QString());
    }

    void theSchemaDefaultIsTheOpaquePreset()
    {
        // T-057's reason to exist: the default FenceStyle has carried an alpha
        // since T-008, so the colour a document already holds *is* a material --
        // which is why no preset ever needed a schema field.
        //
        // T-074 moved that default: a *new* fence is opaque ("opaque by default,
        // not transparent, for now"). Nothing migrates, because a material is the
        // alpha of the colour each document row was saved with.
        QCOMPARE(FenceMaterial::materialOf(starfish::core::FenceStyle().background),
                 QStringLiteral("opaque"));
        QCOMPARE(FenceMaterial::alphaOf(starfish::core::FenceStyle().background), 255);
        // The preset that used to be the default is still a preset -- which is
        // what keeps a document written before T-074 readable and unchanged.
        QCOMPARE(FenceMaterial::materialOf(QStringLiteral("#CC1F2937")),
                 QStringLiteral("translucent"));
    }

    void translucencyOnlyClassifiesTheMaterial()
    {
        // T-075: this predicate no longer picks the window's path. Every fence is
        // pushed per-pixel -- that is the only way a rounded panel can have
        // transparent pixels outside it -- and this says what the material *is*.
        // The consequence is asserted where it belongs, in `--fence-demo`: the
        // frame of an opaque fence has nothing painted outside its rounded shape.
        QVERIFY(!FenceMaterial::isTranslucent(255));
        QVERIFY(FenceMaterial::isTranslucent(254));
        QVERIFY(FenceMaterial::isTranslucent(204));
        QVERIFY(FenceMaterial::isTranslucent(128));
        QVERIFY(FenceMaterial::isTranslucent(0));
    }

    void applyingAPresetRewritesOnlyTheAlpha()
    {
        QCOMPARE(FenceMaterial::withAlpha(QStringLiteral("#CC1F2937"), 128),
                 QStringLiteral("#801F2937"));
        QCOMPARE(FenceMaterial::withAlpha(QStringLiteral("#CC1F2937"), 255),
                 QStringLiteral("#FF1F2937"));
    }

    void theRgbDigitsAreCopiedVerbatim()
    {
        // Lowercase in, lowercase out: the material is ours, the rest of the
        // string belongs to whoever wrote it.
        QCOMPARE(FenceMaterial::withAlpha(QStringLiteral("#1f2937"), 204),
                 QStringLiteral("#CC1f2937"));
        QCOMPARE(FenceMaterial::withAlpha(QStringLiteral("#cc1F2937"), 128),
                 QStringLiteral("#801F2937"));
    }

    void applyingAPresetIsIdempotent()
    {
        const QString once = FenceMaterial::withAlpha(QStringLiteral("#1f2937"), 128);
        const QString twice = FenceMaterial::withAlpha(once, 128);
        QCOMPARE(twice, once);
        QCOMPARE(twice, QStringLiteral("#801f2937"));
    }

    void everyPresetSurvivesARoundTripThroughTheDocumentForm()
    {
        const QString start = QStringLiteral("#1F2937");
        for (const FenceMaterial::Preset& preset : FenceMaterial::presets()) {
            const QString stored =
                FenceMaterial::withAlpha(start, preset.alpha);
            QVERIFY2(FenceMaterial::alphaOf(stored) == preset.alpha,
                     qPrintable(QStringLiteral("%1 stored as %2")
                                    .arg(QString::fromLatin1(preset.id), stored)));
            QVERIFY2(FenceMaterial::materialOf(stored)
                         == QString::fromLatin1(preset.id),
                     qPrintable(stored));
        }
    }

    void aPlainRgbColourReadsAsOpaque()
    {
        QCOMPARE(FenceMaterial::alphaOf(QStringLiteral("#1F2937")), 255);
        QCOMPARE(FenceMaterial::alphaOf(QStringLiteral("1F2937")), 255);
        QCOMPARE(FenceMaterial::materialOf(QStringLiteral("#1F2937")),
                 QStringLiteral("opaque"));
    }

    void aColourThatCannotBeReadIsLeftAlone()
    {
        // Not a material, not a rewrite: a document holding something this parser
        // does not understand keeps it, and the fence stays opaque -- exactly the
        // behaviour of every document written before T-057.
        const QString odd = QStringLiteral("not-a-colour");
        QCOMPARE(FenceMaterial::withAlpha(odd, 128), odd);
        QCOMPARE(FenceMaterial::alphaOf(odd), 255);
        QCOMPARE(FenceMaterial::materialOf(odd), QStringLiteral("opaque"));
        // ... and the same for a colour spelled shorter than #RRGGBB.
        QCOMPARE(FenceMaterial::withAlpha(QStringLiteral("#abc"), 128),
                 QStringLiteral("#abc"));
    }

    void anOutOfRangeAlphaIsRefusedRatherThanWrapped()
    {
        const QString colour = QStringLiteral("#CC1F2937");
        QCOMPARE(FenceMaterial::withAlpha(colour, -1), colour);
        QCOMPARE(FenceMaterial::withAlpha(colour, 256), colour);
    }

    void theParserAgreesWithQColor()
    {
        // The form the schema uses is defined by the toolkit that has to read it
        // back (jsonstore.hpp: "exactly what QColor::name(QColor::HexArgb)
        // produces"), so the premise is checked rather than assumed: what we
        // write is what QColor reads, alpha included.
        for (const FenceMaterial::Preset& preset : FenceMaterial::presets()) {
            const QString stored =
                FenceMaterial::withAlpha(QStringLiteral("#CC1F2937"), preset.alpha);
            const QColor colour(stored);
            QVERIFY2(colour.isValid(), qPrintable(stored));
            QCOMPARE(colour.alpha(), preset.alpha);
            QCOMPARE(colour.red(), 0x1F);
            QCOMPARE(colour.green(), 0x29);
            QCOMPARE(colour.blue(), 0x37);
        }
        // And the schema's default style really is a spelling QColor round-trips
        // (T-074 moved the default, so this is read from the default rather than
        // typed here): a material change never moves the document to another
        // spelling of the same colour.
        const QString defaultBackground = starfish::core::FenceStyle().background;
        const QColor defaultStyle(defaultBackground);
        QVERIFY2(defaultStyle.isValid(), qPrintable(defaultBackground));
        QCOMPARE(defaultStyle.name(QColor::HexArgb).toUpper(), defaultBackground.toUpper());
    }

    void everyPresetHasItsOwnLabel()
    {
        QStringList labels;
        for (const QString& id : FenceMaterial::presetIds()) {
            const QString label = FenceMaterial::presetLabel(id);
            QVERIFY2(!label.isEmpty(), qPrintable(id));
            QVERIFY2(!labels.contains(label), qPrintable(label));
            labels << label;
        }
        QCOMPARE(labels.size(), 3);
        // An alpha that is nobody's preset says so rather than naming one.
        QCOMPARE(FenceMaterial::presetLabel(QString()), QStringLiteral("Custom"));
    }

    void theSummaryNamesEveryPresetWithItsAlpha()
    {
        const QString text = FenceMaterial::summary();
        for (const FenceMaterial::Preset& preset : FenceMaterial::presets()) {
            QVERIFY2(text.contains(QString::fromLatin1(preset.id)), qPrintable(text));
            QVERIFY2(text.contains(QString::number(preset.alpha)), qPrintable(text));
        }
    }
};

QTEST_MAIN(TestFenceMaterial)
#include "test_fence_material.moc"
