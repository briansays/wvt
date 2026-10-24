// Epiq Solutions Sidekiq backend (libsidekiq from the Sidekiq SDK, issue #4).
// Targets the cards in an x86-64 Linux host: mPCIe, M.2 and M.2-2280 (AD9361,
// 12 bits), X2 and X4 (ADRV9009, 16 bits), NV100 (ADRV9002, 16 bits), on PCIe
// or (mPCIe, M.2) USB. wvt receives on one handle: the one of the chosen RF
// port (issue #85), A1 by default.
//
// skiq_init brings up the open card, and skiq_exit shuts the whole library
// down, so one Sidekiq is open at a time. The card streams blocks of 16-bit
// I/Q, each with an RF timestamp that counts samples: a jump between blocks is
// lost samples. A retune is written while the card streams; the RF timestamp
// of an AD9361 card slips then, so the gap check pauses for a moment after it.
#include <sidekiq_api.h>

// sidekiq_api.h includes <syslog.h>, which defines LOG_DEBUG and LOG_INFO as
// numbers; wvt's log macros have the same names. Keep the SDK's levels, then
// make room for the log macros.
namespace {
constexpr int kSkiqLogError = SKIQ_LOG_ERROR, kSkiqLogWarning = SKIQ_LOG_WARNING;
}  // namespace
#undef LOG_DEBUG
#undef LOG_INFO

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/ui_util.h"
#include "device/sdr_device.h"

namespace {

constexpr int32_t kRxTimeoutUs = 25000;  // the SDK manual's advice for a blocking receive
constexpr double kSettleSec = 0.5;        // no gap check this long after a retune (timestamp slip)

// The library is global to the process: skiq_init, skiq_exit and the device
// list take this lock.
std::mutex gLibM;
bool gOpen = false;      // a SkiqDevice has a card initialized
DeviceInfo gOpenInfo;    // the open card, for the device list (it can't be probed while open)

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void addNote(std::string& n, const std::string& s) { n += (n.empty() ? "" : "; ") + s; }

// The SDK prints to stdout unless it has a logging function: send its
// messages to the wvt log instead.
void sdkLog(int32_t priority, const char* message) {
    std::string m = message ? message : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    if (priority <= kSkiqLogError) LOG_ERROR("skiq", "%s", m.c_str());
    else if (priority <= kSkiqLogWarning) LOG_WARN("skiq", "%s", m.c_str());
    else LOG_DEBUG("skiq", "%s", m.c_str());
}

void registerLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        skiq_register_logging(sdkLog);
        uint8_t major = 0, minor = 0, patch = 0;
        const char* label = nullptr;
        if (skiq_read_libsidekiq_version(&major, &minor, &patch, &label) == 0)
            LOG_INFO("skiq", "libsidekiq %u.%u.%u%s", major, minor, patch, label ? label : "");
    });
}

struct PartInfo {
    const char* name;
    double dbPerStep;  // the gain index step (the API's skiq_write_rx_gain description)
};

PartInfo partInfo(skiq_part_t p) {
    switch (p) {
        case skiq_mpcie: return {"mPCIe", 1};
        case skiq_m2: return {"M.2", 1};
        case skiq_m2_2280: return {"M.2-2280", 1};
        case skiq_z2: return {"Z2", 1};
        case skiq_z2p: return {"Z2p", 1};
        case skiq_z3u: return {"Z3u", 1};
        case skiq_x2: return {"X2", 0.5};
        case skiq_x4: return {"X4", 0.5};
        case skiq_x40: return {"X40", 0.5};
        case skiq_nv100: return {"NV100", 0.5};
        case skiq_nvm2: return {"NVM2", 0.5};
        case skiq_z4: return {"Z4", 0.5};
        case skiq_z4_mp: return {"Z4", 0.5};
        default: return {nullptr, 1};
    }
}

std::string partName(skiq_part_t p) {
    if (const char* n = partInfo(p).name) return n;
    const char* s = skiq_part_string(p);
    return s && s[0] ? s : "card";
}

const char* xportName(skiq_xport_type_t x) {
    switch (x) {
        case skiq_xport_type_pcie: return "PCIe";
        case skiq_xport_type_usb: return "USB";
        case skiq_xport_type_net: return "network";
        case skiq_xport_type_custom: return "custom";
        default: return "";
    }
}

