// gpu_check - compares the GPU and CPU results of the file waterfall and the
// live spectrum, for every sample format, and optionally measures speed.
// In a build without CUDA (or without a usable GPU) it runs the CPU timings
// only and passes.
//
//   gpu_check [--bench] [--gpu-device N] [scratch_dir]
//   exit status 0 = all checks passed
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "dsp.h"
#include "engine.h"
#include "gpu.h"
#include "iq_file.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++failures;
}

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A tone, a train of chirped pulses and noise: enough structure that a wrong
// shift, window or row position shows up as a large dB difference.
std::vector<cf> testSignal(int64_t n, uint32_t seed) {
    std::vector<cf> v((size_t)n);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    const double fTone = 0.123, pri = 50000, pw = 8000, bw = 0.2;
    const float aTone = 0.3f, aPulse = 0.5f, aNoise = 1e-3f;
    for (int64_t i = 0; i < n; ++i) {
        const double ph = 2 * M_PI * fTone * i;
        cf x = aTone * cf((float)std::cos(ph), (float)std::sin(ph));
        const double t = std::fmod((double)i, pri);
        if (t < pw) {
            const double k = bw / pw, f0 = -bw / 2;
            const double pp = 2 * M_PI * (f0 * t + 0.5 * k * t * t);
            x += aPulse * cf((float)std::cos(pp), (float)std::sin(pp));
        }
        x += aNoise * cf(g(rng), g(rng));
        v[(size_t)i] = x;
    }
    return v;
}

void byteSwapComponents(std::vector<uint8_t>& b, int bps) {
    const int half = bps / 2;
    for (size_t i = 0; i < b.size(); i += half) std::reverse(b.begin() + i, b.begin() + i + half);
}

std::shared_ptr<IqFile> writeFile(const std::string& path, const std::vector<cf>& sig, const FormatParams& fp) {
    const int bps = formatBytesPerSample(fp.fmt);
    std::vector<uint8_t> bytes(sig.size() * bps);
    std::vector<cf> in = sig;
    if (fp.swapIQ)
        for (auto& x : in) x = cf(x.imag(), x.real());
    encodeSamples(fp.fmt, in.data(), (int64_t)in.size(), bytes.data());
    if (fp.bigEndian) byteSwapComponents(bytes, bps);
    {
        std::ofstream f(path, std::ios::binary);
        std::vector<char> header((size_t)fp.headerBytes, 'h');
        f.write(header.data(), (std::streamsize)header.size());
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }
    std::string err;
    auto file = IqFile::open(path, err);
    if (!file) fprintf(stderr, "gpu_check: %s\n", err.c_str());
    return file;
}

std::shared_ptr<WaterfallRes> waterfall(const WaterfallReq& r, bool gpu) {
    gpuSetEnabled(gpu);
    auto res = computeWaterfall(r, [] { return false; });
    gpuSetEnabled(true);
    return res;
}

// Largest dB difference over the bins at or above the CPU image's median
// (deep nulls below the noise floor are numerically meaningless).
double maxDbDiff(const WaterfallRes& a, const WaterfallRes& b) {
    double d = 0;
    for (size_t i = 0; i < a.db.size(); ++i)
        if (a.db[i] >= a.p50) d = std::max(d, (double)std::fabs(a.db[i] - b.db[i]));
    return d;
}

std::string fmtName(const FormatParams& fp) {
    std::string s = formatName(fp.fmt);
    s = s.substr(0, s.find(' '));
    if (fp.bigEndian) s += "_be";
    if (fp.swapIQ) s += " swap-iq";
    if (fp.headerBytes) s += " header";
    return s;
}

// CPU reference for GpuLiveSpectrum: the loop in LiveEngine::spectrumLoop.
std::vector<float> liveCpu(const std::vector<uint8_t>& raw, SampleFormat fmt, int n, int frames, const std::vector<float>& win) {
    FormatParams fp;
    fp.fmt = fmt;
    std::vector<cf> buf((size_t)frames * n), seg(n);
    decodeSamples(fp, raw.data(), (int64_t)frames * n, buf.data());
    auto fft = getFFT(n);
    std::vector<float> pw(n, 0.f);
    for (int a = 0; a < frames; ++a) {
        for (int i = 0; i < n; ++i) seg[i] = buf[(size_t)a * n + i] * win[i];
        fft->forward(seg.data());
        for (int i = 0; i < n; ++i) pw[i] += std::norm(seg[(i + n / 2) & (n - 1)]);
    }
    return pw;
}

