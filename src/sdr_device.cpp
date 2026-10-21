#include "sdr_device.h"
#include "sdr_tx.h"

#include <cmath>

#include "log.h"
#include "ui_util.h"

std::vector<std::string> sdrDrivers() {
    std::vector<std::string> d{"sim", "siggen"};
#ifdef WVT_HAVE_RTLSDR
    d.push_back("rtlsdr");
#endif
#ifdef WVT_HAVE_UHD
    d.push_back("uhd");
#endif
    return d;
}

namespace {
constexpr double kUsrpMaxRate = 500e6;
}  // namespace

std::vector<std::pair<std::string, std::string>> parseDeviceArgs(const std::string& args) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < args.size()) {
        size_t j = args.find(',', i);
        if (j == std::string::npos) j = args.size();
        const std::string kv = args.substr(i, j - i);
        const size_t e = kv.find('=');
        if (e != std::string::npos) out.emplace_back(kv.substr(0, e), kv.substr(e + 1));
        else if (!kv.empty()) out.emplace_back(kv, "");
        i = j + 1;
    }
    return out;
}

DeviceInfo uhdDeviceInfo(const std::string& found) {
    std::string type, addr, serial, product, name;
    for (const auto& [k, v] : parseDeviceArgs(found)) {
        if (k == "type") type = v;
        else if (k == "addr") addr = v;
        else if (k == "serial") serial = v;
        else if (k == "product") product = v;
        else if (k == "name") name = v;
    }
    if (product.empty()) product = type.empty() ? "USRP" : type;
    // A network device is reached by its address (a serial would make UHD search
    // every interface again); a USB device by its serial.
    std::string args = !addr.empty() ? "addr=" + addr : !serial.empty() ? "serial=" + serial : found;
    std::string label = "USRP " + product, detail;
    if (!addr.empty()) detail = addr;
    if (!serial.empty()) detail += (detail.empty() ? "serial " : ", serial ") + serial;
    if (!name.empty()) detail += (detail.empty() ? "" : ", ") + name;
    if (!detail.empty()) label += " (" + detail + ")";
    return {"uhd", args, label};
}

std::vector<DeviceInfo> enumerateDevices() {
    std::vector<DeviceInfo> out;
    // Undetected drivers still get an entry, so Start explains what is wrong.
#ifdef WVT_HAVE_RTLSDR
    size_t before = out.size();
    enumerateRtlSdr(out);
    if (out.size() == before) out.push_back({"rtlsdr", "", "RTL-SDR (none detected)"});
#endif
#ifdef WVT_HAVE_UHD
    size_t beforeUhd = out.size();
    enumerateUhd(out);
    if (out.size() == beforeUhd) out.push_back({"uhd", "", "USRP (none detected)"});
#endif
    out.push_back({"sim", "", "Simulator (synthetic test signals)"});
    out.push_back({"siggen", "", "Signal generator loopback"});
    return out;
}

static std::unique_ptr<SdrDevice> openBackend(const StreamConfig& cfg, std::string& err) {
    if (cfg.driver == "sim") return openSimDevice(cfg, err);
    if (cfg.driver == "siggen") return openSigGenDevice(cfg, err);
#ifdef WVT_HAVE_RTLSDR
    if (cfg.driver == "rtlsdr") return openRtlSdrDevice(cfg, err);
#endif
#ifdef WVT_HAVE_UHD
    if (cfg.driver == "uhd") return openUhdDevice(cfg, err);
#endif
    if (cfg.driver == "rtlsdr" || cfg.driver == "uhd")
        err = "this wvt was built without " + cfg.driver + " support (see README: Live devices)";
    else
        err = "unknown device driver '" + cfg.driver + "' (use sim, siggen, rtlsdr or uhd)";
    return nullptr;
}

