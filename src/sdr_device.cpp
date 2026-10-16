#include "sdr_device.h"

#include "ui_util.h"

std::vector<std::string> sdrDrivers() {
    std::vector<std::string> d{"sim"};
#ifdef WVT_HAVE_RTLSDR
    d.push_back("rtlsdr");
#endif
#ifdef WVT_HAVE_UHD
    d.push_back("uhd");
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
    out.push_back({"sim", "", "Simulator (synthetic test signals)"});
    return out;
}

std::unique_ptr<SdrDevice> openDevice(const StreamConfig& cfg, std::string& err) {
    std::string warn;
    if (!checkSampleRate(cfg.driver, cfg.rate, err, warn)) return nullptr;
    if (cfg.driver == "sim") return openSimDevice(cfg, err);
#ifdef WVT_HAVE_RTLSDR
    if (cfg.driver == "rtlsdr") return openRtlSdrDevice(cfg, err);
#endif
#ifdef WVT_HAVE_UHD
    if (cfg.driver == "uhd") return openUhdDevice(cfg, err);
#endif
    if (cfg.driver == "rtlsdr" || cfg.driver == "uhd")
        err = "this wvt was built without " + cfg.driver + " support (see README: Live devices)";
    else
        err = "unknown device driver '" + cfg.driver + "' (use sim, rtlsdr or uhd)";
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
    } else if (driver == "sim") {
        if (rate > 20e6) {
            err = "simulator sample rate is limited to 20 MS/s";
            return false;
        }
    }
    return true;
}
