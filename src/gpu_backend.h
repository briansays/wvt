#pragma once
// Interface between gpu_common.cpp and the backend: gpu_cuda.cu in a CUDA
// build, gpu_stub.cpp otherwise. Only those files include this.
#include <string>
#include <vector>

#include "gpu.h"

// Minimum compute capability and free memory that `auto` accepts.
constexpr int kGpuMinCompute = 75;  // 7.5 (Turing)
constexpr double kGpuMinFreeBytes = 1e9;

bool gpuBackendBuilt();  // true in the CUDA build
// Fills info for `device` (< 0 = the best usable one). Sets info.available,
// or info.reason when no GPU can be used.
void gpuBackendProbe(int device, GpuInfo& info);
std::vector<std::string> gpuBackendList();