std::unique_ptr<SdrDevice> openDevice(const StreamConfig& cfg, std::string& err) {
    LOG_INFO("device", "opening %s%s%s: %s at %s, gain %s, antenna %s, bandwidth %s", cfg.driver.c_str(),
             cfg.args.empty() ? "" : ":", cfg.args.c_str(), fmtSI(cfg.rate, "S/s", 6).c_str(),
             fmtSI(cfg.fc, "Hz", 9).c_str(), cfg.agc ? "auto" : fmtSI(cfg.gain, "dB", 4).c_str(),
             cfg.antenna.empty() ? "default" : cfg.antenna.c_str(),
             cfg.bandwidth > 0 ? fmtSI(cfg.bandwidth, "Hz", 4).c_str() : "auto");
    if (cfg.driver == "uhd")
        LOG_DEBUG("device", "uhd options: channel %d, reference %s", cfg.channel, cfg.ref.empty() ? "default" : cfg.ref.c_str());
    if (cfg.driver == "rtlsdr")
        LOG_DEBUG("device", "rtlsdr options: bias-tee %s, direct sampling %d, ppm %d", cfg.biasTee ? "on" : "off",
                  cfg.directSampling, cfg.ppm);
    std::string warn;
    if (!checkSampleRate(cfg.driver, cfg.rate, err, warn)) {
        LOG_DEBUG("device", "rejected: %s", err.c_str());
        return nullptr;
    }
    if (!warn.empty()) LOG_DEBUG("device", "rate check: %s", warn.c_str());
    auto dev = openBackend(cfg, err);
    if (!dev) {
        LOG_DEBUG("device", "open failed: %s", err.c_str());
        return nullptr;
    }
    LOG_INFO("device", "opened %s: %s at %s, %s", dev->description().c_str(), fmtSI(dev->sampleRate(), "S/s", 6).c_str(),
             fmtSI(dev->centerFreq(), "Hz", 9).c_str(), formatName(dev->format()));
    if (std::fabs(dev->sampleRate() - cfg.rate) > 1e-6 * cfg.rate)
        LOG_DEBUG("device", "sample rate %s requested, device gives %s", fmtSI(cfg.rate, "S/s", 9).c_str(),
                 fmtSI(dev->sampleRate(), "S/s", 9).c_str());
    if (std::fabs(dev->centerFreq() - cfg.fc) > 1.0)
        LOG_DEBUG("device", "center frequency %s requested, device gives %s", fmtSI(cfg.fc, "Hz", 12).c_str(),
                  fmtSI(dev->centerFreq(), "Hz", 12).c_str());
    if (!dev->notes().empty()) LOG_DEBUG("device", "notes: %s", dev->notes().c_str());
    return dev;
}

bool checkSampleRate(const std::string& driver, double rate, std::string& err, std::string& warn) {
    warn.clear();
    if (!(rate > 0)) {
        err = "sample rate must be positive";
        return false;
    }
    if (driver == "rtlsdr") {
        // RTL2832U resampler limits.
        if (!((rate > 225e3 && rate <= 300e3) || (rate > 900e3 && rate <= 3.2e6))) {
            err = "RTL-SDR sample rate must be 225-300 kS/s or 0.9-3.2 MS/s (got " + fmtSI(rate, "S/s", 4) + ")";
            return false;
        }
        if (rate > 2.4e6) warn = "RTL-SDR rates above 2.4 MS/s often drop samples";
    } else if (driver == "uhd") {
        // The fastest USRP (X410) streams 500 MS/s per channel; the model's own
        // limit is checked when it opens.
        if (rate > kUsrpMaxRate) {
            err = "USRP sample rate is limited to " + fmtSI(kUsrpMaxRate, "S/s", 4);
            return false;
        }
        if (rate > 100e6) warn = "rates above 100 MS/s need a 10 GbE (or faster) link and a fast host";
    } else if (driver == "sim" || driver == "siggen") {
        if (rate > 20e6) {
            err = std::string(driver == "sim" ? "simulator" : "loopback") + " sample rate is limited to 20 MS/s";
            return false;
        }
    }
    return true;
}

std::unique_ptr<SdrTx> openTx(const TxConfig& cfg, std::string& err) {
    LOG_INFO("tx", "opening transmitter %s:%s at %s, %s", cfg.driver.c_str(), cfg.args.c_str(),
             fmtSI(cfg.rate, "S/s", 6).c_str(), fmtSI(cfg.fc, "Hz", 9).c_str());
    if (!(cfg.rate > 0)) {
        err = "transmit sample rate must be positive";
        return nullptr;
    }
#ifdef WVT_HAVE_UHD
    if (cfg.driver == "uhd") {
        if (cfg.rate > kUsrpMaxRate) {
            err = "USRP sample rate is limited to " + fmtSI(kUsrpMaxRate, "S/s", 4);
            return nullptr;
        }
        return openUhdTx(cfg, err);
    }
#endif
    err = cfg.driver == "uhd" ? "this wvt was built without UHD support, so it can't transmit (see README: Live devices)"
                              : "no transmitter driver '" + cfg.driver + "'";
    return nullptr;
}
