// A simulated Sidekiq card behind the libsidekiq calls that wvt uses (issue
// #4), so the Sidekiq backend can be built and tested without the SDK or a
// card. It checks the calls the way the API documents them (skiq_init once,
// the rate and LO ranges, card availability), streams in real time
// (skiq_receive waits for the block, up to the transfer timeout), and makes a
// test signal:
//   - a CW tone at 100.2 MHz, at -60 dBFS plus the gain in dB above the lowest
//     index (so -30 dBFS at 30 dB); it moves with the tuning;
//   - noise at about -80 dBFS;
//   - the RF overload flag when the tone is above -1 dBFS.
// Samples have the card's resolution, sign-extended in the low bits, in Q/I
// order until skiq_write_iq_order_mode selects I/Q. The RF timestamp counts
// samples, and on the M.2 (AD9361) it slips on a retune, as on the real card.
// RF ports (issue #85): the M.2 has handle A1 with the fixed ports J1 and J2
// (default J2) and the TRX port J1; the X4 has the handles A1, A2, B1 and B2
// with the fixed ports J1, J2, J3 and J4, and lists them only per handle
// (skiq_read_rx_rf_ports_avail_for_hdl), not in skiq_read_parameters. A port
// works only in its RF port configuration, a TRX port receives only after
// skiq_write_rf_port_operation(false), and A2 and B2 stream only in the dual
// channel mode. The tone is 10 kHz higher for each port number above J2 (lower
// below it), so a test can see which port receives.
// The signal is computed once per setting into a table of 64 blocks (the tone
// rounded to whole cycles in it) and copied from there, so the simulator keeps
// up with 245.76 MS/s. Like the FPGA, it holds up to 64 MiB of samples for a
// slow host; beyond that skiq_receive reports an overrun and skips to now.
// Environment, for tests:
//   WVT_SKIQ_SIM_NONE=1       no card attached
//   WVT_SKIQ_SIM_PART=x4      a Sidekiq X4 (16 bits, 0.5 dB steps, no AGC); default an M.2 (12 bits)
//   WVT_SKIQ_SIM_USB=1        the USB transport: no blocking receive, no overload flag in the blocks
//   WVT_SKIQ_SIM_BUSY=PID     another process holds the card
//   WVT_SKIQ_SIM_UNPLUG=S     the card fails S seconds after streaming starts
//   WVT_SKIQ_SIM_OVERRUN=1    0.2 s after streaming starts, one overrun loses 5 blocks
#include "sidekiq_api.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint8_t kCard = 0;
constexpr const char* kSerial = "WVT01";
constexpr double kToneHz = 100.2e6;
constexpr uint32_t kBlock = 1018;  // samples per block (4 KiB with the header)
constexpr uint64_t kSlip = 5000;   // AD9361 timestamp slip on a retune
constexpr int kOverrunBlocks = 5;
constexpr uint32_t kTableBlocks = 64;
constexpr double kDmaSamples = 64.0 * 1024 * 1024 / 4;  // the samples a slow host can fall behind

using Clock = std::chrono::steady_clock;

bool envOn(const char* name) {
    const char* e = getenv(name);
    return e && *e && *e != '0';
}
double envNum(const char* name) {
    const char* e = getenv(name);
    return e && *e ? atof(e) : 0;
}
bool present() { return !envOn("WVT_SKIQ_SIM_NONE"); }
bool usb() { return envOn("WVT_SKIQ_SIM_USB"); }

struct Handle {
    std::vector<skiq_rf_port_t> fixed, trx;
};

struct Part {
    skiq_part_t type;
    int res;
    uint8_t gmin, gmax;
    double dbPerStep;
    bool agc, slips;
    uint32_t rateMin, rateMax;
    uint64_t loMin, loMax;
    std::vector<Handle> handles;  // from A1 on
    bool portsInParams;           // skiq_read_parameters lists the ports
};

Part part() {
    const char* e = getenv("WVT_SKIQ_SIM_PART");
    if (e && std::string(e) == "x4")
        return {skiq_x4, 16, 195, 255, 0.5, false, false, 1000000, 245760000, 1000000, 6000000000ULL,
                {{{skiq_rf_port_J1}, {}}, {{skiq_rf_port_J2}, {}}, {{skiq_rf_port_J3}, {}}, {{skiq_rf_port_J4}, {}}},
                false};
    return {skiq_m2, 12, 0, 76, 1, true, true, 233000, 61440000, 70000000, 6000000000ULL,
            {{{skiq_rf_port_J1, skiq_rf_port_J2}, {skiq_rf_port_J1}}}, true};
}

