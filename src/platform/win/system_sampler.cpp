#include "platform/win/system_sampler.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <vector>

namespace starfish::platform::win {

namespace {

quint64 ticks(const FILETIME& t)
{
    return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

// The largest dedicated memory of a hardware adapter, in bytes (0: unknown).
double dedicatedVideoMemory()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return 0;
    double best = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            best = qMax(best, double(desc.DedicatedVideoMemory));
        adapter->Release();
    }
    factory->Release();
    return best;
}

// Sum of every instance of a wildcard counter; false when it has none.
bool sumCounter(PDH_HCOUNTER counter, double* sum)
{
    DWORD bytes = 0;
    DWORD count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (status != PDH_MORE_DATA || bytes == 0)
        return false;
    std::vector<unsigned char> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items);
    if (status != ERROR_SUCCESS)
        return false;
    double total = 0;
    bool any = false;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
            total += items[i].FmtValue.doubleValue;
            any = true;
        }
    }
    *sum = total;
    return any;
}

} // namespace

struct SystemSampler::Impl {
    quint64 idle = 0, kernel = 0, user = 0;
    bool haveTimes = false;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpu = nullptr;
    PDH_HCOUNTER vram = nullptr;
    bool queryTried = false;
    int collections = 0;
    double vramTotal = -1;

    void openQuery()
    {
        if (queryTried)
            return;
        queryTried = true;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpu) != ERROR_SUCCESS)
            gpu = nullptr;
        if (PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vram) != ERROR_SUCCESS)
            vram = nullptr;
        if (!gpu && !vram) {
            PdhCloseQuery(query);
            query = nullptr;
        }
    }

    void closeQuery()
    {
        if (query)
            PdhCloseQuery(query);
        query = nullptr;
        gpu = vram = nullptr;
        queryTried = false;
        collections = 0;
        haveTimes = false;
    }
};

SystemSampler::SystemSampler()
    : d(new Impl)
{
}

SystemSampler::~SystemSampler()
{
    d->closeQuery();
    delete d;
}

void SystemSampler::close()
{
    d->closeQuery();
}

void SystemSampler::sample(core::MetricPoint out[core::kMetricCount], bool gpu)
{
    for (int i = 0; i < core::kMetricCount; ++i)
        out[i] = core::MetricPoint();

    // CPU: the share of non-idle time since the previous read.
    FILETIME idleT, kernelT, userT;
    if (GetSystemTimes(&idleT, &kernelT, &userT)) {
        const quint64 idle = ticks(idleT), kernel = ticks(kernelT), user = ticks(userT);
        if (d->haveTimes) {
            const quint64 total = (kernel - d->kernel) + (user - d->user); // kernel includes idle
            const quint64 idleDelta = idle - d->idle;
            if (total > 0)
                out[int(core::Metric::Cpu)].percent =
                    float(qBound(0.0, 100.0 * double(total - qMin(idleDelta, total)) / double(total), 100.0));
        }
        d->idle = idle;
        d->kernel = kernel;
        d->user = user;
        d->haveTimes = true;
    }

    // RAM.
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys > 0) {
        core::MetricPoint& ram = out[int(core::Metric::Ram)];
        ram.totalBytes = double(memory.ullTotalPhys);
        ram.usedBytes = double(memory.ullTotalPhys - memory.ullAvailPhys);
        ram.percent = float(100.0 * ram.usedBytes / ram.totalBytes);
    }

    // GPU and VRAM through PDH (absent on some machines and in VMs). The
    // costly part (every GPU engine is a counter instance): only while a GPU
    // or VRAM widget is shown; the query is released otherwise.
    if (!gpu) {
        d->closeQuery();
        return;
    }
    d->openQuery();
    if (d->query && PdhCollectQueryData(d->query) == ERROR_SUCCESS) {
        ++d->collections;
        double value = 0;
        // A rate counter needs two collections before it means anything.
        if (d->gpu && d->collections >= 2 && sumCounter(d->gpu, &value))
            out[int(core::Metric::Gpu)].percent = float(qBound(0.0, value, 100.0));
        if (d->vram && sumCounter(d->vram, &value)) {
            if (d->vramTotal < 0)
                d->vramTotal = dedicatedVideoMemory();
            if (d->vramTotal > 0) {
                core::MetricPoint& v = out[int(core::Metric::Vram)];
                v.totalBytes = d->vramTotal;
                v.usedBytes = qMin(value, d->vramTotal);
                v.percent = float(100.0 * v.usedBytes / v.totalBytes);
            }
        }
    }
}

} // namespace starfish::platform::win