std::string label(const skiq_card_param_t& c) {
    std::string s = "Sidekiq " + partName(c.part_type);
    if (c.serial_string[0]) s += " serial " + std::string(c.serial_string);
    if (xportName(c.xport)[0]) s += std::string(" (") + xportName(c.xport) + ")";
    return s;
}

std::string skiqError(const std::string& what, int32_t r) { return what + " (libsidekiq error " + std::to_string(r) + ")"; }

const char* hdlName(skiq_rx_hdl_t h) {
    static const char* names[] = {"A1", "A2", "B1", "B2", "C1", "D1"};
    return h >= 0 && h < skiq_rx_hdl_end ? names[h] : "?";
}

class SkiqDevice : public SdrDevice {
public:
    ~SkiqDevice() override {
        stop();
        std::lock_guard<std::mutex> lk(gLibM);
        if (inited_) {
            skiq_exit();
            gOpen = false;
            LOG_DEBUG("skiq", "card %u closed", card_);
        }
    }

    bool open(const StreamConfig& cfg, std::string& err) {
        registerLogging();
        {
            std::lock_guard<std::mutex> lk(gLibM);
            if (gOpen) {
                err = "a Sidekiq card is open already (wvt opens one at a time)";
                return false;
            }
            if (!findCard(cfg.args, err)) return false;
            pid_t owner = 0;
            if (const int32_t r = skiq_is_card_avail(card_, &owner); r != 0) {
                err = owner > 0 ? "Sidekiq card " + std::to_string(card_) + " is in use by process " + std::to_string(owner)
                                : skiqError("Sidekiq card " + std::to_string(card_) + " is not available", r);
                return false;
            }
            const double t0 = nowSec();
            if (const int32_t r = skiq_init(skiq_xport_type_auto, skiq_xport_init_level_full, &card_, 1); r != 0) {
                err = skiqError("cannot initialize Sidekiq card " + std::to_string(card_), r) +
                      " (are the SDK's kernel modules loaded? does the SDK's sidekiq_probe see the card?)";
                return false;
            }
            inited_ = true;
            gOpen = true;
            LOG_DEBUG("skiq", "card %u initialized in %.0f ms", card_, (nowSec() - t0) * 1e3);

            if (const int32_t r = skiq_read_parameters(card_, &param_); r != 0) {
                err = skiqError("cannot read the Sidekiq card parameters", r);
                return false;
            }
            part_ = param_.card_param.part_type;
            usb_ = param_.card_param.xport == skiq_xport_type_usb;
            desc_ = label(param_.card_param);
            gOpenInfo = {"skiq",
                         param_.card_param.serial_string[0] ? param_.card_param.serial_string : "#" + std::to_string(card_),
                         desc_};
            listPorts();
        }
        return configure(cfg, err);
    }

    bool start(DataFn data, GapFn gap, std::string& err) override {
        stop();
        data_ = std::move(data);
        gap_ = std::move(gap);
        {
            std::lock_guard<std::mutex> lk(noteM_);
            fatal_.clear();
        }
        if (const int32_t r = skiq_start_rx_streaming(card_, hdl_); r != 0) {
            err = skiqError("cannot start Sidekiq streaming", r);
            return false;
        }
        LOG_DEBUG("skiq", "streaming started (%s receive)", blocking_ ? "blocking" : "polled");
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (!th_.joinable()) return;
        quit_ = true;
        // This also makes a blocking skiq_receive return.
        skiq_stop_rx_streaming(card_, hdl_);
        th_.join();
    }

    bool setCenterFreq(double hz, std::string& err) override {
        if (!checkFreq(hz, err)) return false;
        const double t0 = nowSec();
        tuneAt_ = t0;
        const int32_t r = skiq_write_rx_LO_freq(card_, hdl_, (uint64_t)std::llround(hz));
        tuneAt_ = nowSec();
        if (r != 0) {
            err = skiqError("cannot tune to " + fmtSI(hz, "Hz", 9), r);
            return false;
        }
        uint64_t f = 0;
        double actual = 0;
        fc_ = skiq_read_rx_LO_freq(card_, hdl_, &f, &actual) == 0 && actual > 0 ? actual : hz;
        LOG_DEBUG("skiq", "tuned to %s in %.0f ms", fmtSI(fc_, "Hz", 9).c_str(), (nowSec() - t0) * 1e3);
        return true;
    }

