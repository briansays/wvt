// CUDA GPU backend: device discovery and setup. The kernels for the
// waterfall and the live spectrum come in later steps of
// docs/cuda-gpu-plan.md.
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gpu_backend.h"
#include "log.h"

// Evaluates a CUDA call. On an error it logs the first one (with the file
// and line) as WARN and returns false from the enclosing function, whose
// caller then falls back to the CPU.
#define CUDA_TRY(call)                                                                          \
    do {                                                                                        \
        const cudaError_t e_ = (call);                                                          \
        if (e_ != cudaSuccess) {                                                                \
            cudaGetLastError();                                                                 \
            gpuReportError(#call, e_, __FILE__, __LINE__);                                      \
            return false;                                                                       \
        }                                                                                       \
    } while (0)

namespace {
[[maybe_unused]] void gpuReportError(const char* call, cudaError_t e, const char* file, int line) {
    static bool reported = false;
    if (reported) return;
    reported = true;
    LOG_WARN("gpu", "%s failed at %s:%d: %s (%s); later CUDA errors are not logged", call, file, line,
             cudaGetErrorString(e), cudaGetErrorName(e));
}

std::string versionText(int v) {  // 12040 -> "12.4"
    char b[32];
    snprintf(b, sizeof b, "%d.%d", v / 1000, (v % 1000) / 10);
    return b;
}

std::string describe(const cudaDeviceProp& p) {
    char b[320];
    snprintf(b, sizeof b, "%s (compute %d.%d, %.1f GB)", p.name, p.major, p.minor, p.totalGlobalMem / 1e9);
    return b;
}

// Why no device can be used, from the cudaGetDeviceCount error.
std::string noDeviceReason(cudaError_t e) {
    int driver = 0;
    cudaDriverGetVersion(&driver);
    if (e == cudaErrorNoDevice) return "no NVIDIA GPU found";
    if (driver == 0) return "no NVIDIA driver";
    if (e == cudaErrorInsufficientDriver)
        return "the NVIDIA driver (CUDA " + versionText(driver) + ") is older than this build (CUDA " +
               versionText(CUDART_VERSION) + ")";
    return std::string("CUDA cannot start: ") + cudaGetErrorString(e);
}

int deviceCount(std::string& reason) {
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) {
        cudaGetLastError();
        reason = noDeviceReason(e);
        return 0;
    }
    if (n == 0) reason = "no NVIDIA GPU found";
    return n;
}

// Checks one device against the `auto` rules; reason says why it fails.
// This creates the CUDA context on that device.
bool usable(int d, const cudaDeviceProp& p, std::string& reason) {
    if (p.major * 10 + p.minor < kGpuMinCompute) {
        reason = "GPU " + std::to_string(d) + " " + describe(p) + " is older than compute 7.5";
        return false;
    }
    size_t freeB = 0, totalB = 0;
    cudaError_t e = cudaSetDevice(d);
    if (e == cudaSuccess) e = cudaMemGetInfo(&freeB, &totalB);
    if (e != cudaSuccess) {
        cudaGetLastError();
        reason = "GPU " + std::to_string(d) + " cannot start: " + cudaGetErrorString(e);
        return false;
    }
    if ((double)freeB < kGpuMinFreeBytes) {
        char b[96];
        snprintf(b, sizeof b, " has only %.2f GB of free memory (needs 1 GB)", freeB / 1e9);
        reason = "GPU " + std::to_string(d) + " " + describe(p) + b;
        return false;
    }
    return true;
}
}  // namespace

bool gpuBackendBuilt() { return true; }

void gpuBackendProbe(int device, GpuInfo& info) {
    info.built = true;
    info.available = false;
    const int n = deviceCount(info.reason);
    if (n == 0) return;
    std::vector<cudaDeviceProp> props(n);
    for (int d = 0; d < n; ++d)
        if (cudaGetDeviceProperties(&props[d], d) != cudaSuccess) {
            cudaGetLastError();
            props[d] = cudaDeviceProp{};
        }

    std::vector<int> order;
    if (device >= 0) {
        if (device >= n) {
            info.reason = "--gpu-device " + std::to_string(device) + ": there " + (n == 1 ? "is 1 GPU" : "are " + std::to_string(n) + " GPUs");
            return;
        }
        order.push_back(device);
    } else {
        // Best first: newest architecture, then the most multiprocessors.
        for (int d = 0; d < n; ++d) order.push_back(d);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const int ca = props[a].major * 10 + props[a].minor, cb = props[b].major * 10 + props[b].minor;
            return ca != cb ? ca > cb : props[a].multiProcessorCount > props[b].multiProcessorCount;
        });
    }
    std::string firstReason;
    for (int d : order) {
        std::string why;
        if (!usable(d, props[d], why)) {
            if (firstReason.empty()) firstReason = why;
            continue;
        }
        int runtime = 0, driver = 0;
        cudaRuntimeGetVersion(&runtime);
        cudaDriverGetVersion(&driver);
        info.available = true;
        info.device = d;
        info.name = describe(props[d]);
        info.version = "CUDA runtime " + versionText(runtime) + ", driver " + versionText(driver);
        info.reason.clear();
        return;
    }
    info.reason = firstReason;
}

std::vector<std::string> gpuBackendList() {
    std::string reason;
    const int n = deviceCount(reason);
    if (n == 0) return {"none (" + reason + ")"};
    std::vector<std::string> out;
    for (int d = 0; d < n; ++d) {
        cudaDeviceProp p{};
        if (cudaGetDeviceProperties(&p, d) != cudaSuccess) {
            cudaGetLastError();
            out.push_back(std::to_string(d) + "  (cannot read the device properties)");
            continue;
        }
        std::string line = std::to_string(d) + "  " + describe(p);
        if (p.major * 10 + p.minor < kGpuMinCompute) line += "  - not used: older than compute 7.5";
        out.push_back(line);
    }
    return out;
}
