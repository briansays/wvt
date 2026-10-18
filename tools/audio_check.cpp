// Audio demodulator self-test: synthesizes AM / FM / SSB / CW signals at
// known offsets, demodulates them in blocks of random size, and checks the
// audio tone frequency, its SNR, and the output length. Also checks the WAV
// writer and squelch. Exit status is nonzero on failure.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "audio_dsp.h"
#include "wav_writer.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Tone {
    double freq = 0, snrDb = 0, rms = 0;
};

// Strongest tone in the second half of `a` (rate fs) and its SNR.
Tone analyze(const std::vector<float>& a, double fs) {
    const int n = 32768;
    Tone t;
    if ((int)a.size() < n + 1000) return t;
    std::vector<cf> buf(n);
    const auto win = makeWindow(WindowType::BlackmanHarris, n);
    const size_t s0 = a.size() - n;
    double sq = 0;
    for (int i = 0; i < n; ++i) {
        buf[i] = cf(a[s0 + i] * win[i], 0);
        sq += a[s0 + i] * a[s0 + i];
    }
    t.rms = std::sqrt(sq / n);
    getFFT(n)->forward(buf.data());
    std::vector<double> p(n / 2);
    for (int i = 0; i < n / 2; ++i) p[i] = std::norm(buf[i]);
    int pk = 1;
    for (int i = 1; i < n / 2; ++i)
        if (p[i] > p[pk]) pk = i;
    double sig = 0, rest = 0;
    for (int i = 1; i < n / 2; ++i) (std::abs(i - pk) <= 4 ? sig : rest) += p[i];
    t.freq = pk * fs / n;
    t.snrDb = 10 * std::log10(sig / std::max(rest, 1e-30));
    return t;
}

// Runs `gen` (sample index -> IQ) for `seconds` through the demodulator.
std::vector<float> run(const AudioParams& ap, double fs, double seconds, const std::function<cf(int64_t)>& gen,
                       double trim = 0, double* elapsed = nullptr) {
    AudioDemod d(fs);
    d.setParams(ap);
    if (trim != 0) d.setRatioTrim(trim);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> blk(500, 60000);
    std::normal_distribution<float> noise(0.f, 1.f);
    const int64_t total = (int64_t)(fs * seconds);
    std::vector<cf> in;
    std::vector<float> out;
    double busy = 0;
    for (int64_t s = 0; s < total;) {
        const int64_t m = std::min<int64_t>(blk(rng), total - s);
        in.resize(m);
        for (int64_t i = 0; i < m; ++i) in[i] = gen(s + i) + cf(noise(rng), noise(rng)) * 1e-3f;
        const auto t0 = std::chrono::steady_clock::now();
        d.process(in.data(), in.size(), out);
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        s += m;
    }
    if (elapsed) *elapsed = busy;
    return out;
}

void toneTest(const char* name, AudioMode mode, double fs, double offset, double bw, double expectHz,
              const std::function<cf(int64_t)>& gen, double minSnr = 30) {
    AudioParams ap;
    ap.mode = (int)mode;
    ap.offset = offset;
    ap.bw = bw;
    const double seconds = 1.5;
    double busy = 0;
    auto out = run(ap, fs, seconds, gen, 0, &busy);
    Tone t = analyze(out, 48000);
    char b[256];
    snprintf(b, sizeof b, "%-4s fs=%-6g tone %.1f Hz (want %.0f), SNR %.1f dB, rms %.3f, %.0fx real time", name, fs,
             t.freq, expectHz, t.snrDb, t.rms, seconds / std::max(busy, 1e-9));
    check(std::fabs(t.freq - expectHz) < 6 && t.snrDb > minSnr && t.rms > 0.02 && t.rms < 1.0, b);
    const double want = seconds * 48000;
    snprintf(b, sizeof b, "%-4s output length %zu (want ~%.0f)", name, out.size(), want);
    check(std::fabs(out.size() - want) < 200, b);
}
}  // namespace

