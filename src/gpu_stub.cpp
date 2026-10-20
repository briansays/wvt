// GPU backend for builds without CUDA: never available.
#include "gpu_backend.h"

bool gpuBackendBuilt() { return false; }

void gpuBackendProbe(int, GpuInfo& info) {
    info.built = false;
    info.available = false;
    info.reason = "built without CUDA";
}

std::vector<std::string> gpuBackendList() { return {"none (built without CUDA)"}; }

GpuLiveImpl* gpuBackendLiveCreate() { return nullptr; }
void gpuBackendLiveDestroy(GpuLiveImpl*) {}
GpuStatus gpuBackendLiveRun(GpuLiveImpl*, const uint8_t*, const FormatParams&, int, int, const float*, float*, std::string&,
                            bool&) {
    return GpuStatus::Unavailable;
}

GpuStatus gpuBackendSpectrogram(const SpecJob&, float*, const std::function<bool()>&, std::string&, bool&) {
    return GpuStatus::Unavailable;
}
