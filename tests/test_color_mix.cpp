#include "core/color_mix.hpp"

#include <QtTest/QtTest>
#include <QtGui/QColor>

using starfish::core::mixOklab;

class TestColorMix : public QObject
{
    Q_OBJECT

private slots:
    void endpointsAreExact();
    void sameColourIsIdentity();
    void alphaLerpsLinearlyAndSeparately();
    void oklabMidpointIsNotSrgbMidpoint();
    void blackWhiteMidpointIsPerceptualGray();
};

void TestColorMix::endpointsAreExact()
{
    const QColor from(11, 58, 107, 200);
    const QColor to(224, 122, 43, 40);
    QCOMPARE(mixOklab(from, to, 0.0), from);
    QCOMPARE(mixOklab(from, to, -0.5), from);
    QCOMPARE(mixOklab(from, to, 1.0), to);
    QCOMPARE(mixOklab(from, to, 1.5), to);
}

void TestColorMix::sameColourIsIdentity()
{
    const QColor c(40, 80, 120, 90);
    QCOMPARE(mixOklab(c, c, 0.37), c);
}

void TestColorMix::alphaLerpsLinearlyAndSeparately()
{
    // Transparent red toward opaque red: RGB stays red, alpha is halfway.
    const QColor clearRed(255, 0, 0, 0);
    const QColor solidRed(255, 0, 0, 255);
    const QColor mid = mixOklab(clearRed, solidRed, 0.5);
    QCOMPARE(mid.red(), 255);
    QCOMPARE(mid.green(), 0);
    QCOMPARE(mid.blue(), 0);
    QCOMPARE(mid.alpha(), 128);

    // A fade from opaque blue to fully transparent orange must not collapse
    // the midpoint toward black (the sRGB-premul trap).
    const QColor blue(20, 60, 180, 255);
    const QColor clearOrange(230, 120, 30, 0);
    const QColor faded = mixOklab(blue, clearOrange, 0.5);
    QCOMPARE(faded.alpha(), 128);
    QVERIFY(faded.red() > 40);
    QVERIFY(faded.blue() > 40);
}

void TestColorMix::oklabMidpointIsNotSrgbMidpoint()
{
    const QColor blue(11, 58, 107);
    const QColor orange(224, 122, 43);
    const QColor mid = mixOklab(blue, orange, 0.5);
    const QColor srgb(qRound((11 + 224) / 2.0),
                      qRound((58 + 122) / 2.0),
                      qRound((107 + 43) / 2.0));
    QVERIFY(mid != blue);
    QVERIFY(mid != orange);
    QVERIFY(mid != srgb);
    // And it sits between the two hues (not a muddy collapse).
    QVERIFY(mid.red() > blue.red());
    QVERIFY(mid.red() < orange.red());
}

void TestColorMix::blackWhiteMidpointIsPerceptualGray()
{
    const QColor mid = mixOklab(QColor(Qt::black), QColor(Qt::white), 0.5);
    QCOMPARE(mid.red(), mid.green());
    QCOMPARE(mid.green(), mid.blue());
    QCOMPARE(mid.alpha(), 255);
    // OKLab L = 0.5 is a perceptual mid-gray around sRGB 99, not the sRGB
    // 128 midpoint and not the linear-light midpoint (~186).
    QVERIFY(qAbs(mid.red() - 99) <= 2);
    QVERIFY(mid.red() != 128);
}

QTEST_MAIN(TestColorMix)
#include "test_color_mix.moc"
