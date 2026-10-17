#pragma once
// SDR transmitter abstraction (USRP B200/B205 through UHD). The device thread
// asks for samples with a fill callback and streams them continuously until
// stop(); stop() ends the burst so the transmitter goes quiet.
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "sdr_device.h"

struct TxConfig {
    std::string driver = "uhd";
    std::string args = "type=b200";
    std::string antenna = "TX/RX";  // the only transmit port on the B200/B205
    double fc = 915e6;
    double rate = 1e6;
    double gain = 0;       // dB; B200/B205: 0-89.75
    double bandwidth = 0;  // analog filter, 0 = automatic
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
