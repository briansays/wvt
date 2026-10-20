// GPU code shared by the CUDA and stub builds: option parsing, the
// enabled switch and the startup log line.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>

#include "gpu_backend.h"
#include "log.h"

namespace {
GpuInfo g_info = [] {
    GpuInfo i;
    i.reason = "not initialized";
    return i;
}();
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_specBroken{false};  // the waterfall path failed: CPU from now on
std::atomic<bool> g_specOomLogged{false};
}  // namespace

bool parseGpuMode(const char* s, GpuMode& out) {
    std::string v(s ? s : "");
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (v == "auto") out = GpuMode::Auto;
    else if (v == "on" || v == "1" || v == "yes" || v == "true") out = GpuMode::On;
    else if (v == "off" || v == "0" || v == "no" || v == "false") out = GpuMode::Off;
    else return false;
    return true;
}

const char* gpuModeName(GpuMode m) {
    switch (m) {
        case GpuMode::Auto: return "auto";
        case GpuMode::On: return "on";
        case GpuMode::Off: return "off";
    }
    return "?";
}

bool gpuInit(GpuMode mode, int device, std::string& err) {
    g_info = GpuInfo{};
    g_enabled = false;
    g_specBroken = false;
    g_specOomLogged = false;
    if (mode == GpuMode::Off) {
        // Off does not touch the driver at all.
        g_info.built = gpuBackendBuilt();
        g_info.reason = "switched off (--gpu off or WVT_GPU=off)";
        LOG_INFO("gpu", "GPU not used: %s", g_info.reason.c_str());
        return true;
    }
    gpuBackendProbe(device, g_info);
    if (!g_info.available) {
        if (mode == GpuMode::On) {
            err = "--gpu on, but no GPU can be used: " + g_info.reason;
            return false;  // the caller logs err
        }
        LOG_INFO("gpu", "GPU not used: %s", g_info.reason.c_str());
        return true;
    }
    g_enabled = true;
    LOG_INFO("gpu", "using GPU %d: %s, %s", g_info.device, g_info.name.c_str(), g_info.version.c_str());
    return true;
}

const GpuInfo& gpuInfo() { return g_info; }

bool gpuEnabled() { return g_enabled.load(); }

void gpuSetEnabled(bool on) {
    on = on && g_info.available;
    if (g_enabled.exchange(on) != on) LOG_INFO("gpu", "GPU processing %s", on ? "on" : "off");
}

std::vector<std::string> gpuListDevices() { return gpuBackendList(); }

bool gpuSpectrogramUsable() { return gpuEnabled() && !g_specBroken; }

GpuStatus gpuSpectrogram(const SpecJob& job, float* lin, const std::function<bool()>& cancelled) {
    if (!gpuSpectrogramUsable()) return GpuStatus::Unavailable;
    std::string err;
    bool oom = false;
    GpuStatus st;
    if (const char* f = getenv("WVT_GPU_FORCE_FAIL"); f && *f && *f != '0') {
        st = GpuStatus::Failed;
        err = "forced by WVT_GPU_FORCE_FAIL";
    } else {
        st = gpuBackendSpectrogram(job, lin, cancelled, err, oom);
    }
    if (st != GpuStatus::Failed) return st;
    if (oom) {
        // Smaller views may still fit, so the path stays on.
        if (!g_specOomLogged.exchange(true))
            LOG_WARN("gpu", "waterfall: the GPU is out of memory (%s); such views use the CPU", err.c_str());
        else
            LOG_DEBUG("gpu", "waterfall: out of memory again (%s)", err.c_str());
    } else if (!g_specBroken.exchange(true)) {
        LOG_WARN("gpu", "waterfall on the GPU failed: %s; the waterfall uses the CPU from now on", err.c_str());
    }
    return st;
}
