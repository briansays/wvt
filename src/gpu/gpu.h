#pragma once
// Optional GPU acceleration: CUDA on NVIDIA (issue #3), Metal on Apple
// (issue #21). Plain C++: no CUDA or Metal headers. In a build without a GPU
// backend every call reports "not available", and callers use their CPU
// code. See docs/design/cuda-gpu-plan.md and docs/design/apple-gpu-plan.md.
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/progress.h"
#include "io/iq_file.h"

enum class GpuMode : int { Auto = 0, On, Off };

// "auto", "on", "off" (also 1/0, yes/no, true/false), any case.
bool parseGpuMode(const char* s, GpuMode& out);
const char* gpuModeName(GpuMode m);

struct GpuInfo {
    bool built = false;      // the build has a GPU backend
    std::string backend;     // "CUDA" or "Metal" (empty without a backend)
    bool available = false;  // a usable GPU was found
    int device = -1;
    std::string name;     // "NVIDIA GeForce RTX 4070 (compute 8.9, 12.0 GB)"
    std::string model;    // "GeForce RTX 4070" (for the status bar)
    std::string version;  // "CUDA runtime 12.4, driver 12.6"
    std::string reason;   // why not available: "built without GPU support", "no NVIDIA driver", ...
};

// Once, from main(). device < 0 picks the best usable GPU. Auto and Off
// always succeed; On fails (err says why) when no GPU is usable. Logs the
// result as INFO under the "gpu" component.
bool gpuInit(GpuMode mode, int device, std::string& err);
const GpuInfo& gpuInfo();
bool gpuEnabled();            // available and not switched off
void gpuSetEnabled(bool on);  // UI switch; no effect when not available

// A spectrogram job: `rows` rows of `avg` FFTs of size n (a power of two).
// FFT a of row r starts at specFftStart(job, r, a); samples outside the file
// are zero. Each row's powers are summed over its FFTs, fftshifted, scaled by
// norm / avg and max-pooled to `cols` columns. The file waterfall runs it on
// the CPU (spectrogramCpu in engine.h) or on the GPU (gpuSpectrogram).
struct SpecJob {
    const IqFile* file = nullptr;
    FormatParams fp;
    double s0 = 0, hop = 1;  // first row start and row spacing, in samples
    int rows = 0, avg = 1, n = 0, cols = 0;
    const float* win = nullptr;  // n window values
    float norm = 1;
    Progress* progress = nullptr;  // gets the FFTs done (issue #75); may be null
};
inline int64_t specFftStart(const SpecJob& j, int row, int a) {
    const double center = j.s0 + row * j.hop + (a + 0.5) * j.hop / j.avg;
    return (int64_t)std::llround(center) - j.n / 2;
}

enum class GpuStatus : int { Ok = 0, Cancelled, Unavailable, Failed };

// Runs the job on the GPU and writes rows * cols linear powers to lin.
// Unavailable: no GPU, switched off, or this path failed earlier in the
// session. Failed: a CUDA error (logged as WARN); the caller then uses the
// CPU. After a failure other than "out of memory", it returns Unavailable
// for the rest of the session. Setting WVT_GPU_FORCE_FAIL=1 makes every call
// fail, to test the fallback.
GpuStatus gpuSpectrogram(const SpecJob& job, float* lin, const std::function<bool()>& cancelled);
bool gpuSpectrogramUsable();  // gpuSpectrogram would try the GPU

// Live spectrum rows on the GPU. One object per thread (the live spectrum
// thread owns one); it keeps its stream, buffers and cuFFT plans between
// rows. The fallback rules are the same as for gpuSpectrogram, with their own
// "failed earlier" state.
struct GpuLiveImpl;
class GpuLiveSpectrum {
public:
    GpuLiveSpectrum();
    ~GpuLiveSpectrum();
    GpuLiveSpectrum(const GpuLiveSpectrum&) = delete;
    GpuLiveSpectrum& operator=(const GpuLiveSpectrum&) = delete;
    // raw: frames * n packed samples in format fp (no header). Writes the n
    // fftshifted powers |X|^2 of the windowed FFTs, summed over the frames,
    // to pw: the same values as the CPU loop in LiveEngine::spectrumLoop.
    GpuStatus run(const uint8_t* raw, const FormatParams& fp, int n, int frames, const float* win, float* pw);

private:
    GpuLiveImpl* impl_ = nullptr;
};
bool gpuLiveUsable();  // GpuLiveSpectrum::run would try the GPU

// A channel filter job for the dense demodulator: mix the input down by
// `frac` cycles per sample (phase from the absolute sample index), then
// y[j] = sum over m of h[m] * x[inStart + j * decim + m], for j in [0, outLen).
// Samples outside the file are zero. Runs on the CPU (channelizeCpu in
// engine.h) or on the GPU (gpuChannelize).
struct ChanJob {
    const IqFile* file = nullptr;
    FormatParams fp;
    int64_t inStart = 0, outLen = 0;
    int decim = 1;
    double frac = 0;  // chanOffset / fs
    const float* h = nullptr;
    int taps = 0;
    Progress* progress = nullptr;  // gets the outputs done (issue #75); may be null
};
// y gets outLen complex values (interleaved re, im). Same status and
// fallback rules as gpuSpectrogram, with its own "failed earlier" state.
GpuStatus gpuChannelize(const ChanJob& job, float* y, const std::function<bool()>& cancelled);
bool gpuChannelUsable();  // gpuChannelize would try the GPU

// One line per path that fell back to the CPU, e.g. "waterfall: uses the CPU
// after an error: ...". Empty when nothing failed.
std::string gpuFallbackNotes();

// Work limits of the backend in use, from its benchmarks. Jobs below the
// minimums are faster on the CPU (the GPU has a fixed cost per call).
struct GpuLimits {
    double specBudget = 2e9;    // waterfall: FFT input samples per view (the CPU has 256e6)
    double specMinInput = 0;    // waterfall: smaller views stay on the CPU
    double liveMaxRate = 100e6; // live auto averaging, samples/s (the CPU has 20e6)
    double liveMinInput = 0;    // live: smaller rows stay on the CPU
    double chanMinMacs = 2e7;   // channel filter: smaller jobs (multiply-adds) stay on the CPU
};
GpuLimits gpuLimits();

// One line per CUDA device for --list-devices, or one line that says why
// there are none.
std::vector<std::string> gpuListDevices();
