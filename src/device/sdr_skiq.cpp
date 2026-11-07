// Epiq Solutions Sidekiq backend (libsidekiq from the Sidekiq SDK, issue #4).
// Targets the cards in an x86-64 Linux host: mPCIe, M.2 and M.2-2280 (AD9361,
// 12 bits), X2 and X4 (ADRV9009, 16 bits), NV100 (ADRV9002, 16 bits), on PCIe
// or (mPCIe, M.2) USB. wvt receives on one handle: the one of the chosen RF
// port (issue #85), A1 by default. The signal generator transmits on one TX
// handle (issue #86), also chosen by its port.
//
// skiq_init brings up the open card, and skiq_exit shuts the whole library
// down, so one Sidekiq is open at a time. The receiver and the transmitter
// share it (full duplex) through a reference-counted Card: the first user
// initializes the card and the last one exits. Settings for the whole card
// (rate clock, RF port configuration, channel mode, reference) are refused
// when they would change what the other user runs on.
//
// The card streams blocks of 16-bit I/Q, each with an RF timestamp that
// counts samples: a jump between blocks is lost samples. A retune is written
// while the card streams; the RF timestamp of an AD9361 card slips then (also
// on a transmit retune or start), so the gap check pauses for a moment after it.
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/ui_util.h"
#include "device/sdr_device.h"
#include "device/sdr_tx.h"

namespace {

constexpr int32_t kRxTimeoutUs = 25000;  // the SDK manual's advice for a blocking receive
constexpr double kSettleSec = 0.5;        // no gap check this long after a retune (timestamp slip)

// The library is global to the process: skiq_init, skiq_exit and the device
// list take this lock. Recursive: a Card that fails while it opens exits the
// library in its destructor with the lock held.
std::recursive_mutex gLibM;

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

const char* txHdlName(skiq_tx_hdl_t h) {
    static const char* names[] = {"A1", "A2", "B1", "B2"};
    return h >= 0 && h < skiq_tx_hdl_end ? names[h] : "?";
}

const char* configName(skiq_rf_port_config_t c) { return c == skiq_rf_port_config_trx ? "TRX" : "fixed"; }

// The open card, shared by the receiver and the transmitter (issue #86).
struct Card {
    uint8_t num = 0;
    skiq_param_t param{};
    skiq_part_t part = skiq_part_invalid;
    bool usb = false;
    std::string desc;
    DeviceInfo info;                 // for the device list (an open card can't be probed)
    bool fixedOk = true, trxOk = false;  // the RF port configurations of the card
    // The users, under m. Each user configures the card with m held.
    std::mutex m;
    bool rx = false, tx = false;
    double rxRate = 0, txRate = 0;
    bool rxTrx = false;   // the receiver is on a TRX port
    bool rxDual = false, txDual = false;  // the user needs the dual channel mode
    skiq_rf_port_config_t config = skiq_rf_port_config_fixed;
    // The last transmit event that can slip the receive RF timestamps on an
    // AD9361 card (start, retune, rate): the receiver checks no gaps just after it.
    std::atomic<double> slipAt{-1e9};

