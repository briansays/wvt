// GPU backend for builds without CUDA: never available.
#include "gpu_backend.h"

bool gpuBackendBuilt() { return false; }

void gpuBackendProbe(int, GpuInfo& info) {
    info.built = false;
    info.available = false;
    info.reason = "built without CUDA";
}

std::vector<std::string> gpuBackendList() { return {"none (built without CUDA)"}; }
