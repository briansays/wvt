// Signal generator loopback: a receiver that "hears" the waveform currently
// set in the Signal Generator window, in real time. The signal sits at its
// absolute frequency (generator fc + offset), so retuning the receiver moves
// it across the spectrum like a real one. Gain works like the simulator's
// (30 dB = unity); a faint noise floor stands in for receiver noise.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <thread>
#include <vector>

#include "sdr_device.h"
#include "siggen.h"
#include "ui_util.h"

namespace {

class SigGenDevice : public SdrDevice {
public:
    explicit SigGenDevice(const StreamConfig& cfg)
        : rate_(cfg.rate), fc_(cfg.fc), agc_(cfg.agc), gain_(cfg.gain), gen_(cfg.rate, cfg.fc) {}
    ~SigGenDevice() override { stop(); }

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
    SampleFormat format() const override { return SampleFormat::CF32; }
    GainRange gainRange() const override { return {0, 60, 1, {}, true}; }
    std::string description() const override { return "signal generator loopback"; }
    std::string notes() const override {
        const WaveformParams p = sigGenBus().get();
        const double f = p.fc + p.offset;
        if (std::fabs(f - fc_) < rate_ / 2) return {};
        return "the generator's signal (" + fmtSI(f, "Hz", 9) + ") is outside the received band: tune to it, or set the generator's center to " +
               fmtSI(fc_, "Hz", 9);
    }

private:
    void run() {
        using clock = std::chrono::steady_clock;
        const int64_t chunk = std::max<int64_t>(64, (int64_t)(rate_ / 100));  // 10 ms
        const auto period = std::chrono::duration<double>(chunk / rate_);
        std::vector<std::complex<float>> buf((size_t)chunk);
        auto deadline = clock::now();
        while (!quit_) {
            gen_.setTuning(fc_);
            gen_.generate(buf.data(), chunk);
            const float scale = agc_ ? 1.f : (float)std::pow(10.0, (gain_ - 30) / 20);
            for (auto& s : buf) s = s * scale + std::complex<float>(noise(), noise()) * 1e-5f;
            data_((const uint8_t*)buf.data(), chunk);
            deadline += std::chrono::duration_cast<clock::duration>(period);
            auto now = clock::now();
            if (now - deadline > std::chrono::milliseconds(200)) {
                int64_t lost = (int64_t)(std::chrono::duration<double>(now - deadline).count() * rate_);
                if (gap_) gap_(lost, "signal generator could not keep up");
                deadline = now;
            }
            std::this_thread::sleep_until(deadline);
        }
    }

    float noise() {
        float s = 0;
        for (int i = 0; i < 4; ++i) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            s += (rng_ & 0xffffff) * (1.f / 16777216.f);
        }
        return (s - 2.f) * 1.7320508f;
    }

    const double rate_;
    std::atomic<double> fc_;
    std::atomic<bool> agc_;
    std::atomic<double> gain_;
    BusSigGen gen_;
    DataFn data_;
    GapFn gap_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    uint32_t rng_ = 0x2545f491;
};

}  // namespace

std::unique_ptr<SdrDevice> openSigGenDevice(const StreamConfig& cfg, std::string&) {
    return std::make_unique<SigGenDevice>(cfg);
}