void checkWaterfalls(const std::string& dir) {
    const int64_t N = 1 << 22;
    const auto sig = testSignal(N, 1);
    std::vector<FormatParams> fmts;
    for (SampleFormat f : {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CI8, SampleFormat::CU8,
                           SampleFormat::CF64, SampleFormat::CI32}) {
        FormatParams fp;
        fp.fmt = f;
        fmts.push_back(fp);
    }
    FormatParams be;
    be.fmt = SampleFormat::CI16;
    be.bigEndian = true;
    fmts.push_back(be);
    be.fmt = SampleFormat::CF32;
    fmts.push_back(be);
    FormatParams sw;
    sw.fmt = SampleFormat::CI16;
    sw.swapIQ = true;
    sw.headerBytes = 12;
    fmts.push_back(sw);

    for (const auto& fp : fmts) {
        auto file = writeFile(dir + "/gpu_check.iq", sig, fp);
        if (!file) {
            check(false, "write test file " + fmtName(fp));
            continue;
        }
        struct Case {
            const char* name;
            int64_t s0, s1;
            int nfft;
            double overlap;
            int avg;
        };
        const Case cases[] = {
            {"whole file, N=1024", 0, N, 1024, 0.5, 1},
            {"zoomed in, N=4096", 1000000, 1100000, 4096, 0.75, 1},
            {"start of file (zero fill), N=8192", 0, 20000, 8192, 0.5, 4},
            {"past the end (zero fill), N=2048", N - 30000, N + 30000, 2048, 0.5, 1},
            {"budget-limited (gathered FFTs), N=256", 0, N, 256, 0.5, 1},
        };
        for (const auto& c : cases) {
            WaterfallReq r;
            r.src.file = file;
            r.src.fp = fp;
            r.src.fs = 1e6;
            r.s0 = c.s0;
            r.s1 = c.s1;
            r.nfft = c.nfft;
            r.overlap = c.overlap;
            r.avgPerRow = c.avg;
            if (std::string(c.name).find("budget") != std::string::npos) {
                // 64 rows of 16 FFTs: 1/32 coverage, so the GPU gathers each FFT's samples.
                r.maxRows = r.minRows = 64;
                r.maxInput = 64.0 * 256 * 16;
            }
            auto cpu = waterfall(r, false), gpu = waterfall(r, true);
            const std::string what = fmtName(fp) + ": " + c.name;
            if (!cpu || !gpu || cpu->gpu || !gpu->gpu) {
                check(false, what, "GPU path not taken");
                continue;
            }
            const bool sameShape = cpu->rows == gpu->rows && cpu->cols == gpu->cols && cpu->fftsPerRow == gpu->fftsPerRow;
            const double d = sameShape ? maxDbDiff(*cpu, *gpu) : 1e9;
            double sd = 0;
            for (size_t k = 0; k < cpu->specAvg.size(); ++k) sd = std::max(sd, std::fabs(cpu->specAvg[k] - gpu->specAvg[k]));
            char b[160];
            snprintf(b, sizeof b, "%dx%d, %d FFT/row, coverage %.0f%%, max diff %.4f dB (spectrum %.4f dB)", cpu->rows,
                     cpu->cols, cpu->fftsPerRow, cpu->coverage * 100, d, sd);
            check(sameShape && d <= 0.05 && sd <= 0.05, what, b);
        }
    }
    std::filesystem::remove(dir + "/gpu_check.iq");
}

void checkLive() {
    std::mt19937 rng(7);
    for (SampleFormat fmt : {SampleFormat::CI16, SampleFormat::CU8, SampleFormat::CF32}) {
        for (int n : {256, 2048, 16384}) {
            auto spec = GpuLiveSpectrum::create(n, fmt);
            if (!spec) {
                check(false, "live: create GpuLiveSpectrum", gpuNotes());
                return;
            }
            const auto win = makeWindow(WindowType::BlackmanHarris, n);
            for (int frames : {1, 37, 130}) {
                const auto sig = testSignal((int64_t)frames * n, rng());
                std::vector<uint8_t> raw(sig.size() * formatBytesPerSample(fmt));
                encodeSamples(fmt, sig.data(), (int64_t)sig.size(), raw.data());
                const auto ref = liveCpu(raw, fmt, n, frames, win);
                std::vector<float> pw(n, 0.f);
                const bool ok = spec->run(raw.data(), frames, win.data(), pw.data()) == GpuResult::Ok;
                const float peak = *std::max_element(ref.begin(), ref.end());
                double d = 0;
                for (int i = 0; i < n; ++i)
                    if (ref[i] > peak * 1e-9f) d = std::max(d, std::fabs(10 * std::log10((double)pw[i] / ref[i])));
                char what[96], b[64];
                snprintf(what, sizeof what, "live %s N=%d frames=%d", fmtName(FormatParams{fmt}).c_str(), n, frames);
                snprintf(b, sizeof b, "max diff %.4f dB", d);
                check(ok && d <= 0.05, what, b);
            }
        }
    }
}

