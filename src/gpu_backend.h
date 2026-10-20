#pragma once
// Interface between gpu_common.cpp and the backend: gpu_cuda.cu in a CUDA
// build, gpu_metal.mm in a Metal build (macOS), gpu_stub.cpp otherwise.
// Only those files include this.
#include <string>
#include <vector>

#include "gpu.h"

// Minimum compute capability and free memory that `auto` accepts.
constexpr int kGpuMinCompute = 75;  // CUDA 7.5 (Turing)
constexpr double kGpuMinFreeBytes = 1e9;

bool gpuBackendBuilt();          // a GPU backend is in the build
const char* gpuBackendName();    // "CUDA", "Metal", or "" (none)
// Fills info for `device` (< 0 = the best usable one). Sets info.available,
// or info.reason when no GPU can be used.
void gpuBackendProbe(int device, GpuInfo& info);
std::vector<std::string> gpuBackendList();

// The jobs this backend runs; the others use the CPU.
enum class GpuJob { Spectrogram, Live, Channel };
bool gpuBackendImplements(GpuJob job);

// The jobs, on the device chosen by gpuInit. On Failed, err says why and oom
// is true when the GPU ran out of memory.
GpuStatus gpuBackendSpectrogram(const SpecJob& job, float* lin, const std::function<bool()>& cancelled, std::string& err,
                                bool& oom);
GpuStatus gpuBackendChannelize(const ChanJob& job, float* y, const std::function<bool()>& cancelled, std::string& err,
                               bool& oom);
// Live spectrum state, created on first use by the thread that runs it.
GpuLiveImpl* gpuBackendLiveCreate();
void gpuBackendLiveDestroy(GpuLiveImpl* impl);
GpuStatus gpuBackendLiveRun(GpuLiveImpl* impl, const uint8_t* raw, const FormatParams& fp, int n, int frames,
                            const float* win, float* pw, std::string& err, bool& oom);
