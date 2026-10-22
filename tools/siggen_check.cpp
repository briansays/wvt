// siggen_check - verifies the signal generator: tone frequency and level,
// pulse timing (fixed, stagger, jitter, bursts), chirp sweep, Barker chips,
// edges, noise level, deferred parameter changes, file output, playback
// of recordings (looping, gaps, segments, resampling, SigMF round trip),
// CW frequency and level sweeps, and chains of recordings (order, repeats,
// gaps, levels, offsets, loops, changes while running, chain files, the
// waveform cache, the JSON reader) and the transmit limits.
//
//   siggen_check [scratch_dir]     exit status 0 = all checks passed
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

#include "dsp/dsp.h"
#include "io/iq_file.h"
#include "common/json.h"
#include "siggen/siggen.h"
#include "siggen/siggen_out.h"
#include "common/ui_util.h"

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

    // --- Schedule stays on its grid ---
    {
        // A fractional PRI (666.67 samples) must not round every interval the same way.
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.pri = 1.0 / 3000;
        auto st = pulseStarts(gen(p, 2000 * 667), 0.5 * std::pow(10.0, p.ampDb / 20));
        bool exact = st.size() >= 1999;
        for (size_t k = 0; exact && k < st.size(); ++k) exact = st[k] == std::llround((double)k * 2e6 / 3000);
        check(exact, "PRF 3 kHz at 2 MS/s: pulse k starts at round(k * 666.67), no drift",
              st.size() > 1 ? fmt("last start %g, want %g", (double)st.back(), std::round((double)(st.size() - 1) * 2e6 / 3000)) : "");

        // Unrelated changes mid-stream must not disturb a stagger or jitter sequence.
        for (PriPattern pat : {PriPattern::Stagger, PriPattern::Jitter}) {
            p = WaveformParams();
            p.mode = GenMode::Pulsed;
            p.priPattern = pat;
            const double thr = 0.5 * std::pow(10.0, p.ampDb / 20) * 0.9;
            const auto ref = pulseStarts(gen(p, 200000), thr);
            SigGen g(p);
            std::vector<cf> v(200000);
            for (int64_t done = 0, f = 0; done < 200000; done += 3000, ++f) {
                WaveformParams q = p;
                q.ampDb = p.ampDb + 0.01 * (double)(f % 2);  // as when dragging the amplitude
                q.offset = p.offset + (double)f;
                g.setParams(q);
                g.generate(v.data() + done, std::min<int64_t>(3000, 200000 - done));
            }
            check(pulseStarts(v, thr) == ref, pat == PriPattern::Stagger ? "stagger sequence unchanged by edits mid-stream"
                                                                         : "jitter sequence unchanged by edits mid-stream");
        }
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

    // --- CW sweeps ---
    {
        // Instantaneous frequency from the phase step into sample i (the step uses sample i-1's frequency).
        auto stepHz = [](const std::vector<cf>& v, size_t i, double rate) {
            return std::arg(std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1]))) * rate / (2 * M_PI);
        };
        // Largest error of the measured frequency against the sweep, over the whole of v.
        auto sweepErr = [&](const WaveformParams& p, const std::vector<cf>& v) {
            const int64_t P = p.sweepSamples();
            double e = 0;
            for (size_t i = 1; i < v.size(); ++i)
                e = std::max(e, std::fabs(stepHz(v, i, p.rate) - p.sweepOffsetAt((int64_t)(i - 1) % P)));
            return e;
        };
        WaveformParams p;
        p.rate = 2e6;
        p.fSweep = SweepShape::RampUp;
        p.fFrom = -400e3;
        p.fTo = 400e3;
        p.sweepPeriod = 1e-3;  // 2000 samples
        check(p.sweepSamples() == 2000, "sweep period 1 ms = 2000 samples");
        auto v = gen(p, 7000);
        double e = sweepErr(p, v);
        check(e < 800, "ramp up follows From -> To, wraps and repeats (3.5 periods)", fmt("worst %.3g Hz", e));
        check(std::fabs(stepHz(v, 1, p.rate) + 400e3) < 1 && std::fabs(stepHz(v, 2000, p.rate) - p.sweepOffsetAt(1999)) < 1 &&
                  std::fabs(stepHz(v, 2001, p.rate) + 400e3) < 1,
              "ramp wraps back to From on sample 2000");

        p.fSweep = SweepShape::RampDown;
        v = gen(p, 4000);
        e = sweepErr(p, v);
        check(e < 800 && stepHz(v, 1, p.rate) > 399e3, "ramp down goes To -> From", fmt("worst %.3g Hz", e));

        p.fSweep = SweepShape::Triangle;
        v = gen(p, 4001);
        e = sweepErr(p, v);
        check(e < 800 && std::fabs(stepHz(v, 1001, p.rate) - 400e3) < 1, "triangle turns around at To mid-period",
              fmt("worst %.3g Hz", e));

        p.fSweep = SweepShape::Stepped;
        p.fFrom = -100e3;
        p.fTo = 100e3;
        p.fStep = 50e3;
        p.fDwell = 100e-6;  // 200 samples
        check(p.sweepSteps() == 5 && p.sweepSamples() == 1000, "stepped: 5 steps x 200 samples");
        v = gen(p, 3000);
        e = sweepErr(p, v);
        int held = 0;
        for (size_t i = 1; i < 1001; ++i) held += std::fabs(stepHz(v, i, p.rate) - 50e3) < 1;
        check(e < 1 && held == 200, "stepped holds each frequency for one dwell", fmt("worst %.3g Hz", e));

        // The phase never jumps: the largest step is the largest frequency.
        double maxStep = 0;
        for (size_t i = 1; i < v.size(); ++i) maxStep = std::max(maxStep, std::fabs(stepHz(v, i, p.rate)));
        check(maxStep < 100e3 + 1, "stepped sweep is phase-continuous", fmt("largest step %.6g Hz", maxStep));

        // Level ramp, linear in dB, over the same period as a triangle frequency sweep.
        p.fSweep = SweepShape::Triangle;
        p.aSweep = SweepShape::RampUp;
        p.aFrom = -40;
        p.aTo = -6;
        v = gen(p, 5000);
        double le = 0;
        for (size_t i = 0; i < v.size(); ++i)
            le = std::max(le, std::fabs(20 * std::log10(std::abs(v[i])) - p.sweepLevelAt((int64_t)i % p.sweepSamples())));
        check(le < 0.05, "level ramp -40 -> -6 dBFS is linear in dB", fmt("worst %.4f dB", le));
        check(sweepErr(p, v) < 800, "frequency sweep unchanged by the level sweep");

        p.aSweep = SweepShape::Triangle;
        v = gen(p, 2000);
        check(std::fabs(20 * std::log10(std::abs(v[1000])) + 6) < 0.05 && std::fabs(20 * std::log10(std::abs(v[0])) + 40) < 0.05,
              "level triangle peaks mid-period");

        // A level sweep alone keeps the offset and the drift.
        WaveformParams q;
        q.rate = 2e6;
        q.offset = 100e3;
        q.driftHzPerSec = 1e6;
        q.aSweep = SweepShape::RampDown;
        q.sweepPeriod = 1e-3;
        v = gen(q, 3000);
        e = 0;
        for (size_t i = 1; i < v.size(); ++i)
            e = std::max(e, std::fabs(stepHz(v, i, q.rate) - (100e3 + 1e6 * (double)(i - 1) / q.rate)));
        check(e < 1, "level sweep alone: offset + drift", fmt("worst %.3g Hz", e));

        // Events: one per period, and a change restarts the period without a phase jump.
        p.aSweep = SweepShape::Off;
        p.fSweep = SweepShape::RampUp;
        p.fFrom = -400e3;
        p.fTo = 400e3;
        SigGen g(p);
        std::vector<cf> a(4700), b(1000);
        g.generate(a.data(), 4700);
        bool ev = g.lastPulses().size() == 3 && g.lastPulses()[1].start == 2000 && g.lastPulses()[2].start == 4000 &&
                  g.lastPulses()[0].len == 2000;
        check(ev, "one event per sweep period");
        WaveformParams p2 = p;
        p2.fTo = 200e3;
        g.setParams(p2);
        g.generate(b.data(), 1000);
        const double jump = std::arg(std::complex<double>(b[0]) * std::conj(std::complex<double>(a[4699]))) * p.rate / (2 * M_PI);
        check(g.lastPulses().size() == 1 && g.lastPulses()[0].start == 4700 && std::fabs(jump - p.sweepOffsetAt(699)) < 1 &&
                  std::fabs(stepHz(b, 1, p.rate) + 400e3) < 1,
              "a sweep change restarts the period, phase-continuous");

        // Checks.
        WaveformParams c = p;
        c.fTo = 1.2e6;
        check(!checkParams(c).ok(), "rejects a sweep past the band edge");
        c = p;
        c.fTo = c.fFrom;
        check(!checkParams(c).ok(), "rejects From = To");
        c = p;
        c.sweepPeriod = 0.5e-6;
        check(!checkParams(c).ok(), "rejects a period under 2 samples");
        c = p;
        c.aSweep = SweepShape::Stepped;
        check(!checkParams(c).ok(), "rejects a stepped level");
        c = p;
        c.aSweep = SweepShape::RampUp;
        c.aTo = 3;
        check(!checkParams(c).ok(), "rejects a level above 0 dBFS");
        check(checkParams(p).ok(), "accepts a -400 k to 400 k ramp at 2 MS/s");

        // File: exactly N sweeps, annotated.
        p.fSweep = SweepShape::Stepped;
        p.fFrom = -100e3;
        p.fTo = 100e3;
        FileGenJob job;
        FileGenConfig fc;
        fc.rec.prefix = dir + "/sgcheck_sweep";
        fc.pulses = 4;
        fc.annotatePulses = true;
        std::string err;
        if (job.start(p, fc, err)) {
            auto st = waitJob(job);
            check(fileSize(st.rec.dataPath) == 4 * 1000 * 8, "4 sweeps of cf32 = 32000 bytes",
                  fmt("%g bytes", (double)fileSize(st.rec.dataPath)));
            std::string meta = readFile(st.rec.metaPath);
            check(countOf(meta, "\"core:label\": \"sweep ") == 4 && meta.find("\"sweep\": {") != std::string::npos,
                  "sweep file metadata: annotations and settings");
            remove(st.rec.dataPath.c_str());
            remove(st.rec.metaPath.c_str());
        } else {
            check(false, "sweep file starts", err);
        }

        // Preview: max-hold spectrum covers the swept band.
        p.fSweep = SweepShape::RampUp;
        p.fFrom = -400e3;
        p.fTo = 400e3;
        p.fc = 0;
        p.sweepPeriod = 0.5;  // much longer than the preview blocks
        auto pr = computeSigPreview({p, false}, [] { return false; });
        int lit = 0, inside = 0;
        for (size_t i = 0; pr && i < pr->f.size(); ++i)
            if (std::fabs(pr->f[i]) < 380e3) {
                ++inside;
                lit += pr->db[i] > -60;
            }
        check(pr && pr->hold && !pr->sweepF.empty() && inside > 0 && lit > inside * 0.9,
              "preview: the max-hold spectrum covers the swept band", fmt("%g of %g bins", lit, inside));
    }

    // --- Pulsed sweeps: each pulse takes the sweep's frequency and level at its start ---
    {
        WaveformParams p;
        p.mode = GenMode::Pulsed;
        p.rate = 2e6;
        p.pri = 100e-6;  // 200 samples
        p.pw = 20e-6;    // 40 samples
        p.fSweep = SweepShape::RampUp;
        p.fFrom = -400e3;
        p.fTo = 400e3;
        p.sweepPeriod = 1e-3;  // 2000 samples: 10 pulses per sweep
        p.aSweep = SweepShape::RampUp;
        p.aFrom = -30;
        p.aTo = -6;
        check(checkParams(p).ok(), "pulsed sweep is valid");
        auto v = gen(p, 4000);
        double fe = 0, le = 0;
        for (int j = 0; j < 20; ++j) {
            const size_t a = (size_t)(200 * j);
            const int64_t k = (int64_t)(200 * j) % 2000;
            fe = std::max(fe, std::fabs(meanFreq(v, a, a + 40, p.rate) - p.sweepOffsetAt(k)));
            double pk = 0;
            for (size_t i = a; i < a + 40; ++i) pk = std::max(pk, (double)std::abs(v[i]));
            le = std::max(le, std::fabs(20 * std::log10(pk) - p.sweepLevelAt(k)));
        }
        check(fe < 1, "each pulse sits at the sweep's frequency when it starts", fmt("worst %.3g Hz", fe));
        check(le < 0.01, "each pulse has the sweep's level when it starts", fmt("worst %.3g dB", le));
        bool quiet = true;
        for (int j = 0; j < 20; ++j)
            for (size_t i = (size_t)(200 * j + 40); i < (size_t)(200 * j + 200); ++i) quiet &= std::abs(v[i]) == 0;
        check(quiet, "pulsed sweep: silence between pulses");
        // Stepped: 5 steps of 600 samples; pulses every 200 samples take 3 pulses per step.
        p.aSweep = SweepShape::Off;
        p.fSweep = SweepShape::Stepped;
        p.fFrom = -200e3;
        p.fTo = 200e3;
        p.fStep = 100e3;
        p.fDwell = 300e-6;
        v = gen(p, 3000);
        fe = 0;
        for (int j = 0; j < 15; ++j)
            fe = std::max(fe, std::fabs(meanFreq(v, (size_t)(200 * j), (size_t)(200 * j + 40), p.rate) - (-200e3 + 100e3 * (j / 3))));
        check(fe < 1, "pulsed stepped sweep: 3 pulses per step", fmt("worst %.3g Hz", fe));
        // A chirp centred on the swept frequency.
        p.intra = IntraMod::LfmUp;
        p.chirpBw = 200e3;
        v = gen(p, 1400);
        // Pulse 6 is on step 2 (0 Hz): the same as an unswept chirp at offset 0.
        WaveformParams fixed = p;
        fixed.fSweep = SweepShape::Off;
        fixed.offset = 0;
        const auto u = gen(fixed, 1400);
        const double df = meanFreq(v, 1200, 1240, p.rate) - meanFreq(u, 1200, 1240, p.rate);
        check(std::fabs(df) < 1, "a chirp is centred on the swept frequency", fmt("%.3g Hz apart", df));
        p.intra = IntraMod::None;
        WaveformParams c = p;
        c.fSweep = SweepShape::RampUp;
        c.sweepPeriod = 150e-6;
        check(!checkParams(c).warnings.empty(), "warns when the sweep is shorter than two PRIs");
        check(describeParams(p).find("stepped") != std::string::npos && paramsJson(p).find("\"per_pulse\": true") != std::string::npos,
              "pulsed sweep description and metadata");
        // Preview: the max-hold spectrum covers the swept band.
        p.fSweep = SweepShape::Triangle;
        p.fFrom = -400e3;
        p.fTo = 400e3;
        p.sweepPeriod = 0.2;
        p.fc = 0;
        auto pr = computeSigPreview({p, false}, [] { return false; });
        int lit = 0, inside = 0;
        for (size_t i = 0; pr && i < pr->f.size(); ++i)
            if (std::fabs(pr->f[i]) < 380e3) {
                ++inside;
                lit += pr->db[i] > -70;
            }
        check(pr && pr->hold && !pr->sweepF.empty() && !pr->t.empty() && lit > inside * 0.9,
              "pulsed sweep preview: the pulse, the sweep curves and a max-hold over the band", fmt("%g of %g bins", lit, inside));
    }

    // --- Chains ---
    {
        const double rate = 2e6;
        auto tone = [&](double f, double a, int n) {
            std::vector<cf> v((size_t)n);
            for (int i = 0; i < n; ++i) v[(size_t)i] = std::polar((float)a, (float)(2 * M_PI * f * i / rate));
            return v;
        };
        const std::vector<cf> A = tone(100e3, 0.5, 1000), B = tone(-200e3, 0.25, 600), C = tone(300e3, 0.8, 400);
        const std::string pa = dir + "/sgcheck_chain_a.cf32", pb = dir + "/sgcheck_chain_b.cf32", pc = dir + "/sgcheck_chain_c.cf32";
        writeCf32(pa, A);
        writeCf32(pb, B);
        writeCf32(pc, C);
        forgetPlayback();
        WaveformParams p;
        p.mode = GenMode::Chain;
        p.rate = rate;
        p.fc = 915e6;
        ChainItem ia, ib, ic;
        ia.path = pa;
        ia.gapAfter = 50e-6;  // 100 samples
        ib.path = pb;
        ib.repeats = 2;
        ic.path = pc;
        ic.gapAfter = 100e-6;  // 200 samples
        p.chain = {ia, ib, ic};
        check(checkParams(p).ok(), "a 3-item chain is valid", checkParams(p).ok() ? "" : checkParams(p).errors.front());
        // One pass: 1000 + 100, 600, 600, 400 + 200 = 2900 samples.
        SigGen g(p);
        std::vector<cf> v(2 * 2900);
        g.generate(v.data(), (int64_t)v.size());
        std::vector<int64_t> starts;
        std::vector<int> items;
        for (const auto& pl : g.lastPulses()) {
            starts.push_back(pl.start);
            items.push_back(pl.item);
        }
        check(items == std::vector<int>{0, 1, 1, 2, 0, 1, 1, 2} &&
                  starts == std::vector<int64_t>{0, 1100, 1700, 2300, 2900, 4000, 4600, 5200},
              "chain order, repeats and gaps, to the sample");
        bool quiet = true;
        for (size_t i = 1000; i < 1100; ++i) quiet &= std::abs(v[i]) == 0;
        for (size_t i = 2700; i < 2900; ++i) quiet &= std::abs(v[i]) == 0;
        check(quiet, "silence in the gaps");
        const double dA = maxDiff(v.data(), A.data(), 1000), dB = maxDiff(v.data() + 1700, B.data(), 600),
                     dC = maxDiff(v.data() + 2300, C.data(), 400);
        check(std::max({dA, dB, dC}) < 1e-6, "as recorded: the samples play unchanged", fmt("max diff %.2g", std::max({dA, dB, dC})));
        check(g.chainPasses() == 2 && !g.finished() && g.endSample() < 0, "forever: 2 passes, not finishing");

        // Level and offset per item.
        WaveformParams q = p;
        q.chain[1].keepLevel = false;
        q.chain[1].ampDb = -6;
        q.chain[2].offset = 100e3;
        v = gen(q, 2900);
        double pk = 0;
        for (size_t i = 1100; i < 1700; ++i) pk = std::max(pk, (double)std::abs(v[i]));
        check(std::fabs(20 * std::log10(pk) + 6) < 0.01, "item level: the peak plays at -6 dBFS", fmt("%.3f dBFS", 20 * std::log10(pk)));
        const double f = meanFreq(v, 2300, 2700, rate);
        check(std::fabs(f - 400e3) < 1, "item offset: +100 kHz moves the 300 kHz tone to 400 kHz", fmt("%.2f Hz", f));
        q.chain[0].offset = 1.5e6;
        check(!checkParams(q).ok(), "rejects an item offset outside the band");
        q = p;
        q.chain[0].repeats = 0;
        check(!checkParams(q).ok(), "rejects 0 plays");
        q.chain.clear();
        check(!checkParams(q).ok(), "rejects an empty chain");

        // A set number of passes ends after the last gap, then silence.
        q = p;
        q.chainLoops = 3;
        q.startDelay = 100e-6;  // 200 samples
        SigGen g3(q);
        std::vector<cf> w(12000);
        g3.generate(w.data(), (int64_t)w.size());
        bool tail = true;
        for (size_t i = 200 + 3 * 2900; i < w.size(); ++i) tail &= std::abs(w[i]) == 0;
        check(g3.finished() && g3.endSample() == 200 + 3 * 2900 && g3.chainPasses() == 3 && tail,
              "3 loops: ends after 3 passes (start delay + 3 x 2900 samples)",
              fmt("end %g, passes %g", (double)g3.endSample(), g3.chainPasses()));
        // A larger loop count revives it.
        q.chainLoops = 4;
        g3.setParams(q);
        g3.generate(w.data(), 2000);  // (the 4th pass would end 2900 samples on)
        check(!g3.finished() && g3.chainPasses() == 4, "raising the loop count plays on");

        // Reordering while an item plays: it finishes, then the new order starts.
        SigGen gr(p);
        std::vector<cf> x(1200), y(1000);
        gr.generate(x.data(), 1200);  // B started at 1100
        q = p;
        q.chain = {ic, ia, ib};
        gr.setParams(q);
        gr.generate(y.data(), 1000);
        const auto& pls = gr.lastPulses();
        check(pls.size() == 1 && pls[0].start == 1700 && pls[0].item == 0 && pls[0].len == 400 &&
                  maxDiff(y.data(), B.data() + 100, 500) < 1e-6,
              "a new order starts after the item that plays");
        // A level change applies at once.
        SigGen gl(p);
        gl.generate(x.data(), 1150);  // B plays from 1100
        q = p;
        q.chain[1].keepLevel = false;
        q.chain[1].ampDb = -20;
        gl.setParams(q);
        gl.generate(y.data(), 100);
        pk = 0;
        for (size_t i = 0; i < 100; ++i) pk = std::max(pk, (double)std::abs(y[i]));
        check(std::fabs(20 * std::log10(pk) + 20) < 0.01, "an item's level changes while it plays");

        // File: exactly 2 passes, items annotated.
        FileGenJob job;
        FileGenConfig c;
        c.rec.prefix = dir + "/sgcheck_chain";
        c.pulses = 2;
        c.annotatePulses = true;
        std::string err;
        if (job.start(p, c, err)) {
            auto st = waitJob(job);
            check(fileSize(st.rec.dataPath) == 2 * 2900 * 8, "2 passes of cf32 = 46400 bytes", fmt("%g bytes", (double)fileSize(st.rec.dataPath)));
            const std::string meta = readFile(st.rec.metaPath);
            check(countOf(meta, "\"core:label\": \"") == 8 && countOf(meta, "sgcheck_chain_b.cf32 (2/2)") == 2 &&
                      meta.find("\"mode\": \"chain\"") != std::string::npos,
                  "chain file metadata: 8 item annotations and the chain");
            remove(st.rec.dataPath.c_str());
            remove(st.rec.metaPath.c_str());
        } else {
            check(false, "chain file starts", err);
        }
        // A finite chain ends the file by itself, whatever the duration.
        q = p;
        q.chainLoops = 1;
        c.pulses = 0;
        c.seconds = 1;
        if (job.start(q, c, err)) {
            auto st = waitJob(job);
            check(fileSize(st.rec.dataPath) == 2900 * 8 && st.error.empty(), "1 loop: the file is one pass long",
                  fmt("%g bytes", (double)fileSize(st.rec.dataPath)));
            remove(st.rec.dataPath.c_str());
            remove(st.rec.metaPath.c_str());
        }

        // The streaming wrapper reports the end (the transmitter stops on it).
        sigGenBus().publish(q);
        BusSigGen bg(rate, NAN);
        std::vector<cf> z(4000);
        bg.generate(z.data(), 4000);
        check(bg.finished() && bg.chainPasses() == 1 && sigGenBus().chainFinished(), "the bus stream reports a finished chain");

        // Chain files.
        const std::string cf = dir + "/sgcheck.wvtchain.json";
        q = p;
        q.chainLoops = 5;
        q.startDelay = 0.25;
        q.chain[1].keepLevel = false;
        q.chain[1].ampDb = -12.5;
        q.chain[2].offset = -250e3;
        q.chain[0].start = 1e-4;
        q.chain[0].len = 2e-4;
        WaveformParams r;
        if (saveChain(cf, q, err) && loadChain(cf, r, err)) {
            const std::string text = readFile(cf);
            check(r.chain == q.chain && r.chainLoops == 5 && r.startDelay == 0.25 && r.rate == rate && r.fc == 915e6 &&
                      r.mode == GenMode::Chain && text.find("\"path\": \"sgcheck_chain_a.cf32\"") != std::string::npos,
                  "chain file round trip (paths stored relative to it)");
        } else {
            check(false, "chain file round trip", err);
        }
        remove(cf.c_str());
        check(!loadChain(dir + "/no-such.wvtchain.json", r, err), "a missing chain file is an error", err);

        // Bad numbers in a chain file (issue #39): a negative header put sample 0
        // before the mapped file, and casting a huge count was undefined.
        auto chainWith = [&](const std::string& item, const std::string& top) {
            std::ofstream(cf) << "{\"wvt_chain\": 1" << top << ", \"items\": [{\"path\": " << jsonQuote(pa) << item << "}]}";
            const bool ok = loadChain(cf, r, err);
            remove(cf.c_str());
            return ok;
        };
        const bool negLoads = chainWith(", \"header_bytes\": -65536", "");
        check(negLoads && r.chain[0].fp.headerBytes == -65536 && !checkParams(r).ok(),
              "chain file: a negative header loads, then checkParams() rejects it",
              negLoads && !checkParams(r).ok() ? checkParams(r).errors.front() : err);
        if (negLoads) {
            check(!loadPlayback(chainItemParams(r, 0), err), "chain file: playback refuses a negative header", err);
            forgetPlayback();
        }
        check(!chainWith(", \"repeats\": 1e30", "") && err.find("repeats must be a whole number") != std::string::npos,
              "chain file: rejects repeats 1e30", err);
        check(!chainWith(", \"header_bytes\": 1.5", "") && err.find("header_bytes") != std::string::npos,
              "chain file: rejects a fractional header", err);
        check(!chainWith("", ", \"loops\": -1e300") && err.find("loops") != std::string::npos, "chain file: rejects loops -1e300",
              err);
        {
            std::string e;
            auto f = IqFile::open(pa, e);
            FormatParams fp;
            fp.headerBytes = -65536;
            std::vector<::cf> x(64, ::cf(1, 1));
            if (f) f->read(fp, 0, (int64_t)x.size(), x.data());
            bool zero = true;
            for (const ::cf& s : x) zero &= s == ::cf(0, 0);
            check(f && f->numSamples(fp) == 0 && zero, "IqFile: a negative header gives no samples (zero-filled read)", e);
        }

        // The cache keeps waveforms in use and drops the least recently used others.
        forgetPlayback();
        setPlaybackCacheBudget(2000);
        auto held = loadPlayback(chainItemParams(p, 0), err);  // A: 1000, held
        loadPlayback(chainItemParams(p, 1), err);              // B: 600
        loadPlayback(chainItemParams(p, 2), err);              // C: 400 (2000 in all)
        WaveformParams d = chainItemParams(p, 0);
        d.playStart = 250e-6;  // A from sample 500: 500 more samples, over the budget
        auto extra = loadPlayback(d, err);
        check(held && extra && cachedPlayback(chainItemParams(p, 0)) && !cachedPlayback(chainItemParams(p, 1)) &&
                  cachedPlayback(chainItemParams(p, 2)) && playbackCacheSamples() == 1900,
              "cache budget: drops the least recently used waveform nobody holds",
              fmt("%g samples cached", (double)playbackCacheSamples()));
        setPlaybackCacheBudget(0);
        forgetPlayback();
        remove(pa.c_str());
        remove(pb.c_str());
        remove(pc.c_str());
    }

    // --- the JSON writer and the SigMF reader (issue #54) ---
    {
        check(jsonNumber(915e6) == "915000000" && jsonNumber(0.1) == "0.1" && jsonNumber(-2.5e-7) == "-2.5e-07" &&
                  jsonNumber(NAN) == "null" && jsonNumber(INFINITY) == "null",
              "jsonNumber: plain digits, no NaN or infinity",
              jsonNumber(915e6) + " " + jsonNumber(0.1) + " " + jsonNumber(-2.5e-7) + " " + jsonNumber(NAN));
        bool same = true;
        std::mt19937_64 rng(12345);
        std::uniform_real_distribution<double> mant(-1, 1);
        std::uniform_int_distribution<int> expo(-20, 20);
        for (int i = 0; i < 1000 && same; ++i) {
            const double v = mant(rng) * std::pow(10.0, expo(rng));
            JsonValue j;
            std::string e;
            same = parseJson(jsonNumber(v), j, e) && j.num == v;
        }
        check(same, "jsonNumber: 1000 values read back exactly");
        const std::string text = std::string("a \"q\" \\ / \n\t\x01 \xc2\xb5 end");
        JsonValue j;
        std::string e;
        check(parseJson("{\"k\": " + jsonQuote(text) + "}", j, e) && j.string("k") == text,
              "jsonQuote: quotes, backslash and control characters read back", e);

        const std::string meta = "/tmp/wvt_siggen_check_meta.sigmf-meta";
        auto guess = [&](const std::string& body) {
            std::ofstream(meta) << body;
            return guessMeta(meta);
        };
        FileMetaGuess g = guess(R"({"global": {"core:datatype": "ci16_le", "core:sample_rate": 2.4e6},
            "captures": [{"core:sample_start": 0, "core:frequency": 915e6, "core:datetime": "2026-10-21T12:00:00Z"},
                         {"core:sample_start": 100, "core:frequency": 433e6}],
            "annotations": [{"core:sample_start": 0, "core:frequency": 1}]})");
        check(g.haveFmt && g.fmt == SampleFormat::CI16 && g.haveRate && g.rate == 2.4e6 && g.haveFc && g.fc == 915e6 &&
                  g.startTime == parseIsoTime("2026-10-21T12:00:00Z") && g.source == "SigMF metadata",
              "SigMF: global datatype and rate, frequency and time of the first capture",
              fmt("rate %g fc %g", g.rate, g.fc));
        g = guess(R"({"global": {"core:sample_rate": "fast"}, "captures": []})");
        check(!g.haveRate && !g.haveFc && g.source.rfind("SigMF metadata", 0) == 0, "SigMF: a string rate and no captures give no values");
        check(!g.haveFmt && g.badType.empty() && g.source.find("cf32 assumed") != std::string::npos,
              "SigMF: no datatype says cf32 is assumed", g.source);

        // Datatypes wvt cannot read are refused, not shown as cf32 (issue #44).
        std::string e2;
        bool refused = true;
        for (const char* t : {"ri16_le", "rf32_le", "cu16_le", "ci64_le", "cf16_le"}) {
            g = guess(std::string(R"({"global": {"core:datatype": ")") + t + R"(", "core:sample_rate": 1e6}})");
            refused = refused && !g.haveFmt && g.badType == t && !metaReadable(g, e2) && e2.find(t) != std::string::npos;
        }
        check(refused, "SigMF: real and unknown datatypes are refused", e2);
        g = guess(R"({"global": {"core:datatype": "cf32_be"}})");
        check(g.haveFmt && g.fmt == SampleFormat::CF32 && g.bigEndian && metaReadable(g, e2), "SigMF: cf32_be is read");
        g = guess("{\"global\": {\"core:sample_rate\": 2e6");
        check(g.source != "SigMF metadata", "SigMF: invalid JSON falls back to the file name", g.source);
        remove(meta.c_str());
    }

    // --- the recorder never replaces an existing .sigmf-meta (issue #47) ---
    {
        const std::string base = dir + "/sgcheck_excl";
        remove((base + ".sigmf-data").c_str());
        std::ofstream(base + ".sigmf-meta") << "{\"old\": true}";
        RecordConfig rc;
        rc.exactPath = base;
        RecordSource src;
        src.rate = 1e6;
        src.fc = 915e6;
        Recorder rec;
        std::string err;
        const bool started = rec.start(std::make_shared<SampleRing>(1 << 16, 4), src, rc, err);
        rec.stop();
        std::ifstream in(base + ".sigmf-meta");
        const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        struct stat st{};
        check(!started && kept == "{\"old\": true}" && stat((base + ".sigmf-data").c_str(), &st) != 0 &&
                  err.find(".sigmf-meta") != std::string::npos,
              "recorder: an existing .sigmf-meta is kept, nothing is recorded", err);
        remove((base + ".sigmf-meta").c_str());
        remove(base.c_str());
        // Without the old file it starts, and writes both.
        const bool ok = rec.start(std::make_shared<SampleRing>(1 << 16, 4), src, rc, err);
        rec.stop();
        check(ok && stat((base + ".sigmf-meta").c_str(), &st) == 0 && st.st_size > 20 &&
                  stat((base + ".sigmf-data").c_str(), &st) == 0,
              "recorder: a new name records data and metadata", err);
        remove((base + ".sigmf-meta").c_str());
        remove((base + ".sigmf-data").c_str());
    }

    // --- JSON reader ---
    {
        JsonValue v;
        std::string err;
        const bool ok = parseJson(R"({"a": [1, -2.5e3, true, null, {"b": "x\"y\\z\né"}], "c": {}, "d": []})", v, err);
        const JsonValue* a = v.get("a");
        check(ok && a && a->arr.size() == 5 && a->arr[1].num == -2500 && a->arr[2].b && a->arr[3].isNull() &&
                  a->arr[4].string("b") == "x\"y\\z\n\xc3\xa9" && v.get("c") && v.get("d"),
              "JSON: arrays, numbers, escapes, unicode", err);
        check(!parseJson("{\"a\": [1, 2}", v, err) && err.find("line 1") != std::string::npos, "JSON: reports errors", err);
        check(jsonQuote("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"", "JSON: quoting");
    }

    // --- SI numbers (issue #46): a unit may follow, other text may not ---
    {
        struct Case {
            const char* s;
            bool ok;
            double v;
        };
        const Case cases[] = {{"2.4M", true, 2.4e6},   {"2.4 MHz", true, 2.4e6}, {"10 MS/s", true, 10e6},
                              {"-6 dBFS", true, -6},    {"5u", true, 5e-6},       {"5\xc2\xb5s", true, 5e-6},
                              {" 1k ", true, 1e3},      {"20%", true, 20},        {"1e3", true, 1e3},
                              {"", false, 0},           {"abc", false, 0},        {"12abc3", false, 0},
                              {"1.5.2", false, 0},      {"4:3", false, 0},        {"2.4Mx3", false, 0},
                              {"nan", false, 0},        {"inf", false, 0},        {"1e999", false, 0}};
        bool all = true;
        std::string bad;
        for (const Case& c : cases) {
            double v = 0;
            const bool ok = parseSI(c.s, v);
            if (ok != c.ok || (ok && std::fabs(v - c.v) > 1e-9 * std::fabs(c.v))) {
                all = false;
                bad += std::string(" '") + c.s + "'";
            }
        }
        check(all, "parseSI: units accepted, trailing text refused", bad);
    }

    // --- transmit limits (issue #49) ---
    {
        std::vector<TxBand> b;
        std::string err;
        check(parseTxBands("902M:928M,2.4G:2.4835G", b, err) && b.size() == 2 && b[0].lo == 902e6 && b[0].hi == 928e6 &&
                  b[1].lo == 2.4e9 && b[1].hi == 2.4835e9,
              "TX limits: two bands", err);
        bool allBad = true;
        for (const char* s : {"", "902M", "928M:902M", "a:b", "902M:928M,", "-1M:1M"}) allBad = allBad && !parseTxBands(s, b, err);
        check(allBad, "TX limits: bad bands are refused");

        TxLimits l;
        check(txFreqAllowed(l, 6e9, 50e6, err) && txGainAllowed(l, 89, err) && describeTxLimits(l) == "none",
              "TX limits: none set, nothing refused");
        l.maxGain = 30;
        parseTxBands("902M:928M,2.4G:2.4835G", l.allow, err);
        check(txGainAllowed(l, 30, err) && !txGainAllowed(l, 30.25, err), "TX limits: gain at and above the maximum", err);
        check(txFreqAllowed(l, 915e6, 1e6, err) && txFreqAllowed(l, 902.5e6, 1e6, err) && txFreqAllowed(l, 2.44e9, 20e6, err),
              "TX limits: bands inside, at the edge, in the second band", err);
        check(!txFreqAllowed(l, 927.8e6, 1e6, err) && !txFreqAllowed(l, 1.5e9, 1e6, err) && !txFreqAllowed(l, 915e6, 30e6, err),
              "TX limits: a band that crosses an edge, outside, wider than the band", err);
        check(describeTxLimits(l) == "max gain 30.00 dB; 902 MHz to 928 MHz, 2.4 GHz to 2.4835 GHz", "TX limits: description",
              describeTxLimits(l));

        setenv("WVT_TX_MAX_GAIN", "12.5", 1);
        setenv("WVT_TX_ALLOW", "1G:2G", 1);
        TxLimits e;
        const bool envOk = txLimitsFromEnv(e, err) && e.maxGain == 12.5 && e.allow.size() == 1 && e.allow[0].hi == 2e9;
        setenv("WVT_TX_ALLOW", "2G:1G", 1);
        const bool envBad = !txLimitsFromEnv(e, err) && err.find("WVT_TX_ALLOW") != std::string::npos;
        unsetenv("WVT_TX_MAX_GAIN");
        unsetenv("WVT_TX_ALLOW");
        check(envOk && envBad, "TX limits: from the environment", err);

        // TxEngine refuses before it opens a device, so this needs no USRP.
        WaveformParams p;
        p.rate = 1e6;
        sigGenBus().publish(p);
        TxEngine tx;
        TxConfig c;
        c.fc = 915e6;
        c.rate = 1e6;
        c.limits = std::move(l);
        c.gain = 40;
        const bool gainRefused = !tx.start(c, err) && err.find("limit") != std::string::npos;
        c.gain = 10;
        c.fc = 950e6;
        check(gainRefused && !tx.start(c, err) && err.find("allowed band") != std::string::npos && !tx.running(),
              "TX limits: the transmit engine refuses gain and frequency", err);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
