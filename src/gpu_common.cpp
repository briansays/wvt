// GPU code that does not need CUDA: option parsing and UI text. Linked into every build.
#include <algorithm>
#include <cctype>
#include <cstring>

#include "gpu.h"

bool parseGpuMode(const std::string& in, GpuMode& mode) {
    std::string s = in;
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "auto") mode = GpuMode::Auto;
    else if (s == "on" || s == "1" || s == "yes") mode = GpuMode::On;
    else if (s == "off" || s == "0" || s == "no") mode = GpuMode::Off;
    else return false;
    return true;
}

std::string gpuStatusTag() {
    const GpuInfo& g = gpuInfo();
    if (!g.available) return "";
    if (!gpuEnabled()) return "GPU off";
    std::string name = g.name;
    for (const char* pre : {"NVIDIA ", "GeForce "})
        if (name.rfind(pre, 0) == 0) name.erase(0, strlen(pre));
    return "GPU " + name + (gpuNotes().empty() ? "" : " (CPU fallback)");
}
