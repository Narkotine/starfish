#include "core/system_metrics.hpp"

#include <QCoreApplication>
#include <QtMath>

namespace starfish::core {

namespace {
const char* const kTypes[kMetricCount] = {"cpu", "ram", "gpu", "vram"};
} // namespace

QString metricType(Metric metric)
{
    return QString::fromLatin1(kTypes[int(metric)]);
}

bool metricFromType(const QString& type, Metric* metric)
{
    for (int i = 0; i < kMetricCount; ++i) {
        if (type == QLatin1String(kTypes[i])) {
            if (metric)
                *metric = Metric(i);
            return true;
        }
    }
    return false;
}

bool hasBytes(Metric metric)
{
    return metric == Metric::Ram || metric == Metric::Vram;
}

QString metricShortName(Metric metric)
{
    switch (metric) {
    case Metric::Cpu: return QStringLiteral("CPU");
    case Metric::Ram: return QStringLiteral("RAM");
    case Metric::Gpu: return QStringLiteral("GPU");
    case Metric::Vram: return QStringLiteral("VRAM");
    }
    return {};
}

MetricHistory::MetricHistory(int capacity)
    : m_ring(qMax(1, capacity))
{
}

void MetricHistory::push(const MetricPoint& point)
{
    m_ring[m_next] = point;
    m_next = (m_next + 1) % m_ring.size();
    m_size = qMin(m_size + 1, int(m_ring.size()));
}

MetricPoint MetricHistory::latest() const
{
    if (m_size == 0)
        return {};
    return m_ring.at((m_next - 1 + m_ring.size()) % m_ring.size());
}

QVector<MetricPoint> MetricHistory::last(int count) const
{
    const int n = qBound(0, count, m_size);
    QVector<MetricPoint> out;
    out.reserve(n);
    const int cap = int(m_ring.size());
    for (int i = n; i >= 1; --i)
        out.append(m_ring.at((m_next - i + cap) % cap));
    return out;
}

float shownPercent(const MetricPoint& point, bool free)
{
    if (!point.valid())
        return -1.0f;
    const float used = qBound(0.0f, point.percent, 100.0f);
    return free ? 100.0f - used : used;
}

QVector<float> downsample(const QVector<MetricPoint>& points, int step, int count, bool free)
{
    step = qMax(1, step);
    QVector<float> out;
    int end = int(points.size());
    while (end > 0 && out.size() < count) {
        const int begin = qMax(0, end - step);
        float sum = 0;
        int n = 0;
        for (int i = begin; i < end; ++i) {
            const float v = shownPercent(points.at(i), free);
            if (v >= 0) {
                sum += v;
                ++n;
            }
        }
        out.prepend(n > 0 ? sum / n : -1.0f);
        end = begin;
    }
    return out;
}

MetricSettings MetricSettings::fromMap(const QVariantMap& map, const QString& type)
{
    MetricSettings s;
    Metric single;
    const bool isSingle = metricFromType(type, &single);
    s.showFree = map.value(QStringLiteral("show")).toString() == QLatin1String("free");
    const QString mode = map.value(QStringLiteral("mode")).toString();
    s.mode = mode == QLatin1String("number") ? Mode::Number
             : mode == QLatin1String("graph") ? Mode::Graph
                                              : Mode::Bar;
    s.bytes = (!isSingle || hasBytes(single)) && map.value(QStringLiteral("unit"), (isSingle && hasBytes(single)) ? QStringLiteral("gb") : QStringLiteral("percent")).toString() == QLatin1String("gb");
    s.history = qBound(10, map.value(QStringLiteral("history"), 60).toInt(), 600);
    s.interval = qBound(1, map.value(QStringLiteral("interval"), 1).toInt(), 5);
    if (isSingle) {
        s.metrics = {single};
    } else {
        const QString list = map.value(QStringLiteral("metrics"), QStringLiteral("cpu,ram,gpu,vram")).toString();
        for (const QString& name : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            Metric m;
            if (metricFromType(name.trimmed(), &m) && !s.metrics.contains(m))
                s.metrics.append(m);
        }
        if (s.metrics.isEmpty()) // nothing readable: all of them rather than an empty card
            s.metrics = {Metric::Cpu, Metric::Ram, Metric::Gpu, Metric::Vram};
    }
    return s;
}

void MetricSettings::writeTo(QVariantMap& map, const QString& type) const
{
    map.insert(QStringLiteral("show"), showFree ? QStringLiteral("free") : QStringLiteral("used"));
    map.insert(QStringLiteral("mode"), mode == Mode::Number ? QStringLiteral("number")
                                       : mode == Mode::Graph ? QStringLiteral("graph")
                                                             : QStringLiteral("bar"));
    map.insert(QStringLiteral("history"), qBound(10, history, 600));
    map.insert(QStringLiteral("interval"), qBound(1, interval, 5));
    Metric single;
    if (metricFromType(type, &single)) {
        if (hasBytes(single))
            map.insert(QStringLiteral("unit"), bytes ? QStringLiteral("gb") : QStringLiteral("percent"));
    } else {
        map.insert(QStringLiteral("unit"), bytes ? QStringLiteral("gb") : QStringLiteral("percent"));
        QStringList names;
        for (Metric m : metrics)
            names << metricType(m);
        map.insert(QStringLiteral("metrics"), names.join(QLatin1Char(',')));
    }
}

QVariantMap MetricSettings::defaults(const QString& type)
{
    MetricSettings s;
    Metric single;
    if (metricFromType(type, &single))
        s.bytes = hasBytes(single); // RAM / VRAM read best in GB
    else
        s.metrics = {Metric::Cpu, Metric::Ram, Metric::Gpu, Metric::Vram};
    QVariantMap map;
    s.writeTo(map, type);
    return map;
}

QString formatMetric(Metric metric, const MetricPoint& point, bool free, bool bytes, const QLocale& locale)
{
    if (!point.valid())
        return QCoreApplication::translate("SystemMetrics", "n/a");
    if (bytes && hasBytes(metric) && point.totalBytes > 0) {
        const double gb = (free ? point.totalBytes - point.usedBytes : point.usedBytes) / (1024.0 * 1024.0 * 1024.0);
        return QCoreApplication::translate("SystemMetrics", "%1 GB").arg(locale.toString(qMax(0.0, gb), 'f', 1));
    }
    return QStringLiteral("%1 %").arg(locale.toString(qRound(shownPercent(point, free))));
}

} // namespace starfish::core
