#include "snapshot.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {
const char* kNames[kSnapPlots] = {"overview", "spectrum", "waterfall", "iq", "am", "fm", "pm"};
const char* kLabels[kSnapPlots] = {"Overview strip", "Spectrum", "Waterfall", "I / Q", "AM", "FM", "PM"};

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}
}  // namespace

const char* snapPlotName(SnapPlot p) { return kNames[(int)p]; }
const char* snapPlotLabel(SnapPlot p) { return kLabels[(int)p]; }

RgbImage cropImage(const RgbImage& src, SnapRect r) {
    r.x0 = std::clamp(r.x0, 0, src.w);
    r.x1 = std::clamp(r.x1, 0, src.w);
    r.y0 = std::clamp(r.y0, 0, src.h);
    r.y1 = std::clamp(r.y1, 0, src.h);
    RgbImage out;
    if (r.empty() || src.px.size() < (size_t)src.w * src.h * 3) return out;
    out.w = r.w();
    out.h = r.h();
    out.px.resize((size_t)out.w * out.h * 3);
    for (int y = 0; y < out.h; ++y)
        memcpy(&out.px[(size_t)y * out.w * 3], &src.px[((size_t)(r.y0 + y) * src.w + r.x0) * 3], (size_t)out.w * 3);
    return out;
}

bool writePng(const std::string& path, const RgbImage& img, std::string& err) {
    if (img.w <= 0 || img.h <= 0) {
        err = "empty image";
        return false;
    }
    // Level 9 is slower but makes the plots (large flat areas) much smaller.
    stbi_write_png_compression_level = 9;
    if (!stbi_write_png(path.c_str(), img.w, img.h, 3, img.px.data(), img.w * 3)) {
        err = "cannot write '" + path + "'" + (errno ? std::string(": ") + strerror(errno) : "");
        return false;
    }
    return true;
}

std::string snapshotBase(const std::string& recordingPath) {
    if (recordingPath.empty()) return "live";
    std::string name = recordingPath.substr(recordingPath.find_last_of('/') + 1);
    for (const char* ext : {".sigmf-meta", ".sigmf-data", ".sigmf"}) {
        const size_t n = strlen(ext);
        if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) return name.substr(0, name.size() - n);
    }
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name.empty() ? "capture" : name;
}

std::string snapshotPath(const std::string& dir, const std::string& base, SnapPlot p, int n) {
    std::string d = dir.empty() ? "." : dir;
    if (d.back() != '/') d += '/';
    return d + base + "_capture_" + snapPlotName(p) + "_" + std::to_string(n) + ".png";
}

int nextSnapshotIndex(const std::string& dir, const std::string& base) {
    for (int n = 1;; ++n) {
        bool used = false;
        for (int p = 0; p < kSnapPlots && !used; ++p) used = exists(snapshotPath(dir, base, (SnapPlot)p, n));
        if (!used) return n;
    }
}