bool hasTrx(const Part& p) {
    for (const auto& h : p.handles)
        if (!h.trx.empty()) return true;
    return false;
}

struct Sim {
    std::mutex m;
    void (*log)(int32_t, const char*) = nullptr;
    bool init = false, full = false;
    std::atomic<bool> streaming{false};
    int32_t timeout = RX_TRANSFER_NO_WAIT;
    skiq_iq_order_t order = skiq_iq_order_qi;
    skiq_ref_clock_select_t ref = skiq_ref_clock_internal;
    skiq_rf_port_t port[skiq_rx_hdl_end] = {};
    skiq_rf_port_config_t config = skiq_rf_port_config_fixed;
    bool transmit = true;  // the TRX port operation
    skiq_chan_mode_t chanMode = skiq_chan_mode_single;
    skiq_rx_hdl_t hdl = skiq_rx_hdl_A1;  // streaming
    uint32_t rate = 10000000, bw = 8000000;
    uint64_t lo = 1000000000ULL;
    skiq_rx_gain_t mode = skiq_rx_gain_manual;
    uint8_t gain = 0;
    bool overloaded = false;
    // stream state
    Clock::time_point t0;
    uint64_t blocks = 0, tsBase = 0;
    bool overrunDone = false;
    std::mt19937 rng{1};
    // The signal for the settings in `key`, kTableBlocks blocks long.
    std::vector<int16_t> table;
    std::vector<double> key;
    std::vector<uint64_t> buf = std::vector<uint64_t>((SKIQ_RX_HEADER_SIZE_IN_BYTES + kBlock * 4) / 8 + 1);
} g;

void say(int32_t prio, const char* msg) {
    if (g.log) g.log(prio, msg);
}

bool active(uint8_t card) { return g.init && card == kCard && present(); }

bool validHdl(skiq_rx_hdl_t hdl) { return hdl >= 0 && (size_t)hdl < part().handles.size(); }

// The ports of a handle in the current RF port configuration.
const std::vector<skiq_rf_port_t>& usablePorts(const Part& p, skiq_rx_hdl_t hdl) {
    return g.config == skiq_rf_port_config_trx ? p.handles[hdl].trx : p.handles[hdl].fixed;
}

// Each handle on J2 if it has it in the configuration, else on its first port.
void defaultPorts(const Part& p) {
    for (size_t h = 0; h < p.handles.size(); ++h) {
        const auto& l = usablePorts(p, (skiq_rx_hdl_t)h);
        const bool j2 = std::find(l.begin(), l.end(), skiq_rf_port_J2) != l.end();
        g.port[h] = j2 ? skiq_rf_port_J2 : l.empty() ? skiq_rf_port_unknown : l.front();
    }
}

double gainDb() {
    const Part p = part();
    if (g.mode == skiq_rx_gain_auto) return 40;  // the AGC puts the tone at -20 dBFS
    return (g.gain - p.gmin) * p.dbPerStep;
}
}  // namespace

