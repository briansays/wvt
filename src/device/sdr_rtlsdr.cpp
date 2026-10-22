// RTL-SDR backend (osmocom librtlsdr). Targets the RTL-SDR Blog v3:
// R820T2 tuner, 24-1766 MHz, HF via direct sampling on the Q branch, bias-tee.
#include <rtl-sdr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/log.h"
#include "device/sdr_device.h"
#include "common/ui_util.h"

namespace {

const char* tunerName(enum rtlsdr_tuner t) {
    switch (t) {
        case RTLSDR_TUNER_E4000: return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T: return "R820T";
        case RTLSDR_TUNER_R828D: return "R828D";
        default: return "unknown tuner";
    }
}

// Resolves "" (first device), a serial number or "N" (index) to an index.
// Serials are tried first: many v3 units ship with serial "00000001".
bool resolveIndex(const std::string& args, uint32_t& index, std::string& err) {
    const uint32_t count = rtlsdr_get_device_count();
    if (count == 0) {
        err = "no RTL-SDR found (is it plugged in? on Linux, is dvb_usb_rtl28xxu blacklisted?)";
        return false;
    }
    if (args.empty()) {
        index = 0;
        return true;
    }
    int i = rtlsdr_get_index_by_serial(args.c_str());
    if (i >= 0) {
        index = (uint32_t)i;
        return true;
    }
    char* end = nullptr;
    unsigned long v = strtoul(args.c_str(), &end, 10);
    if (end && *end == 0 && v < count) {
        index = (uint32_t)v;
        return true;
    }
    err = "no RTL-SDR with serial or index '" + args + "'";
    return false;
}

class RtlSdrDevice : public SdrDevice {
public:
    ~RtlSdrDevice() override {
        stop();
        if (dev_) {
#ifdef WVT_RTLSDR_HAS_BIAS_TEE
            if (biasTee_) rtlsdr_set_bias_tee(dev_, 0);  // don't leave the LNA powered
#endif
            rtlsdr_close(dev_);
        }
    }

    bool open(const StreamConfig& cfg, std::string& err) {
        uint32_t index;
        if (!resolveIndex(cfg.args, index, err)) return false;
        if (rtlsdr_open(&dev_, index) != 0) {
            dev_ = nullptr;
            err = "cannot open RTL-SDR #" + std::to_string(index) +
                  " (in use by another program, or missing udev permissions?)";
            return false;
        }
        char manuf[256] = {}, product[256] = {}, serial[256] = {};
        rtlsdr_get_device_usb_strings(index, manuf, product, serial);
        tuner_ = rtlsdr_get_tuner_type(dev_);
        desc_ = std::string("RTL-SDR (") + tunerName(tuner_) + ")" + (serial[0] ? std::string(" serial ") + serial : "");

        // Discrete tuner gains, in tenths of a dB.
        int n = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (n > 0) {
            std::vector<int> g(n);
            rtlsdr_get_tuner_gains(dev_, g.data());
            for (int v : g) gains_.push_back(v / 10.0);
        }
        LOG_DEBUG("rtlsdr", "#%u: %s %s serial '%s', tuner %s, %d gain steps (%.1f-%.1f dB)", index, manuf, product, serial,
                  tunerName(tuner_), (int)gains_.size(), gains_.empty() ? 0.0 : gains_.front(),
                  gains_.empty() ? 0.0 : gains_.back());

        if (cfg.ppm != 0) {
            int r = rtlsdr_set_freq_correction(dev_, cfg.ppm);
            if (r != 0 && r != -2) {  // -2: already set to this value
                err = "cannot set frequency correction";
                return false;
            }
        }
        if (cfg.directSampling) {
            if (rtlsdr_set_direct_sampling(dev_, cfg.directSampling) != 0) {
                err = "cannot enable direct sampling";
                return false;
            }
            notes_ = std::string("direct sampling on the ") + (cfg.directSampling == 1 ? "I" : "Q") +
                     " branch (HF, no tuner gain)";
        }
#ifdef WVT_RTLSDR_HAS_BIAS_TEE
        if (cfg.biasTee) {
            if (rtlsdr_set_bias_tee(dev_, 1) != 0) {
                err = "cannot enable the bias-tee";
                return false;
            }
            biasTee_ = true;
        }
#else
        if (cfg.biasTee) {
            err = "this librtlsdr has no bias-tee support (needs librtlsdr 0.6.0 or newer)";
            return false;
        }
#endif
        if (rtlsdr_set_sample_rate(dev_, (uint32_t)std::lround(cfg.rate)) != 0) {
            err = "RTL-SDR rejected sample rate " + fmtSI(cfg.rate, "S/s", 6);
            return false;
        }
        rate_ = rtlsdr_get_sample_rate(dev_);
        LOG_DEBUG("rtlsdr", "sample rate %.0f S/s requested, %.0f S/s set", cfg.rate, rate_);
        std::string warn, e2;
        checkSampleRate("rtlsdr", rate_, e2, warn);
        if (!warn.empty()) notes_ += (notes_.empty() ? "" : "; ") + warn;
        if (!setCenterFreq(cfg.fc, err)) return false;
        if (!setGain(cfg.agc, cfg.gain, err)) return false;
        return true;
    }

