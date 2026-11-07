#include <QtTest>

#include <QImage>
#include <QPainter>

#include "core/desktop_item.hpp"
#include "core/system_metrics.hpp"
#include "ui/fence_widgets.hpp"

using namespace starfish::core;
using namespace starfish::ui;

// Session 88: the system widgets' pure half -- history ring, downsampling,
// formatting, settings round-trip, and a paint with a fake source.
class TestSystemMetrics : public QObject {
    Q_OBJECT
private slots:
    void historyRing()
    {
        MetricHistory h(3);
        QCOMPARE(h.size(), 0);
        QVERIFY(!h.latest().valid());
        for (int i = 1; i <= 5; ++i) {
            MetricPoint p;
            p.percent = float(i * 10);
            h.push(p);
        }
        QCOMPARE(h.size(), 3);
        QCOMPARE(h.latest().percent, 50.0f);
        const QVector<MetricPoint> last = h.last(10);
        QCOMPARE(last.size(), 3);
        QCOMPARE(last.first().percent, 30.0f);
        QCOMPARE(last.last().percent, 50.0f);
        QCOMPARE(h.last(2).first().percent, 40.0f);
        h.clear();
        QCOMPARE(h.size(), 0);
    }

    void downsampling()
    {
        QVector<MetricPoint> points;
        for (int i = 0; i < 6; ++i) {
            MetricPoint p;
            p.percent = float(i * 10); // 0..50
            points << p;
        }
        points[0].percent = -1; // unknown
        const QVector<float> one = downsample(points, 1, 3, false);
        QCOMPARE(one.size(), 3);
        QCOMPARE(one.last(), 50.0f);
        const QVector<float> two = downsample(points, 2, 10, false);
        QCOMPARE(two.size(), 3);
        QCOMPARE(two.last(), 45.0f);
        QCOMPARE(two.first(), 10.0f); // the unknown reading skipped
        const QVector<float> free = downsample(points, 1, 1, true);
        QCOMPARE(free.last(), 50.0f);
        QVector<MetricPoint> unknown(2);
        QCOMPARE(downsample(unknown, 2, 1, false).value(0), -1.0f);
    }

    void formatting()
    {
        const QLocale c(QLocale::C);
        MetricPoint p;
        QCOMPARE(formatMetric(Metric::Cpu, p, false, false, c), QStringLiteral("n/a"));
        p.percent = 42.4f;
        QCOMPARE(formatMetric(Metric::Cpu, p, false, false, c), QStringLiteral("42 %"));
        QCOMPARE(formatMetric(Metric::Cpu, p, true, false, c), QStringLiteral("58 %"));
        p.percent = 25.0f;
        p.usedBytes = 4.0 * 1024 * 1024 * 1024;
        p.totalBytes = 16.0 * 1024 * 1024 * 1024;
        QVERIFY(formatMetric(Metric::Ram, p, false, true, c).startsWith(QStringLiteral("4")));
        QVERIFY(formatMetric(Metric::Ram, p, true, true, c).startsWith(QStringLiteral("12")));
        QVERIFY(formatMetric(Metric::Ram, p, false, true, c).contains(QStringLiteral("GB")));
        QCOMPARE(shownPercent(p, true), 75.0f);
        QCOMPARE(shownPercent(MetricPoint(), false), -1.0f);
    }

    void settingsRoundTrip()
    {
        QVariantMap map;
        map.insert(QStringLiteral("future"), QStringLiteral("kept"));
        MetricSettings s = MetricSettings::fromMap(map, QStringLiteral("ram"));
        QCOMPARE(s.history, 60);
        QCOMPARE(s.interval, 1);
        QVERIFY(s.bytes); // RAM defaults to GB
        QVERIFY(!MetricSettings::fromMap({}, QStringLiteral("cpu")).bytes);
        s.showFree = true;
        s.mode = MetricSettings::Mode::Graph;
        s.history = 9999; // clamped
        s.interval = 3;
        s.writeTo(map, QStringLiteral("ram"));
        QCOMPARE(map.value(QStringLiteral("future")).toString(), QStringLiteral("kept"));
        const MetricSettings back = MetricSettings::fromMap(map, QStringLiteral("ram"));
        QVERIFY(back.showFree);
        QCOMPARE(back.mode, MetricSettings::Mode::Graph);
        QCOMPARE(back.history, 600);
        QCOMPARE(back.interval, 3);

        QVariantMap group;
        MetricSettings g = MetricSettings::fromMap(group, QStringLiteral("system"));
        QCOMPARE(g.metrics.size(), kMetricCount);
        g.metrics = {Metric::Gpu, Metric::Cpu};
        g.writeTo(group, QStringLiteral("system"));
        QCOMPARE(MetricSettings::fromMap(group, QStringLiteral("system")).metrics,
                 (QList<Metric>{Metric::Gpu, Metric::Cpu}));
        QVariantMap bad;
        bad.insert(QStringLiteral("metrics"), QStringLiteral("bogus"));
        QVERIFY(!MetricSettings::fromMap(bad, QStringLiteral("system")).metrics.isEmpty());
    }

    void kindsAndPaint()
    {
        QStringList types;
        for (const auto& kind : widgets::kinds())
            types << kind.type;
        for (const char* t : {"cpu", "ram", "gpu", "vram", "system"})
            QVERIFY2(types.contains(QLatin1String(t)), t);

        static MetricHistory fake(100);
        for (int i = 0; i < 100; ++i) {
            MetricPoint p;
            p.percent = float(i % 100);
            fake.push(p);
        }
        int demands = 0;
        widgets::setMetricsSource(
            [](Metric m) -> const MetricHistory* { return m == Metric::Gpu ? nullptr : &fake; },
            [&demands](bool) { ++demands; });
        for (const char* t : {"cpu", "gpu", "system"}) {
            for (const char* mode : {"number", "bar", "graph"}) {
                DesktopItem item = widgets::create(QLatin1String(t));
                item.widgetSettings.insert(QStringLiteral("mode"), QLatin1String(mode));
                QImage image(240, 160, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                widgets::PaintContext context;
                context.text = Qt::white;
                widgets::paint(painter, image.rect(), item, context);
                painter.end();
                bool drawn = false;
                for (int y = 0; y < image.height() && !drawn; ++y)
                    for (int x = 0; x < image.width() && !drawn; ++x)
                        drawn = qAlpha(image.pixel(x, y)) > 0;
                QVERIFY2(drawn, t);
                QVERIFY(widgets::msUntilNextChange(item, QDateTime::currentDateTime()) > 0);
            }
        }
        QVERIFY(demands > 0);
        QCOMPARE(widgets::metricText(widgets::create(QStringLiteral("cpu")), Metric::Cpu), QStringLiteral("99 %"));
        QCOMPARE(widgets::metricText(widgets::create(QStringLiteral("gpu")), Metric::Gpu), QStringLiteral("n/a"));
        widgets::setMetricsSource({}, {});
    }
};

QTEST_MAIN(TestSystemMetrics)
#include "test_system_metrics.moc"