    bool setGain(bool agc, double db, std::string& err) override {
        std::lock_guard<std::mutex> lk(gainM_);
        if (agc) {
            if (!agcOk_) {
                err = "this Sidekiq has no automatic gain";
                return false;
            }
            if (const int32_t r = skiq_write_rx_gain_mode(card_, hdl_, skiq_rx_gain_auto); r != 0) {
                err = skiqError("cannot select automatic gain", r);
                return false;
            }
            LOG_DEBUG("skiq", "automatic gain");
            return true;
        }
        if (const int32_t r = skiq_write_rx_gain_mode(card_, hdl_, skiq_rx_gain_manual); r != 0) {
            err = skiqError("cannot select manual gain", r);
            return false;
        }
        const int index = skiqGainIndex(db, gainMin_, gainMax_, dbPerStep_);
        if (const int32_t r = skiq_write_rx_gain(card_, hdl_, (uint8_t)index); r != 0) {
            err = skiqError("cannot set gain index " + std::to_string(index), r);
            return false;
        }
        LOG_DEBUG("skiq", "gain %.1f dB (index %d of %d-%d)", skiqGainDb(index, gainMin_, dbPerStep_), index, gainMin_,
                  gainMax_);
        return true;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return SampleFormat::CI16; }
    GainRange gainRange() const override {
        GainRange g;
        g.min = 0;
        g.max = skiqGainDb(gainMax_, gainMin_, dbPerStep_);
        g.step = dbPerStep_;
        g.agc = agcOk_;
        return g;
    }
    std::string description() const override { return desc_; }
    std::string notes() const override {
        std::string n = openNotes_;
        if (nowSec() - lastOverload_.load() < 1) addNote(n, "RF overload (input above 0 dBm)");
        return n;
    }
    DeviceCaps caps() const override {
        DeviceCaps c;
        c.product = partName(part_);
        c.usb = usb_;
        for (const auto& p : ports_) c.antennas.push_back(skiqPortLabel(p.name));
        c.freq = {loMin_, loMax_};
        c.rate = {rateMin_, rateMax_};
        return c;
    }
    std::string fatalError() const override {
        std::lock_guard<std::mutex> lk(noteM_);
        return fatal_;
    }

private:
    // "" = the first free card, "#N" = card number N, else a serial number.
    // Runs with gLibM held.
    bool findCard(const std::string& args, std::string& err) {
        uint8_t n = 0, cards[SKIQ_MAX_NUM_CARDS] = {};
        if (skiq_get_cards(skiq_xport_type_auto, &n, cards) != 0) n = 0;
        n = std::min<uint8_t>(n, SKIQ_MAX_NUM_CARDS);
        if (args.empty()) {
            if (n == 0) {
                err = "no Sidekiq card found (are the SDK's kernel modules loaded? for USB, is the udev rule installed?)";
                return false;
            }
            card_ = cards[0];
            for (int i = 0; i < n; ++i)
                if (skiq_is_card_avail(cards[i], nullptr) == 0) {
                    card_ = cards[i];
                    break;
                }
            return true;
        }
        if (args[0] == '#') {
            char* end = nullptr;
            const long c = strtol(args.c_str() + 1, &end, 10);
            if (args.size() < 2 || !end || *end != 0 || c < 0 || c >= SKIQ_MAX_NUM_CARDS) {
                err = "Sidekiq card number must be #0 to #" + std::to_string(SKIQ_MAX_NUM_CARDS - 1) + " (got '" + args + "')";
                return false;
            }
            card_ = (uint8_t)c;
            if (std::find(cards, cards + n, card_) == cards + n) {
                err = "no Sidekiq card " + args;
                return false;
            }
            return true;
        }
        std::string serial = args;
        if (skiq_get_card_from_serial_string(serial.data(), &card_) != 0) {
            err = "no Sidekiq card with serial number " + args;
            return false;
        }
        return true;
    }

