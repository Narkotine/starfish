#include "ui/system_monitor.hpp"

#include "platform/win/system_sampler.hpp"
#include "ui/fence_widgets.hpp"

namespace starfish::ui {

SystemMonitor& SystemMonitor::instance()
{
    static SystemMonitor* monitor = new SystemMonitor; // lives as long as the app
    return *monitor;
}

void SystemMonitor::install()
{
    widgets::setMetricsSource(
        [](core::Metric metric) { return instance().history(metric); },
        [](bool gpu) { instance().demand(gpu); });
}

SystemMonitor::SystemMonitor()
{
    m_timer.setInterval(1000);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SystemMonitor::tick);
}

SystemMonitor::~SystemMonitor() = default;

void SystemMonitor::demand(bool gpu)
{
    m_sinceDemand.start();
    if (gpu)
        m_sinceGpuDemand.start();
    if (m_timer.isActive())
        return;
    if (!m_sampler)
        m_sampler = std::make_unique<platform::win::SystemSampler>();
    for (auto& history : m_history)
        if (!history)
            history = std::make_unique<core::MetricHistory>(kCapacity);
    tick(); // a first reading now (CPU and GPU need a second one)
    m_timer.start();
}

const core::MetricHistory* SystemMonitor::history(core::Metric metric) const
{
    return m_history[int(metric)].get();
}

void SystemMonitor::tick()
{
    if (m_timer.isActive() && m_sinceDemand.isValid() && m_sinceDemand.elapsed() > 3000) {
        // Nobody painted a system widget for 3 s: stop, release PDH.
        m_timer.stop();
        if (m_sampler)
            m_sampler->close();
        return;
    }
    core::MetricPoint points[core::kMetricCount];
    const bool gpu = m_sinceGpuDemand.isValid() && m_sinceGpuDemand.elapsed() <= 3000;
    m_sampler->sample(points, gpu);
    for (int i = 0; i < core::kMetricCount; ++i)
        m_history[i]->push(points[i]);
    ++m_samples;
}

} // namespace starfish::ui