extern "C" {

void skiq_register_logging(void (*log_msg)(int32_t priority, const char* message)) {
    std::lock_guard<std::mutex> lk(g.m);
    g.log = log_msg;
}

int32_t skiq_read_libsidekiq_version(uint8_t* p_major, uint8_t* p_minor, uint8_t* p_patch, const char** p_label) {
    if (!p_major || !p_minor || !p_patch) return -EFAULT;
    *p_major = 4;
    *p_minor = 26;
    *p_patch = 0;
    if (p_label) *p_label = "-sim";
    return 0;
}

int32_t skiq_get_cards(skiq_xport_type_t, uint8_t* p_num_cards, uint8_t* p_cards) {
    if (!p_num_cards || !p_cards) return -EFAULT;
    *p_num_cards = 0;
    if (present()) {
        p_cards[0] = kCard;
        *p_num_cards = 1;
    }
    return 0;
}

int32_t skiq_get_card_from_serial_string(char* p_serial_num, uint8_t* p_card) {
    if (!p_serial_num || !p_card) return -EFAULT;
    if (!present() || strcmp(p_serial_num, kSerial) != 0) return -ENODEV;
    *p_card = kCard;
    return 0;
}

int32_t skiq_is_card_avail(uint8_t card, pid_t* p_card_owner) {
    if (card >= SKIQ_MAX_NUM_CARDS) return -ERANGE;
    if (card != kCard || !present()) return -ENODEV;
    std::lock_guard<std::mutex> lk(g.m);
    const pid_t busy = (pid_t)envNum("WVT_SKIQ_SIM_BUSY");
    if (busy > 0 || g.init) {
        if (p_card_owner) *p_card_owner = busy > 0 ? busy : getpid();
        return EBUSY;
    }
    return 0;
}

int32_t skiq_init(skiq_xport_type_t, skiq_xport_init_level_t level, uint8_t* p_card_nums, uint8_t num_cards) {
    std::lock_guard<std::mutex> lk(g.m);
    if (g.init) return -EEXIST;
    if (!p_card_nums || num_cards != 1 || p_card_nums[0] != kCard || !present()) return -EINVAL;
    if (envNum("WVT_SKIQ_SIM_BUSY") > 0) return -EBUSY;
    const Part p = part();
    g.init = true;
    g.full = level == skiq_xport_init_level_full;
    g.streaming = false;
    g.timeout = RX_TRANSFER_NO_WAIT;
    g.order = skiq_iq_order_qi;
    g.ref = skiq_ref_clock_internal;
    g.config = skiq_rf_port_config_fixed;
    g.transmit = true;
    g.chanMode = skiq_chan_mode_single;
    defaultPorts(p);
    g.mode = skiq_rx_gain_manual;
    g.gain = p.gmin;
    say(SKIQ_LOG_INFO, "Sidekiq simulator: card 0 initialized\n");
    return 0;
}

int32_t skiq_exit(void) {
    std::lock_guard<std::mutex> lk(g.m);
    g.init = g.full = false;
    g.streaming = false;
    return 0;
}

int32_t skiq_read_parameters(uint8_t card, skiq_param_t* p_param) {
    if (!p_param) return -EFAULT;
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    const Part p = part();
    *p_param = skiq_param_t{};
    p_param->card_param.init_level = g.full ? skiq_xport_init_level_full : skiq_xport_init_level_basic;
    p_param->card_param.part_type = p.type;
    p_param->card_param.xport = usb() ? skiq_xport_type_usb : skiq_xport_type_pcie;
    p_param->card_param.card = kCard;
    snprintf(p_param->card_param.serial_string, SKIQ_SERIAL_NUM_STRLEN, "%s", kSerial);
    if (!g.full) return 0;  // the rest needs a full initialization
    for (size_t h = 0; h < p.handles.size(); ++h) {
        skiq_rx_param_t& rx = p_param->rx_param[h];
        rx.handle = (skiq_rx_hdl_t)h;
        rx.gain_index_min = p.gmin;
        rx.gain_index_max = p.gmax;
        rx.iq_resolution = (uint8_t)p.res;
        rx.lo_freq_min = p.loMin;
        rx.lo_freq_max = p.loMax;
        rx.sample_rate_min = p.rateMin;
        rx.sample_rate_max = p.rateMax;
        if (!p.portsInParams) continue;
        rx.num_fixed_rf_ports = (uint8_t)p.handles[h].fixed.size();
        std::copy(p.handles[h].fixed.begin(), p.handles[h].fixed.end(), rx.fixed_rf_ports);
        rx.num_trx_rf_ports = (uint8_t)p.handles[h].trx.size();
        std::copy(p.handles[h].trx.begin(), p.handles[h].trx.end(), rx.trx_rf_ports);
    }
    return 0;
}

const char* skiq_part_string(skiq_part_t part) {
    switch (part) {
        case skiq_m2: return "M.2";
        case skiq_x4: return "X4";
        default: return "unknown";
    }
}

const char* skiq_rf_port_string(skiq_rf_port_t rf_port) {
    static const char* names[] = {"J1", "J2", "J3", "J4", "J5", "J6", "J7", "J300", "Jxxx_RX1", "Jxxx_TX1RX2", "J8"};
    return rf_port >= 0 && rf_port < skiq_rf_port_max ? names[rf_port] : "unknown";
}

int32_t skiq_write_ref_clock_select(uint8_t card, skiq_ref_clock_select_t ref_clock_source) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (ref_clock_source >= skiq_ref_clock_invalid) return -EINVAL;
    if (part().type == skiq_m2) return -ENOTSUP;  // as documented for the M.2 and mPCIe
    g.ref = ref_clock_source;
    return 0;
}

