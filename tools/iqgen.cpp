// iqgen - writes a synthetic IQ recording containing AM, FM, PM, FSK, a chirp
// and noise, for exercising wvt.
//
//   iqgen out.cf32 [--fmt cf32|ci16|ci8|cu8] [--rate 2e6] [--dur 2] [--fc 100e6]
//
// If the output name ends in .sigmf-data a matching .sigmf-meta is written.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: iqgen out_file [--fmt cf32|ci16|ci8|cu8] [--rate Hz] [--dur s] [--fc Hz]\n");
        return 2;
    }
    std::string out = argv[1], fmt = "cf32";
    double fs = 2e6, dur = 2.0, fc = 100e6;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--fmt") fmt = argv[i + 1];
        else if (a == "--rate") fs = atof(argv[i + 1]);
        else if (a == "--dur") dur = atof(argv[i + 1]);
        else if (a == "--fc") fc = atof(argv[i + 1]);
    }
    if (out.size() > 4 && out.substr(out.size() - 4) == ".cs8") fmt = "ci8";
    if (out.size() > 4 && out.substr(out.size() - 4) == ".cu8") fmt = "cu8";
    if (out.size() > 5 && out.substr(out.size() - 5) == ".cs16") fmt = "ci16";

    FILE* f = fopen(out.c_str(), "wb");
    if (!f) { perror("fopen"); return 1; }
    const int64_t N = (int64_t)(fs * dur);
    std::mt19937 rng(1234);
    std::normal_distribution<float> gauss(0.f, 1.f);
    const double TWO_PI = 2 * M_PI;

    // phase accumulators
    double phFm = 0, phFsk = 0, phChirp = 0;
    const int64_t chunk = 1 << 16;
    std::vector<std::complex<float>> buf(chunk);
    std::vector<uint8_t> raw(chunk * 8);
    const float noise = 0.0015f;
    // FSK symbol stream
    const double baud = 5000;
    std::uniform_int_distribution<int> bit(0, 1);
    int64_t lastSym = -1;
    int sym = 0;

    for (int64_t base = 0; base < N; base += chunk) {
        int64_t n = std::min(chunk, N - base);
        for (int64_t k = 0; k < n; ++k) {
            double t = (base + k) / fs;
            std::complex<double> s = 0;

            // 1) FM: carrier at +300 kHz, 1 kHz tone, 50 kHz deviation, slow fade
            {
                double dev = 50e3 * std::sin(TWO_PI * 1000 * t);
                phFm += TWO_PI * (300e3 + dev) / fs;
                double amp = 0.20 * (0.75 + 0.25 * std::cos(TWO_PI * 0.5 * t));
                s += amp * std::polar(1.0, phFm);
            }
            // 2) AM: carrier at -250 kHz, 400 Hz tone, depth 0.6
            {
                double env = 0.15 * (1 + 0.6 * std::sin(TWO_PI * 400 * t));
                s += env * std::polar(1.0, TWO_PI * -250e3 * t);
            }
            // 3) PM: carrier at -600 kHz, 2 rad peak at 500 Hz, plus 90 deg steps every 20 ms
            {
                double step = (M_PI / 2) * (int)std::floor(t / 0.02);
                double ph = TWO_PI * -600e3 * t + 2.0 * std::sin(TWO_PI * 500 * t) + std::fmod(step, TWO_PI);
                s += 0.10 * std::polar(1.0, ph);
            }
            // 4) 2-FSK bursts at +650 kHz: +/-20 kHz, 5 kBd, 60 ms every 250 ms
            {
                double tb = std::fmod(t, 0.25);
                if (tb < 0.06) {
                    int64_t si = (int64_t)(t * baud);
                    if (si != lastSym) { lastSym = si; sym = bit(rng); }
                    phFsk += TWO_PI * (650e3 + (sym ? 20e3 : -20e3)) / fs;
                    s += 0.12 * std::polar(1.0, phFsk);
                }
            }
            // 5) Linear chirp -900 kHz -> +900 kHz between t = 1.0 s and 1.4 s
            if (t >= 1.0 && t < 1.4) {
                double f0 = -900e3, k1 = 1.8e6 / 0.4;
                double fi = f0 + k1 * (t - 1.0);
                phChirp += TWO_PI * fi / fs;
                s += 0.08 * std::polar(1.0, phChirp);
            }
            // 6) Weak CW tone at +100 kHz (tests dynamic range)
            s += 0.003 * std::polar(1.0, TWO_PI * 100e3 * t);

            buf[k] = std::complex<float>((float)s.real() + noise * gauss(rng), (float)s.imag() + noise * gauss(rng));
        }
        size_t bytes = 0;
        for (int64_t k = 0; k < n; ++k) {
            float re = buf[k].real(), im = buf[k].imag();
            if (fmt == "cf32") {
                memcpy(&raw[bytes], &re, 4);
                memcpy(&raw[bytes + 4], &im, 4);
                bytes += 8;
            } else if (fmt == "ci16") {
                int16_t a = (int16_t)std::lround(std::clamp(re, -1.f, 1.f) * 32767);
                int16_t b = (int16_t)std::lround(std::clamp(im, -1.f, 1.f) * 32767);
                memcpy(&raw[bytes], &a, 2);
                memcpy(&raw[bytes + 2], &b, 2);
                bytes += 4;
            } else if (fmt == "ci8") {
                raw[bytes++] = (uint8_t)(int8_t)std::lround(std::clamp(re, -1.f, 1.f) * 127);
                raw[bytes++] = (uint8_t)(int8_t)std::lround(std::clamp(im, -1.f, 1.f) * 127);
            } else {  // cu8
                raw[bytes++] = (uint8_t)std::lround(std::clamp(re, -1.f, 1.f) * 127.5f + 127.5f);
                raw[bytes++] = (uint8_t)std::lround(std::clamp(im, -1.f, 1.f) * 127.5f + 127.5f);
            }
        }
        fwrite(raw.data(), 1, bytes, f);
    }
    fclose(f);

    if (out.size() > 11 && out.substr(out.size() - 11) == ".sigmf-data") {
        std::string meta = out.substr(0, out.size() - 4) + "meta";
        FILE* m = fopen(meta.c_str(), "w");
        if (m) {
            std::string dt = fmt == "cf32" ? "cf32_le" : fmt == "ci16" ? "ci16_le" : fmt;
            fprintf(m,
                    "{\n  \"global\": {\"core:datatype\": \"%s\", \"core:sample_rate\": %.0f, \"core:version\": \"1.0.0\"},\n"
                    "  \"captures\": [{\"core:sample_start\": 0, \"core:frequency\": %.0f}],\n  \"annotations\": []\n}\n",
                    dt.c_str(), fs, fc);
            fclose(m);
        }
    }
    printf("wrote %lld samples (%s, %.3g Sps, %.3g s) to %s\n", (long long)N, fmt.c_str(), fs, dur, out.c_str());
    return 0;
}