    bool checkFreq(double hz, std::string& err) const {
        if (loMax_ > 0 && (hz < loMin_ || hz > loMax_)) {
            err = "Sidekiq " + partName(part_) + " cannot tune to " + fmtSI(hz, "Hz", 9) + " (range " +
                  fmtSI(loMin_, "Hz", 4) + " - " + fmtSI(loMax_, "Hz", 4) + ")";
            return false;
        }
        if (!(hz > 0)) {
            err = "center frequency must be positive";
            return false;
        }
        return true;
    }

    // The receive ports of every handle (issue #85). The SDK has a fixed and a
    // TRX list for each handle; each list is usable only in its RF port
    // configuration, which is one for the whole card. Runs with gLibM held.
    void listPorts() {
        uint8_t n = 1;
        if (skiq_read_num_rx_chans(card_, &n) != 0 || n == 0) n = 1;
        numHandles_ = std::min<int>(n, skiq_rx_hdl_end);
        if (skiq_read_rf_port_config_avail(card_, &fixedOk_, &trxOk_) != 0 || (!fixedOk_ && !trxOk_)) {
            fixedOk_ = true;
            trxOk_ = false;
        }
        for (int h = 0; h < numHandles_; ++h) {
            const auto hdl = (skiq_rx_hdl_t)h;
            uint8_t nFixed = 0, nTrx = 0;
            skiq_rf_port_t fixed[skiq_rf_port_max] = {}, trx[skiq_rf_port_max] = {};
            if (skiq_read_rx_rf_ports_avail_for_hdl(card_, hdl, &nFixed, fixed, &nTrx, trx) != 0) {
                const skiq_rx_param_t& rx = param_.rx_param[h];
                nFixed = std::min<uint8_t>(rx.num_fixed_rf_ports, skiq_rf_port_max);
                nTrx = std::min<uint8_t>(rx.num_trx_rf_ports, skiq_rf_port_max);
                std::copy(rx.fixed_rf_ports, rx.fixed_rf_ports + nFixed, fixed);
                std::copy(rx.trx_rf_ports, rx.trx_rf_ports + nTrx, trx);
            }
            auto add = [&](const skiq_rf_port_t* list, uint8_t count, bool isTrx) {
                for (int i = 0; i < count && i < skiq_rf_port_max; ++i) {
                    const char* s = list[i] == skiq_rf_port_unknown ? nullptr : skiq_rf_port_string(list[i]);
                    if (s && s[0]) ports_.push_back({hdl, list[i], {s, hdlName(hdl), isTrx}});
                }
            };
            if (fixedOk_) add(fixed, nFixed, false);
            if (trxOk_) add(trx, nTrx, true);
        }
        std::string list;
        for (const auto& p : ports_) list += (list.empty() ? "" : ", ") + skiqPortLabel(p.name);
        LOG_DEBUG("skiq", "%d receive handle(s), ports: %s", numHandles_, list.empty() ? "none listed" : list.c_str());
    }