int32_t skiq_write_iq_order_mode(uint8_t card, skiq_iq_order_t mode) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    g.order = mode;
    return 0;
}

int32_t skiq_read_num_rx_chans(uint8_t card, uint8_t* p_num_rx_chans) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (!p_num_rx_chans) return -EFAULT;
    *p_num_rx_chans = (uint8_t)part().handles.size();
    return 0;
}

int32_t skiq_read_rf_port_config_avail(uint8_t card, bool* p_fixed, bool* p_trx) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (!p_fixed || !p_trx) return -EINVAL;
    *p_fixed = true;
    *p_trx = hasTrx(part());
    return 0;
}

int32_t skiq_read_rf_port_config(uint8_t card, skiq_rf_port_config_t* p_config) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (!p_config) return -EFAULT;
    *p_config = g.config;
    return 0;
}

int32_t skiq_write_rf_port_config(uint8_t card, skiq_rf_port_config_t config) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    const Part p = part();
    if (config != skiq_rf_port_config_fixed && !(config == skiq_rf_port_config_trx && hasTrx(p))) return -ENOTSUP;
    if (g.streaming) return -EBUSY;
    if (config != g.config) {
        g.config = config;
        defaultPorts(p);
    }
    return 0;
}

int32_t skiq_write_rf_port_operation(uint8_t card, bool transmit) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (g.config != skiq_rf_port_config_trx) return -ENOTSUP;
    g.transmit = transmit;
    return 0;
}

int32_t skiq_read_rx_rf_ports_avail_for_hdl(uint8_t card, skiq_rx_hdl_t hdl, uint8_t* p_num_fixed_rf_ports,
                                            skiq_rf_port_t* p_fixed_rf_port_list, uint8_t* p_num_trx_rf_ports,
                                            skiq_rf_port_t* p_trx_rf_port_list) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!p_num_fixed_rf_ports || !p_fixed_rf_port_list || !p_num_trx_rf_ports || !p_trx_rf_port_list) return -EFAULT;
    if (!validHdl(hdl)) return -EDOM;
    const Handle h = part().handles[hdl];
    *p_num_fixed_rf_ports = (uint8_t)h.fixed.size();
    std::copy(h.fixed.begin(), h.fixed.end(), p_fixed_rf_port_list);
    *p_num_trx_rf_ports = (uint8_t)h.trx.size();
    std::copy(h.trx.begin(), h.trx.end(), p_trx_rf_port_list);
    return 0;
}

int32_t skiq_write_chan_mode(uint8_t card, skiq_chan_mode_t mode) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (mode == skiq_chan_mode_dual && part().handles.size() < 2) return -ENOTSUP;
    g.chanMode = mode;
    return 0;
}

int32_t skiq_write_rx_rf_port_for_hdl(uint8_t card, skiq_rx_hdl_t hdl, skiq_rf_port_t rf_port) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!validHdl(hdl)) return -EDOM;
    const Part p = part();
    const auto& l = usablePorts(p, hdl);
    if (std::find(l.begin(), l.end(), rf_port) == l.end()) return -EINVAL;
    g.port[hdl] = rf_port;
    return 0;
}

int32_t skiq_read_rx_rf_port_for_hdl(uint8_t card, skiq_rx_hdl_t hdl, skiq_rf_port_t* p_rf_port) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!p_rf_port) return -EFAULT;
    if (!validHdl(hdl)) return -EDOM;
    *p_rf_port = g.port[hdl];
    return 0;
}

int32_t skiq_write_rx_sample_rate_and_bandwidth(uint8_t card, skiq_rx_hdl_t hdl, uint32_t rate, uint32_t bandwidth) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!validHdl(hdl)) return -EDOM;
    const Part p = part();
    if (rate < p.rateMin || rate > p.rateMax || bandwidth == 0) return -EINVAL;
    g.rate = rate;
    g.bw = std::min(bandwidth, rate);
    return 0;
}

