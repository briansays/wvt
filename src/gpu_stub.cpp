// GPU interface for builds without CUDA: nothing is ever available.
#include "gpu.h"

namespace {
GpuInfo makeInfo() {
    GpuInfo i;
#ifdef __APPLE__
    i.reason = "built without CUDA (not available on macOS)";
#else
    i.reason = "built without CUDA (WVT_WITH_CUDA)";
#endif
    return i;
}
const GpuInfo kInfo = makeInfo();
}  // namespace

bool gpuInit(GpuMode mode, int, std::string& err) {
    if (mode == GpuMode::On) {
        err = "--gpu on: " + kInfo.reason;
        return false;
    }
    return true;
}
const GpuInfo& gpuInfo() { return kInfo; }
std::string gpuDeviceList() { return ""; }
bool gpuEnabled() { return false; }
void gpuSetEnabled(bool) {}
std::string gpuNotes() { return ""; }
GpuResult gpuSpectrogram(const GpuSpectrogramJob&, const std::function<bool()>&) { return GpuResult::Failed; }
std::unique_ptr<GpuLiveSpectrum> GpuLiveSpectrum::create(int, SampleFormat) { return nullptr; }
