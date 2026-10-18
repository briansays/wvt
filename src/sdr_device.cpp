#include "sdr_device.h"
#include "sdr_tx.h"

#include <cmath>

#include "ui_util.h"

namespace {
constexpr double kBbBaseRate = 40e6;  // BB60 IQ rate before decimation
constexpr int kBbMaxDecimation = 8192;
}  // namespace

std::vector<std::string> sdrDrivers() {
    std::vector<std::string> d{"sim", "siggen"};
#ifdef WVT_HAVE_RTLSDR
    d.push_back("rtlsdr");
#endif
#ifdef WVT_HAVE_UHD
    d.push_back("uhd");
#endif
#ifdef WVT_HAVE_BB
    d.push_back("bb");
#endif
    return d;
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
    if (out.size() == beforeUhd) out.push_back({"uhd", "type=b200", "USRP B200/B205 (none detected)"});
#endif
#ifdef WVT_HAVE_BB
    size_t beforeBb = out.size();
    enumerateBb(out);
    if (out.size() == beforeBb) out.push_back({"bb", "", "Signal Hound BB60 (none detected)"});
#endif
    out.push_back({"sim", "", "Simulator (synthetic test signals)"});
    out.push_back({"siggen", "", "Signal generator loopback"});
    return out;
}

std::unique_ptr<SdrDevice> openDevice(const StreamConfig& cfg, std::string& err) {
    std::string warn;
    if (!checkSampleRate(cfg.driver, cfg.rate, err, warn)) return nullptr;
    if (cfg.driver == "sim") return openSimDevice(cfg, err);
    if (cfg.driver == "siggen") return openSigGenDevice(cfg, err);
#ifdef WVT_HAVE_RTLSDR
    if (cfg.driver == "rtlsdr") return openRtlSdrDevice(cfg, err);
#endif
#ifdef WVT_HAVE_UHD
    if (cfg.driver == "uhd") return openUhdDevice(cfg, err);
#endif
#ifdef WVT_HAVE_BB
    if (cfg.driver == "bb") return openBbDevice(cfg, err);
#endif
    if (cfg.driver == "rtlsdr" || cfg.driver == "uhd" || cfg.driver == "bb")
        err = "this wvt was built without " + cfg.driver + " support (see README: Live devices)";
    else
        err = "unknown device driver '" + cfg.driver + "' (use sim, siggen, rtlsdr, uhd or bb)";
    return nullptr;
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
        if (rate > 61.44e6) {
            err = "USRP B200/B205 sample rate is limited to 61.44 MS/s";
            return false;
        }
        if (rate > 8e6) warn = "rates above ~8 MS/s need the USRP on a USB 3 port";
    } else if (driver == "bb") {
        if (rate > kBbBaseRate * 1.01 || rate < kBbBaseRate / kBbMaxDecimation * 0.99) {
            err = "BB60 sample rate must be 40 MS/s divided by a power of 2, from 4.883 kS/s to 40 MS/s";
            return false;
        }
        if (rate > 10e6) warn = "BB60 rates above 10 MS/s need a fast CPU (the API filters on the host)";
    } else if (driver == "sim" || driver == "siggen") {
        if (rate > 20e6) {
            err = std::string(driver == "sim" ? "simulator" : "loopback") + " sample rate is limited to 20 MS/s";
            return false;
        }
    }
    return true;
}

std::vector<double> supportedRates(const std::string& driver) {
    std::vector<double> r;
    if (driver == "bb")
        for (int d = 1; d <= kBbMaxDecimation; d *= 2) r.push_back(kBbBaseRate / d);
    return r;
}

double snapSampleRate(const std::string& driver, double rate) {
    const auto rates = supportedRates(driver);
    if (rates.empty() || !(rate > 0)) return rate;
    // Nearest on a log scale: the steps are factors of 2.
    double best = rates.front();
    for (double r : rates)
        if (std::fabs(std::log(r / rate)) < std::fabs(std::log(best / rate))) best = r;
    return best;
}

std::unique_ptr<SdrTx> openTx(const TxConfig& cfg, std::string& err) {
    if (!(cfg.rate > 0)) {
        err = "transmit sample rate must be positive";
        return nullptr;
    }
#ifdef WVT_HAVE_UHD
    if (cfg.driver == "uhd") {
        if (cfg.rate > 61.44e6) {
            err = "USRP B200/B205 sample rate is limited to 61.44 MS/s";
            return nullptr;
        }
        return openUhdTx(cfg, err);
    }
#endif
    err = cfg.driver == "uhd" ? "this wvt was built without UHD support, so it can't transmit (see README: Live devices)"
                              : "no transmitter driver '" + cfg.driver + "'";
    return nullptr;
}