int32_t skiq_read_rx_sample_rate_and_bandwidth(uint8_t card, skiq_rx_hdl_t hdl, uint32_t* p_rate, double* p_actual_rate,
                                               uint32_t* p_bandwidth, uint32_t* p_actual_bandwidth) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!p_rate || !p_actual_rate || !p_bandwidth || !p_actual_bandwidth) return -EFAULT;
    *p_rate = g.rate;
    *p_actual_rate = g.rate;
    *p_bandwidth = *p_actual_bandwidth = g.bw;
    return 0;
}

int32_t skiq_write_rx_LO_freq(uint8_t card, skiq_rx_hdl_t hdl, uint64_t freq) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!validHdl(hdl)) return -EDOM;
    const Part p = part();
    if (freq < p.loMin || freq > p.loMax) return -EINVAL;
    g.lo = freq;
    // The AD9361 stops its sample clock to retune: the RF timestamp slips.
    if (g.streaming && p.slips) g.tsBase += kSlip;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return 0;
}

int32_t skiq_read_rx_LO_freq(uint8_t card, skiq_rx_hdl_t hdl, uint64_t* p_freq, double* p_actual_freq) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!p_freq || !p_actual_freq) return -EFAULT;
    *p_freq = g.lo;
    *p_actual_freq = (double)g.lo;
    return 0;
}

int32_t skiq_write_rx_gain_mode(uint8_t card, skiq_rx_hdl_t hdl, skiq_rx_gain_t gain_mode) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (gain_mode == skiq_rx_gain_auto && !part().agc) return -ENOTSUP;
    g.mode = gain_mode;
    return 0;
}

int32_t skiq_read_rx_gain_index_range(uint8_t card, skiq_rx_hdl_t hdl, uint8_t* p_gain_index_min,
                                      uint8_t* p_gain_index_max) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!p_gain_index_min || !p_gain_index_max) return -EFAULT;
    *p_gain_index_min = part().gmin;
    *p_gain_index_max = part().gmax;
    return 0;
}

int32_t skiq_write_rx_gain(uint8_t card, skiq_rx_hdl_t hdl, uint8_t gain_index) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    const Part p = part();
    if (gain_index < p.gmin || gain_index > p.gmax) return -EINVAL;
    if (g.mode != skiq_rx_gain_manual) return -EINVAL;
    g.gain = gain_index;
    return 0;
}

int32_t skiq_read_rx_overload_state(uint8_t card, skiq_rx_hdl_t hdl, uint8_t* p_overload) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!p_overload) return -EFAULT;
    *p_overload = g.overloaded ? 1 : 0;
    return 0;
}

int32_t skiq_set_rx_transfer_timeout(const uint8_t card, const int32_t timeout_us) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (usb()) return -ENOTSUP;  // the USB transport has no blocking receive
    if (timeout_us != RX_TRANSFER_NO_WAIT && timeout_us != RX_TRANSFER_WAIT_FOREVER &&
        (timeout_us < 20 || timeout_us > 1000000))
        return -EINVAL;
    g.timeout = timeout_us;
    return 0;
}

int32_t skiq_get_rx_transfer_timeout(const uint8_t card, int32_t* p_timeout_us) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (!p_timeout_us) return -EFAULT;
    *p_timeout_us = usb() ? RX_TRANSFER_WAIT_NOT_SUPPORTED : g.timeout;
    return 0;
}

int32_t skiq_start_rx_streaming(uint8_t card, skiq_rx_hdl_t hdl) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card) || !g.full) return -ENODEV;
    if (!validHdl(hdl)) return -EDOM;
    if (g.streaming) return -EBUSY;
    if ((hdl == skiq_rx_hdl_A2 || hdl == skiq_rx_hdl_B2) && g.chanMode != skiq_chan_mode_dual) return -EINVAL;
    if (g.config == skiq_rf_port_config_trx && g.transmit) return -EINVAL;  // the TRX port is set to transmit
    g.hdl = hdl;
    g.t0 = Clock::now();
    g.blocks = 0;
    g.tsBase = 1000;  // the counter does not start at 0
    g.overrunDone = false;
    g.streaming = true;
    return 0;
}

int32_t skiq_stop_rx_streaming(uint8_t card, skiq_rx_hdl_t hdl) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!active(card)) return -ENODEV;
    if (!g.streaming) return -ENODEV;
    g.streaming = false;
    return 0;
}

