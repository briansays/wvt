// siggen_check - verifies the signal generator: tone frequency and level,
// pulse timing (fixed, stagger, jitter, bursts), chirp sweep, Barker chips,
// edges, noise level, deferred parameter changes and file output.
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

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
