// Simulated receiver: generates the same kinds of signals as tools/iqgen
// (FM, AM, FSK bursts, a chirp, a weak CW tone and noise) in real time.
// The "stations" sit at fixed absolute frequencies around the center
// frequency given at open, so retuning moves them across the spectrum.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <thread>
#include <vector>

#include "sdr_device.h"

namespace {

class SimDevice : public SdrDevice {
public:
    explicit SimDevice(const StreamConfig& cfg)
        : rate_(cfg.rate), anchor_(cfg.fc), fc_(cfg.fc), agc_(cfg.agc), gain_(cfg.gain) {
        // Offsets are laid out for 2 MS/s (as in iqgen) and scaled to the rate.
        k_ = std::clamp(rate_ / 2e6, 0.1, 5.0);
    }
    ~SimDevice() override { stop(); }

    bool start(DataFn data, GapFn gap, std::string&) override {
        stop();
        data_ = std::move(data);
        gap_ = std::move(gap);
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        quit_ = true;
        if (th_.joinable()) th_.join();
    }
    bool setCenterFreq(double hz, std::string&) override {
        fc_ = hz;
        return true;
    }
    bool setGain(bool agc, double db, std::string&) override {
        agc_ = agc;
        gain_ = db;
        return true;
    }
    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return SampleFormat::CI16; }
    GainRange gainRange() const override { return {0, 60, 1, {}, true}; }
    std::string description() const override { return "wvt simulator"; }

private:
    void run() {
        using clock = std::chrono::steady_clock;
        const int64_t chunk = std::max<int64_t>(64, (int64_t)(rate_ / 100));  // 10 ms
        const auto period = std::chrono::duration<double>(chunk / rate_);
        std::vector<int16_t> buf((size_t)chunk * 2);
        auto deadline = clock::now();
        while (!quit_) {
            generate(buf.data(), chunk);
            data_((const uint8_t*)buf.data(), chunk);
            deadline += std::chrono::duration_cast<clock::duration>(period);
            auto now = clock::now();
            if (now - deadline > std::chrono::milliseconds(200)) {
                // Could not keep up: skip ahead like a real device would overflow.
                int64_t lost = (int64_t)(std::chrono::duration<double>(now - deadline).count() * rate_);
                n_ += lost;
                if (gap_) gap_(lost, "simulator could not keep up");
                deadline = now;
            }
            std::this_thread::sleep_until(deadline);
        }
    }

    // Phase of a signal at absolute frequency `fabs` for the current tuning.
    double step(double fabs, double fc) const { return 2 * M_PI * (fabs - fc) / rate_; }

    float noise() {
        // Irwin-Hall approximation of a unit Gaussian (cheap, good enough here).
        float s = 0;
        for (int i = 0; i < 4; ++i) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            s += (rng_ & 0xffffff) * (1.f / 16777216.f);
        }
        return (s - 2.f) * 1.7320508f;
    }

    void generate(int16_t* out, int64_t count) {
        const double fc = fc_, fs = rate_, k = k_;
        const double scale = (agc_ ? 1.0 : std::pow(10.0, (gain_ - 30) / 20)) * 16384;
        const double fFm = anchor_ + 300e3 * k, fAm = anchor_ - 250e3 * k, fFsk = anchor_ + 650e3 * k,
                     fCw = anchor_ + 100e3 * k;
        const double sAm = step(fAm, fc), sCw = step(fCw, fc);
        for (int64_t i = 0; i < count; ++i, ++n_) {
            const double t = n_ / fs;
            std::complex<float> s = 0;
            // FM: 1 kHz tone, 50 kHz (scaled) deviation, slow fade
            phFm_ += step(fFm, fc) + 2 * M_PI * 50e3 * k * std::sin(2 * M_PI * 1000 * t) / fs;
            s += std::polar((float)(0.2 * (0.75 + 0.25 * std::cos(2 * M_PI * 0.5 * t))), (float)phFm_);
            // AM: 400 Hz tone, 60% depth
            phAm_ += sAm;
            s += std::polar((float)(0.15 * (1 + 0.6 * std::sin(2 * M_PI * 400 * t))), (float)phAm_);
            // 2-FSK bursts: 60 ms every 250 ms, 5 kBd
            if (std::fmod(t, 0.25) < 0.06) {
                int64_t sym = (int64_t)(t * 5000);
                bool bit = ((sym * 2654435761u) >> 7) & 1;
                phFsk_ += step(fFsk + (bit ? 20e3 : -20e3) * k, fc);
                s += std::polar(0.12f, (float)phFsk_);
            }
            // Chirp across 90% of the band, 0.4 s out of every 2 s
            double tc = std::fmod(t, 2.0);
            if (tc >= 1.0 && tc < 1.4) {
                double f = anchor_ - 0.45 * fs + 0.9 * fs * (tc - 1.0) / 0.4;
                phChirp_ += step(f, fc);
                s += std::polar(0.08f, (float)phChirp_);
            }
            // Weak CW tone
            phCw_ += sCw;
            s += std::polar(0.003f, (float)phCw_);
            s += std::complex<float>(noise(), noise()) * 0.0015f;

            out[2 * i] = (int16_t)std::clamp(std::lround(s.real() * scale), -32767L, 32767L);
            out[2 * i + 1] = (int16_t)std::clamp(std::lround(s.imag() * scale), -32767L, 32767L);
        }
        // Keep accumulators small so float conversion stays precise.
        for (double* p : {&phFm_, &phAm_, &phFsk_, &phChirp_, &phCw_}) *p = std::remainder(*p, 2 * M_PI);
    }

    const double rate_, anchor_;
    double k_;
    std::atomic<double> fc_;
    std::atomic<bool> agc_;
    std::atomic<double> gain_;
    DataFn data_;
    GapFn gap_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    int64_t n_ = 0;
    double phFm_ = 0, phAm_ = 0, phFsk_ = 0, phChirp_ = 0, phCw_ = 0;
    uint32_t rng_ = 0x12345678;
};

}  // namespace

std::unique_ptr<SdrDevice> openSimDevice(const StreamConfig& cfg, std::string&) {
    return std::make_unique<SimDevice>(cfg);
}
