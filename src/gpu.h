#pragma once
// Optional NVIDIA GPU acceleration (issue #3). Plain C++: no CUDA headers.
// In a build without CUDA every call reports "not available", and callers
// use their CPU code. See docs/cuda-gpu-plan.md.
#include <string>
#include <vector>

enum class GpuMode : int { Auto = 0, On, Off };

// "auto", "on", "off" (also 1/0, yes/no, true/false), any case.
bool parseGpuMode(const char* s, GpuMode& out);
const char* gpuModeName(GpuMode m);

struct GpuInfo {
    bool built = false;      // the build has the CUDA backend
    bool available = false;  // a usable GPU was found
    int device = -1;
    std::string name;     // "NVIDIA GeForce RTX 4070 (compute 8.9, 12.0 GB)"
    std::string version;  // "CUDA runtime 12.4, driver 12.6"
    std::string reason;   // why not available: "built without CUDA", "no NVIDIA driver", ...
};

// Once, from main(). device < 0 picks the best usable GPU. Auto and Off
// always succeed; On fails (err says why) when no GPU is usable. Logs the
// result as INFO under the "gpu" component.
bool gpuInit(GpuMode mode, int device, std::string& err);
const GpuInfo& gpuInfo();
bool gpuEnabled();            // available and not switched off
void gpuSetEnabled(bool on);  // UI switch; no effect when not available

// One line per CUDA device for --list-devices, or one line that says why
// there are none.
std::vector<std::string> gpuListDevices();
