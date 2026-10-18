// Signal Hound BB60 backend (bb_api from the Signal Hound SDK). Targets the
// BB60D, and also runs the BB60C/A: 9 kHz-6.4 GHz, I/Q at 40 MS/s divided by
// a power of 2 (up to 8192), up to 27 MHz of bandwidth. Samples arrive as sc16.
//
// The BB60 has no gain control as such: a reference level (the maximum
// expected input, dBm) picks the attenuator and the amplifier, unless both are
// set by hand. Settings only take effect when streaming starts, so a retune or
// a new reference level aborts the stream, reconfigures it and starts again.
#include <bb_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "sdr_device.h"
#include "ui_util.h"

namespace {

constexpr double kBaseRate = 40e6;
constexpr double kMinRefLevel = -130, kMaxRefLevel = BB_MAX_REFERENCE;

std::string bbError(const std::string& what, bbStatus s) {
    const char* msg = bbGetErrorString(s);
    return what + ": " + (msg && msg[0] ? msg : "error " + std::to_string((int)s));
}

const char* typeName(int type) {
    switch (type) {
        case BB_DEVICE_BB60A: return "BB60A";
        case BB_DEVICE_BB60C: return "BB60C";
        case BB_DEVICE_BB60D: return "BB60D";
        default: return "BB60";
    }
}

// Widest I/Q filter the API allows at each decimation (27 MHz at 40 MS/s,
// 17.8 MHz at 20 MS/s, then about 80% of the rate).
double maxBandwidth(int decimation) {
    if (decimation == 1) return 27e6;
    if (decimation == 2) return 17.8e6;
    return 0.8 * kBaseRate / decimation;
}

class BbDevice : public SdrDevice {
public:
    ~BbDevice() override {
        stop();
        if (dev_ >= 0) bbCloseDevice(dev_);
    }

    bool open(const StreamConfig& cfg, std::string& err) {
        bbStatus s;
        if (cfg.args.empty()) {
            s = bbOpenDevice(&dev_);
        } else {
            char* end = nullptr;
            const long serial = strtol(cfg.args.c_str(), &end, 10);
            if (!end || *end != 0 || serial <= 0) {
                err = "BB60 device argument must be a serial number (got '" + cfg.args + "')";
                return false;
            }
            s = bbOpenDeviceBySerialNumber(&dev_, (int)serial);
        }
        if (s < bbNoError) {
            dev_ = -1;
            err = bbError("cannot open BB60" + (cfg.args.empty() ? std::string() : " serial " + cfg.args), s) +
                  " (is it on a USB 3 port? in use by Spike or another program? on Linux, is the udev rule installed?)";
            return false;
        }
        int type = BB_DEVICE_NONE;
        bbGetDeviceType(dev_, &type);
        uint32_t serial = 0;
        bbGetSerialNumber(dev_, &serial);
        desc_ = std::string("Signal Hound ") + typeName(type) + (serial ? " serial " + std::to_string(serial) : "");

        if (cfg.extRef) {
            s = type == BB_DEVICE_BB60D ? bbConfigureIO(dev_, BB60D_PORT1_10MHZ_REF_IN, BB60D_PORT2_DISABLED)
                                        : bbConfigureIO(dev_, BB60C_PORT1_10MHZ_REF_IN, 0);
            if (s < bbNoError) {
                err = bbError("cannot select the external 10 MHz reference", s);
                return false;
            }
        }
        // sc16 halves the USB-to-ring and recording bandwidth compared with cf32.
        if ((s = bbConfigureIQDataType(dev_, bbDataType16sc)) < bbNoError) {
            err = bbError("cannot select 16-bit I/Q", s);
            return false;
        }

        rate_ = snapSampleRate("bb", cfg.rate);
        decimation_ = std::max(1, (int)std::lround(kBaseRate / rate_));
        if (std::fabs(rate_ - cfg.rate) > 1) openNotes_ = "rate coerced to " + fmtSI(rate_, "S/s", 6);
        bandwidth_ = cfg.bandwidth > 0 ? cfg.bandwidth : maxBandwidth(decimation_);
        if (bandwidth_ > maxBandwidth(decimation_)) {
            bandwidth_ = maxBandwidth(decimation_);
            addNote(openNotes_, "bandwidth limited to " + fmtSI(bandwidth_, "Hz", 4) + " at this rate");
        }
        fc_ = cfg.fc;
        refLevel_ = std::clamp(cfg.gain, kMinRefLevel, kMaxRefLevel);
        gain_ = std::clamp(cfg.rfGain, BB_AUTO_GAIN, BB_MAX_GAIN);
        atten_ = std::clamp(cfg.atten, BB_AUTO_ATTEN, BB_MAX_ATTEN);
        if (!checkFreq(fc_, err)) return false;
        return configure(err);
    }

