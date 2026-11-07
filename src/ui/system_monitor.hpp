#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>

#include "core/system_metrics.hpp"

namespace starfish::platform::win {
class SystemSampler;
}

namespace starfish::ui {

// Session 88: the one sampler every system widget reads. It runs only while a
// fence painting a system widget keeps asking for it (demand(), called from
// the widget's repaint schedule): a rolled-up or hidden fence stops painting,
// stops asking, and three seconds later the timer stops and the PDH query is
// closed. One tick a second, whatever the number of widgets.
class SystemMonitor : public QObject {
    Q_OBJECT
public:
    static SystemMonitor& instance();
    // Plugs the monitor into the widgets' painter (ui::widgets).
    static void install();

    void demand(bool gpu = true); // gpu: a GPU / VRAM reading is shown
    bool running() const { return m_timer.isActive(); }
    const core::MetricHistory* history(core::Metric metric) const;
    int samples() const { return m_samples; }

    static constexpr int kCapacity = 3000; // 600 graph points x 5 s

private:
    SystemMonitor();
    ~SystemMonitor() override;
    void tick();

    QTimer m_timer;
    QElapsedTimer m_sinceDemand;
    QElapsedTimer m_sinceGpuDemand;
    std::unique_ptr<platform::win::SystemSampler> m_sampler;
    std::unique_ptr<core::MetricHistory> m_history[core::kMetricCount];
    int m_samples = 0;
};

} // namespace starfish::ui
