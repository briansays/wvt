// USRP backend through UHD's C API (uhd.h), so no Boost headers are needed.
// Targets the B200 / B200mini / B205mini (all UHD "type=b200"): 70 MHz-6 GHz,
// up to 56 MS/s on USB 3, antennas RX2 and TX/RX. Samples arrive as sc16.
#include <uhd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "sdr_device.h"
#include "ui_util.h"

namespace {

// UHD's last error on one line (its messages span several, which the status
// bar and terminal would cut off).
std::string lastError(const char* what) {
    char buf[1024] = {};
    uhd_get_last_error(buf, sizeof buf);
    std::string s = what, msg;
    for (const char* p = buf; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            if (!msg.empty() && msg.back() != ' ') msg += ' ';
        } else {
            msg += *p;
        }
    }
    while (!msg.empty() && msg.back() == ' ') msg.pop_back();
    if (!msg.empty()) s += ": " + msg;
    return s;
}

// "type=b200,name=x,serial=31A2B3C,product=B205mini" -> {type: b200, ...}
std::map<std::string, std::string> parseArgs(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        std::string kv = s.substr(i, j - i);
        size_t e = kv.find('=');
        if (e != std::string::npos) m[kv.substr(0, e)] = kv.substr(e + 1);
        i = j + 1;
    }
    return m;
}

class UhdDevice : public SdrDevice {
public:
    ~UhdDevice() override {
        stop();
        if (md_) uhd_rx_metadata_free(&md_);
        if (rx_) uhd_rx_streamer_free(&rx_);
        if (usrp_) uhd_usrp_free(&usrp_);
    }

    bool open(const StreamConfig& cfg, std::string& err) {
        const std::string args = cfg.args.empty() ? "type=b200" : cfg.args;
        if (uhd_usrp_make(&usrp_, args.c_str()) != UHD_ERROR_NONE) {
            usrp_ = nullptr;
            err = lastError(("cannot open USRP '" + args + "'").c_str()) +
                  " (is it plugged in? FPGA images downloaded with uhd_images_downloader?"
                  " on Linux, is UHD's udev rule installed?)";
            return false;
        }
        uhd_usrp_rx_info_t info{};
        if (uhd_usrp_get_rx_info(usrp_, 0, &info) == UHD_ERROR_NONE) {
            desc_ = std::string("USRP ") + (info.mboard_id ? info.mboard_id : "") +
                    (info.mboard_serial && info.mboard_serial[0] ? std::string(" serial ") + info.mboard_serial : "");
            uhd_usrp_rx_info_free(&info);
        } else {
            desc_ = "USRP";
        }

        if (!cfg.antenna.empty() && uhd_usrp_set_rx_antenna(usrp_, cfg.antenna.c_str(), 0) != UHD_ERROR_NONE) {
            err = lastError(("cannot select antenna " + cfg.antenna).c_str());
            return false;
        }
        if (uhd_usrp_set_rx_rate(usrp_, cfg.rate, 0) != UHD_ERROR_NONE) {
            err = lastError("cannot set sample rate");
            return false;
        }
        uhd_usrp_get_rx_rate(usrp_, 0, &rate_);
        if (std::fabs(rate_ - cfg.rate) > 1)
            notes_ = "rate coerced to " + fmtSI(rate_, "S/s", 9);
        if (cfg.bandwidth > 0 && uhd_usrp_set_rx_bandwidth(usrp_, cfg.bandwidth, 0) != UHD_ERROR_NONE) {
            err = lastError("cannot set bandwidth");
            return false;
        }
        uhd_meta_range_handle r = nullptr;
        if (uhd_meta_range_make(&r) == UHD_ERROR_NONE) {
            if (uhd_usrp_get_rx_gain_range(usrp_, "", 0, r) == UHD_ERROR_NONE) {
                uhd_meta_range_start(r, &gain_.min);
                uhd_meta_range_stop(r, &gain_.max);
                uhd_meta_range_step(r, &gain_.step);
            }
            uhd_meta_range_free(&r);
        }
        if (!setCenterFreq(cfg.fc, err)) return false;
        if (!setGain(cfg.agc, cfg.gain, err)) return false;

        // sc16 on the host matches the wire format: no conversion, smallest recordings.
        size_t chan = 0;
        uhd_stream_args_t sa{};
        sa.cpu_format = (char*)"sc16";
        sa.otw_format = (char*)"sc16";
        sa.args = (char*)"";
        sa.channel_list = &chan;
        sa.n_channels = 1;
        if (uhd_rx_streamer_make(&rx_) != UHD_ERROR_NONE || uhd_rx_metadata_make(&md_) != UHD_ERROR_NONE ||
            uhd_usrp_get_rx_stream(usrp_, &sa, rx_) != UHD_ERROR_NONE) {
            err = lastError("cannot create the receive stream");
            return false;
        }
        uhd_rx_streamer_max_num_samps(rx_, &spb_);
        if (spb_ == 0) spb_ = 4096;
        return true;
    }

