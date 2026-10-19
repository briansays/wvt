// GPU code shared by the CUDA and stub builds: option parsing, the
// enabled switch and the startup log line.
#include <algorithm>
#include <atomic>
#include <cctype>

#include "gpu_backend.h"
#include "log.h"

namespace {
GpuInfo g_info = [] {
    GpuInfo i;
    i.reason = "not initialized";
    return i;
}();
std::atomic<bool> g_enabled{false};
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