    bool start(DataFn data, GapFn gap, std::string& err) override {
        stop();
        data_ = std::move(data);
        gap_ = std::move(gap);
        std::lock_guard<std::mutex> lk(cfgM_);
        return startStream(err);
    }

    void stop() override {
        std::lock_guard<std::mutex> lk(cfgM_);
        stopStream();
    }

    bool setCenterFreq(double hz, std::string& err) override {
        if (!checkFreq(hz, err)) return false;
        return reconfigure([&] { fc_ = hz; }, err);
    }

    bool setGain(bool, double db, std::string& err) override {
        db = std::clamp(db, kMinRefLevel, kMaxRefLevel);
        if (db == refLevel_) return true;
        return reconfigure([&] { refLevel_ = db; }, err);
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return SampleFormat::CI16; }
    GainRange gainRange() const override {
        GainRange g;
        g.min = kMinRefLevel;
        g.max = kMaxRefLevel;
        g.step = 1;
        g.agc = false;
        g.label = "Ref level";
        g.unit = "dBm";
        return g;
    }
    std::string description() const override { return desc_; }
    std::string notes() const override {
        std::lock_guard<std::mutex> lk(noteM_);
        std::string n = notes_;
        const double sinceOverload = nowSec() - lastOverload_.load();
        if (sinceOverload < 1) addNote(n, "ADC overload: raise the reference level");
        return n;
    }
    std::string fatalError() const override {
        std::lock_guard<std::mutex> lk(noteM_);
        return fatal_;
    }

private:
    static double nowSec() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static void addNote(std::string& n, const std::string& s) { n += (n.empty() ? "" : "; ") + s; }

    bool checkFreq(double hz, std::string& err) const {
        if (hz < BB_MIN_FREQ || hz > BB_MAX_FREQ) {
            err = "BB60 cannot tune to " + fmtSI(hz, "Hz", 9) + " (range 9 kHz - 6.4 GHz)";
            return false;
        }
        return true;
    }

    // Sends the stored settings to the API. The device must not be streaming.
    bool configure(std::string& err) {
        bbStatus s;
        if ((s = bbConfigureIQCenter(dev_, fc_)) < bbNoError) {
            err = bbError("cannot tune to " + fmtSI(fc_, "Hz", 9), s);
            return false;
        }
        if ((s = bbConfigureRefLevel(dev_, refLevel_)) < bbNoError) {
            err = bbError("cannot set reference level " + fmtSI(refLevel_, "dBm", 4), s);
            return false;
        }
        if ((s = bbConfigureGainAtten(dev_, gain_, atten_)) < bbNoError) {
            err = bbError("cannot set gain / attenuation", s);
            return false;
        }
        if ((s = bbConfigureIQ(dev_, decimation_, bandwidth_)) < bbNoError) {
            err = bbError("cannot set I/Q rate " + fmtSI(rate_, "S/s", 6) + ", bandwidth " + fmtSI(bandwidth_, "Hz", 4), s);
            return false;
        }
        return true;
    }

