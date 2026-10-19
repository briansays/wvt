// gpu_check - GPU acceleration self-test and benchmark (issue #3).
//
//   gpu_check [--bench] [--samples N] [scratch_dir]
//
// --bench prints CPU times for the jobs a GPU could take over: the file
// waterfall, the live spectrum row, the channel filter and the waterfall
// colouring. The test file is a ci16 recording of N samples (default 64M,
// 256 MB) with tones, a chirp, short pulses and noise. Exit status 0 = all
// checks passed. Use a larger --samples (e.g. 1G) to time wide views past the
// CPU budget; smaller files clip them.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "engine.h"
#include "gpu.h"
#include "iq_file.h"
#include "ui_util.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Best of `reps` runs, in ms. The first run also warms the page cache.
template <class F>
double timeIt(int reps, F&& f) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        const double t0 = nowMs();
        f();
        best = std::min(best, nowMs() - t0);
    }
    return best;
}

// Test signal at fs: two tones, a 2 MHz/ms chirp, 1 us pulses every 100 us
// and noise at -60 dBFS. Phases come from the absolute index, so blocks join.
void makeSignal(int64_t first, int64_t n, double fs, cf* out) {
    uint32_t rng = (uint32_t)(first * 2654435761u) | 1;
    auto noise = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float)((rng & 0xffff) / 32768.0 - 1.0);
    };
    for (int64_t k = 0; k < n; ++k) {
        const int64_t i = first + k;
        const double t = i / fs;
        cf v = 0.3f * cf((float)std::cos(2 * M_PI * 0.11 * fs * t), (float)std::sin(2 * M_PI * 0.11 * fs * t));
        v += 0.01f * cf((float)std::cos(-2 * M_PI * 0.27 * fs * t), (float)std::sin(-2 * M_PI * 0.27 * fs * t));
        const double tc = std::fmod(t, 1e-3);  // chirp restarts each ms
        const double ph = 2 * M_PI * (-0.2 * fs * tc + 0.5 * 2e9 * tc * tc);
        v += 0.05f * cf((float)std::cos(ph), (float)std::sin(ph));
        if (std::fmod(t, 100e-6) < 1e-6) v += 0.4f * cf((float)std::cos(2 * M_PI * 0.4 * fs * t), 0.f);
        v += 0.0007f * cf(noise(), noise());
        out[k] = v;
    }
}

bool writeTestFile(const std::string& path, int64_t n, double fs) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const int64_t block = 1 << 20;
    std::vector<cf> x(block);
    std::vector<uint8_t> raw(block * 4);
    for (int64_t s = 0; s < n; s += block) {
        const int64_t m = std::min(block, n - s);
        makeSignal(s, m, fs, x.data());
        encodeSamples(SampleFormat::CI16, x.data(), m, raw.data());
        if (fwrite(raw.data(), 4, (size_t)m, f) != (size_t)m) {
            fclose(f);
            return false;
        }
    }
    return fclose(f) == 0;
}

// --- GPU setup --------------------------------------------------------------
// Option parsing and the init rules. Without a usable GPU, on must fail with
// a reason, and auto and off must succeed on the CPU.
void testSetup() {
    printf("GPU setup\n");
    GpuMode m = GpuMode::Off;
    check(parseGpuMode("AUTO", m) && m == GpuMode::Auto, "parse 'AUTO'");
    check(parseGpuMode("on", m) && m == GpuMode::On && parseGpuMode("0", m) && m == GpuMode::Off, "parse 'on' and '0'");
    check(!parseGpuMode("maybe", m) && !parseGpuMode("", m), "reject 'maybe' and ''");

    std::string err;
    check(gpuInit(GpuMode::Off, -1, err) && !gpuEnabled() && !gpuInfo().available, "off: CPU only");
    gpuSetEnabled(true);
    check(!gpuEnabled(), "off: the switch cannot turn it on");

    err.clear();
    check(gpuInit(GpuMode::Auto, -1, err), "auto always starts");
    const GpuInfo info = gpuInfo();
    printf("  build: %s; GPU: %s\n", info.built ? "CUDA" : "no CUDA",
           info.available ? (info.name + ", " + info.version).c_str() : ("none, " + info.reason).c_str());
    check(info.available || !info.reason.empty(), "auto without a GPU gives the reason");
    check(gpuEnabled() == info.available, "auto: enabled when a GPU is usable");
    if (info.available) {
        gpuSetEnabled(false);
        check(!gpuEnabled(), "the switch turns it off");
        gpuSetEnabled(true);
        check(gpuEnabled(), "the switch turns it on again");
    }
    check(gpuInit(GpuMode::Auto, 9999, err) && !gpuInfo().available, "a device that does not exist is not used");

    err.clear();
    const bool on = gpuInit(GpuMode::On, -1, err);
    check(on == info.available && (on || !err.empty()), "on: " + std::string(on ? "uses the GPU" : "fails: " + err));
    check(!gpuListDevices().empty(), "the device list has a line");
    // Leave it as the benchmark wants it.
    gpuInit(GpuMode::Auto, -1, err);
}