    bool start(DataFn data, GapFn gap, std::string& err) override {
        stop();
        data_ = std::move(data);
        gap_ = std::move(gap);
        uhd_stream_cmd_t cmd{};
        cmd.stream_mode = UHD_STREAM_MODE_START_CONTINUOUS;
        cmd.stream_now = true;
        if (uhd_rx_streamer_issue_stream_cmd(rx_, &cmd) != UHD_ERROR_NONE) {
            err = lastError("cannot start streaming");
            return false;
        }
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (!th_.joinable()) return;
        quit_ = true;
        th_.join();
        uhd_stream_cmd_t cmd{};
        cmd.stream_mode = UHD_STREAM_MODE_STOP_CONTINUOUS;
        cmd.stream_now = true;
        uhd_rx_streamer_issue_stream_cmd(rx_, &cmd);
        // Drain what is still in flight so a restart begins cleanly.
        std::vector<int16_t> buf(spb_ * 2);
        void* b[1] = {buf.data()};
        size_t n = 0;
        for (int i = 0; i < 100; ++i) {
            if (uhd_rx_streamer_recv(rx_, b, spb_, &md_, 0.05, false, &n) != UHD_ERROR_NONE || n == 0) break;
        }
    }

    bool setCenterFreq(double hz, std::string& err) override {
        uhd_tune_request_t req{};
        req.target_freq = hz;
        req.rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO;
        req.dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO;
        req.args = (char*)"";
        uhd_tune_result_t res{};
        if (uhd_usrp_set_rx_freq(usrp_, &req, 0, &res) != UHD_ERROR_NONE) {
            err = lastError(("cannot tune to " + fmtSI(hz, "Hz", 9)).c_str());
            return false;
        }
        double f = hz;
        uhd_usrp_get_rx_freq(usrp_, 0, &f);
        fc_ = f;
        if (std::fabs(f - hz) > 1) {
            err = "USRP cannot tune to " + fmtSI(hz, "Hz", 9) + " (B200/B205: 70 MHz - 6 GHz); tuned to " +
                  fmtSI(f, "Hz", 9);
            return false;
        }
        return true;
    }