    // Selects the handle and the RF port named `name` ("" = handle A1 on the
    // card's port), and takes the limits of that handle.
    bool selectPort(const std::string& name, std::string& err) {
        hdl_ = skiq_rx_hdl_A1;
        const Port* sel = nullptr;
        if (!name.empty()) {
            std::vector<SkiqPort> names;
            std::string list;
            for (const auto& p : ports_) {
                names.push_back(p.name);
                list += (list.empty() ? "" : ", ") + skiqPortLabel(p.name);
            }
            const int i = skiqFindPort(names, name);
            if (i < 0) {
                err = "Sidekiq " + partName(part_) + " has no receive port '" + name + "'" +
                      (list.empty() ? std::string() : " (ports: " + list + ")");
                return false;
            }
            sel = &ports_[i];
            hdl_ = sel->hdl;
        }
        // A2 and B2 receive in the dual channel mode, as in the SDK's rx_samples.
        if (numHandles_ > 1) {
            const bool dual = hdl_ == skiq_rx_hdl_A2 || hdl_ == skiq_rx_hdl_B2;
            const int32_t r = skiq_write_chan_mode(card_, dual ? skiq_chan_mode_dual : skiq_chan_mode_single);
            if (r != 0 && dual) {
                err = skiqError(std::string("cannot select the dual channel mode for handle ") + hdlName(hdl_), r);
                return false;
            }
        }
        if (sel) {
            const std::string what = skiqPortLabel(sel->name);
            // A card with both configurations switches to the port's one.
            if (fixedOk_ && trxOk_) {
                const int32_t r =
                    skiq_write_rf_port_config(card_, sel->name.trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed);
                if (r != 0) {
                    err = skiqError("cannot select the RF port configuration for " + what, r);
                    return false;
                }
            }
            // A TRX port also transmits: set it to receive. Sharing it with the
            // transmitter is issue #86.
            if (sel->name.trx)
                if (const int32_t r = skiq_write_rf_port_operation(card_, false); r != 0) {
                    err = skiqError("cannot set " + what + " to receive", r);
                    return false;
                }
            if (const int32_t r = skiq_write_rx_rf_port_for_hdl(card_, hdl_, sel->rf); r != 0) {
                err = skiqError("cannot select receive port " + what, r);
                return false;
            }
        }
        std::string port = "default port";
        skiq_rf_port_t rf = skiq_rf_port_unknown;
        if (skiq_read_rx_rf_port_for_hdl(card_, hdl_, &rf) == 0 && rf != skiq_rf_port_unknown)
            if (const char* s = skiq_rf_port_string(rf)) port = std::string("port ") + s;

        // A handle without parameters takes those of A1 (the same RFIC family):
        // a wrong resolution would scale 16-bit samples by 16.
        const skiq_rx_param_t& rx =
            param_.rx_param[hdl_].sample_rate_max > 0 ? param_.rx_param[hdl_] : param_.rx_param[skiq_rx_hdl_A1];
        resolution_ = rx.iq_resolution > 0 && rx.iq_resolution <= 16 ? rx.iq_resolution : 12;
        rateMin_ = rx.sample_rate_min;
        rateMax_ = rx.sample_rate_max;
        loMin_ = (double)rx.lo_freq_min;
        loMax_ = (double)rx.lo_freq_max;
        LOG_INFO("skiq", "opened %s, handle %s, %s: %d-bit I/Q, %s to %s, %s to %s", desc_.c_str(), hdlName(hdl_),
                 port.c_str(), resolution_, fmtSI(rateMin_, "S/s", 4).c_str(), fmtSI(rateMax_, "S/s", 4).c_str(),
                 fmtSI(loMin_, "Hz", 4).c_str(), fmtSI(loMax_, "Hz", 4).c_str());
        return true;
    }

