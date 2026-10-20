// GPU code shared by the CUDA and stub builds: option parsing, the
// enabled switch and the startup log line.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>

#include "gpu_backend.h"
#include "log.h"

namespace {
std::mutex g_noteMutex;
GpuInfo g_info = [] {
    GpuInfo i;
    i.reason = "not initialized";
    return i;
}();
std::atomic<bool> g_enabled{false};
// One per GPU path: after a failure (not out of memory) the path uses the
// CPU until the next gpuInit.
struct PathState {
    explicit PathState(const char* n) : name(n) {}
    const char* name;
    std::atomic<bool> broken{false}, oomLogged{false};
    std::string note;  // for gpuFallbackNotes, under g_noteMutex
    void reset() {
        broken = false;
        oomLogged = false;
        std::lock_guard<std::mutex> lk(g_noteMutex);
        note.clear();
    }
};
PathState g_spec{"waterfall"}, g_live{"live spectrum"};

bool forcedFail(std::string& err) {
    const char* f = getenv("WVT_GPU_FORCE_FAIL");
    if (!f || !*f || *f == '0') return false;
    err = "forced by WVT_GPU_FORCE_FAIL";
    return true;
}

// Logs a failed call and applies the fallback rule.
void noteFailure(PathState& p, const std::string& err, bool oom) {
    {
        std::lock_guard<std::mutex> lk(g_noteMutex);
        if (!p.broken) p.note = std::string(p.name) + (oom ? ": the GPU ran out of memory; large jobs use the CPU (" : ": uses the CPU after an error (") + err + ")";
    }
    if (oom) {
        // Smaller jobs may still fit, so the path stays on.
        if (!p.oomLogged.exchange(true))
            LOG_WARN("gpu", "%s: the GPU is out of memory (%s); such jobs use the CPU", p.name, err.c_str());
        else
            LOG_DEBUG("gpu", "%s: out of memory again (%s)", p.name, err.c_str());
    } else if (!p.broken.exchange(true)) {
        LOG_WARN("gpu", "%s on the GPU failed: %s; it uses the CPU from now on", p.name, err.c_str());
    }
}
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
    g_spec.reset();
    g_live.reset();
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

bool gpuSpectrogramUsable() { return gpuEnabled() && !g_spec.broken; }

GpuStatus gpuSpectrogram(const SpecJob& job, float* lin, const std::function<bool()>& cancelled) {
    if (!gpuSpectrogramUsable()) return GpuStatus::Unavailable;
    std::string err;
    bool oom = false;
    const GpuStatus st = forcedFail(err) ? GpuStatus::Failed : gpuBackendSpectrogram(job, lin, cancelled, err, oom);
    if (st == GpuStatus::Failed) noteFailure(g_spec, err, oom);
    return st;
}

std::string gpuFallbackNotes() {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    std::string s;
    for (const PathState* p : {&g_spec, &g_live})
        if (!p->note.empty()) s += (s.empty() ? "" : "\n") + p->note;
    return s;
}

bool gpuLiveUsable() { return gpuEnabled() && !g_live.broken; }

GpuLiveSpectrum::GpuLiveSpectrum() = default;
GpuLiveSpectrum::~GpuLiveSpectrum() { gpuBackendLiveDestroy(impl_); }

GpuStatus GpuLiveSpectrum::run(const uint8_t* raw, const FormatParams& fp, int n, int frames, const float* win, float* pw) {
    if (!gpuLiveUsable()) return GpuStatus::Unavailable;
    std::string err;
    bool oom = false;
    GpuStatus st;
    if (forcedFail(err)) {
        st = GpuStatus::Failed;
    } else {
        if (!impl_) impl_ = gpuBackendLiveCreate();
        st = impl_ ? gpuBackendLiveRun(impl_, raw, fp, n, frames, win, pw, err, oom) : GpuStatus::Unavailable;
    }
    if (st == GpuStatus::Failed) noteFailure(g_live, err, oom);
    return st;
}
