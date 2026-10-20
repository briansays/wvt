// GPU backend for builds without CUDA or Metal: never available.
#include "gpu_backend.h"

bool gpuBackendBuilt() { return false; }
const char* gpuBackendName() { return ""; }
bool gpuBackendImplements(GpuJob) { return false; }

void gpuBackendProbe(int, GpuInfo& info) {
    info.built = false;
    info.available = false;
    info.reason = "built without GPU support";
}

std::vector<std::string> gpuBackendList() { return {"none (built without GPU support)"}; }

GpuLiveImpl* gpuBackendLiveCreate() { return nullptr; }
void gpuBackendLiveDestroy(GpuLiveImpl*) {}
GpuStatus gpuBackendLiveRun(GpuLiveImpl*, const uint8_t*, const FormatParams&, int, int, const float*, float*, std::string&,
                            bool&) {
    return GpuStatus::Unavailable;
}

GpuStatus gpuBackendChannelize(const ChanJob&, float*, const std::function<bool()>&, std::string&, bool&) {
    return GpuStatus::Unavailable;
}

GpuStatus gpuBackendSpectrogram(const SpecJob&, float*, const std::function<bool()>&, std::string&, bool&) {
    return GpuStatus::Unavailable;
}