    bool configure(const StreamConfig& cfg, std::string& err) {
        // A new reference clock starts a full RF initialization, so it comes
        // before the other settings.
        if (cfg.extRef) {
            if (const int32_t r = skiq_write_ref_clock_select(card_, skiq_ref_clock_external); r != 0) {
                err = skiqError("cannot select the external reference clock", r) +
                      (part_ == skiq_m2 || part_ == skiq_mpcie
                           ? "; the M.2 and mPCIe select it only in their stored settings (the SDK's ref_clock app)"
                           : "");
                return false;
            }
            LOG_INFO("skiq", "external reference clock");
        }
        // The default order is Q then I; ci16 is I then Q. An FPGA that can't
        // swap gets swapped on the host.
        swap_ = skiq_write_iq_order_mode(card_, skiq_iq_order_iq) != 0;
        if (swap_) LOG_DEBUG("skiq", "the FPGA keeps Q/I order: wvt swaps the samples");

        if (!selectPort(cfg.antenna, err)) return false;

        if (rateMax_ > 0 && (cfg.rate < rateMin_ * 0.999 || cfg.rate > rateMax_ * 1.001)) {
            err = "Sidekiq " + partName(part_) + " sample rate must be " + fmtSI(rateMin_, "S/s", 4) + " to " +
                  fmtSI(rateMax_, "S/s", 4) + " (got " + fmtSI(cfg.rate, "S/s", 4) + ")";
            return false;
        }
        const uint32_t rate = (uint32_t)std::lround(cfg.rate);
        const uint32_t bw = (uint32_t)std::lround(cfg.bandwidth > 0 ? cfg.bandwidth : 0.8 * cfg.rate);
        if (const int32_t r = skiq_write_rx_sample_rate_and_bandwidth(card_, hdl_, rate, bw); r != 0) {
            err = skiqError("cannot set sample rate " + fmtSI(cfg.rate, "S/s", 6) + ", bandwidth " +
                                fmtSI(bw, "Hz", 4),
                            r);
            return false;
        }
        uint32_t gotRate = 0, gotBw = 0, actualBw = 0;
        double actualRate = 0;
        rate_ = cfg.rate;
        if (skiq_read_rx_sample_rate_and_bandwidth(card_, hdl_, &gotRate, &actualRate, &gotBw, &actualBw) == 0 &&
            actualRate > 0)
            rate_ = actualRate;
        if (std::fabs(rate_ - cfg.rate) > 1) addNote(openNotes_, "rate set to " + fmtSI(rate_, "S/s", 7));
        std::string warn, e2;
        checkSampleRate("skiq", rate_, e2, warn);
        if (!warn.empty()) addNote(openNotes_, warn);
        LOG_DEBUG("skiq", "rate %s, bandwidth %s", fmtSI(rate_, "S/s", 7).c_str(),
                  fmtSI(actualBw ? actualBw : bw, "Hz", 4).c_str());

        if (!setCenterFreq(cfg.fc, err)) return false;

        dbPerStep_ = partInfo(part_).dbPerStep;
        uint8_t gmin = 0, gmax = 0;
        if (skiq_read_rx_gain_index_range(card_, hdl_, &gmin, &gmax) == 0 && gmax >= gmin) {
            gainMin_ = gmin;
            gainMax_ = gmax;
        }
        // Not every card has automatic gain: try it.
        agcOk_ = skiq_write_rx_gain_mode(card_, hdl_, skiq_rx_gain_auto) == 0;
        LOG_DEBUG("skiq", "gain index %d-%d, %.1f dB per step, automatic gain %s", gainMin_, gainMax_, dbPerStep_,
                  agcOk_ ? "yes" : "no");
        if (!setGain(cfg.agc && agcOk_, cfg.gain, err)) return false;
        if (cfg.agc && !agcOk_) addNote(openNotes_, "no automatic gain on this card: manual gain");

        // A blocking receive saves CPU; the USB transport has none, so the
        // receive thread polls there.
        blocking_ = skiq_set_rx_transfer_timeout(card_, kRxTimeoutUs) == 0;
        int32_t t = 0;
        if (skiq_get_rx_transfer_timeout(card_, &t) == 0 && (t == RX_TRANSFER_WAIT_NOT_SUPPORTED || t == RX_TRANSFER_NO_WAIT))
            blocking_ = false;
        tuneAt_ = -1e9;  // the tuning above was not while streaming: no timestamp slip
        return true;
    }

