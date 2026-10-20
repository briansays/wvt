// gpu_check - GPU acceleration self-test and benchmark (issue #3).
//
//   gpu_check [--bench] [--samples N] [scratch_dir]
//
// Without --bench it checks the GPU setup rules and, when a GPU can be used,
// compares the GPU waterfall, live spectrum and channel filter with the CPU.
// --bench also prints times for the jobs a GPU could take over: the file
// waterfall (CPU and GPU), the live spectrum row, the channel filter and the
// waterfall colouring. The test file is a ci16 recording of N samples (default 64M,
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

// Writes n samples of makeSignal in `fmt`, after `header` bytes of 0x5a.
// bigEndian swaps the bytes of each I and Q value.
bool writeTestFile(const std::string& path, int64_t n, double fs, SampleFormat fmt = SampleFormat::CI16,
                   bool bigEndian = false, int header = 0) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    for (int k = 0; k < header; ++k) fputc(0x5a, f);
    const int bps = formatBytesPerSample(fmt), half = bps / 2;
    const int64_t block = 1 << 20;
    std::vector<cf> x(block);
    std::vector<uint8_t> raw(block * bps);
    for (int64_t s = 0; s < n; s += block) {
        const int64_t m = std::min(block, n - s);
        makeSignal(s, m, fs, x.data());
        encodeSamples(fmt, x.data(), m, raw.data());
        if (bigEndian)
            for (int64_t v = 0; v < m * 2; ++v) std::reverse(&raw[v * half], &raw[v * half] + half);
        if (fwrite(raw.data(), bps, (size_t)m, f) != (size_t)m) {
            fclose(f);
            return false;
        }
    }
    return fclose(f) == 0;
}

