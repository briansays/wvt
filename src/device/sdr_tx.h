#pragma once
// SDR transmitter abstraction (a USRP through UHD). The device thread
// asks for samples with a fill callback and streams them continuously until
// stop(); stop() ends the burst so the transmitter goes quiet.
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "device/sdr_device.h"

// Transmit safety limits (issue #49): --tx-max-gain and --tx-allow, or
// WVT_TX_MAX_GAIN and WVT_TX_ALLOW. TxEngine applies them, so the command
// line and the GUI both obey them.
struct TxBand {
    double lo = 0, hi = 0;  // Hz
};
struct TxLimits {
    double maxGain = NAN;       // dB; NaN = no limit but the device's range
    std::vector<TxBand> allow;  // empty = any frequency the device can tune
    bool none = false;          // the allowed bands of two sources do not overlap: no frequency is allowed
    bool any() const { return !std::isnan(maxGain) || !allow.empty() || none; }
};

struct TxConfig {
    std::string driver = "uhd";
    std::string args;      // empty: the first USRP found
    std::string antenna;   // empty: the channel's first transmit antenna (TX/RX on the B2xx)
    double fc = 915e6;
    double rate = 1e6;
    double gain = 0;       // dB; B200/B205: 0-89.75
    double bandwidth = 0;  // analog filter, 0 = automatic
    int channel = 0;       // transmit channel
    std::string ref;       // clock and time source; empty = device default
    TxLimits limits;
};

class SdrTx {
public:
    // Fill `n` normalized complex float samples (|x| <= 1).
    using FillFn = std::function<void(std::complex<float>* out, int64_t n)>;
    struct Stats {
        int64_t sent = 0;        // samples handed to the device
        int64_t underflows = 0;  // the host did not deliver samples in time
        int64_t seqErrors = 0;   // packets lost between host and device
        int64_t timeErrors = 0;
    };

    virtual ~SdrTx() = default;
    virtual bool start(FillFn fill, std::string& err) = 0;
    virtual void stop() = 0;

    virtual bool setCenterFreq(double hz, std::string& err) = 0;
    virtual bool setGain(double db, std::string& err) = 0;

    virtual double centerFreq() const = 0;
    virtual double sampleRate() const = 0;
    virtual double gain() const = 0;
    virtual GainRange gainRange() const = 0;
    virtual std::string description() const = 0;
    virtual std::string antenna() const { return {}; }  // the transmit port in use
    virtual std::string notes() const { return {}; }
    virtual std::string fatalError() const { return {}; }
    virtual Stats stats() const = 0;
};

// Opens and configures the transmitter (rate, frequency, gain) without
// starting it.
std::unique_ptr<SdrTx> openTx(const TxConfig& cfg, std::string& err);

#ifdef WVT_HAVE_UHD
std::unique_ptr<SdrTx> openUhdTx(const TxConfig& cfg, std::string& err);
#endif
