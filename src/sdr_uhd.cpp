// USRP backend through UHD's C API (uhd.h), so no Boost headers are needed.
// Targets the B200 / B200mini / B205mini (all UHD "type=b200"): 70 MHz-6 GHz,
// up to 56 MS/s on USB 3, antennas RX2 and TX/RX. Samples arrive as sc16.
//
// UHD lets a process open a device only once, so the receiver and the
// transmitter share one reference-counted handle (full duplex on one B2xx).
#include <uhd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"
#include "sdr_device.h"
#include "sdr_tx.h"
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

// ---------------------------------------------------------------------------
// Shared device handle
// ---------------------------------------------------------------------------
struct Usrp;
std::mutex gUsrpM;
std::condition_variable gUsrpCv;
std::weak_ptr<Usrp> gUsrp;
int gUsrpAlive = 0;  // handles not yet freed (a new open must wait for them)

struct Usrp {
    uhd_usrp_handle h = nullptr;
    std::string args, serial;
    std::atomic<int> rxUsers{0}, txUsers{0};
    ~Usrp() {
        if (h) uhd_usrp_free(&h);
        std::lock_guard<std::mutex> lk(gUsrpM);
        --gUsrpAlive;
        gUsrpCv.notify_all();
    }
};

// Args select the open device unless they name a different serial.
bool sameDevice(const std::string& args, const Usrp& u) {
    if (args == u.args) return true;
    auto a = parseArgs(args);
    return !a.count("serial") || a["serial"] == u.serial;
}

