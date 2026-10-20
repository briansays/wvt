// Metal GPU backend for Apple GPUs (issue #21, docs/apple-gpu-plan.md):
// device discovery. The jobs come in later steps.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <string>
#include <vector>

#include "gpu_backend.h"
#include "log.h"

namespace {
id<MTLDevice> g_device = nil;  // the device gpuInit chose

std::string describe(id<MTLDevice> d) {
    char b[256];
    snprintf(b, sizeof b, "%s (%s, %.1f GB working set)", d.name.UTF8String,
             d.hasUnifiedMemory ? "unified memory" : "own memory", d.recommendedMaxWorkingSetSize / 1e9);
    return b;
}

NSArray<id<MTLDevice>>* allDevices() {
    NSArray<id<MTLDevice>>* devs = MTLCopyAllDevices();
    if (devs.count == 0) {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (d) devs = @[ d ];
    }
    return devs;
}
}  // namespace

bool gpuBackendBuilt() { return true; }
const char* gpuBackendName() { return "Metal"; }
bool gpuBackendImplements(GpuJob) { return false; }

void gpuBackendProbe(int device, GpuInfo& info) {
    info.built = true;
    info.backend = "Metal";
    info.available = false;
    @autoreleasepool {
        if (@available(macOS 14.0, *)) {
        } else {
            info.reason = "the Metal GPU path needs macOS 14 or later";
            return;
        }
        NSArray<id<MTLDevice>>* devs = allDevices();
        if (devs.count == 0) {
            info.reason = "no Metal GPU found";
            return;
        }
        id<MTLDevice> d = nil;
        if (device >= 0) {
            if (device >= (int)devs.count) {
                info.reason = "--gpu-device " + std::to_string(device) + ": there " +
                              (devs.count == 1 ? "is 1 GPU" : "are " + std::to_string(devs.count) + " GPUs");
                return;
            }
            d = devs[device];
        } else {
            d = MTLCreateSystemDefaultDevice();
            if (!d) d = devs[0];
        }
        if (d.recommendedMaxWorkingSetSize < kGpuMinFreeBytes) {
            info.reason = describe(d) + " has less than 1 GB of working set";
            return;
        }
        g_device = d;
        info.available = true;
        info.device = (int)[devs indexOfObject:d];
        if (info.device < 0 || info.device >= (int)devs.count) info.device = 0;
        info.name = describe(d);
        info.model = d.name.UTF8String;  // "Apple M5"
        const NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
        info.version = "Metal, macOS " + std::to_string(v.majorVersion) + "." + std::to_string(v.minorVersion) + "." +
                       std::to_string(v.patchVersion);
        info.reason.clear();
    }
}

std::vector<std::string> gpuBackendList() {
    std::vector<std::string> out;
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devs = allDevices();
        if (devs.count == 0) return {"none (no Metal GPU found)"};
        for (NSUInteger i = 0; i < devs.count; ++i) out.push_back(std::to_string(i) + "  " + describe(devs[i]));
    }
    return out;
}

GpuStatus gpuBackendSpectrogram(const SpecJob&, float*, const std::function<bool()>&, std::string&, bool&) {
    return GpuStatus::Unavailable;
}
GpuStatus gpuBackendChannelize(const ChanJob&, float*, const std::function<bool()>&, std::string&, bool&) {
    return GpuStatus::Unavailable;
}
GpuLiveImpl* gpuBackendLiveCreate() { return nullptr; }
void gpuBackendLiveDestroy(GpuLiveImpl*) {}
GpuStatus gpuBackendLiveRun(GpuLiveImpl*, const uint8_t*, const FormatParams&, int, int, const float*, float*, std::string&,
                            bool&) {
    return GpuStatus::Unavailable;
}