skiq_rx_status_t skiq_receive(uint8_t card, skiq_rx_hdl_t* p_hdl, skiq_rx_block_t** pp_block, uint32_t* p_data_len) {
    if (!p_hdl || !pp_block || !p_data_len) return skiq_rx_status_error_generic;
    Clock::time_point due, deadline;
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (!active(card)) return skiq_rx_status_error_card_not_active;
        if (!g.streaming) return skiq_rx_status_error_not_streaming;
        const double secs = std::chrono::duration<double>(Clock::now() - g.t0).count();
        const double unplug = envNum("WVT_SKIQ_SIM_UNPLUG");
        if (unplug > 0 && secs > unplug) return skiq_rx_status_error_generic;
        if (envOn("WVT_SKIQ_SIM_OVERRUN") && !g.overrunDone && secs > 0.2) {
            g.overrunDone = true;
            g.blocks += kOverrunBlocks;  // these samples are gone
            return skiq_rx_status_error_overrun;
        }
        // The host fell behind by more than the DMA buffer: the oldest samples are gone.
        if (secs * g.rate - (double)g.blocks * kBlock > kDmaSamples) {
            g.blocks = (uint64_t)(secs * g.rate / kBlock);
            return skiq_rx_status_error_overrun;
        }
        due = g.t0 + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>((double)(g.blocks + 1) * kBlock / g.rate));
        const int32_t t = g.timeout;
        deadline = t == RX_TRANSFER_WAIT_FOREVER ? Clock::time_point::max()
                                                 : Clock::now() + std::chrono::microseconds(std::max(0, t));
    }
    // Wait for the block, in short steps so a stop ends the wait.
    while (Clock::now() < due) {
        if (Clock::now() >= deadline || !g.streaming) return skiq_rx_status_no_data;
        std::this_thread::sleep_for(std::min<Clock::duration>(due - Clock::now(), std::chrono::milliseconds(1)));
    }
    std::lock_guard<std::mutex> lk(g.m);
    if (!g.streaming) return skiq_rx_status_no_data;
    const Part p = part();
    auto* b = reinterpret_cast<skiq_rx_block_t*>(g.buf.data());
    b->rf_timestamp = g.tsBase + g.blocks * kBlock;
    b->sys_timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g.t0).count();
    b->raw_meta = 0;
    b->hdl = g.hdl;
    const double amp = std::pow(10.0, (-60 + gainDb()) / 20);
    g.overloaded = amp > std::pow(10.0, -1 / 20.0);
    if (!usb()) b->overload = g.overloaded ? 1 : 0;
    const bool iq = g.order == skiq_iq_order_iq;
    const double toneHz = kToneHz + 10e3 * ((int)g.port[g.hdl] - (int)skiq_rf_port_J2);
    const std::vector<double> key = {(double)g.lo, (double)g.rate, amp, iq ? 1.0 : 0.0, (double)p.res, toneHz};
    if (key != g.key) {
        g.key = key;
        const uint32_t n = kTableBlocks * kBlock;
        g.table.resize((size_t)n * 2);
        const double full = (double)(1 << (p.res - 1));
        const double cycles = std::round((toneHz - (double)g.lo) / g.rate * n);
        const double w = 2 * M_PI * cycles / n;
        std::normal_distribution<double> noise(0, std::pow(10.0, -80 / 20.0) / std::sqrt(2.0));
        auto adc = [&](double v) { return (int16_t)std::clamp(std::lround(v * full), (long)-full, (long)full - 1); };
        for (uint32_t i = 0; i < n; ++i) {
            const int16_t si = adc(amp * std::cos(w * i) + noise(g.rng));
            const int16_t sq = adc(amp * std::sin(w * i) + noise(g.rng));
            g.table[2 * i] = iq ? si : sq;
            g.table[2 * i + 1] = iq ? sq : si;
        }
    }
    const size_t at = (size_t)(g.blocks % kTableBlocks) * kBlock * 2;
    memcpy((void*)b->data, g.table.data() + at, (size_t)kBlock * 4);
    ++g.blocks;
    *p_hdl = g.hdl;
    *pp_block = b;
    *p_data_len = (uint32_t)(SKIQ_RX_HEADER_SIZE_IN_BYTES + kBlock * 4);
    return skiq_rx_status_success;
}

}  // extern "C"
