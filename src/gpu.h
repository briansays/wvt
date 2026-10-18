#pragma once
// Optional NVIDIA GPU acceleration (CUDA + cuFFT). Every function here is safe
// to call in a build without CUDA: the GPU then reports "not available" and the
// callers keep using their CPU code. On any CUDA error a GPU path returns
// Failed, turns itself off for the rest of the session and records a note.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "iq_file.h"

struct GpuInfo {
    bool available = false;  // a usable device was found and initialized
    std::string name;        // e.g. "NVIDIA GeForce RTX 4070"
    int device = 0;
    int ccMajor = 0, ccMinor = 0;
    size_t memFree = 0, memTotal = 0;
    std::string reason;  // why the GPU is not available
};

enum class GpuMode : int { Auto = 0, On, Off };
enum class GpuResult : int { Ok = 0, Cancelled, Failed };

// Parses "auto" / "on" / "off" (also 1/0, yes/no).
bool parseGpuMode(const std::string& s, GpuMode& mode);
// Call once from main() before any worker starts. Returns false only when
// mode is On and no usable GPU exists (err says why).
bool gpuInit(GpuMode mode, int device, std::string& err);
const GpuInfo& gpuInfo();
// Lists the CUDA devices for --list-devices ("" when built without CUDA).
std::string gpuDeviceList();

// True when the GPU is available and the user has not switched it off.
bool gpuEnabled();
void gpuSetEnabled(bool on);
// Short notes about paths that fell back to the CPU after an error.
std::string gpuNotes();
// Status-bar text: "" (no GPU), "GPU off", or "GPU <name>" (+ " (CPU fallback)").
std::string gpuStatusTag();

// Power spectrogram of a file region, the inner loop of computeWaterfall.
// Row r averages `avg` FFTs of size n. FFT a of row r is centered at
// s0 + r*hop + (a+0.5)*hop/avg (rounded), as on the CPU. Each row's summed,
// fftshifted power is multiplied by `scale`, max-pooled to `cols` columns
// and written to lin (linear) and db (10*log10(lin + 1e-20)).
struct GpuSpectrogramJob {
    const IqFile* file = nullptr;
    FormatParams fp;
    int n = 1024, rows = 0, cols = 0, avg = 1;
    int64_t s0 = 0;
    double hop = 1;
    const float* win = nullptr;  // n window coefficients
    float scale = 1;
    float* lin = nullptr;  // rows * cols
    float* db = nullptr;   // rows * cols
};
GpuResult gpuSpectrogram(const GpuSpectrogramJob& job, const std::function<bool()>& cancelled);

// Live spectrum: one object per spectrum thread. Owns its CUDA stream,
// buffers and FFT plan, so it must be used from a single thread.
class GpuLiveSpectrum {
public:
    // nullptr when the GPU is not enabled or setup fails.
    static std::unique_ptr<GpuLiveSpectrum> create(int n, SampleFormat fmt);
    virtual ~GpuLiveSpectrum() = default;
    // raw: frames * n packed samples (little-endian, no I/Q swap). Writes the n
    // fftshifted powers, summed over the frames and not normalized, to pw.
    virtual GpuResult run(const uint8_t* raw, int frames, const float* win, float* pw) = 0;
};