    bool setGain(bool agc, double db, std::string& err) override {
        if (agc) {
            if (uhd_usrp_set_rx_agc(usrp_, true, 0) != UHD_ERROR_NONE) {
                err = lastError("cannot enable AGC");
                return false;
            }
            return true;
        }
        uhd_usrp_set_rx_agc(usrp_, false, 0);
        if (uhd_usrp_set_rx_gain(usrp_, std::clamp(db, gain_.min, gain_.max), 0, "") != UHD_ERROR_NONE) {
            err = lastError("cannot set gain");
            return false;
        }
        return true;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return SampleFormat::CI16; }
    GainRange gainRange() const override { return gain_; }
    std::string description() const override { return desc_; }
    std::string notes() const override {
        std::string n = notes_;
        // The C API can't report the USB speed; frequent overflows at high rates usually mean USB 2.
        if (overflows_ > 5 && rate_ > 8e6)
            n += std::string(n.empty() ? "" : "; ") + "frequent overflows: is the USRP on a USB 3 port?";
        return n;
    }
    std::string fatalError() const override {
        std::lock_guard<std::mutex> lk(errM_);
        return fatal_;
    }

private:
    void run() {
        std::vector<int16_t> buf(spb_ * 2);
        void* b[1] = {buf.data()};
        bool overflow = false, haveNext = false;
        double tNext = 0;
        int timeouts = 0;
        while (!quit_) {
            size_t n = 0;
            if (uhd_rx_streamer_recv(rx_, b, spb_, &md_, 0.2, false, &n) != UHD_ERROR_NONE) {
                fail(lastError("receive failed"));
                return;
            }
            uhd_rx_metadata_error_code_t ec = UHD_RX_METADATA_ERROR_CODE_NONE;
            uhd_rx_metadata_error_code(md_, &ec);
            if (ec == UHD_RX_METADATA_ERROR_CODE_TIMEOUT) {
                if (++timeouts > 25) {
                    fail("USRP stopped sending samples (unplugged?)");
                    return;
                }
                continue;
            }
            timeouts = 0;
            if (ec == UHD_RX_METADATA_ERROR_CODE_OVERFLOW) {
                overflow = true;
                continue;
            }
            if (ec != UHD_RX_METADATA_ERROR_CODE_NONE) {
                fail("USRP receive error (code " + std::to_string((int)ec) + ")");
                return;
            }
            if (n == 0) continue;
            bool hasTime = false;
            int64_t full = 0;
            double frac = 0;
            uhd_rx_metadata_has_time_spec(md_, &hasTime);
            if (hasTime) uhd_rx_metadata_time_spec(md_, &full, &frac);
            const double t = (double)full + frac;
            if (overflow) {
                // The packet timestamps tell how many samples the overflow cost.
                int64_t lost = hasTime && haveNext ? std::llround((t - tNext) * rate_) : -1;
                ++overflows_;
                if (gap_) gap_(lost > 0 ? lost : -1, "USB overflow (host too slow, or USB 2 at a high rate)");
                overflow = false;
            }
            data_((const uint8_t*)buf.data(), (int64_t)n);
            haveNext = hasTime;
            tNext = t + n / rate_;
        }
    }

    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lk(errM_);
        fatal_ = why;
    }

    uhd_usrp_handle usrp_ = nullptr;
    uhd_rx_streamer_handle rx_ = nullptr;
    uhd_rx_metadata_handle md_ = nullptr;
    size_t spb_ = 0;
    double rate_ = 0;
    std::atomic<double> fc_{0};
    GainRange gain_{0, 76, 1, {}, true};
    std::string desc_, notes_;
    DataFn data_;
    GapFn gap_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::atomic<int> overflows_{0};
    mutable std::mutex errM_;
    std::string fatal_;
};

}  // namespace

void enumerateUhd(std::vector<DeviceInfo>& out) {
    // Only USB B2xx devices: a full network scan would stall the UI for seconds.
    uhd_string_vector_handle v = nullptr;
    if (uhd_string_vector_make(&v) != UHD_ERROR_NONE) return;
    if (uhd_usrp_find("type=b200", &v) == UHD_ERROR_NONE) {
        size_t n = 0;
        uhd_string_vector_size(v, &n);
        for (size_t i = 0; i < n; ++i) {
            char buf[512] = {};
            uhd_string_vector_at(v, i, buf, sizeof buf);
            auto a = parseArgs(buf);
            std::string product = a.count("product") ? a["product"] : "B200";
            std::string serial = a.count("serial") ? a["serial"] : "";
            out.push_back({"uhd", serial.empty() ? "type=b200" : "serial=" + serial,
                           "USRP " + product + (serial.empty() ? "" : " (serial " + serial + ")")});
        }
    }
    uhd_string_vector_free(&v);
}

std::unique_ptr<SdrDevice> openUhdDevice(const StreamConfig& cfg, std::string& err) {
    auto d = std::make_unique<UhdDevice>();
    if (!d->open(cfg, err)) return nullptr;
    return d;
}