    ~Card() {
        std::lock_guard<std::recursive_mutex> lk(gLibM);
        skiq_exit();
        LOG_DEBUG("skiq", "card %u closed", num);
    }
};
std::weak_ptr<Card> gCard;

// "" = the first free card, "#N" = card number N, else a serial number.
// Runs with gLibM held.
bool findCard(const std::string& args, uint8_t& card, std::string& err) {
    uint8_t n = 0, cards[SKIQ_MAX_NUM_CARDS] = {};
    if (skiq_get_cards(skiq_xport_type_auto, &n, cards) != 0) n = 0;
    n = std::min<uint8_t>(n, SKIQ_MAX_NUM_CARDS);
    if (args.empty()) {
        if (n == 0) {
            err = "no Sidekiq card found (are the SDK's kernel modules loaded? for USB, is the udev rule installed?)";
            return false;
        }
        card = cards[0];
        for (int i = 0; i < n; ++i)
            if (skiq_is_card_avail(cards[i], nullptr) == 0) {
                card = cards[i];
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
        card = (uint8_t)c;
        if (std::find(cards, cards + n, card) == cards + n) {
            err = "no Sidekiq card " + args;
            return false;
        }
        return true;
    }
    std::string serial = args;
    if (skiq_get_card_from_serial_string(serial.data(), &card) != 0) {
        err = "no Sidekiq card with serial number " + args;
        return false;
    }
    return true;
}

// The open card if `args` selects it (or is empty), else a new one.
std::shared_ptr<Card> acquireCard(const std::string& args, std::string& err) {
    registerLogging();
    std::lock_guard<std::recursive_mutex> lk(gLibM);
    if (auto c = gCard.lock()) {
        if (args.empty() || args == c->info.args || args == c->info.serial || args == "#" + std::to_string(c->num))
            return c;
        err = c->desc + " is open already (wvt opens one Sidekiq at a time)";
        return nullptr;
    }
    uint8_t num = 0;
    if (!findCard(args, num, err)) return nullptr;
    pid_t owner = 0;
    if (const int32_t r = skiq_is_card_avail(num, &owner); r != 0) {
        err = owner > 0 ? "Sidekiq card " + std::to_string(num) + " is in use by process " + std::to_string(owner)
                        : skiqError("Sidekiq card " + std::to_string(num) + " is not available", r);
        return nullptr;
    }
    const double t0 = nowSec();
    if (const int32_t r = skiq_init(skiq_xport_type_auto, skiq_xport_init_level_full, &num, 1); r != 0) {
        err = skiqError("cannot initialize Sidekiq card " + std::to_string(num), r) +
              " (are the SDK's kernel modules loaded? does the SDK's sidekiq_probe see the card?)";
        return nullptr;
    }
    auto c = std::make_shared<Card>();  // from here on, a failure exits the library
    c->num = num;
    LOG_DEBUG("skiq", "card %u initialized in %.0f ms", num, (nowSec() - t0) * 1e3);
    if (const int32_t r = skiq_read_parameters(num, &c->param); r != 0) {
        err = skiqError("cannot read the Sidekiq card parameters", r);
        return nullptr;
    }
    c->part = c->param.card_param.part_type;
    c->usb = c->param.card_param.xport == skiq_xport_type_usb;
    c->desc = label(c->param.card_param);
    c->info = {"skiq", c->param.card_param.serial_string[0] ? c->param.card_param.serial_string : "#" + std::to_string(num),
               c->desc, partName(c->part), c->param.card_param.serial_string};
    if (skiq_read_rf_port_config_avail(num, &c->fixedOk, &c->trxOk) != 0 || (!c->fixedOk && !c->trxOk)) {
        c->fixedOk = true;
        c->trxOk = false;
    }
    if (skiq_read_rf_port_config(num, &c->config) != 0) c->config = skiq_rf_port_config_fixed;
    gCard = c;
    return c;
}

class SkiqDevice : public SdrDevice {
public:
    ~SkiqDevice() override {
        stop();
        if (shared_) {
            std::lock_guard<std::mutex> lk(shared_->m);
            if (counted_) {
                shared_->rx = false;
                shared_->rxTrx = shared_->rxDual = false;
            }
        }
        shared_.reset();  // the last user exits the library
    }

    bool open(const StreamConfig& cfg, std::string& err) {
        shared_ = acquireCard(cfg.args, err);
        if (!shared_) return false;
        std::lock_guard<std::mutex> lk(shared_->m);
        if (shared_->rx) {
            err = shared_->desc + " is open already (wvt receives from one Sidekiq handle at a time)";
            return false;
        }
        card_ = shared_->num;
        param_ = shared_->param;
        part_ = shared_->part;
        usb_ = shared_->usb;
        desc_ = shared_->desc;
        fixedOk_ = shared_->fixedOk;
        trxOk_ = shared_->trxOk;
        listPorts();
        if (!configure(cfg, err)) return false;
        shared_->rx = counted_ = true;
        shared_->rxRate = rate_;
        shared_->rxTrx = trx_;
        shared_->rxDual = dual_;
        return true;
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
    // configuration, which is one for the whole card.
    void listPorts() {
        uint8_t n = 1;
        if (skiq_read_num_rx_chans(card_, &n) != 0 || n == 0) n = 1;
        numHandles_ = std::min<int>(n, skiq_rx_hdl_end);
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
    // card's port), and takes the limits of that handle. Runs with the card's
    // lock held: a transmitter on the card keeps its port configuration and
    // channel mode.
    bool selectPort(const std::string& name, std::string& err) {
        hdl_ = skiq_rx_hdl_A1;
        trx_ = dual_ = false;
        const bool txOn = shared_->tx;
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
        trx_ = sel ? sel->name.trx : shared_->config == skiq_rf_port_config_trx;
        dual_ = hdl_ == skiq_rx_hdl_A2 || hdl_ == skiq_rx_hdl_B2;
        // The transmitter holds the RF port configuration and the channel mode.
        if (txOn) {
            const std::string what = sel ? skiqPortLabel(sel->name) : std::string("the default port");
            const auto need = trx_ ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed;
            if (trx_) {
                err = what + " cannot receive while the Sidekiq transmits (a TRX port does one at a time): choose a "
                             "fixed receive port, or stop the transmitter";
                return false;
            }
            if (need != shared_->config) {
                err = "the Sidekiq transmits in the " + std::string(configName(shared_->config)) +
                      " RF port configuration: " + what + " needs the " + configName(need) + " one";
                return false;
            }
            if (dual_ && !shared_->txDual) {
                err = std::string("receive handle ") + hdlName(hdl_) +
                      " needs the dual channel mode, but the Sidekiq transmits in the single mode: receive on A1 or B1, "
                      "or stop the transmitter";
                return false;
            }
        }
        // A2 and B2 receive in the dual channel mode, as in the SDK's rx_samples.
        if (numHandles_ > 1 && !(txOn && shared_->txDual)) {
            const int32_t r = skiq_write_chan_mode(card_, dual_ ? skiq_chan_mode_dual : skiq_chan_mode_single);
            if (r != 0 && dual_) {
                err = skiqError(std::string("cannot select the dual channel mode for handle ") + hdlName(hdl_), r);
                return false;
            }
        }
        if (sel) {
            const std::string what = skiqPortLabel(sel->name);
            // A card with both configurations switches to the port's one.
            if (fixedOk_ && trxOk_ && !txOn) {
                const auto config = sel->name.trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed;
                if (const int32_t r = skiq_write_rf_port_config(card_, config); r != 0) {
                    err = skiqError("cannot select the RF port configuration for " + what, r);
                    return false;
                }
                shared_->config = config;
            }
            // A TRX port also transmits: set it to receive.
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
        // before the other settings. It would stop a transmitter on the card.
        if (cfg.extRef && shared_->tx) {
            addNote(openNotes_, "reference clock: as the transmitter set it");
        } else if (cfg.extRef) {
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
        // The receiver and the transmitter share one sample clock (the API's
        // skiq_write_tx_sample_rate_and_bandwidth): a new rate would move the transmitter's.
        if (shared_->tx && std::fabs(cfg.rate - shared_->txRate) > 1) {
            err = "the Sidekiq is transmitting at " + fmtSI(shared_->txRate, "S/s", 9) +
                  ": receive must use the same rate (one sample clock)";
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
                const double settled = std::max(tuneAt_.load(), shared_->slipAt.load());
                const int64_t lost = haveTs && now - settled > kSettleSec ? skiqLostSamples(nextTs, ts) : 0;
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

    std::shared_ptr<Card> shared_;
    bool counted_ = false;           // shared_->rx is this device
    bool trx_ = false, dual_ = false;  // on a TRX port; in the dual channel mode
    uint8_t card_ = 0;
    bool usb_ = false, swap_ = false, blocking_ = false, agcOk_ = false;
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

// The transmitter (issue #86): one TX handle, chosen by its port, in the
// immediate data flow mode. skiq_transmit blocks until the FPGA takes the
// block (the default synchronous transfer mode), so the thread runs at the
// sample rate. Until start and after stop, the card has the most attenuation.
class SkiqTx : public SdrTx {
public:
    ~SkiqTx() override {
        stop();
        if (blk_) skiq_tx_block_free(blk_);
        if (shared_) {
            std::lock_guard<std::mutex> lk(shared_->m);
            if (trx_) skiq_write_rf_port_operation(card_, false);  // back to receive
            if (counted_) {
                shared_->tx = false;
                shared_->txDual = false;
            }
        }
        shared_.reset();  // the last user exits the library
    }

    bool open(const TxConfig& cfg, std::string& err) {
        shared_ = acquireCard(cfg.args, err);
        if (!shared_) return false;
        std::lock_guard<std::mutex> lk(shared_->m);
        if (shared_->tx) {
            err = shared_->desc + " transmits already";
            return false;
        }
        card_ = shared_->num;
        part_ = shared_->part;
        desc_ = shared_->desc;
        const bool rxOn = shared_->rx;
        if (rxOn && std::fabs(cfg.rate - shared_->rxRate) > 1) {
            err = "the Sidekiq is receiving at " + fmtSI(shared_->rxRate, "S/s", 9) +
                  ": transmit must use the same rate (one sample clock)";
            return false;
        }
        if (!cfg.ref.empty() && cfg.ref != "internal" && cfg.ref != "external") {
            err = "Sidekiq reference must be internal or external (got '" + cfg.ref + "')";
            return false;
        }
        if (cfg.ref == "external" && rxOn) {
            addNote(notes_, "reference clock: as the live view set it");
        } else if (cfg.ref == "external") {
            if (const int32_t r = skiq_write_ref_clock_select(card_, skiq_ref_clock_external); r != 0) {
                err = skiqError("cannot select the external reference clock", r) +
                      (part_ == skiq_m2 || part_ == skiq_mpcie
                           ? "; the M.2 and mPCIe select it only in their stored settings (the SDK's ref_clock app)"
                           : "");
                return false;
            }
            LOG_INFO("skiq", "transmit: external reference clock");
        }
        // The card has one I/Q order for both directions.
        swap_ = skiq_write_iq_order_mode(card_, skiq_iq_order_iq) != 0;
        if (swap_) LOG_DEBUG("skiq", "transmit: the FPGA keeps Q/I order: wvt swaps the samples");

        listPorts();
        if (!selectPort(cfg.antenna, rxOn, err)) return false;
        if (!configure(cfg, rxOn, err)) return false;

        shared_->tx = counted_ = true;
        shared_->txRate = rate_;
        shared_->txDual = dual_;
        LOG_INFO("skiq", "transmitter %s, handle %s, port %s: %d-bit I/Q, %s, attenuation %.2f-%.2f dB, %s to %s",
                 desc_.c_str(), txHdlName(hdl_), port_.c_str(), res_, fmtSI(rate_, "S/s", 7).c_str(), attMin_ / 4.0,
                 attMax_ / 4.0, fmtSI(loMin_, "Hz", 4).c_str(), fmtSI(loMax_, "Hz", 4).c_str());
        return true;
    }

    bool start(FillFn fill, std::string& err) override {
        stop();
        fill_ = std::move(fill);
        {
            std::lock_guard<std::mutex> lk(errM_);
            fatal_.clear();
        }
        if (const int32_t r = skiq_write_tx_attenuation(card_, hdl_, (uint16_t)att_.load()); r != 0) {
            err = skiqError("cannot set the transmit attenuation", r);
            return false;
        }
        shared_->slipAt = nowSec();
        if (const int32_t r = skiq_start_tx_streaming(card_, hdl_); r != 0) {
            skiq_write_tx_attenuation(card_, hdl_, (uint16_t)attMax_);
            err = skiqError("cannot start transmitting", r);
            return false;
        }
        LOG_DEBUG("skiq", "transmit started: %d samples per block, attenuation %.2f dB", block_, att_ / 4.0);
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        if (!th_.joinable()) return;
        quit_ = true;
        th_.join();
        skiq_stop_tx_streaming(card_, hdl_);
        // Quiet: the LO can leak while the card is not streaming.
        skiq_write_tx_attenuation(card_, hdl_, (uint16_t)attMax_);
        shared_->slipAt = nowSec();
        LOG_DEBUG("skiq", "transmit stopped: %lld samples, %lld underruns", (long long)sent_.load(),
                  (long long)underruns_.load());
    }

    // The SDK refuses a frequency it can't tune (it does not clip). If the LO
    // moved anyway, tune back to the last good frequency; if that fails too,
    // stop with a fatal error (as for the USRP, issue #40).
    bool setCenterFreq(double hz, std::string& err) override {
        if (loMax_ > 0 && (hz < loMin_ - 1 || hz > loMax_ + 1)) {
            err = "Sidekiq " + partName(part_) + " cannot transmit at " + fmtSI(hz, "Hz", 9) + " (range " +
                  fmtSI(loMin_, "Hz", 4) + " - " + fmtSI(loMax_, "Hz", 4) + ")";
            return false;
        }
        if (!(hz > 0)) {
            err = "center frequency must be positive";
            return false;
        }
        const double prev = fc_;
        std::string tuneErr;  // `err` can hold an old message
        double f = tune(hz, tuneErr);
        if (tuneErr.empty() && std::fabs(f - hz) <= 1) {
            fc_ = f;
            return true;
        }
        err = !tuneErr.empty() ? tuneErr
                               : "Sidekiq cannot transmit at " + fmtSI(hz, "Hz", 9) + "; it tuned to " + fmtSI(f, "Hz", 9);
        if (!th_.joinable() || std::fabs(f - prev) <= 1) {
            fc_ = f;
            if (th_.joinable()) err += "; still transmitting at " + fmtSI(prev, "Hz", 9);
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
        fail("retune failed and the transmitter could not return to " + fmtSI(prev, "Hz", 9) + " (now at " +
             fmtSI(f, "Hz", 9) + "): " + err);
        quit_ = true;  // stop sending now; the owner's stop() joins the thread
        return false;
    }

    bool setGain(double db, std::string& err) override {
        const int a = skiqTxAttenuation(db, attMin_, attMax_);
        // Before start the card stays at the most attenuation; start writes this one.
        if (th_.joinable())
            if (const int32_t r = skiq_write_tx_attenuation(card_, hdl_, (uint16_t)a); r != 0) {
                err = skiqError("cannot set the transmit attenuation", r);
                return false;
            }
        att_ = a;
        LOG_DEBUG("skiq", "transmit gain %.2f dB (attenuation %.2f dB)", skiqTxGainDb(a, attMax_), a / 4.0);
        return true;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    double gain() const override { return skiqTxGainDb(att_, attMax_); }
    GainRange gainRange() const override { return {0, (attMax_ - attMin_) / 4.0, 0.25, {}, false}; }
    std::string description() const override { return desc_; }
    std::string antenna() const override { return port_; }
    std::string notes() const override {
        std::string n = notes_;
        if (underruns_ > 5) addNote(n, "underruns: the host could not keep up (gaps in the transmitted signal)");
        return n;
    }
    std::string fatalError() const override {
        std::lock_guard<std::mutex> lk(errM_);
        return fatal_;
    }
    Stats stats() const override {
        Stats s;
        s.sent = sent_;
        s.underflows = underruns_;
        return s;
    }

private:
    struct Port {
        skiq_tx_hdl_t hdl;
        skiq_rf_port_t rf;
        SkiqPort name;
    };

    // The transmit ports of every TX handle, fixed and TRX, as for receive.
    void listPorts() {
        uint8_t n = 1;
        if (skiq_read_num_tx_chans(card_, &n) != 0 || n == 0) n = 1;
        numHandles_ = std::min<int>(n, skiq_tx_hdl_end);
        for (int h = 0; h < numHandles_; ++h) {
            const auto hdl = (skiq_tx_hdl_t)h;
            uint8_t nFixed = 0, nTrx = 0;
            skiq_rf_port_t fixed[skiq_rf_port_max] = {}, trx[skiq_rf_port_max] = {};
            if (skiq_read_tx_rf_ports_avail_for_hdl(card_, hdl, &nFixed, fixed, &nTrx, trx) != 0) {
                const skiq_tx_param_t& tp = shared_->param.tx_param[h];
                nFixed = std::min<uint8_t>(tp.num_fixed_rf_ports, skiq_rf_port_max);
                nTrx = std::min<uint8_t>(tp.num_trx_rf_ports, skiq_rf_port_max);
                std::copy(tp.fixed_rf_ports, tp.fixed_rf_ports + nFixed, fixed);
                std::copy(tp.trx_rf_ports, tp.trx_rf_ports + nTrx, trx);
            }
            auto add = [&](const skiq_rf_port_t* list, uint8_t count, bool isTrx) {
                for (int i = 0; i < count && i < skiq_rf_port_max; ++i) {
                    const char* s = list[i] == skiq_rf_port_unknown ? nullptr : skiq_rf_port_string(list[i]);
                    if (s && s[0]) ports_.push_back({hdl, list[i], {s, txHdlName(hdl), isTrx, !isTrx}});
                }
            };
            if (shared_->fixedOk) add(fixed, nFixed, false);
            if (shared_->trxOk) add(trx, nTrx, true);
        }
        std::string list;
        for (const auto& p : ports_) list += (list.empty() ? "" : ", ") + skiqPortLabel(p.name);
        LOG_DEBUG("skiq", "%d transmit handle(s), ports: %s", numHandles_, list.empty() ? "none listed" : list.c_str());
    }

    std::string portList() const {
        std::string list;
        for (const auto& p : ports_) list += (list.empty() ? "" : ", ") + skiqPortLabel(p.name);
        return list;
    }

    // Selects the TX handle and port named `name` ("" = handle A1 on the
    // card's port). With a receiver on the card (rxOn), the receiver keeps its
    // RF port configuration and channel mode, and a TRX port can't transmit.
    bool selectPort(const std::string& name, bool rxOn, std::string& err) {
        hdl_ = skiq_tx_hdl_A1;
        const Port* sel = nullptr;
        if (!name.empty()) {
            std::vector<SkiqPort> names;
            names.reserve(ports_.size());
            for (const auto& p : ports_) names.push_back(p.name);
            const int i = skiqFindPort(names, name);
            if (i < 0) {
                const std::string list = portList();
                err = "Sidekiq " + partName(part_) + " has no transmit port '" + name + "'" +
                      (list.empty() ? std::string() : " (ports: " + list + ")");
                return false;
            }
            sel = &ports_[i];
            hdl_ = sel->hdl;
        }
        const auto need =
            sel ? (sel->name.trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed) : shared_->config;
        trx_ = need == skiq_rf_port_config_trx;
        dual_ = hdl_ == skiq_tx_hdl_A2 || hdl_ == skiq_tx_hdl_B2;
        const std::string what = sel ? skiqPortLabel(sel->name) : std::string("the default transmit port");
        if (rxOn) {
            if (shared_->rxTrx) {
                err = "the live view receives on a TRX port, so the Sidekiq cannot transmit: receive on a fixed port";
                return false;
            }
            if (trx_) {
                err = what + " cannot transmit while the live view receives (a TRX port does one at a time): "
                             "choose a fixed transmit port" +
                      (portList().empty() ? std::string() : " (ports: " + portList() + ")");
                return false;
            }
            if (need != shared_->config) {
                err = "the live view receives in the " + std::string(configName(shared_->config)) +
                      " RF port configuration: " + what + " needs the " + configName(need) + " one";
                return false;
            }
            if (dual_ && !shared_->rxDual) {
                err = std::string("transmit handle ") + txHdlName(hdl_) +
                      " needs the dual channel mode, but the live view receives in the single mode: transmit on A1 "
                      "or B1, or stop the live view";
                return false;
            }
        } else {
            if (sel && shared_->fixedOk && shared_->trxOk && need != shared_->config) {
                if (const int32_t r = skiq_write_rf_port_config(card_, need); r != 0) {
                    err = skiqError("cannot select the RF port configuration for " + what, r);
                    return false;
                }
                shared_->config = need;
            }
            if (numHandles_ > 1 || dual_) {
                const int32_t r = skiq_write_chan_mode(card_, dual_ ? skiq_chan_mode_dual : skiq_chan_mode_single);
                if (r != 0 && dual_) {
                    err = skiqError(std::string("cannot select the dual channel mode for TX handle ") + txHdlName(hdl_), r);
                    return false;
                }
            }
        }
        if (sel)
            if (const int32_t r = skiq_write_tx_rf_port_for_hdl(card_, hdl_, sel->rf); r != 0) {
                err = skiqError("cannot select transmit port " + what, r);
                return false;
            }
        if (trx_)
            if (const int32_t r = skiq_write_rf_port_operation(card_, true); r != 0) {
                err = skiqError("cannot set " + what + " to transmit", r);
                return false;
            }
        skiq_rf_port_t rf = skiq_rf_port_unknown;
        port_ = sel ? skiqPortLabel(sel->name) : "default";
        if (!sel && skiq_read_tx_rf_port_for_hdl(card_, hdl_, &rf) == 0 && rf != skiq_rf_port_unknown)
            if (const char* s = skiq_rf_port_string(rf)) port_ = skiqPortLabel({s, txHdlName(hdl_), trx_, !trx_});
        return true;
    }

    bool configure(const TxConfig& cfg, bool rxOn, std::string& err) {
        // A handle without parameters takes those of A1 (the same RFIC family).
        const skiq_param_t& par = shared_->param;
        const skiq_tx_param_t& tp = par.tx_param[hdl_].sample_rate_max > 0 ? par.tx_param[hdl_] : par.tx_param[skiq_tx_hdl_A1];
        uint8_t res = tp.iq_resolution;
        if (res == 0 || res > 16) skiq_read_tx_iq_resolution(card_, &res);
        res_ = res > 0 && res <= 16 ? res : 12;
        loMin_ = (double)tp.lo_freq_min;
        loMax_ = (double)tp.lo_freq_max;
        attMin_ = tp.atten_quarter_db_min;
        attMax_ = tp.atten_quarter_db_max;
        if (attMax_ <= attMin_) {
            // Not in the parameters: the AD9361's 0-89.75 dB, and say so.
            attMin_ = 0;
            attMax_ = 359;
            LOG_WARN("skiq", "the card gives no transmit attenuation range: wvt assumes 0-89.75 dB");
        }
        // The card is quiet until start.
        att_ = skiqTxAttenuation(cfg.gain, attMin_, attMax_);
        skiq_write_tx_attenuation(card_, hdl_, (uint16_t)attMax_);

        if (tp.sample_rate_max > 0 && (cfg.rate < tp.sample_rate_min * 0.999 || cfg.rate > tp.sample_rate_max * 1.001)) {
            err = "Sidekiq " + partName(part_) + " transmit rate must be " + fmtSI(tp.sample_rate_min, "S/s", 4) + " to " +
                  fmtSI(tp.sample_rate_max, "S/s", 4) + " (got " + fmtSI(cfg.rate, "S/s", 4) + ")";
            return false;
        }
        const uint32_t rate = (uint32_t)std::lround(cfg.rate);
        const uint32_t bw = (uint32_t)std::lround(cfg.bandwidth > 0 ? cfg.bandwidth : 0.8 * cfg.rate);
        uint32_t gotRate = 0, gotBw = 0, actualBw = 0;
        double actualRate = 0;
        const bool have = skiq_read_tx_sample_rate_and_bandwidth(card_, hdl_, &gotRate, &actualRate, &gotBw, &actualBw) == 0;
        // With a receiver on the card, the clock has this rate already: write
        // only a new bandwidth, which does not stop the clock.
        if (!rxOn || !have || gotRate != rate || (cfg.bandwidth > 0 && gotBw != bw)) {
            shared_->slipAt = nowSec();
            if (const int32_t r = skiq_write_tx_sample_rate_and_bandwidth(card_, hdl_, rate, bw); r != 0) {
                err = skiqError("cannot set transmit rate " + fmtSI(cfg.rate, "S/s", 6) + ", bandwidth " + fmtSI(bw, "Hz", 4), r);
                return false;
            }
        }
        rate_ = cfg.rate;
        if (skiq_read_tx_sample_rate_and_bandwidth(card_, hdl_, &gotRate, &actualRate, &gotBw, &actualBw) == 0 &&
            actualRate > 0)
            rate_ = actualRate;
        if (std::fabs(rate_ - cfg.rate) > 1) addNote(notes_, "rate set to " + fmtSI(rate_, "S/s", 7));

        if (!setCenterFreq(cfg.fc, err)) return false;
        std::string e2;
        setGain(cfg.gain, e2);

        block_ = skiqTxBlockSamples(rate_);
        if (const int32_t r = skiq_write_tx_data_flow_mode(card_, hdl_, skiq_tx_immediate_data_flow_mode); r != 0) {
            err = skiqError("cannot select the immediate transmit mode", r);
            return false;
        }
        if (const int32_t r = skiq_write_tx_block_size(card_, hdl_, (uint16_t)block_); r != 0) {
            err = skiqError("cannot set the transmit block size to " + std::to_string(block_), r);
            return false;
        }
        blk_ = skiq_tx_block_allocate((uint32_t)block_);
        if (!blk_) {
            err = "out of memory for the transmit block";
            return false;
        }
        return true;
    }

    // Tunes and returns the LO that the card reports. Sets err if the SDK fails.
    double tune(double hz, std::string& err) {
        if (th_.joinable()) shared_->slipAt = nowSec();
        const int32_t r = skiq_write_tx_LO_freq(card_, hdl_, (uint64_t)std::llround(hz));
        if (r != 0) err = skiqError("cannot tune the transmitter to " + fmtSI(hz, "Hz", 9), r);
        uint64_t f = 0;
        double actual = 0;
        if (skiq_read_tx_LO_freq(card_, hdl_, &f, &actual) != 0 || !(actual > 0)) actual = r == 0 ? hz : fc_.load();
        return actual;
    }

    void run() {
        std::vector<std::complex<float>> buf((size_t)block_);
        // The samples follow the 4-word header (the SDK's skiq_tx_block_t::data).
        auto* data = reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(blk_) + SKIQ_TX_HEADER_SIZE_IN_BYTES);
        int errors = 0;
        double lastPoll = 0;
        while (!quit_) {
            fill_(buf.data(), block_);
            skiqTxConvert(buf.data(), data, (size_t)block_, res_, swap_);
            const int32_t r = skiq_transmit(card_, hdl_, blk_, nullptr);
            if (r != 0) {
                if (quit_) break;
                if (++errors >= 10) {
                    fail("Sidekiq stopped accepting samples (transmit error " + std::to_string(r) + "; card removed?)");
                    LOG_ERROR("skiq", "%s", fatalError().c_str());
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            errors = 0;
            sent_ += block_;
            if (const double now = nowSec(); now - lastPoll > 0.5) {
                lastPoll = now;
                pollUnderruns();
            }
        }
        pollUnderruns();
    }

    void pollUnderruns() {
        uint32_t n = 0;
        if (skiq_read_tx_num_underruns(card_, hdl_, &n) == 0) underruns_ = n;
    }

    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lk(errM_);
        fatal_ = why;
    }

    std::shared_ptr<Card> shared_;
    bool counted_ = false;  // shared_->tx is this transmitter
    uint8_t card_ = 0;
    skiq_part_t part_ = skiq_part_invalid;
    skiq_tx_hdl_t hdl_ = skiq_tx_hdl_A1;
    std::vector<Port> ports_;
    int numHandles_ = 1;
    bool trx_ = false, dual_ = false, swap_ = false;
    int res_ = 12, attMin_ = 0, attMax_ = 359, block_ = 0;
    std::atomic<int> att_{359};
    double loMin_ = 0, loMax_ = 0;
    std::atomic<double> fc_{0}, rate_{0};
    std::string desc_, port_, notes_;
    skiq_tx_block_t* blk_ = nullptr;
    FillFn fill_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::atomic<int64_t> sent_{0}, underruns_{0};
    mutable std::mutex errM_;
    std::string fatal_;
};

}  // namespace

void enumerateSkiq(std::vector<DeviceInfo>& out) {
    registerLogging();
    std::lock_guard<std::recursive_mutex> lk(gLibM);
    // An open card can't be probed (skiq_init is once per process); list it as it was opened.
    if (auto c = gCard.lock()) {
        out.push_back(c->info);
        return;
    }
    uint8_t n = 0, cards[SKIQ_MAX_NUM_CARDS] = {};
    if (skiq_get_cards(skiq_xport_type_auto, &n, cards) != 0 || n == 0) return;
    n = std::min<uint8_t>(n, SKIQ_MAX_NUM_CARDS);
    std::vector<DeviceInfo> found(n);
    std::vector<uint8_t> avail;
    for (int i = 0; i < n; ++i) {
        const std::string num = "#" + std::to_string(cards[i]);
        found[i] = {"skiq", num, "Sidekiq card " + std::to_string(cards[i]), "", ""};
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
            if (p.card_param.serial_string[0]) found[i].args = found[i].serial = p.card_param.serial_string;
            found[i].label = label(p.card_param);
            found[i].product = partName(p.card_param.part_type);
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

std::unique_ptr<SdrTx> openSkiqTx(const TxConfig& cfg, std::string& err) {
    auto t = std::make_unique<SkiqTx>();
    if (!t->open(cfg, err)) return nullptr;
    return t;
}