std::shared_ptr<Usrp> acquireUsrp(std::string args, std::string& err) {
    if (args.empty()) args = "type=b200";
    std::unique_lock<std::mutex> lk(gUsrpM);
    if (auto u = gUsrp.lock()) {
        if (sameDevice(args, *u)) {
            LOG_DEBUG("uhd", "sharing the open USRP %s for '%s'", u->serial.c_str(), args.c_str());
            return u;
        }
        err = "USRP " + u->serial + " is already open in this wvt (one USRP at a time)";
        // Drop our reference without the lock: if it is the last one, ~Usrp locks.
        lk.unlock();
        return nullptr;
    }
    // A handle that is being released still holds the USB device.
    gUsrpCv.wait(lk, [] { return gUsrpAlive == 0; });
    uhd_usrp_handle h = nullptr;
    LOG_INFO("uhd", "opening USRP '%s' (loading the FPGA image can take a few seconds)", args.c_str());
    if (uhd_usrp_make(&h, args.c_str()) != UHD_ERROR_NONE) {
        err = lastError(("cannot open USRP '" + args + "'").c_str()) +
              " (is it plugged in? FPGA images downloaded with uhd_images_downloader?"
              " on Linux, is UHD's udev rule installed?)";
        return nullptr;
    }
    ++gUsrpAlive;
    auto u = std::make_shared<Usrp>();
    u->h = h;
    u->args = args;
    uhd_usrp_rx_info_t info{};
    if (uhd_usrp_get_rx_info(h, 0, &info) == UHD_ERROR_NONE) {
        if (info.mboard_serial) u->serial = info.mboard_serial;
        uhd_usrp_rx_info_free(&info);
    }
    gUsrp = u;
    LOG_DEBUG("uhd", "USRP open, serial %s", u->serial.c_str());
    return u;
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------
class UhdDevice : public SdrDevice {
public:
    ~UhdDevice() override {
        stop();
        if (md_) uhd_rx_metadata_free(&md_);
        if (rx_) uhd_rx_streamer_free(&rx_);
        if (counted_) --dev_->rxUsers;
    }

    bool open(const StreamConfig& cfg, std::string& err) {
        dev_ = acquireUsrp(cfg.args, err);
        if (!dev_) return false;
        usrp_ = dev_->h;
        if (dev_->txUsers > 0) {
            // RX and TX rates come from one master clock on the B2xx; changing one
            // while the other streams would silently change the other.
            double txRate = 0;
            uhd_usrp_get_tx_rate(usrp_, 0, &txRate);
            if (std::fabs(txRate - cfg.rate) > 1) {
                err = "the USRP is transmitting at " + fmtSI(txRate, "S/s", 9) +
                      "; receive at the same rate while transmitting (or stop transmitting first)";
                return false;
            }
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
        LOG_DEBUG("uhd", "rx: %s, rate %.3f S/s, gain range %.2f-%.2f dB step %.2f, %zu samples per packet", desc_.c_str(),
                  rate_, gain_.min, gain_.max, gain_.step, spb_);
        ++dev_->rxUsers;
        counted_ = true;
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
        LOG_DEBUG("uhd", "rx tune %.0f Hz: RF %.0f Hz, DSP %.0f Hz, result %.0f Hz", hz, res.actual_rf_freq,
                  res.actual_dsp_freq, f);
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
        if (db < gain_.min || db > gain_.max)
            LOG_WARN("uhd", "gain %.2f dB is outside %.2f-%.2f dB, clamped", db, gain_.min, gain_.max);
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

    std::shared_ptr<Usrp> dev_;
    bool counted_ = false;
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

// ---------------------------------------------------------------------------
// Transmitter
// ---------------------------------------------------------------------------
class UhdTx : public SdrTx {
public:
    ~UhdTx() override {
        stop();
        if (mdFirst_) uhd_tx_metadata_free(&mdFirst_);
        if (mdMid_) uhd_tx_metadata_free(&mdMid_);
        if (mdLast_) uhd_tx_metadata_free(&mdLast_);
        if (async_) uhd_async_metadata_free(&async_);
        if (tx_) uhd_tx_streamer_free(&tx_);
        if (counted_) --dev_->txUsers;
    }

    bool open(const TxConfig& cfg, std::string& err) {
        dev_ = acquireUsrp(cfg.args, err);
        if (!dev_) return false;
        usrp_ = dev_->h;
        if (dev_->rxUsers > 0) {
            double rxRate = 0;
            uhd_usrp_get_rx_rate(usrp_, 0, &rxRate);
            if (std::fabs(rxRate - cfg.rate) > 1) {
                err = "the USRP is receiving at " + fmtSI(rxRate, "S/s", 9) +
                      "; transmit at the same rate while receiving (the B2xx derives both from one clock)";
                return false;
            }
        }
        uhd_usrp_tx_info_t info{};
        if (uhd_usrp_get_tx_info(usrp_, 0, &info) == UHD_ERROR_NONE) {
            desc_ = std::string("USRP ") + (info.mboard_id ? info.mboard_id : "") +
                    (info.mboard_serial && info.mboard_serial[0] ? std::string(" serial ") + info.mboard_serial : "");
            uhd_usrp_tx_info_free(&info);
        } else {
            desc_ = "USRP";
        }
        const std::string ant = cfg.antenna.empty() ? "TX/RX" : cfg.antenna;
        if (uhd_usrp_set_tx_antenna(usrp_, ant.c_str(), 0) != UHD_ERROR_NONE) {
            err = lastError(("cannot select transmit antenna " + ant).c_str());
            return false;
        }
        if (uhd_usrp_set_tx_rate(usrp_, cfg.rate, 0) != UHD_ERROR_NONE) {
            err = lastError("cannot set the transmit sample rate");
            return false;
        }
        uhd_usrp_get_tx_rate(usrp_, 0, &rate_);
        if (std::fabs(rate_ - cfg.rate) > 1) {
            // The generator was built for the requested rate: refuse rather than shift every frequency.
            err = "the USRP can't transmit at " + fmtSI(cfg.rate, "S/s", 9) + " (nearest: " + fmtSI(rate_, "S/s", 9) + ")";
            return false;
        }
        if (cfg.bandwidth > 0 && uhd_usrp_set_tx_bandwidth(usrp_, cfg.bandwidth, 0) != UHD_ERROR_NONE) {
            err = lastError("cannot set the transmit bandwidth");
            return false;
        }
        uhd_meta_range_handle r = nullptr;
        if (uhd_meta_range_make(&r) == UHD_ERROR_NONE) {
            if (uhd_usrp_get_tx_gain_range(usrp_, "", 0, r) == UHD_ERROR_NONE) {
                uhd_meta_range_start(r, &range_.min);
                uhd_meta_range_stop(r, &range_.max);
                uhd_meta_range_step(r, &range_.step);
            }
            uhd_meta_range_free(&r);
        }
        range_.agc = false;
        if (uhd_meta_range_make(&r) == UHD_ERROR_NONE) {
            if (uhd_usrp_get_tx_freq_range(usrp_, 0, r) == UHD_ERROR_NONE) {
                uhd_meta_range_start(r, &freqMin_);
                uhd_meta_range_stop(r, &freqMax_);
            }
            uhd_meta_range_free(&r);
        }
        if (!setGain(cfg.gain, err)) return false;
        if (!setCenterFreq(cfg.fc, err)) return false;

        // Float samples on the host, sc16 on the wire: UHD converts and scales 1.0 to full scale.
        size_t chan = 0;
        uhd_stream_args_t sa{};
        sa.cpu_format = (char*)"fc32";
        sa.otw_format = (char*)"sc16";
        sa.args = (char*)"";
        sa.channel_list = &chan;
        sa.n_channels = 1;
        if (uhd_tx_streamer_make(&tx_) != UHD_ERROR_NONE || uhd_usrp_get_tx_stream(usrp_, &sa, tx_) != UHD_ERROR_NONE ||
            uhd_tx_metadata_make(&mdFirst_, false, 0, 0, true, false) != UHD_ERROR_NONE ||
            uhd_tx_metadata_make(&mdMid_, false, 0, 0, false, false) != UHD_ERROR_NONE ||
            uhd_tx_metadata_make(&mdLast_, false, 0, 0, false, true) != UHD_ERROR_NONE ||
            uhd_async_metadata_make(&async_) != UHD_ERROR_NONE) {
            err = lastError("cannot create the transmit stream");
            return false;
        }
        uhd_tx_streamer_max_num_samps(tx_, &spb_);
        if (spb_ == 0) spb_ = 2048;
        ++dev_->txUsers;
        counted_ = true;
        return true;
    }

    bool start(FillFn fill, std::string& err) override {
        stop();
        fill_ = std::move(fill);
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (!th_.joinable()) return;
        quit_ = true;
        th_.join();
    }

    // UHD clips an out-of-range target and tunes anyway. While transmitting,
    // that would move the signal to a frequency nobody chose (issue #40), so:
    // check the range first; after a mismatch, tune back to the last good
    // frequency; if that fails too, stop with a fatal error.
    bool setCenterFreq(double hz, std::string& err) override {
        if (freqMax_ > freqMin_ && (hz < freqMin_ - 1 || hz > freqMax_ + 1)) {
            err = "USRP cannot transmit at " + fmtSI(hz, "Hz", 9) + " (this device: " + fmtSI(freqMin_, "Hz", 9) + " to " +
                  fmtSI(freqMax_, "Hz", 9) + ")";
            return false;
        }
        const double prev = fc_;
        double f = tune(hz, err);
        if (err.empty() && std::fabs(f - hz) <= 1) {
            fc_ = f;
            return true;
        }
        if (err.empty())
            err = "USRP cannot transmit at " + fmtSI(hz, "Hz", 9) + "; it tuned to " + fmtSI(f, "Hz", 9);
        if (!th_.joinable()) {
            fc_ = f;  // not transmitting yet: open() fails and nothing is sent
            return false;
        }
        std::string backErr;
        f = tune(prev, backErr);
        fc_ = f;
        if (backErr.empty() && std::fabs(f - prev) <= 1) {
            err += "; still transmitting at " + fmtSI(prev, "Hz", 9);
            LOG_WARN("tx", "retune to %s failed, back at %s", fmtSI(hz, "Hz", 9).c_str(), fmtSI(prev, "Hz", 9).c_str());
            return false;
        }
        fail("retune failed and the transmitter could not return to " + fmtSI(prev, "Hz", 9) + " (now at " + fmtSI(f, "Hz", 9) +
             "): " + err);
        quit_ = true;  // stop sending now; the owner's stop() joins the thread
        return false;
    }

    bool setGain(double db, std::string& err) override {
        if (uhd_usrp_set_tx_gain(usrp_, std::clamp(db, range_.min, range_.max), 0, "") != UHD_ERROR_NONE) {
            err = lastError("cannot set the transmit gain");
            return false;
        }
        double g = db;
        uhd_usrp_get_tx_gain(usrp_, 0, "", &g);
        gain_ = g;
        return true;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    double gain() const override { return gain_; }
    GainRange gainRange() const override { return range_; }
    std::string description() const override { return desc_; }
    std::string notes() const override {
        if (underflows_ > 5 && rate_ > 8e6) return "frequent underflows: is the USRP on a USB 3 port?";
        if (underflows_ > 5) return "underflows: the host could not keep up (gaps in the transmitted signal)";
        return {};
    }
    std::string fatalError() const override {
        std::lock_guard<std::mutex> lk(errM_);
        return fatal_;
    }
    Stats stats() const override {
        Stats s;
        s.sent = sent_;
        s.underflows = underflows_;
        s.seqErrors = seqErrors_;
        s.timeErrors = timeErrors_;
        return s;
    }

private:
    // Tunes and returns the frequency the device reports. Sets err if UHD fails.
    double tune(double hz, std::string& err) {
        uhd_tune_request_t req{};
        req.target_freq = hz;
        req.rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO;
        req.dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO;
        req.args = (char*)"";
        uhd_tune_result_t res{};
        if (uhd_usrp_set_tx_freq(usrp_, &req, 0, &res) != UHD_ERROR_NONE)
            err = lastError(("cannot tune the transmitter to " + fmtSI(hz, "Hz", 9)).c_str());
        double f = hz;
        uhd_usrp_get_tx_freq(usrp_, 0, &f);
        return f;
    }

    void run() {
        std::vector<std::complex<float>> buf(spb_);
        const void* b[1] = {buf.data()};
        uhd_tx_metadata_handle* md = &mdFirst_;
        while (!quit_) {
            fill_(buf.data(), (int64_t)spb_);
            size_t done = 0;
            while (done < spb_ && !quit_) {
                b[0] = buf.data() + done;
                size_t n = 0;
                if (uhd_tx_streamer_send(tx_, b, spb_ - done, md, 0.5, &n) != UHD_ERROR_NONE) {
                    fail(lastError("transmit failed"));
                    return;
                }
                if (n == 0) {
                    if (++timeouts_ > 10) {
                        fail("USRP stopped accepting samples (unplugged?)");
                        return;
                    }
                    continue;
                }
                timeouts_ = 0;
                md = &mdMid_;
                done += n;
                sent_ += (int64_t)n;
            }
            pollAsync();
        }
        // End the burst so the transmitter stops instead of underflowing forever.
        std::complex<float> zero[1] = {};
        const void* z[1] = {zero};
        size_t n = 0;
        uhd_tx_streamer_send(tx_, z, 0, &mdLast_, 0.5, &n);
        pollAsync();
    }

    void pollAsync() {
        bool valid = true;
        while (valid) {
            valid = false;
            if (uhd_tx_streamer_recv_async_msg(tx_, &async_, 0.0, &valid) != UHD_ERROR_NONE || !valid) return;
            uhd_async_metadata_event_code_t ec{};
            uhd_async_metadata_event_code(async_, &ec);
            switch (ec) {
                case UHD_ASYNC_METADATA_EVENT_CODE_UNDERFLOW:
                case UHD_ASYNC_METADATA_EVENT_CODE_UNDERFLOW_IN_PACKET: ++underflows_; break;
                case UHD_ASYNC_METADATA_EVENT_CODE_SEQ_ERROR:
                case UHD_ASYNC_METADATA_EVENT_CODE_SEQ_ERROR_IN_BURST: ++seqErrors_; break;
                case UHD_ASYNC_METADATA_EVENT_CODE_TIME_ERROR: ++timeErrors_; break;
                default: break;
            }
        }
    }

    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lk(errM_);
        fatal_ = why;
    }

    std::shared_ptr<Usrp> dev_;
    bool counted_ = false;
    uhd_usrp_handle usrp_ = nullptr;
    uhd_tx_streamer_handle tx_ = nullptr;
    uhd_tx_metadata_handle mdFirst_ = nullptr, mdMid_ = nullptr, mdLast_ = nullptr;
    uhd_async_metadata_handle async_ = nullptr;
    size_t spb_ = 0;
    double rate_ = 0;
    std::atomic<double> fc_{0}, gain_{0};
    double freqMin_ = 0, freqMax_ = 0;  // the tunable range; 0, 0 = unknown
    GainRange range_{0, 89.75, 0.25, {}, false};
    std::string desc_;
    FillFn fill_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    int timeouts_ = 0;
    std::atomic<int64_t> sent_{0}, underflows_{0}, seqErrors_{0}, timeErrors_{0};
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

std::unique_ptr<SdrTx> openUhdTx(const TxConfig& cfg, std::string& err) {
    auto t = std::make_unique<UhdTx>();
    if (!t->open(cfg, err)) return nullptr;
    return t;
}