template <class F>
double best(int reps, F&& f) {
    double t = 1e9;
    for (int i = 0; i < reps; ++i) {
        const double a = now();
        f();
        t = std::min(t, now() - a);
    }
    return t;
}

void bench(const std::string& dir, bool haveGpu) {
    printf("\nbenchmark (best of 3)\n");
    const int64_t N = 1 << 26;  // 64 M ci16 samples = 256 MB
    FormatParams fp;
    fp.fmt = SampleFormat::CI16;
    auto file = writeFile(dir + "/gpu_bench.iq", testSignal(N, 3), fp);
    if (!file) return;
    printf("%-34s %10s %10s %8s %s\n", "file waterfall", "CPU", "GPU", "speedup", "coverage CPU / GPU");
    for (int nfft : {1024, 8192, 65536})
        for (int zoom : {1, 10, 100}) {
            WaterfallReq r;
            r.src.file = file;
            r.src.fp = fp;
            r.src.fs = 1e6;
            r.s0 = 0;
            r.s1 = N * zoom / 100;
            r.nfft = nfft;
            std::shared_ptr<WaterfallRes> c, g;
            const double tc = best(3, [&] { c = waterfall(r, false); });
            const double tg = haveGpu ? best(3, [&] { g = waterfall(r, true); }) : 0;
            char name[64];
            snprintf(name, sizeof name, "N=%d, %.1f M samples", nfft, r.s1 / 1048576.0);
            if (haveGpu)
                printf("%-34s %8.1f ms %8.1f ms %7.1fx %5.0f%% / %3.0f%%\n", name, tc * 1e3, tg * 1e3, tc / tg,
                       c->coverage * 100, g->coverage * 100);
            else
                printf("%-34s %8.1f ms %10s %8s %5.0f%%\n", name, tc * 1e3, "-", "-", c->coverage * 100);
        }
    std::filesystem::remove(dir + "/gpu_bench.iq");

    printf("\n%-34s %10s %10s\n", "live row (30 rows/s, N=2048)", "CPU", "GPU");
    const int n = 2048;
    const auto win = makeWindow(WindowType::BlackmanHarris, n);
    auto spec = haveGpu ? GpuLiveSpectrum::create(n, SampleFormat::CI16) : nullptr;
    for (double rate : {20e6, 40e6, 61.44e6}) {
        const int frames = (int)(rate / 30 / n);
        const auto sig = testSignal((int64_t)frames * n, 5);
        std::vector<uint8_t> raw(sig.size() * 4);
        encodeSamples(SampleFormat::CI16, sig.data(), (int64_t)sig.size(), raw.data());
        std::vector<float> pw(n);
        const double tc = best(3, [&] { liveCpu(raw, SampleFormat::CI16, n, frames, win); });
        const double tg = spec ? best(3, [&] { spec->run(raw.data(), frames, win.data(), pw.data()); }) : 0;
        char name[64];
        snprintf(name, sizeof name, "%.2f MS/s (%d FFTs per row)", rate / 1e6, frames);
        char gpuCol[32] = "         -";
        if (spec) snprintf(gpuCol, sizeof gpuCol, "%7.1f ms", tg * 1e3);
        printf("%-34s %8.1f ms %s%s\n", name, tc * 1e3, gpuCol, tc > 1.0 / 30 ? "  (CPU cannot keep up)" : "");
    }
}
}  // namespace

int main(int argc, char** argv) {
    bool doBench = false;
    int device = 0;
    std::string dir = std::filesystem::temp_directory_path().string();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bench") doBench = true;
        else if (a == "--gpu-device" && i + 1 < argc) device = atoi(argv[++i]);
        else dir = a;
    }
    std::string err;
    gpuInit(GpuMode::Auto, device, err);
    const GpuInfo& g = gpuInfo();
    if (g.available)
        printf("GPU: %s (compute %d.%d, %.1f of %.1f GB free)\n\n", g.name.c_str(), g.ccMajor, g.ccMinor,
               g.memFree / 1073741824.0, g.memTotal / 1073741824.0);
    else
        printf("GPU not available: %s. Only CPU timings are run.\n", g.reason.c_str());

    if (g.available) {
        checkWaterfalls(dir);
        checkLive();
    }
    if (doBench) bench(dir, g.available);
    if (const std::string notes = gpuNotes(); !notes.empty()) printf("\nGPU notes:\n%s\n", notes.c_str());
    if (g.available) printf("\n%s (%d failures)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
