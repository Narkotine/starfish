#pragma once

#include "core/system_metrics.hpp"

namespace starfish::platform::win {

// Session 88: one read of the machine, through documented Win32 only:
//   CPU   GetSystemTimes (delta between two reads)
//   RAM   GlobalMemoryStatusEx
//   GPU   PDH "\GPU Engine(*engtype_3D)\Utilization Percentage" (summed)
//   VRAM  PDH "\GPU Adapter Memory(*)\Dedicated Usage" / DXGI adapter size
// No thread, no process: the caller ticks it (once a second). A counter the
// machine does not have reads as unknown ("n/a"), never as an error.
class SystemSampler {
public:
    SystemSampler();
    ~SystemSampler();
    SystemSampler(const SystemSampler&) = delete;
    SystemSampler& operator=(const SystemSampler&) = delete;

    // Reads all four metrics into `out[Metric]`.
    // `gpu` false: CPU and RAM only (no PDH query, GPU / VRAM left unknown).
    void sample(core::MetricPoint out[core::kMetricCount], bool gpu = true);
    // Releases the PDH query (reopened by the next sample).
    void close();

private:
    struct Impl;
    Impl* d;
};

} // namespace starfish::platform::win
