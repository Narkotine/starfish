#pragma once

#include <QLocale>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace starfish::core {

// Session 88: the system widgets' data -- pure (no Win32, no widgets), so the
// formatting, the history and the settings are unit-tested.

enum class Metric { Cpu = 0, Ram = 1, Gpu = 2, Vram = 3 };
constexpr int kMetricCount = 4;

// The widget type that shows one metric ("cpu", "ram", "gpu", "vram"), and the
// compact group ("system").
QString metricType(Metric metric);
bool metricFromType(const QString& type, Metric* metric);
inline bool isSystemWidgetType(const QString& type)
{
    Metric m;
    return metricFromType(type, &m) || type == QLatin1String("system");
}
// Whether the metric has a size in bytes (RAM, VRAM) besides its percentage.
bool hasBytes(Metric metric);

// One reading. `percent` < 0: unknown (no counter, first CPU read, ...).
struct MetricPoint {
    float percent = -1.0f;
    double usedBytes = 0;  // RAM / VRAM only
    double totalBytes = 0; // RAM / VRAM only
    bool valid() const { return percent >= 0.0f; }
};

// A fixed-size ring of readings, oldest dropped first.
class MetricHistory {
public:
    explicit MetricHistory(int capacity = 3600);
    void push(const MetricPoint& point);
    int size() const { return m_size; }
    int capacity() const { return int(m_ring.size()); }
    MetricPoint latest() const;
    // The last `count` readings, oldest first (fewer when there are fewer).
    QVector<MetricPoint> last(int count) const;
    void clear() { m_size = 0; m_next = 0; }

private:
    QVector<MetricPoint> m_ring;
    int m_next = 0;
    int m_size = 0;
};

// `points` (oldest first, one per sampler tick) grouped by `step` from the
// newest end and averaged, at most `count` values, oldest first. Unknown
// readings are skipped; a group with none of them is -1.
QVector<float> downsample(const QVector<MetricPoint>& points, int step, int count, bool free);

// What a system widget is set to show (its "settings" in layout.json).
struct MetricSettings {
    enum class Mode { Number, Bar, Graph };
    bool showFree = false;      // "show": "used" | "free"
    Mode mode = Mode::Bar;      // "mode": "number" | "bar" | "graph"
    bool bytes = false;         // "unit": "percent" | "gb" (RAM / VRAM; the group: its RAM / VRAM rows)
    int history = 60;           // "history": samples drawn by the graph
    int interval = 1;           // "interval": seconds per graph point (1-5)
    QList<Metric> metrics;      // "metrics": the group's, e.g. "cpu,ram"

    static MetricSettings fromMap(const QVariantMap& map, const QString& type);
    // Writes the keys above into `map`, leaving every other key untouched.
    void writeTo(QVariantMap& map, const QString& type) const;
    static QVariantMap defaults(const QString& type);
};

// "42 %", "6.3 GB", "n/a" -- used or free.
QString formatMetric(Metric metric, const MetricPoint& point, bool free, bool bytes, const QLocale& locale);
// The percentage drawn (used, or 100 - used), -1 when unknown.
float shownPercent(const MetricPoint& point, bool free);
// "CPU", "RAM", "GPU", "VRAM" (not translated: they read the same).
QString metricShortName(Metric metric);

} // namespace starfish::core