// --- File waterfall ---------------------------------------------------------
// Zoom 1 = the span where one FFT per row at 50 % overlap covers the view.
void benchWaterfall(const Source& src, int64_t nSamples) {
    printf("\nfile waterfall (1024 rows, Blackman-Harris, 50 %% overlap, CPU budget 256 MS)\n");
    printf("  %-7s %-6s %-10s %-6s %-9s %s\n", "N", "zoom", "span", "FFTs", "FFT sees", "time");
    CancelFn never = [] { return false; };
    for (int n : {1024, 8192, 65536})
        for (int zoom : {1, 10, 100}) {
            WaterfallReq r;
            r.src = src;
            r.nfft = n;
            const int64_t span = std::min<int64_t>((int64_t)1024 * n / 2 * zoom, nSamples);
            r.s0 = 0;
            r.s1 = span;
            std::shared_ptr<WaterfallRes> res;
            const double ms = timeIt(2, [&] { res = computeWaterfall(r, never); });
            if (!res || res->rows < 128) check(false, "waterfall N " + std::to_string(n) + " gives an image");
            if (!res) continue;
            const bool clipped = span < (int64_t)1024 * n / 2 * zoom;
            printf("  %-7d %-6s %-10s %-6d %7.1f %%  %8.1f ms%s\n", n, (std::to_string(zoom) + "x").c_str(),
                   fmtSI((double)span, "S", 3).c_str(), res->fftsPerRow, 100 * res->coverage, ms,
                   clipped ? "  (clipped to the file)" : "");
        }
}

// --- Live spectrum row ------------------------------------------------------
// The row math of LiveEngine::spectrumLoop, at 30 rows/s and N = 2048, with
// auto averaging over every new sample (no 20 MS/s cap).
void benchLive(const std::vector<uint8_t>& raw) {
    printf("\nlive spectrum row (N 2048, 30 rows/s, every sample analyzed, ci16)\n");
    printf("  %-11s %-6s %-11s %s\n", "rate", "FFTs", "time/row", "CPU share of one core");
    const int n = 2048;
    FormatParams fp;
    fp.fmt = SampleFormat::CI16;
    const auto win = makeWindow(WindowType::BlackmanHarris, n);
    auto fft = getFFT(n);
    std::vector<cf> buf, seg(n);
    std::vector<float> pw(n);
    for (double rate : {20e6, 40e6, 61.44e6}) {
        const int frames = (int)(rate / 30 / n);
        const int64_t need = (int64_t)frames * n;
        if ((int64_t)raw.size() < need * 4) continue;
        buf.resize(need);
        const double ms = timeIt(5, [&] {
            decodeSamples(fp, raw.data(), need, buf.data());
            std::fill(pw.begin(), pw.end(), 0.f);
            for (int a = 0; a < frames; ++a) {
                const cf* s = &buf[(size_t)a * n];
                for (int i = 0; i < n; ++i) seg[i] = s[i] * win[i];
                fft->forward(seg.data());
                for (int i = 0; i < n; ++i) pw[i] += std::norm(seg[(i + n / 2) & (n - 1)]);
            }
        });
        printf("  %-11s %-6d %8.2f ms  %5.0f %%\n", fmtSI(rate, "S/s", 4).c_str(), frames, ms, ms * 30 / 10);
    }
}