    // Both run with cfgM_ held.
    bool startStream(std::string& err) {
        bbStatus s = bbInitiate(dev_, BB_STREAMING, BB_STREAM_IQ);
        if (s < bbNoError) {
            err = bbError("cannot start I/Q streaming", s);
            return false;
        }
        std::string n = openNotes_;
        if (s > bbNoError && bbGetErrorString(s)) addNote(n, bbGetErrorString(s));
        double rate = 0, bw = 0;
        if (bbQueryIQParameters(dev_, &rate, &bw) == bbNoError && rate > 0) {
            if (std::fabs(rate - rate_) > 1) addNote(n, "BB60 streams at " + fmtSI(rate, "S/s", 6));
            rate_ = rate;
        }
        std::string warn, e2;
        checkSampleRate("bb", rate_, e2, warn);
        if (!warn.empty()) addNote(n, warn);
        {
            std::lock_guard<std::mutex> lk(noteM_);
            notes_ = n;
        }
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stopStream() {
        if (!th_.joinable()) return;
        quit_ = true;
        th_.join();
        bbAbort(dev_);
    }

    // Applies a change that needs the stream restarted. On failure the old
    // settings come back, so the device keeps streaming as before.
    template <class F>
    bool reconfigure(F change, std::string& err) {
        std::lock_guard<std::mutex> lk(cfgM_);
        const bool streaming = th_.joinable();
        stopStream();
        const double fc = fc_, ref = refLevel_;
        change();
        bool ok = configure(err);
        if (!ok) {
            fc_ = fc;
            refLevel_ = ref;
            std::string e2;
            configure(e2);
        }
        // Retune dead time is not reported as a gap: the recorder starts a new
        // capture segment at a retune, and the purge drops samples from before
        // it. The first block takes ~20 ms to fill, so LiveEngine marks the
        // retune in the ring before any new samples arrive.
        if (streaming) {
            std::string e2;
            if (!startStream(e2)) {
                std::lock_guard<std::mutex> nl(noteM_);
                fatal_ = e2;
                if (ok) err = e2;
                return false;
            }
        }
        return ok;
    }

    void run() {
        // About 20 ms per call, so stop and retune stay responsive.
        const int block = std::clamp((int)(rate_ * 0.02), 1024, 262144);
        std::vector<int16_t> buf((size_t)block * 2);
        bool purge = true;  // drop samples buffered before this (re)start
        while (!quit_) {
            int remaining = 0, loss = 0, sec = 0, nano = 0;
            bbStatus s = bbGetIQUnpacked(dev_, buf.data(), block, nullptr, 0, purge ? BB_TRUE : BB_FALSE, &remaining,
                                         &loss, &sec, &nano);
            if (s < bbNoError) {
                std::lock_guard<std::mutex> lk(noteM_);
                fatal_ = s == bbDeviceConnectionErr ? "BB60 stopped streaming (unplugged?)"
                                                    : bbError("BB60 receive failed", s);
                return;
            }
            if (s == bbADCOverflow) lastOverload_ = nowSec();
            if (quit_) break;
            if (loss && !purge) gap_(-1, "BB60 sample loss (USB or host too slow)");
            purge = false;
            data_((const uint8_t*)buf.data(), block);
        }
    }

    int dev_ = -1;
    int decimation_ = 1;
    int gain_ = BB_AUTO_GAIN, atten_ = BB_AUTO_ATTEN;
    std::atomic<double> fc_{0}, rate_{0};
    double bandwidth_ = 0, refLevel_ = 0;
    std::string desc_, openNotes_;
    DataFn data_;
    GapFn gap_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::mutex cfgM_;  // serializes start / stop / reconfigure
    mutable std::mutex noteM_;
    std::string notes_, fatal_;
    std::atomic<double> lastOverload_{-1e9};
};

}  // namespace

void enumerateBb(std::vector<DeviceInfo>& out) {
    int serials[BB_MAX_DEVICES] = {}, types[BB_MAX_DEVICES] = {}, n = 0;
    if (bbGetSerialNumberList2(serials, types, &n) < bbNoError) return;
    for (int i = 0; i < std::min(n, (int)BB_MAX_DEVICES); ++i) {
        const std::string s = std::to_string(serials[i]);
        out.push_back({"bb", s, std::string("Signal Hound ") + typeName(types[i]) + " (serial " + s + ")"});
    }
}

std::unique_ptr<SdrDevice> openBbDevice(const StreamConfig& cfg, std::string& err) {
    auto d = std::make_unique<BbDevice>();
    if (!d->open(cfg, err)) return nullptr;
    return d;
}