int main() {
    printf("audio_check\n");
    for (double fs : {250e3, 2.4e6, 10e6}) {
        // NFM: 1 kHz tone, 2.5 kHz deviation
        {
            const double off = 0.12 * fs, dev = 2.5e3;
            toneTest("NFM", AudioMode::NFM, fs, off, 12.5e3, 1000, [=](int64_t n) {
                const double t = n / fs;
                const double ph = 2 * M_PI * off * t - dev / 1000 * std::cos(2 * M_PI * 1000 * t);
                return std::polar(0.3f, (float)ph);
            });
        }
        // AM: 400 Hz, 60 % depth
        {
            const double off = -0.1 * fs;
            toneTest("AM", AudioMode::AM, fs, off, 10e3, 400, [=](int64_t n) {
                const double t = n / fs;
                return std::polar((float)(0.2 * (1 + 0.6 * std::sin(2 * M_PI * 400 * t))), (float)(2 * M_PI * off * t));
            });
        }
        // WFM: 1 kHz tone, 60 kHz deviation (needs fs > 2 * 200 kHz)
        if (fs >= 1e6) {
            const double off = 0.05 * fs, dev = 60e3;
            toneTest("WFM", AudioMode::WFM, fs, off, 200e3, 1000, [=](int64_t n) {
                const double t = n / fs;
                const double ph = 2 * M_PI * off * t - dev / 1000 * std::cos(2 * M_PI * 1000 * t);
                return std::polar(0.3f, (float)ph);
            });
        }
        // USB: a tone 1200 Hz above the carrier; the passband center is 1400 Hz above it
        {
            const double car = 0.2 * fs;
            toneTest("USB", AudioMode::USB, fs, car + 1400, 2.8e3, 1200, [=](int64_t n) {
                return std::polar(0.05f, (float)(2 * M_PI * (car + 1200) * n / fs));
            });
            toneTest("LSB", AudioMode::LSB, fs, car - 1400, 2.8e3, 900, [=](int64_t n) {
                return std::polar(0.05f, (float)(2 * M_PI * (car - 900) * n / fs));
            });
            toneTest("CW", AudioMode::CW, fs, car, 500, 700, [=](int64_t n) {
                return std::polar(0.01f, (float)(2 * M_PI * car * n / fs));
            });
        }
    }

    // Drift trim: +500 ppm gives 500 ppm more output.
    {
        AudioParams ap;
        ap.mode = (int)AudioMode::AM;
        const double fs = 1e6, sec = 4;
        auto a = run(ap, fs, sec, [](int64_t) { return cf(0.1f, 0); }, 0);
        auto b = run(ap, fs, sec, [](int64_t) { return cf(0.1f, 0); }, 500e-6);
        const double ppm = ((double)b.size() / a.size() - 1) * 1e6;
        char m[128];
        snprintf(m, sizeof m, "ratio trim +500 ppm -> %+.0f ppm more output", ppm);
        check(std::fabs(ppm - 500) < 30, m);
    }

    // Squelch: noise only stays silent; a strong carrier opens it.
    {
        AudioParams ap;
        ap.mode = (int)AudioMode::NFM;
        ap.squelch = true;
        ap.squelchDb = -40;
        auto quiet = run(ap, 1e6, 1, [](int64_t) { return cf(0, 0); });
        Tone tq = analyze(quiet, 48000);
        auto loud = run(ap, 1e6, 1.5, [](int64_t n) {
            return std::polar(0.3f, (float)(-2.5 * std::cos(2 * M_PI * 1000 * n / 1e6)));
        });
        Tone tl = analyze(loud, 48000);
        char m[128];
        snprintf(m, sizeof m, "squelch closed on noise (rms %.2g), open on a signal (rms %.3f)", tq.rms, tl.rms);
        check(tq.rms < 1e-4 && tl.rms > 0.05, m);
    }

    // WAV writer round trip: header fields and length.
    {
        const char* path = "/tmp/wvt_audio_check.wav";
        WavWriter w;
        std::string err;
        bool ok = w.open(path, 48000, err);
        std::vector<float> s(48000);
        for (int i = 0; i < 48000; ++i) s[i] = 0.5f * std::sin(2 * M_PI * 440 * i / 48000.0);
        ok = ok && w.write(s.data(), s.size(), err);
        w.close();
        FILE* f = fopen(path, "rb");
        unsigned char h[44] = {};
        ok = ok && f && fread(h, 1, 44, f) == 44;
        if (f) {
            fseek(f, 0, SEEK_END);
            ok = ok && ftell(f) == 44 + 96000;
            fclose(f);
        }
        auto u32 = [&](int o) { return h[o] | h[o + 1] << 8 | h[o + 2] << 16 | (unsigned)h[o + 3] << 24; };
        ok = ok && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0 && u32(24) == 48000 &&
             u32(40) == 96000 && u32(4) == 36 + 96000;
        check(ok, "WAV writer: 1 s mono 16-bit, header sizes correct");
        remove(path);
    }

    printf(failures ? "audio_check: %d FAILED\n" : "audio_check: all passed\n", failures);
    return failures ? 1 : 0;
}
