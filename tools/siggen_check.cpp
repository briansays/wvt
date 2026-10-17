// siggen_check - verifies the signal generator: tone frequency and level,
// pulse timing (fixed, stagger, jitter, bursts), chirp sweep, Barker chips,
// edges, noise level, deferred parameter changes, file output and playback
// of recordings (looping, gaps, segments, resampling, SigMF round trip).
//
//   siggen_check [scratch_dir]     exit status 0 = all checks passed
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "dsp.h"
#include "iq_file.h"
#include "siggen.h"
#include "siggen_out.h"

namespace {
int failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++failures;
}

std::string fmt(const char* f, double a, double b = 0) {
    char buf[128];
    snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

std::vector<cf> gen(const WaveformParams& p, int64_t n) {
    SigGen g(p);
    std::vector<cf> v((size_t)n);
    g.generate(v.data(), n);
    for (const cf& x : v)
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            check(false, "all samples finite", describeParams(p));
            break;
        }
    return v;
}

// Rising edges of |x| through half of the peak amplitude.
std::vector<int64_t> pulseStarts(const std::vector<cf>& v, double amp) {
    std::vector<int64_t> s;
    bool on = false;
    for (size_t i = 0; i < v.size(); ++i) {
        bool now = std::abs(v[i]) > amp / 2;
        if (now && !on) s.push_back((int64_t)i);
        on = now;
    }
    return s;
}

int64_t widthAt(const std::vector<cf>& v, int64_t start, double amp) {
    int64_t i = start;
    while (i < (int64_t)v.size() && std::abs(v[(size_t)i]) > amp / 2) ++i;
    return i - start;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int64_t fileSize(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
}

void writeCf32(const std::string& path, const std::vector<cf>& v) {
    std::vector<uint8_t> raw(v.size() * 8);
    encodeSamples(SampleFormat::CF32, v.data(), (int64_t)v.size(), raw.data());
    std::ofstream(path, std::ios::binary).write((const char*)raw.data(), (std::streamsize)raw.size());
}

double maxDiff(const cf* a, const cf* b, int64_t n, float scale = 1) {
    double d = 0;
    for (int64_t i = 0; i < n; ++i) d = std::max(d, (double)std::abs(a[i] - b[i] * scale));
    return d;
}

// Mean frequency from the phase steps of v[a, b).
double meanFreq(const std::vector<cf>& v, size_t a, size_t b, double rate) {
    std::complex<double> acc = 0;
    for (size_t i = a + 1; i < b; ++i) acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1]));
    return std::arg(acc) * rate / (2 * M_PI);
}

// Hann-windowed power of the tone at f in v[a, b), relative to a full-scale tone.
double toneDb(const std::vector<cf>& v, size_t a, size_t b, double f, double rate) {
    std::complex<double> acc = 0;
    double wsum = 0;
    const double n = (double)(b - a);
    for (size_t i = a; i < b; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (double)(i - a) / n);
        acc += w * std::complex<double>(v[i]) * std::polar(1.0, -2 * M_PI * f * (double)i / rate);
        wsum += w;
    }
    return 20 * std::log10(std::abs(acc) / wsum + 1e-30);
}

int countOf(const std::string& s, const std::string& needle) {
    int n = 0;
    for (size_t i = s.find(needle); i != std::string::npos; i = s.find(needle, i + 1)) ++n;
    return n;
}