    bool start(DataFn data, GapFn, std::string& err) override {
        stop();
        data_ = std::move(data);
        if (rtlsdr_reset_buffer(dev_) != 0) {
            err = "cannot reset the RTL-SDR buffer";
            return false;
        }
        // About 20 ms per USB transfer, in multiples of 16 KiB.
        const uint32_t want = (uint32_t)(rate_ * 2 * 0.02);
        const uint32_t bufLen = std::clamp<uint32_t>((want + 16383) / 16384 * 16384, 16384, 256 * 1024);
        finished_ = false;
        LOG_DEBUG("rtlsdr", "streaming with %u-byte USB transfers", bufLen);
        th_ = std::thread([this, bufLen] {
            int r = rtlsdr_read_async(dev_, &RtlSdrDevice::callback, this, 0, bufLen);
            if (!stopping_) {
                std::lock_guard<std::mutex> lk(errM_);
                fatal_ = "RTL-SDR stopped streaming (unplugged?)" + (r < 0 ? " error " + std::to_string(r) : "");
            }
            finished_ = true;
        });
        return true;
    }

    void stop() override {
        if (!th_.joinable()) return;
        stopping_ = true;
        // rtlsdr_cancel_async() is ignored if the read loop has not started yet,
        // so keep asking until the thread exits.
        while (!finished_) {
            rtlsdr_cancel_async(dev_);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        th_.join();
        stopping_ = false;
    }

    bool setCenterFreq(double hz, std::string& err) override {
        if (hz < 0 || hz > 4.29e9 || rtlsdr_set_center_freq(dev_, (uint32_t)std::lround(hz)) != 0) {
            err = "RTL-SDR cannot tune to " + fmtSI(hz, "Hz", 9) + " (the v3 covers 24-1766 MHz, HF with direct sampling)";
            return false;
        }
        fc_ = rtlsdr_get_center_freq(dev_);
        return true;
    }

    bool setGain(bool agc, double db, std::string& err) override {
        if (agc) {
            if (rtlsdr_set_tuner_gain_mode(dev_, 0) != 0) {
                err = "cannot enable automatic gain";
                return false;
            }
            return true;
        }
        if (rtlsdr_set_tuner_gain_mode(dev_, 1) != 0) {
            err = "cannot set manual gain mode";
            return false;
        }
        // Pick the nearest gain the tuner supports.
        double best = db;
        if (!gains_.empty()) {
            best = gains_.front();
            for (double g : gains_)
                if (std::fabs(g - db) < std::fabs(best - db)) best = g;
        }
        if (rtlsdr_set_tuner_gain(dev_, (int)std::lround(best * 10)) != 0) {
            err = "cannot set gain";
            return false;
        }
        if (std::fabs(best - db) > 0.05) LOG_DEBUG("rtlsdr", "gain %.1f dB requested, nearest tuner step %.1f dB", db, best);
        return true;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return SampleFormat::CU8; }
    GainRange gainRange() const override {
        GainRange g;
        g.steps = gains_;
        g.min = gains_.empty() ? 0 : gains_.front();
        g.max = gains_.empty() ? 49.6 : gains_.back();
        g.step = 0.1;
        g.agc = true;
        return g;
    }
    std::string description() const override { return desc_; }
    std::string notes() const override { return notes_; }
    std::string fatalError() const override {
        std::lock_guard<std::mutex> lk(errM_);
        return fatal_;
    }

private:
    static void callback(unsigned char* buf, uint32_t len, void* ctx) {
        auto* self = (RtlSdrDevice*)ctx;
        if (!self->stopping_) self->data_(buf, len / 2);
    }

    rtlsdr_dev_t* dev_ = nullptr;
    enum rtlsdr_tuner tuner_ = RTLSDR_TUNER_UNKNOWN;
    std::vector<double> gains_;
    std::atomic<double> fc_{0};
    double rate_ = 0;
    bool biasTee_ = false;
    std::string desc_, notes_;
    DataFn data_;
    std::thread th_;
    std::atomic<bool> stopping_{false}, finished_{true};
    mutable std::mutex errM_;
    std::string fatal_;
};

}  // namespace

void enumerateRtlSdr(std::vector<DeviceInfo>& out) {
    const uint32_t n = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < n; ++i) {
        char manuf[256] = {}, product[256] = {}, serial[256] = {};
        rtlsdr_get_device_usb_strings(i, manuf, product, serial);
        std::string label = std::string("RTL-SDR #") + std::to_string(i) + " " + (product[0] ? product : rtlsdr_get_device_name(i));
        if (serial[0]) label += " (serial " + std::string(serial) + ")";
        // Select by serial when it is unique, so the choice survives replugging.
        std::string args = serial[0] && rtlsdr_get_index_by_serial(serial) == (int)i ? serial : std::to_string(i);
        out.push_back({"rtlsdr", args, label});
    }
}

std::unique_ptr<SdrDevice> openRtlSdrDevice(const StreamConfig& cfg, std::string& err) {
    auto d = std::make_unique<RtlSdrDevice>();
    if (!d->open(cfg, err)) return nullptr;
    return d;
}
