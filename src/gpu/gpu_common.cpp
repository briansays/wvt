// GPU code shared by the CUDA and stub builds: option parsing, the
// enabled switch and the startup log line.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "gpu/gpu_backend.h"
#include "common/log.h"

namespace {
std::mutex g_noteMutex;
GpuInfo g_info = [] {
    GpuInfo i;
    i.reason = "not initialized";
    return i;
}();
std::atomic<bool> g_enabled{false};
// One per GPU path. After a failure (not out of memory) the path uses the CPU
// for a while, then tries the GPU again: 1 s after the first failure, doubling
// up to 60 s while the failures go on. A success, gpuInit or switching the GPU
// on again clears it.
constexpr int64_t kRetryFirstMs = 1000, kRetryMaxMs = 60000;
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct PathState {
    explicit PathState(const char* n) : name(n) {}
    const char* name;
    std::atomic<int> failures{0};       // in a row, not counting out of memory
    std::atomic<int64_t> retryAt{0};    // nowMs() from which the GPU is tried again
    std::atomic<bool> oomLogged{false};
    std::string note;  // for gpuFallbackNotes, under g_noteMutex
    bool waiting() const { return failures > 0 && nowMs() < retryAt; }
    void reset() {
        failures = 0;
        retryAt = 0;
        oomLogged = false;
        std::lock_guard<std::mutex> lk(g_noteMutex);
        note.clear();
    }
};
PathState g_spec{"waterfall"}, g_live{"live spectrum"}, g_chan{"channel filter"};

bool forcedFail(std::string& err) {
    const char* f = envVar("WVT_GPU_FORCE_FAIL");
    if (!f || !*f || *f == '0') return false;
    err = "forced by WVT_GPU_FORCE_FAIL";
    return true;
}

// Logs a failed call and applies the fallback rule.
void noteFailure(PathState& p, const std::string& err, bool oom) {
    if (oom) {
        // Smaller jobs may still fit, so the path stays on.
        {
            std::lock_guard<std::mutex> lk(g_noteMutex);
            p.note = std::string(p.name) + ": the GPU ran out of memory; large jobs use the CPU (" + err + ")";
        }
        if (!p.oomLogged.exchange(true))
            LOG_WARN("gpu", "%s: the GPU is out of memory (%s); such jobs use the CPU", p.name, err.c_str());
        else
            LOG_DEBUG("gpu", "%s: out of memory again (%s)", p.name, err.c_str());
        return;
    }
    const int n = ++p.failures;
    const int64_t delay = std::min(kRetryMaxMs, kRetryFirstMs << std::min(n - 1, 6));
    p.retryAt = nowMs() + delay;
    {
        std::lock_guard<std::mutex> lk(g_noteMutex);
        p.note = std::string(p.name) + ": uses the CPU after an error, tries the GPU again in " + std::to_string(delay / 1000) +
                 " s (" + err + ")";
    }
    if (n == 1)
        LOG_WARN("gpu", "%s on the GPU failed: %s; it uses the CPU and tries the GPU again in %lld s", p.name, err.c_str(),
                 (long long)(delay / 1000));
    else
        LOG_DEBUG("gpu", "%s on the GPU failed again (%d in a row): %s; next try in %lld s", p.name, n, err.c_str(),
                  (long long)(delay / 1000));
}

// A call that worked: a path that had failed is back on the GPU.
void noteSuccess(PathState& p) {
    if (p.failures == 0) return;
    const int n = p.failures.exchange(0);
    if (n == 0) return;
    {
        std::lock_guard<std::mutex> lk(g_noteMutex);
        p.note.clear();
    }
    LOG_INFO("gpu", "%s is back on the GPU after %d failed %s", p.name, n, n == 1 ? "try" : "tries");
}

// Applies the result of one GPU call to its path.
void noteResult(PathState& p, GpuStatus st, const std::string& err, bool oom) {
    if (st == GpuStatus::Failed) noteFailure(p, err, oom);
    else if (st == GpuStatus::Ok) noteSuccess(p);
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
    g_chan.reset();
    if (mode == GpuMode::Off) {
        // Off does not touch the driver at all.
        g_info.built = gpuBackendBuilt();
        g_info.backend = gpuBackendName();
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
    LOG_INFO("gpu", "using %s GPU %d: %s, %s", g_info.backend.c_str(), g_info.device, g_info.name.c_str(),
             g_info.version.c_str());
    return true;
}

const GpuInfo& gpuInfo() { return g_info; }

bool gpuEnabled() { return g_enabled.load(); }

void gpuSetEnabled(bool on) {
    on = on && g_info.available;
    if (g_enabled.exchange(on) == on) return;
    LOG_INFO("gpu", "GPU processing %s", on ? "on" : "off");
    if (on) {
        // The user asks for the GPU: paths that failed try it at once.
        g_spec.reset();
        g_live.reset();
        g_chan.reset();
    }
}

std::vector<std::string> gpuListDevices() { return gpuBackendList(); }

GpuLimits gpuLimits() { return gpuBackendLimits(); }

bool gpuSpectrogramUsable() { return gpuEnabled() && !g_spec.waiting() && gpuBackendImplements(GpuJob::Spectrogram); }

GpuStatus gpuSpectrogram(const SpecJob& job, float* lin, const std::function<bool()>& cancelled) {
    if (!gpuSpectrogramUsable()) return GpuStatus::Unavailable;
    std::string err;
    bool oom = false;
    const GpuStatus st = forcedFail(err) ? GpuStatus::Failed : gpuBackendSpectrogram(job, lin, cancelled, err, oom);
    noteResult(g_spec, st, err, oom);
    return st;
}

std::string gpuFallbackNotes() {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    std::string s;
    for (const PathState* p : {&g_spec, &g_live, &g_chan})
        if (!p->note.empty()) s += (s.empty() ? "" : "\n") + p->note;
    return s;
}

bool gpuChannelUsable() { return gpuEnabled() && !g_chan.waiting() && gpuBackendImplements(GpuJob::Channel); }

GpuStatus gpuChannelize(const ChanJob& job, float* y, const std::function<bool()>& cancelled) {
    if (!gpuChannelUsable()) return GpuStatus::Unavailable;
    std::string err;
    bool oom = false;
    const GpuStatus st = forcedFail(err) ? GpuStatus::Failed : gpuBackendChannelize(job, y, cancelled, err, oom);
    noteResult(g_chan, st, err, oom);
    return st;
}

bool gpuLiveUsable() { return gpuEnabled() && !g_live.waiting() && gpuBackendImplements(GpuJob::Live); }

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
    if (st == GpuStatus::Failed && !oom) {
        // Start the next try with a new queue and buffers.
        gpuBackendLiveDestroy(impl_);
        impl_ = nullptr;
    }
    noteResult(g_live, st, err, oom);
    return st;
}