// --- Channel filter -----------------------------------------------------------
void benchDemod(const Source& src, int64_t nSamples) {
    printf("\nchannel filter + demod (dense, 8 MS input, decimation 8)\n");
    printf("  %-6s %s\n", "taps", "time");
    CancelFn never = [] { return false; };
    for (int taps : {255, 2047, 8191}) {
        DemodReq r;
        r.src = src;
        r.s0 = 0;
        r.s1 = std::min<int64_t>(8'000'000, nSamples);
        r.chanEnable = true;
        r.chanOffset = 0.11 * src.fs;
        r.chanBw = src.fs / 20;
        r.taps = taps;
        r.decim = 8;
        std::shared_ptr<DemodRes> res;
        const double ms = timeIt(2, [&] { res = computeDemod(r, never); });
        printf("  %-6d %8.1f ms%s\n", taps, ms, res && res->sparse ? "  (sparse!)" : "");
    }
}

// --- Waterfall colouring ----------------------------------------------------
// The App::recolor loop on the largest image (1024 x 2048). The texture
// upload needs a GL context, so the GUI log covers that part.
void benchColor() {
    printf("\nwaterfall colouring (1024 x 2048, 256-entry table)\n");
    const size_t n = 1024 * 2048;
    std::vector<float> db(n);
    for (size_t i = 0; i < n; ++i) db[i] = -120.f + (float)(i % 997) * 0.12f;
    std::vector<uint32_t> lut(256), rgba(n);
    for (int i = 0; i < 256; ++i) lut[i] = 0xff000000u | (uint32_t)i * 0x010101u;
    const float lo = -110, scale = 255.f / 90.f;
    uint64_t sum = 0;  // uses the pixels, so the loop is not optimized away
    const double ms = timeIt(5, [&] {
        for (size_t i = 0; i < n; ++i) rgba[i] = lut[std::clamp((int)((db[i] - lo) * scale), 0, 255)];
        sum += rgba[n / 3];
    });
    printf("  %.2f ms per image (the texture upload is not measured here)\n", ms);
    if (sum == 0) check(false, "colouring writes pixels");
}
}  // namespace

int main(int argc, char** argv) {
    bool bench = false;
    double samples = 64 * 1024 * 1024;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bench") bench = true;
        else if (a == "--samples" && i + 1 < argc) {
            if (!parseSI(argv[++i], samples) || samples < 1e6) {
                fprintf(stderr, "gpu_check: bad --samples\n");
                return 2;
            }
        } else if (a == "-h" || a == "--help") {
            printf("usage: gpu_check [--bench] [--samples N] [scratch_dir]\n");
            return 0;
        } else dir = a;
    }
    std::string tmp;
    if (dir.empty()) {
        char tmpl[] = "/tmp/wvt_gpu_check_XXXXXX";
        if (!mkdtemp(tmpl)) {
            perror("mkdtemp");
            return 1;
        }
        dir = tmp = tmpl;
    }

    testSetup();
    const double fs = 20e6;
    if (bench) {
        const int64_t n = (int64_t)samples;
        const std::string path = dir + "/gpu_check.ci16";
        printf("writing %s (%s, ci16, %s) ...\n", path.c_str(), fmtSI((double)n, "S", 3).c_str(),
               fmtSI(n * 4.0, "B", 3).c_str());
        if (!writeTestFile(path, n, fs)) {
            fprintf(stderr, "gpu_check: cannot write %s\n", path.c_str());
            return 1;
        }
        std::string err;
        auto file = IqFile::open(path, err);
        if (!file) {
            fprintf(stderr, "gpu_check: %s\n", err.c_str());
            return 1;
        }
        FormatParams fp;
        fp.fmt = SampleFormat::CI16;
        const Source src{file, fp, fs, 0};
        printf("CPU: %u threads\n", std::max(1u, std::thread::hardware_concurrency()));
        benchWaterfall(src, n);
        // About 2.1 s at 61.44 MS/s / 30 rows/s, taken from the start of the file.
        std::vector<uint8_t> raw(std::min<int64_t>(n, 4 << 20) * 4);
        {
            std::vector<cf> x(raw.size() / 4);
            file->read(fp, 0, (int64_t)x.size(), x.data());
            encodeSamples(SampleFormat::CI16, x.data(), (int64_t)x.size(), raw.data());
        }
        benchLive(raw);
        benchDemod(src, n);
        benchColor();
        file.reset();
        unlink(path.c_str());
    }

    if (!tmp.empty()) rmdir(tmp.c_str());
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
