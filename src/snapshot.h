#pragma once
// Plot snapshots (issue #13): PNG files of the plots as they show on the screen.
// The window records where each plot is drawn; after the frame is rendered, the
// app reads the frame and writes one cropped PNG per selected plot.
#include <cstdint>
#include <string>
#include <vector>

enum class SnapPlot { Overview, Spectrum, Waterfall, IQ, AM, FM, PM, Count };
constexpr int kSnapPlots = (int)SnapPlot::Count;

// The name in the file name ("spectrum") and the label in the window ("Spectrum").
const char* snapPlotName(SnapPlot p);
const char* snapPlotLabel(SnapPlot p);

// A frame in RGB, 3 bytes per pixel, top row first.
struct RgbImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

// Pixel rectangle [x0, x1) x [y0, y1).
struct SnapRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// The part of src inside r (clipped to the image). Empty if nothing is inside.
RgbImage cropImage(const RgbImage& src, SnapRect r);

// Creates `path` and writes img as PNG. It never replaces a file: if `path`
// exists, it fails and sets *exists (issue #47).
bool writePng(const std::string& path, const RgbImage& img, std::string& err, bool* exists = nullptr);

// The file name base: the recording's name without its folder and extension
// ("cap.sigmf-meta" -> "cap"), or "live" without a file.
std::string snapshotBase(const std::string& recordingPath);

// <dir>/<base>_capture_<plot>_<n>.png
std::string snapshotPath(const std::string& dir, const std::string& base, SnapPlot p, int n);

// The smallest n >= 1 for which no <base>_capture_<plot>_<n>.png exists in dir,
// for any plot, so that one export gets one number.
int nextSnapshotIndex(const std::string& dir, const std::string& base);