    void run() {
        // About 20 ms per delivery: fewer ring writes than one per block.
        const size_t chunk = (size_t)std::clamp(rate_.load() * 0.02, 4096.0, 4194304.0);
        std::vector<int16_t> buf;
        size_t fill = 0;  // samples in buf
        bool haveTs = false, overrun = false;
        uint64_t nextTs = 0;
        int errors = 0;
        double lastPoll = 0;
        while (!quit_) {
            skiq_rx_hdl_t hdl = hdl_;
            skiq_rx_block_t* blk = nullptr;
            uint32_t len = 0;
            const skiq_rx_status_t st = skiq_receive(card_, &hdl, &blk, &len);
            const double now = nowSec();
            if (st == skiq_rx_status_success) {
                errors = 0;
                if (hdl != hdl_ || !blk || len <= SKIQ_RX_HEADER_SIZE_IN_BYTES) continue;
                const size_t n = (len - SKIQ_RX_HEADER_SIZE_IN_BYTES) / 4;
                const uint64_t ts = blk->rf_timestamp;
                // The timestamps are checked once they have settled after a retune.
                const int64_t lost = haveTs && now - tuneAt_.load() > kSettleSec ? skiqLostSamples(nextTs, ts) : 0;
                if (gap_ && lost > 0) gap_(lost, overrun ? "Sidekiq overrun (host too slow)" : "Sidekiq sample loss");
                else if (gap_ && overrun) gap_(-1, "Sidekiq overrun (host too slow)");
                overrun = false;
                nextTs = ts + n;
                haveTs = true;
                if (blk->overload) lastOverload_ = now;
                if (buf.size() < (fill + n) * 2) buf.resize((std::max(chunk, fill + n) + n) * 2);
                skiqConvert((const int16_t*)blk->data, buf.data() + fill * 2, n * 2, resolution_, swap_);
                fill += n;
                if (fill >= chunk) {
                    data_((const uint8_t*)buf.data(), (int64_t)fill);
                    fill = 0;
                }
            } else if (st == skiq_rx_status_no_data) {
                if (!blocking_) std::this_thread::sleep_for(std::chrono::microseconds(500));
            } else if (st == skiq_rx_status_error_overrun) {
                // The next block's timestamp tells how many samples were lost.
                overrun = true;
            } else {
                if (quit_) break;
                if (++errors >= 10) {
                    std::lock_guard<std::mutex> lk(noteM_);
                    fatal_ = "Sidekiq stopped streaming (receive error " + std::to_string((int)st) + "; card removed?)";
                    LOG_ERROR("skiq", "%s", fatal_.c_str());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            // The block header has no overload flag on USB: ask the card too.
            if (now - lastPoll > 0.5) {
                lastPoll = now;
                uint8_t o = 0;
                if (skiq_read_rx_overload_state(card_, hdl_, &o) == 0 && o) lastOverload_ = now;
            }
        }
    }

    uint8_t card_ = 0;
    bool inited_ = false, usb_ = false, swap_ = false, blocking_ = false, agcOk_ = false;
    skiq_part_t part_ = skiq_part_invalid;
    int resolution_ = 12;
    double rateMin_ = 0, rateMax_ = 0, loMin_ = 0, loMax_ = 0;
    int gainMin_ = 0, gainMax_ = 0;
    double dbPerStep_ = 1;
    struct Port {
        skiq_rx_hdl_t hdl;
        skiq_rf_port_t rf;
        SkiqPort name;
    };
    skiq_param_t param_{};
    std::vector<Port> ports_;
    int numHandles_ = 1;
    bool fixedOk_ = true, trxOk_ = false;  // the RF port configurations of the card
    skiq_rx_hdl_t hdl_ = skiq_rx_hdl_A1;   // the handle that receives
    std::atomic<double> fc_{0}, rate_{0};
    std::atomic<double> tuneAt_{-1e9}, lastOverload_{-1e9};
    std::string desc_, openNotes_;
    DataFn data_;
    GapFn gap_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::mutex gainM_;
    mutable std::mutex noteM_;
    std::string fatal_;
};

}  // namespace

void enumerateSkiq(std::vector<DeviceInfo>& out) {
    registerLogging();
    std::lock_guard<std::mutex> lk(gLibM);
    // An open card can't be probed (skiq_init is once per process); list it as it was opened.
    if (gOpen) {
        out.push_back(gOpenInfo);
        return;
    }
    uint8_t n = 0, cards[SKIQ_MAX_NUM_CARDS] = {};
    if (skiq_get_cards(skiq_xport_type_auto, &n, cards) != 0 || n == 0) return;
    n = std::min<uint8_t>(n, SKIQ_MAX_NUM_CARDS);
    std::vector<DeviceInfo> found(n);
    std::vector<uint8_t> avail;
    for (int i = 0; i < n; ++i) {
        const std::string num = "#" + std::to_string(cards[i]);
        found[i] = {"skiq", num, "Sidekiq card " + std::to_string(cards[i])};
        pid_t owner = 0;
        if (skiq_is_card_avail(cards[i], &owner) == 0) avail.push_back(cards[i]);
        else if (owner > 0) found[i].label += " (in use by process " + std::to_string(owner) + ")";
    }
    // A basic initialization reads the part, serial number and transport.
    if (!avail.empty() && skiq_init(skiq_xport_type_auto, skiq_xport_init_level_basic, avail.data(),
                                    (uint8_t)avail.size()) == 0) {
        for (int i = 0; i < n; ++i) {
            if (std::find(avail.begin(), avail.end(), cards[i]) == avail.end()) continue;
            skiq_param_t p{};
            if (skiq_read_parameters(cards[i], &p) != 0) continue;
            if (p.card_param.serial_string[0]) found[i].args = p.card_param.serial_string;
            found[i].label = label(p.card_param);
        }
        skiq_exit();
    }
    out.insert(out.end(), found.begin(), found.end());
}

std::unique_ptr<SdrDevice> openSkiqDevice(const StreamConfig& cfg, std::string& err) {
    auto d = std::make_unique<SkiqDevice>();
    if (!d->open(cfg, err)) return nullptr;
    return d;
}