// --- CPU FFT ----------------------------------------------------------------
// FFT::forward (vDSP on macOS, radix-2 elsewhere) against a direct DFT in
// double, and a tone in the right bin at N 65536.
void testFft() {
    printf("CPU FFT\n");
    for (int n : {8, 256, 1024, 4096}) {
        std::vector<cf> x(n);
        uint32_t r = 12345;
        for (auto& v : x) {
            r = r * 1664525u + 1013904223u;
            const float a = (r >> 8) / 16777216.f - 0.5f;
            r = r * 1664525u + 1013904223u;
            v = cf(a, (r >> 8) / 16777216.f - 0.5f);
        }
        std::vector<cf> y = x;
        getFFT(n)->forward(y.data());
        double err = 0, peak = 0;
        for (int k = 0; k < n; ++k) {
            std::complex<double> acc = 0;
            for (int i = 0; i < n; ++i) acc += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * (double)k * i / n);
            err = std::max(err, std::abs(acc - std::complex<double>(y[k])));
            peak = std::max(peak, std::abs(acc));
        }
        char b[96];
        snprintf(b, sizeof b, "N %d matches a direct DFT (max error %.1e of the peak)", n, err / peak);
        check(err <= 1e-5 * peak, b);
    }
    const int n = 65536, bin = 12345;
    std::vector<cf> x(n);
    for (int i = 0; i < n; ++i) x[i] = std::polar(1.f, (float)(2 * M_PI * (double)bin * i / n));
    getFFT(n)->forward(x.data());
    int best = 0;
    for (int k = 1; k < n; ++k)
        if (std::norm(x[k]) > std::norm(x[best])) best = k;
    check(best == bin && std::fabs(std::abs(x[bin]) / n - 1.0) < 1e-4, "N 65536: a tone lands in its bin with amplitude N");
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
    printf("  build: %s; GPU: %s\n", info.built ? info.backend.c_str() : "no GPU backend",
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

// --- GPU waterfall against the CPU --------------------------------------------
// Largest dB difference on bins more than 20 dB above the median (the noise
// floor). Bins that are zero on the CPU must be zero on the GPU.
struct Diff {
    double strongDb = 0;
    int64_t compared = 0, zeroMismatch = 0;
};
Diff compareLin(const std::vector<float>& cpu, const std::vector<float>& gpu) {
    Diff d;
    std::vector<float> sorted(cpu);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float floor = sorted[sorted.size() / 2];
    for (size_t i = 0; i < cpu.size(); ++i) {
        if (cpu[i] == 0.f) {
            if (gpu[i] != 0.f) ++d.zeroMismatch;
            continue;
        }
        if (cpu[i] < floor * 100.f) continue;
        d.strongDb = std::max(d.strongDb, std::fabs(10.0 * std::log10((double)gpu[i] / cpu[i])));
        ++d.compared;
    }
    return d;
}

std::string describeJob(const SpecJob& j) {
    char b[160];
    snprintf(b, sizeof b, "N %d, %d rows x %d FFTs, %d cols, start %.0f", j.n, j.rows, j.avg, j.cols, j.s0);
    return b;
}

void compareJob(const std::string& what, SpecJob j, const std::vector<float>& win) {
    j.win = win.data();
    double wsum = 0;
    for (float w : win) wsum += w;
    j.norm = (float)(1.0 / (wsum * wsum));
    CancelFn never = [] { return false; };
    std::vector<float> cpu((size_t)j.rows * j.cols), gpu(cpu.size(), -1.f);
    spectrogramCpu(j, cpu.data(), never);
    const GpuStatus st = gpuSpectrogram(j, gpu.data(), never);
    if (st != GpuStatus::Ok) {
        check(false, what + ": GPU status " + std::to_string((int)st));
        return;
    }
    const Diff d = compareLin(cpu, gpu);
    char b[160];
    snprintf(b, sizeof b, ": max %.4f dB on %lld strong bins", d.strongDb, (long long)d.compared);
    check(d.strongDb <= 0.05 && d.zeroMismatch == 0,
          what + " (" + describeJob(j) + ")" + b + (d.zeroMismatch ? ", nonzero outside the file" : ""));
}

void testSpectrogram(const std::string& dir) {
    printf("\nGPU waterfall\n");
    const double fs = 20e6;
    CancelFn never = [] { return false; };
    if (!gpuSpectrogramUsable()) {
        SpecJob j;
        std::vector<float> lin(1);
        check(gpuSpectrogram(j, lin.data(), never) == GpuStatus::Unavailable, "no GPU: gpuSpectrogram is unavailable");
        printf("  GPU against CPU comparison skipped: %s\n", gpuInfo().reason.c_str());
        return;
    }
    struct FileCase {
        const char* name;
        SampleFormat fmt;
        bool bigEndian, swapIQ;
        int header;
    };
    const FileCase files[] = {
        {"cf32", SampleFormat::CF32, false, false, 0}, {"cf64", SampleFormat::CF64, false, false, 0},
        {"ci16", SampleFormat::CI16, false, false, 0}, {"ci32", SampleFormat::CI32, false, false, 0},
        {"ci8", SampleFormat::CI8, false, false, 0},   {"cu8", SampleFormat::CU8, false, false, 0},
        {"cf32_be", SampleFormat::CF32, true, false, 0}, {"ci16_be", SampleFormat::CI16, true, false, 0},
        {"ci16 swap IQ", SampleFormat::CI16, false, true, 0}, {"ci16 header 3 bytes", SampleFormat::CI16, false, false, 3},
    };
    const int64_t n = 4'000'000;
    const auto win1k = makeWindow(WindowType::BlackmanHarris, 1024), win8k = makeWindow(WindowType::Hann, 8192);
    for (const auto& fc : files) {
        const std::string path = dir + "/gpu_cmp.bin";
        if (!writeTestFile(path, n, fs, fc.fmt, fc.bigEndian, fc.header)) {
            check(false, std::string("write ") + fc.name);
            continue;
        }
        std::string err;
        auto file = IqFile::open(path, err);
        SpecJob j;
        j.file = file.get();
        j.fp.fmt = fc.fmt;
        j.fp.bigEndian = fc.bigEndian;
        j.fp.swapIQ = fc.swapIQ;
        j.fp.headerBytes = fc.header;
        // Starts before the file, so the first FFTs are partly zero.
        j.s0 = -3000;
        j.rows = 256;
        j.avg = 4;
        j.n = 1024;
        j.cols = 512;
        j.hop = (double)n / j.rows;
        compareJob(fc.name, j, win1k);
        if (fc.fmt == SampleFormat::CI16 && !fc.bigEndian && !fc.swapIQ && !fc.header) {
            // One FFT per row, many rows per chunk, no pooling.
            j.s0 = 0;
            j.rows = 1024;
            j.avg = 1;
            j.n = 8192;
            j.cols = 8192;
            j.hop = 3000;
            compareJob("ci16 N 8192", j, win8k);
            // Past the end of the file: all zero.
            j.s0 = (double)n + 10000;
            j.rows = 16;
            compareJob("ci16 past the end", j, win8k);
        }
        file.reset();
        unlink(path.c_str());
    }

    // Rows longer than one GPU chunk (8 M samples) are summed in pieces.
    {
        const std::string path = dir + "/gpu_long.ci16";
        const int64_t m = 24'000'000;
        writeTestFile(path, m, fs);
        std::string err;
        auto file = IqFile::open(path, err);
        SpecJob j;
        j.file = file.get();
        j.fp.fmt = SampleFormat::CI16;
        j.rows = 2;
        j.avg = 300;
        j.n = 65536;
        j.cols = 2048;
        j.hop = (double)m / j.rows;
        compareJob("ci16 rows of 300 FFTs of 65536", j, makeWindow(WindowType::BlackmanHarris, 65536));

        // The whole engine path: same image as the CPU, flagged as GPU.
        WaterfallReq r;
        r.src = Source{file, j.fp, fs, 0};
        r.s0 = 1000;
        r.s1 = 5'000'000;
        r.nfft = 2048;
        auto g = computeWaterfall(r, never);
        gpuSetEnabled(false);
        auto c = computeWaterfall(r, never);
        gpuSetEnabled(true);
        bool same = g && c && g->db.size() == c->db.size() && g->fftsPerRow == c->fftsPerRow;
        double maxDb = 0;
        if (same)
            for (size_t i = 0; i < g->db.size(); ++i)
                if (c->db[i] > c->p50 + 20) maxDb = std::max(maxDb, (double)std::fabs(g->db[i] - c->db[i]));
        check(same && g->gpu && !c->gpu && maxDb <= 0.05, "computeWaterfall on the GPU matches the CPU (max " +
                                                                std::to_string(maxDb) + " dB)");

        // Cancel.
        std::vector<float> lin((size_t)j.rows * j.cols);
        const auto w = makeWindow(WindowType::Hann, j.n);
        j.win = w.data();
        check(gpuSpectrogram(j, lin.data(), [] { return true; }) == GpuStatus::Cancelled, "cancel stops the GPU job");

        // A forced failure: the CPU takes over, and the path stays off.
        setenv("WVT_GPU_FORCE_FAIL", "1", 1);
        auto f = computeWaterfall(r, never);
        unsetenv("WVT_GPU_FORCE_FAIL");
        check(f && !f->gpu && f->db == c->db, "a GPU failure falls back to the CPU result");
        check(!gpuSpectrogramUsable(), "after a failure the waterfall stays on the CPU");
        std::string e2;
        gpuInit(GpuMode::Auto, -1, e2);
        check(gpuSpectrogramUsable(), "gpuInit turns the path on again");
        file.reset();
        unlink(path.c_str());
    }
}

// --- Live spectrum: GPU against CPU -------------------------------------------
// The CPU row math of LiveEngine::spectrumLoop: summed fftshifted powers.
void liveCpu(const uint8_t* raw, const FormatParams& fp, int n, int frames, const float* win, float* pw) {
    std::vector<cf> buf((size_t)n * frames), seg(n);
    decodeSamples(fp, raw, (int64_t)n * frames, buf.data());
    auto fft = getFFT(n);
    std::fill(pw, pw + n, 0.f);
    for (int a = 0; a < frames; ++a) {
        const cf* s = &buf[(size_t)a * n];
        for (int i = 0; i < n; ++i) seg[i] = s[i] * win[i];
        fft->forward(seg.data());
        for (int i = 0; i < n; ++i) pw[i] += std::norm(seg[(i + n / 2) & (n - 1)]);
    }
}

// Raw samples of makeSignal in fmt (little-endian unless bigEndian).
std::vector<uint8_t> rawSignal(int64_t count, SampleFormat fmt, bool bigEndian = false) {
    std::vector<cf> x(count);
    makeSignal(0, count, 20e6, x.data());
    const int bps = formatBytesPerSample(fmt), half = bps / 2;
    std::vector<uint8_t> raw((size_t)count * bps);
    encodeSamples(fmt, x.data(), count, raw.data());
    if (bigEndian)
        for (int64_t v = 0; v < count * 2; ++v) std::reverse(&raw[v * half], &raw[v * half] + half);
    return raw;
}

void testLive() {
    printf("\nGPU live spectrum\n");
    GpuLiveSpectrum live;
    if (!gpuLiveUsable()) {
        std::vector<uint8_t> raw(4 * 1024);
        std::vector<float> win(1024, 1.f), pw(1024);
        check(live.run(raw.data(), FormatParams{}, 1024, 1, win.data(), pw.data()) == GpuStatus::Unavailable,
              "no GPU: the live spectrum is unavailable");
        printf("  GPU against CPU comparison skipped: %s\n", gpuInfo().reason.c_str());
        return;
    }
    struct Case {
        const char* name;
        SampleFormat fmt;
        bool bigEndian, swapIQ;
        int n, frames;
    };
    const Case cases[] = {
        {"cu8 (RTL-SDR)", SampleFormat::CU8, false, false, 2048, 39},
        {"ci16 (USRP), 20 MS/s", SampleFormat::CI16, false, false, 2048, 325},
        {"ci16, 61.44 MS/s", SampleFormat::CI16, false, false, 2048, 1000},
        {"ci16, one frame", SampleFormat::CI16, false, false, 1024, 1},
        {"cf32 big-endian, swap IQ", SampleFormat::CF32, true, true, 4096, 50},
        {"ci16, N 65536, 2 chunks", SampleFormat::CI16, false, false, 65536, 200},
    };
    for (const auto& c : cases) {
        FormatParams fp;
        fp.fmt = c.fmt;
        fp.bigEndian = c.bigEndian;
        fp.swapIQ = c.swapIQ;
        const auto raw = rawSignal((int64_t)c.n * c.frames, c.fmt, c.bigEndian);
        const auto win = makeWindow(WindowType::BlackmanHarris, c.n);
        std::vector<float> cpu(c.n), gpu(c.n, -1.f);
        liveCpu(raw.data(), fp, c.n, c.frames, win.data(), cpu.data());
        const GpuStatus st = live.run(raw.data(), fp, c.n, c.frames, win.data(), gpu.data());
        if (st != GpuStatus::Ok) {
            check(false, std::string(c.name) + ": GPU status " + std::to_string((int)st));
            continue;
        }
        const Diff d = compareLin(cpu, gpu);
        char b[200];
        snprintf(b, sizeof b, "%s (N %d, %d frames): max %.4f dB on %lld strong bins", c.name, c.n, c.frames, d.strongDb,
                 (long long)d.compared);
        check(d.strongDb <= 0.05 && d.zeroMismatch == 0, b);
    }
    // A forced failure turns the live path off until gpuInit.
    const auto raw = rawSignal(2048, SampleFormat::CI16);
    const auto win = makeWindow(WindowType::Hann, 2048);
    std::vector<float> pw(2048);
    FormatParams fp;
    fp.fmt = SampleFormat::CI16;
    setenv("WVT_GPU_FORCE_FAIL", "1", 1);
    const GpuStatus st = live.run(raw.data(), fp, 2048, 1, win.data(), pw.data());
    unsetenv("WVT_GPU_FORCE_FAIL");
    check(st == GpuStatus::Failed && !gpuLiveUsable() && gpuSpectrogramUsable(),
          "a live failure turns off only the live path");
    std::string err;
    gpuInit(GpuMode::Auto, -1, err);
    check(gpuLiveUsable(), "gpuInit turns the live path on again");
}

// --- Channel filter: GPU against CPU -------------------------------------------
void testChannel(const std::string& dir) {
    printf("\nGPU channel filter\n");
    CancelFn never = [] { return false; };
    if (!gpuChannelUsable()) {
        ChanJob j;
        std::vector<float> y(2);
        check(gpuChannelize(j, y.data(), never) == GpuStatus::Unavailable, "no GPU: the channel filter is unavailable");
        printf("  GPU against CPU comparison skipped: %s\n", gpuInfo().reason.c_str());
        return;
    }
    const double fs = 20e6;
    const int64_t n = 26'000'000;
    struct Case {
        const char* name;
        SampleFormat fmt;
        int64_t inStart, outLen;
        int decim, taps;
        double frac;
    };
    const Case cases[] = {
        {"ci16, 255 taps, decimation 8", SampleFormat::CI16, 1000, 1'000'000, 8, 255, 0.11},
        {"ci16, 8191 taps, decimation 1", SampleFormat::CI16, 5000, 100'000, 1, 8191, -0.27},
        {"ci16, no mix", SampleFormat::CI16, 0, 200'000, 4, 2047, 0.0},
        {"ci16, starts before the file", SampleFormat::CI16, -50'000, 100'000, 2, 511, 0.2},
        {"ci16, runs past the end", SampleFormat::CI16, n - 100'000, 50'000, 4, 1023, 0.05},
        {"ci16, 3 chunks (24 MS in)", SampleFormat::CI16, 0, 3'000'000, 8, 255, 0.11},
        {"ci16, 13001 taps (not in shared memory)", SampleFormat::CI16, 0, 20'000, 10, 13001, 0.3},
        {"cu8", SampleFormat::CU8, 1000, 500'000, 8, 255, 0.11},
        {"cf32", SampleFormat::CF32, 1000, 500'000, 8, 255, 0.11},
    };
    SampleFormat written = SampleFormat::Count;
    std::shared_ptr<IqFile> file;
    const std::string path = dir + "/gpu_chan.bin";
    for (const auto& c : cases) {
        if (c.fmt != written) {
            file.reset();
            const int64_t len = c.fmt == SampleFormat::CI16 ? n : 2'000'000;
            if (!writeTestFile(path, len, fs, c.fmt)) {
                check(false, "write the test file");
                return;
            }
            std::string err;
            file = IqFile::open(path, err);
            written = c.fmt;
        }
        const auto h = designLowpass(c.taps, 0.4 / c.decim);
        ChanJob j;
        j.file = file.get();
        j.fp.fmt = c.fmt;
        j.inStart = c.inStart;
        j.outLen = c.outLen;
        j.decim = c.decim;
        j.frac = c.frac;
        j.h = h.data();
        j.taps = c.taps;
        std::vector<cf> cpu(c.outLen), gpu(c.outLen, cf(-9.f, -9.f));
        channelizeCpu(j, cpu.data(), never);
        const GpuStatus st = gpuChannelize(j, reinterpret_cast<float*>(gpu.data()), never);
        if (st != GpuStatus::Ok) {
            check(false, std::string(c.name) + ": GPU status " + std::to_string((int)st));
            continue;
        }
        double peak = 0, diff = 0;
        for (size_t k = 0; k < cpu.size(); ++k) {
            peak = std::max(peak, (double)std::abs(cpu[k]));
            diff = std::max(diff, (double)std::abs(gpu[k] - cpu[k]));
        }
        char b[200];
        snprintf(b, sizeof b, "%s: max error %.2g of the peak", c.name, peak > 0 ? diff / peak : diff);
        check(peak > 0 ? diff <= 1e-4 * peak : diff == 0, b);
    }
    // Cancel, and a forced failure that turns off only this path.
    const auto h = designLowpass(255, 0.05);
    ChanJob j;
    j.file = file.get();
    j.fp.fmt = written;
    j.outLen = 1000;
    j.decim = 8;
    j.h = h.data();
    j.taps = 255;
    std::vector<cf> y(j.outLen);
    check(gpuChannelize(j, reinterpret_cast<float*>(y.data()), [] { return true; }) == GpuStatus::Cancelled,
          "cancel stops the GPU job");
    setenv("WVT_GPU_FORCE_FAIL", "1", 1);
    const GpuStatus st = gpuChannelize(j, reinterpret_cast<float*>(y.data()), never);
    unsetenv("WVT_GPU_FORCE_FAIL");
    check(st == GpuStatus::Failed && !gpuChannelUsable() && gpuSpectrogramUsable() && gpuLiveUsable(),
          "a channel filter failure turns off only that path");
    std::string err;
    gpuInit(GpuMode::Auto, -1, err);
    check(gpuChannelUsable(), "gpuInit turns the path on again");
    file.reset();
    unlink(path.c_str());
}

// --- File waterfall ---------------------------------------------------------
// Zoom 1 = the span where one FFT per row at 50 % overlap covers the view.
void benchWaterfall(const Source& src, int64_t nSamples) {
    const bool gpu = gpuSpectrogramUsable();
    printf("\nfile waterfall (1024 rows, Blackman-Harris, 50 %% overlap, budget CPU 256 MS%s)\n", gpu ? ", GPU 2 GS" : "");
    printf("  %-7s %-6s %-10s %-6s %-9s %-11s", "N", "zoom", "span", "FFTs", "FFT sees", "CPU time");
    printf(gpu ? "%-6s %-9s %-11s %s\n" : "\n", "FFTs", "FFT sees", "GPU time", "speed-up");
    CancelFn never = [] { return false; };
    for (int n : {1024, 8192, 65536})
        for (int zoom : {1, 10, 100}) {
            WaterfallReq r;
            r.src = src;
            r.nfft = n;
            const int64_t span = std::min<int64_t>((int64_t)1024 * n / 2 * zoom, nSamples);
            r.s0 = 0;
            r.s1 = span;
            std::shared_ptr<WaterfallRes> res, gres;
            gpuSetEnabled(false);
            const double ms = timeIt(2, [&] { res = computeWaterfall(r, never); });
            gpuSetEnabled(true);
            if (!res || res->rows < 128) check(false, "waterfall N " + std::to_string(n) + " gives an image");
            if (!res) continue;
            const bool clipped = span < (int64_t)1024 * n / 2 * zoom;
            printf("  %-7d %-6s %-10s %-6d %7.1f %%  %8.1f ms ", n, (std::to_string(zoom) + "x").c_str(),
                   fmtSI((double)span, "S", 3).c_str(), res->fftsPerRow, 100 * res->coverage, ms);
            if (gpu) {
                const double gms = timeIt(2, [&] { gres = computeWaterfall(r, never); });
                if (!gres || !gres->gpu) check(false, "GPU waterfall N " + std::to_string(n));
                if (gres) printf("%-6d %7.1f %%  %8.1f ms  %5.1fx", gres->fftsPerRow, 100 * gres->coverage, gms, ms / gms);
            }
            printf("%s\n", clipped ? "  (clipped to the file)" : "");
        }
}

// --- Live spectrum row ------------------------------------------------------
// The row math of LiveEngine::spectrumLoop, at 30 rows/s and N = 2048, with
// auto averaging over every new sample (no 20 MS/s cap).
void benchLive(const std::vector<uint8_t>& raw) {
    printf("\nlive spectrum row (N 2048, 30 rows/s, every sample analyzed, ci16)\n");
    const bool gpuOk = gpuLiveUsable();
    printf("  %-11s %-6s %-11s %-23s%s\n", "rate", "FFTs", "time/row", "CPU share of one core", gpuOk ? "GPU time/row" : "");
    GpuLiveSpectrum gpu;
    std::vector<float> gpw(2048);
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
        printf("  %-11s %-6d %8.2f ms  %5.0f %%                ", fmtSI(rate, "S/s", 4).c_str(), frames, ms, ms * 30 / 10);
        if (gpuOk) {
            GpuStatus st = GpuStatus::Ok;
            const double gms = timeIt(5, [&] { st = gpu.run(raw.data(), fp, n, frames, win.data(), gpw.data()); });
            if (st != GpuStatus::Ok) check(false, "GPU live row at " + fmtSI(rate, "S/s", 4));
            printf("%8.2f ms", gms);
        }
        printf("\n");
    }
}

// --- Channel filter -----------------------------------------------------------
void benchDemod(const Source& src, int64_t nSamples) {
    const bool gpu = gpuChannelUsable();
    printf("\nchannel filter + demod (dense, 8 MS input, decimation 8)\n");
    printf("  %-6s %-11s%s\n", "taps", "CPU time", gpu ? "GPU time   speed-up" : "");
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
        gpuSetEnabled(false);
        const double ms = timeIt(2, [&] { res = computeDemod(r, never); });
        gpuSetEnabled(true);
        printf("  %-6d %8.1f ms", taps, ms);
        if (gpu) {
            const double gms = timeIt(2, [&] { res = computeDemod(r, never); });
            printf("  %8.1f ms  %5.1fx", gms, ms / gms);
        }
        printf("%s\n", res && res->sparse ? "  (sparse!)" : "");
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

    testFft();
    testSetup();
    testSpectrogram(dir);
    testLive();
    testChannel(dir);
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
        printf("CPU: %u threads; GPU: %s\n", std::max(1u, std::thread::hardware_concurrency()),
               gpuInfo().available ? gpuInfo().name.c_str() : gpuInfo().reason.c_str());
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