FileGenJob::Status waitJob(FileGenJob& job) {
    while (job.status().active) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return job.status();
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";

    // --- CW tone frequency and level ---
    {
        WaveformParams p;
        p.rate = 2e6;
        p.offset = 123.4e3;
        p.ampDb = -6;
        const int n = 65536;
        auto v = gen(p, n);
        double peak = 0;
        for (auto& x : v) peak = std::max(peak, (double)std::abs(x));
        check(std::fabs(20 * std::log10(peak) - p.ampDb) < 0.01, "CW level", fmt("%.3f dBFS", 20 * std::log10(peak)));
        // Phase slope gives the frequency exactly.
        double sum = 0;
        for (int i = 1; i < n; ++i) sum += std::arg(v[i] * std::conj(v[i - 1]));
        double f = sum / (n - 1) * p.rate / (2 * M_PI);
        check(std::fabs(f - p.offset) < 0.01, "CW frequency", fmt("%.4f Hz", f));
        // Spectral purity: FFT peak bin vs the largest other bin away from the tone.
        std::vector<cf> w(v.begin(), v.begin() + 8192);
        auto win = makeWindow(WindowType::BlackmanHarris, 8192);
        for (int i = 0; i < 8192; ++i) w[i] *= win[i];
        getFFT(8192)->forward(w.data());
        int kpk = 0;
        for (int i = 0; i < 8192; ++i)
            if (std::abs(w[i]) > std::abs(w[kpk])) kpk = i;
        double spur = 0;
        for (int i = 0; i < 8192; ++i)
            if (std::abs(i - kpk) > 8 && std::abs(i - kpk) < 8192 - 8) spur = std::max(spur, (double)std::abs(w[i]));
        double sfdr = 20 * std::log10(std::abs(w[kpk]) / spur);
        check(sfdr > 90, "CW spurious-free dynamic range", fmt("%.1f dB", sfdr));
        check(std::fabs(kpk * p.rate / 8192 - p.offset) < p.rate / 8192, "CW FFT peak bin");
    }

    // --- CW stays phase-continuous across a frequency change ---
    {
        WaveformParams p;
        p.offset = 50e3;
        SigGen g(p);
        std::vector<cf> a(1000), b(1000);
        g.generate(a.data(), 1000);
        p.offset = 80e3;
        g.setParams(p);
        g.generate(b.data(), 1000);
        double jump = std::fabs(std::arg(b[0] * std::conj(a[999])));
        check(jump < 2 * M_PI * 80e3 / 2e6 + 1e-3, "CW retune is phase-continuous", fmt("step %.4f rad", jump));
    }

    // --- Pulsed, fixed PRI ---
    const double amp = std::pow(10.0, -6 / 20.0);
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.rate = 2e6;
        p.pri = 1e-3;
        p.pw = 10e-6;
        p.startDelay = 100e-6;
        auto v = gen(p, 21000);  // 10.5 ms
        auto s = pulseStarts(v, amp);
        bool spacing = s.size() == 11;
        for (size_t i = 1; i < s.size(); ++i) spacing &= s[i] - s[i - 1] == 2000;
        check(spacing, "fixed PRI: 11 pulses 2000 samples apart", fmt("%g pulses", (double)s.size()));
        check(!s.empty() && s[0] == 200, "start delay 100 us = 200 samples", s.empty() ? "" : fmt("%g", (double)s[0]));
        check(!s.empty() && widthAt(v, s[0], amp) == 20, "pulse width 10 us = 20 samples",
              s.empty() ? "" : fmt("%g", (double)widthAt(v, s[0], amp)));
        bool quiet = true;
        for (int64_t i = s[0] + 20; i < s[1]; ++i) quiet &= std::abs(v[(size_t)i]) == 0.f;
        check(quiet, "silence between pulses");
    }

    // --- Stagger ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.priPattern = PriPattern::Stagger;
        p.stagger = {1e-3, 1.25e-3, 0.8e-3};
        auto v = gen(p, 2e6 * 0.012);
        auto s = pulseStarts(v, amp);
        const int64_t want[] = {2000, 2500, 1600};
        bool ok = s.size() > 6;
        for (size_t i = 1; i < s.size(); ++i) ok &= s[i] - s[i - 1] == want[(i - 1) % 3];
        check(ok, "stagger 1 / 1.25 / 0.8 ms cycles in order");
    }

    // --- Jitter: bounded, repeatable for a seed ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.priPattern = PriPattern::Jitter;
        p.jitterPct = 10;
        p.seed = 7;
        auto a = pulseStarts(gen(p, 2e6 * 0.05), amp);
        auto b = pulseStarts(gen(p, 2e6 * 0.05), amp);
        p.seed = 8;
        auto c = pulseStarts(gen(p, 2e6 * 0.05), amp);
        bool bounded = a.size() > 40;
        int distinct = 0;
        for (size_t i = 1; i < a.size(); ++i) {
            int64_t d = a[i] - a[i - 1];
            bounded &= d >= 1800 - 1 && d <= 2200 + 1;
            distinct += d != 2000;
        }
        check(bounded && distinct > 30, "jitter +-10% stays in 1800-2200 samples and varies");
        check(a == b, "same seed repeats the jitter sequence");
        check(a != c, "different seed gives a different sequence");
    }

    // --- Bursts ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.burstPulses = 3;
        p.burstGap = 5e-3;
        auto s = pulseStarts(gen(p, 2e6 * 0.03), amp);
        const std::vector<int64_t> want{0, 2000, 4000, 16000, 18000, 20000, 32000, 34000, 36000, 48000, 50000, 52000};
        check(s == want, "bursts of 3 with a 5 ms gap");
    }

    // --- LFM chirp sweeps the set bandwidth ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.rate = 4e6;
        p.offset = 0;
        p.pw = 100e-6;
        p.intra = IntraMod::LfmUp;
        p.chirpBw = 1e6;
        auto v = gen(p, 400);
        auto fAt = [&](int i) { return std::arg(v[i + 1] * std::conj(v[i])) * p.rate / (2 * M_PI); };
        double f0 = fAt(2), f1 = fAt(396);
        check(std::fabs(f0 + 0.5e6) < 10e3 && std::fabs(f1 - 0.5e6) < 10e3, "LFM up-chirp sweeps -500 kHz to +500 kHz",
              fmt("%.0f -> %.0f Hz", f0, f1));
        p.intra = IntraMod::LfmDown;
        v = gen(p, 400);
        check(fAt(2) > 0.45e6 && fAt(396) < -0.45e6, "LFM down-chirp sweeps +500 kHz to -500 kHz");
        p.intra = IntraMod::LfmTriangle;
        v = gen(p, 400);
        check(fAt(2) < -0.45e6 && std::fabs(fAt(199)) < 0.5e6 && fAt(198) > 0.45e6 && fAt(396) < -0.45e6,
              "LFM triangle goes up then down");
    }

    // --- Barker-13 chips ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.rate = 1e6;
        p.offset = 0;
        p.pw = 130e-6;
        p.intra = IntraMod::Barker;
        p.barkerLen = 13;
        auto v = gen(p, 130);
        const char* code = "+++++--++-+-+";
        bool ok = true;
        for (int c = 0; c < 13; ++c) ok &= code[c] == '+' ? v[c * 10 + 5].real() > 0.4f : v[c * 10 + 5].real() < -0.4f;
        check(ok, "Barker-13 chip signs");
    }

    // --- Edges ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.pw = 20e-6;
        p.edge = EdgeShape::RaisedCos;
        p.rise = 5e-6;
        p.fall = 5e-6;
        auto v = gen(p, 40);
        double a0 = std::abs(v[0]) / amp, aMid = std::abs(v[20]) / amp, aEnd = std::abs(v[39]) / amp;
        check(a0 < 0.05 && std::fabs(aMid - 1) < 1e-4 && aEnd < 0.05, "raised-cosine edges ramp from ~0 to full",
              fmt("first %.3f, last %.3f", a0, aEnd));
    }

    // --- Noise level ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.pri = 10e-3;
        p.pw = 10e-6;
        p.noise = true;
        p.snrDb = 20;
        p.startDelay = 1e-3;
        auto v = gen(p, 2000);  // all before the first pulse: noise only
        double pw = 0;
        for (auto& x : v) pw += std::norm(x);
        pw /= (double)v.size();
        double snr = 10 * std::log10(amp * amp / pw);
        check(std::fabs(snr - 20) < 0.5, "SNR 20 dB", fmt("measured %.2f dB", snr));
    }

    // --- A change arriving mid-pulse waits for the pulse to end ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.pw = 50e-6;  // 100 samples
        SigGen g(p);
        std::vector<cf> v(4000);
        g.generate(v.data(), 50);
        p.pw = 20e-6;
        g.setParams(p);
        g.generate(v.data() + 50, 3950);
        auto s = pulseStarts(v, amp);
        check(s.size() == 2 && widthAt(v, s[0], amp) == 100 && widthAt(v, s[1], amp) == 40,
              "mid-pulse change applies from the next pulse");
    }

    // --- Validation ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.pw = 2e-3;
        check(!checkParams(p).ok(), "rejects pulse width >= PRI");
        p.pw = 0.1e-6;
        check(!checkParams(p).ok(), "rejects pulse width < 1 sample");
        p.pw = 10e-6;
        p.intra = IntraMod::Barker;
        p.barkerLen = 6;
        check(!checkParams(p).ok(), "rejects Barker length 6");
        p.barkerLen = 13;
        check(checkParams(p).ok(), "accepts Barker-13 in a 20-sample pulse");
        p.offset = 1.5e6;
        check(!checkParams(p).ok(), "rejects offset outside the band");
    }

    // --- Encode / decode round trip ---
    {
        std::vector<cf> in{{0.5f, -0.25f}, {1.f, -1.f}, {0.f, 0.f}};
        bool ok = true;
        for (SampleFormat f : {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CI8, SampleFormat::CU8}) {
            std::vector<uint8_t> raw(in.size() * formatBytesPerSample(f));
            encodeSamples(f, in.data(), (int64_t)in.size(), raw.data());
            std::vector<cf> out(in.size());
            FormatParams fp;
            fp.fmt = f;
            decodeSamples(fp, raw.data(), (int64_t)in.size(), out.data());
            for (size_t i = 0; i < in.size(); ++i) ok &= std::abs(out[i] - in[i]) < 0.02f;
        }
        check(ok, "encode/decode round trip (cf32, ci16, ci8, cu8)");
    }

    // --- File output ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.fc = 915e6;
        FileGenConfig c;
        c.rec.prefix = dir + "/sgcheck";
        c.rec.sigmf = true;
        c.fmt = SampleFormat::CI16;
        c.pulses = 5;
        c.annotatePulses = true;
        FileGenJob job;
        std::string err;
        bool started = job.start(p, c, err);
        check(started, "SigMF pulse file starts", err);
        if (started) {
            auto st = waitJob(job);
            check(st.error.empty(), "SigMF pulse file finishes", st.error);
            check(fileSize(st.rec.dataPath) == 5 * 2000 * 4, "5 PRIs of ci16 = 40000 bytes",
                  fmt("%g bytes", (double)fileSize(st.rec.dataPath)));
            std::string meta = readFile(st.rec.metaPath);
            check(countOf(meta, "\"core:label\": \"pulse ") == 5, "5 pulse annotations");
            check(meta.find("\"core:datatype\": \"ci16_le\"") != std::string::npos &&
                      meta.find("\"core:frequency\": 915000000") != std::string::npos &&
                      meta.find("\"wvt:siggen\": {\"mode\": \"pulsed\"") != std::string::npos,
                  "metadata has datatype, frequency and generator settings");
            auto g = guessMeta(st.rec.metaPath);
            check(g.haveRate && g.rate == 2e6 && g.haveFc && g.fc == 915e6, "wvt reads the file back");
            remove(st.rec.dataPath.c_str());
            remove(st.rec.metaPath.c_str());
        }
        p.mode = GenMode::CW;
        c.rec.sigmf = false;
        c.fmt = SampleFormat::CU8;
        c.pulses = 0;
        c.seconds = 0.25;
        started = job.start(p, c, err);
        if (started) {
            auto st = waitJob(job);
            check(fileSize(st.rec.dataPath) == 500000 * 2, "raw cu8 0.25 s = 1 MB", st.rec.dataPath);
            auto g = guessMeta(st.rec.dataPath);
            check(g.haveFmt && g.fmt == SampleFormat::CU8 && g.haveRate && g.rate == 2e6, "raw name carries format and rate");
            remove(st.rec.dataPath.c_str());
        } else {
            check(false, "raw file starts", err);
        }
    }

    // --- Playback ---
    {
        // A 50 kHz tone whose amplitude ramps 0.2 -> 0.8, so every sample is distinct.
        const int64_t L = 10000;
        std::vector<cf> x((size_t)L);
        for (int64_t k = 0; k < L; ++k)
            x[(size_t)k] = (float)(0.2 + 0.6 * (double)k / L) * std::polar(1.f, (float)(2 * M_PI * 50e3 * (double)k / 1e6));
        float peak = 0;
        for (const cf& v : x) peak = std::max(peak, std::abs(v));
        const std::string rampPath = dir + "/sgcheck_ramp.cf32";
        writeCf32(rampPath, x);

        WaveformParams p;
        p.mode = GenMode::Playback;
        p.rate = 1e6;
        p.offset = 0;
        p.ampDb = 0;
        p.playPath = rampPath;
        std::string err;
        check(checkParams(p).ok(), "playback params valid");
        check(loadPlayback(p, err) && cachedPlayback(p), "recording loads and is cached", err);

        auto y = gen(p, 2 * L + 100);
        check(maxDiff(y.data(), x.data(), L, 1 / peak) < 1e-5, "plays the recording sample for sample, peak scaled to amplitude");
        check(maxDiff(y.data() + L, x.data(), L, 1 / peak) < 1e-5, "loops without a seam");

        p.playGap = 1e-3;
        {
            SigGen g(p);
            std::vector<cf> buf(4096);
            std::vector<int64_t> starts;
            double gapMax = 0;
            for (int64_t done = 0; done < 3 * (L + 1000); done += 4096) {
                g.generate(buf.data(), 4096);
                for (const auto& pl : g.lastPulses()) starts.push_back(pl.start);
                for (int64_t k = 0; k < 4096; ++k)
                    if ((done + k) % (L + 1000) >= L) gapMax = std::max(gapMax, (double)std::abs(buf[(size_t)k]));
            }
            check(starts.size() >= 3 && starts[0] == 0 && starts[1] == L + 1000 && starts[2] == 2 * (L + 1000),
                  "repeats every recording + 1 ms gap");
            check(gapMax == 0, "silent in the gaps");
        }

        p.playGap = 0;
        p.playLoop = false;
        p.startDelay = 2e-3;
        y = gen(p, 3 * L);
        double around = 0;
        for (int64_t k = 0; k < 2000; ++k) around = std::max(around, (double)std::abs(y[(size_t)k]));
        for (int64_t k = 2000 + L; k < 3 * L; ++k) around = std::max(around, (double)std::abs(y[(size_t)k]));
        check(around == 0 && maxDiff(y.data() + 2000, x.data(), L, 1 / peak) < 1e-5, "plays once after the start delay");

        p.playLoop = true;
        p.startDelay = 0;
        p.offset = 100e3;
        y = gen(p, L);
        const double f = meanFreq(y, 0, (size_t)L, 1e6);
        check(std::fabs(f - 150e3) < 1, "offset shifts the recording (50 + 100 = 150 kHz)", fmt("%.2f Hz", f));

        p.offset = 0;
        p.playStart = 2e-3;
        p.playLen = 3e-3;
        {
            SigGen g(p);
            y.assign(3000, {});
            g.generate(y.data(), 3000);
            const float segPeak = std::abs(x[4999]);
            check(g.lastPulses().size() == 1 && g.lastPulses()[0].len == 3000 &&
                      maxDiff(y.data(), x.data() + 2000, 3000, 1 / segPeak) < 1e-5,
                  "segment plays samples 2000-4999");
        }
        p.playStart = 20e-3;
        {
            SigGen g(p);
            y.assign(1000, {});
            g.generate(y.data(), 1000);
            double mx = 0;
            for (const cf& v : y) mx = std::max(mx, (double)std::abs(v));
            check(!g.error().empty() && mx == 0, "a segment past the end is an error, and silent", g.error());
        }
        p.playStart = 0;
        p.playLen = 0;
        p.playPath = dir + "/does_not_exist.cf32";
        check(!loadPlayback(p, err), "a missing file is an error", err);
        p.playPath.clear();
        check(!checkParams(p).ok(), "playback without a file is refused");

        // Resampling: 1 MS/s -> 2.5 MS/s keeps a 100 kHz tone clean.
        std::vector<cf> t(20000);
        for (size_t k = 0; k < t.size(); ++k) t[k] = 0.5f * std::polar(1.f, (float)(2 * M_PI * 100e3 * (double)k / 1e6));
        const std::string tonePath = dir + "/sgcheck_tone.cf32";
        writeCf32(tonePath, t);
        p.playPath = tonePath;
        p.playRate = 1e6;
        p.rate = 2.5e6;
        p.ampDb = -6;
        err.clear();
        auto w = loadPlayback(p, err);
        check(w && w->s.size() == 50000, "resampled length 20000 x 2.5 = 50000", err);
        y = gen(p, 50000);
        {
            const size_t a = 5000, b = 45000;  // away from the ends
            const double fu = meanFreq(y, a, b, 2.5e6);
            // Fit the ideal tone and measure what's left.
            std::complex<double> c = 0;
            for (size_t k = a; k < b; ++k) c += std::complex<double>(y[k]) * std::polar(1.0, -2 * M_PI * 100e3 * (double)k / 2.5e6);
            c /= (double)(b - a);
            double e = 0, s2 = 0;
            for (size_t k = a; k < b; ++k) {
                const auto ref = c * std::polar(1.0, 2 * M_PI * 100e3 * (double)k / 2.5e6);
                e += std::norm(std::complex<double>(y[k]) - ref);
                s2 += std::norm(ref);
            }
            const double evm = 10 * std::log10(e / s2);
            check(std::fabs(fu - 100e3) < 1 && evm < -70, "upsampled tone: 100 kHz, error < -70 dB",
                  fmt("%.2f Hz, %.1f dB", fu, evm));
            check(std::fabs(20 * std::log10(std::abs(c)) + 6) < 0.1, "upsampled tone level -6 dBFS",
                  fmt("%.2f dBFS", 20 * std::log10(std::abs(c))));
        }
        // 1 MS/s -> 400 kS/s: the 100 kHz tone stays, a 300 kHz tone (outside +-200 kHz) is filtered, not aliased.
        for (size_t k = 0; k < t.size(); ++k)
            t[k] = 0.4f * (std::polar(1.f, (float)(2 * M_PI * 100e3 * (double)k / 1e6)) +
                           std::polar(1.f, (float)(2 * M_PI * 300e3 * (double)k / 1e6)));
        writeCf32(tonePath, t);
        forgetPlayback();
        p.rate = 400e3;
        p.ampDb = 0;
        check(!checkParams(p).warnings.empty(), "downsampling warns that out-of-band content is filtered");
        y = gen(p, 8000);
        {
            const double in = toneDb(y, 1000, 7000, 100e3, 400e3), alias = toneDb(y, 1000, 7000, -100e3, 400e3);
            check(in - alias > 70, "downsampled: 300 kHz tone suppressed > 70 dB (no alias at -100 kHz)",
                  fmt("tone %.1f dB, alias %.1f dB", in, alias));
        }

        // Round trip: a generated SigMF file plays back exactly as generated.
        WaveformParams q;
        q.mode = GenMode::Pulsed;
        q.intra = IntraMod::LfmUp;
        q.fc = 433e6;
        FileGenConfig c;
        c.rec.prefix = dir + "/sgcheck_rt";
        c.pulses = 3;
        FileGenJob job;
        if (job.start(q, c, err)) {
            auto st = waitJob(job);
            auto g = guessMeta(st.rec.metaPath);
            WaveformParams r;
            r.mode = GenMode::Playback;
            r.playPath = g.dataPath;
            r.playFp.fmt = g.fmt;
            r.playRate = g.rate;
            r.rate = g.rate;
            r.fc = g.fc;
            r.offset = 0;
            r.ampDb = q.ampDb;
            auto orig = gen(q, 3 * 2000);
            y = gen(r, 3 * 2000);
            check(maxDiff(y.data(), orig.data(), 6000) < 1e-5, "a generated SigMF file plays back as generated");

            // File output from playback: whole plays, annotated.
            r.playGap = 0.5e-3;
            FileGenConfig c2;
            c2.rec.prefix = dir + "/sgcheck_pb";
            c2.pulses = 2;
            c2.annotatePulses = true;
            if (job.start(r, c2, err)) {
                auto st2 = waitJob(job);
                check(fileSize(st2.rec.dataPath) == 2 * (6000 + 1000) * 8, "2 plays + gaps of cf32 = 112000 bytes",
                      fmt("%g bytes", (double)fileSize(st2.rec.dataPath)));
                std::string meta = readFile(st2.rec.metaPath);
                check(countOf(meta, "\"core:label\": \"playback ") == 2 &&
                          meta.find("\"mode\": \"playback\", ") != std::string::npos &&
                          meta.find("\"source_file\": ") != std::string::npos,
                      "playback file metadata: annotations and source");
                remove(st2.rec.dataPath.c_str());
                remove(st2.rec.metaPath.c_str());
            } else {
                check(false, "playback file starts", err);
            }
            r.playLoop = false;
            r.startDelay = 1e-3;
            if (job.start(r, c2, err)) {
                auto st2 = waitJob(job);
                check(fileSize(st2.rec.dataPath) == (2000 + 6000) * 8, "play once: delay + recording, whatever the count",
                      fmt("%g bytes", (double)fileSize(st2.rec.dataPath)));
                remove(st2.rec.dataPath.c_str());
                remove(st2.rec.metaPath.c_str());
            }
            remove(st.rec.dataPath.c_str());
            remove(st.rec.metaPath.c_str());
        } else {
            check(false, "round-trip file starts", err);
        }
        remove(rampPath.c_str());
        remove(tonePath.c_str());
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
