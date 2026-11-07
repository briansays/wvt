#pragma once
// SDR transmitter abstraction (a USRP through UHD, or a Sidekiq through
// libsidekiq, issue #86). The device thread
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
    std::string driver = "uhd";  // "uhd" | "skiq"
    std::string args;      // uhd: device args, empty = the first USRP found;
                           // skiq: serial or "#N", empty = the open card, else the first free one
    std::string antenna;   // uhd: empty = the channel's first transmit antenna (TX/RX on the B2xx);
                           // skiq: the TX port, "J1 (TX A1)" or "J1 (TRX A1)"; empty = handle A1, default port
    double fc = 915e6;
    double rate = 1e6;
    double gain = 0;       // dB; B200/B205: 0-89.75; skiq: dB above the most attenuation
    double bandwidth = 0;  // analog filter, 0 = automatic
    int channel = 0;       // uhd: transmit channel (a Sidekiq takes the handle from the port)
    std::string ref;       // clock and time source; empty = device default; skiq: "external" or empty
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

// The transmit drivers of this build ("uhd", "skiq").
std::vector<std::string> txDrivers();

#ifdef WVT_HAVE_UHD
std::unique_ptr<SdrTx> openUhdTx(const TxConfig& cfg, std::string& err);
#endif
#ifdef WVT_HAVE_SKIQ
std::unique_ptr<SdrTx> openSkiqTx(const TxConfig& cfg, std::string& err);
#endif
