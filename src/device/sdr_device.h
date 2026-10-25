#pragma once
// SDR receiver abstraction. One open device delivers a continuous stream of
// samples in its native packed format (RTL-SDR: cu8, USRP: ci16, BB60: ci16,
// Sidekiq: ci16, simulator: ci16) to a callback on the device's own thread.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "io/iq_file.h"

struct StreamConfig {
    std::string driver = "sim";  // "sim" | "siggen" | "rtlsdr" | "uhd" | "bb" | "skiq"
    std::string args;            // rtlsdr: index or serial; uhd: device args ("serial=...", "addr=192.168.10.2"); bb: serial;
                                 // skiq: serial, or "#N" for card number N
    double fc = 100e6;
    double rate = 2.4e6;
    bool agc = false;
    double gain = 30;        // dB; bb: the reference level in dBm; skiq: dB above the lowest gain index
    std::string antenna;     // uhd: "RX2", "TX/RX", "RX1"...; skiq: RF port ("J1"...); empty = device default
    double bandwidth = 0;    // uhd analog bandwidth, bb I/Q filter bandwidth, skiq RF bandwidth; 0 = auto
    int channel = 0;         // uhd: receive channel (X310 / N310 / X410 have several)
    std::string ref;         // uhd: clock and time source ("internal", "external", "gpsdo"); empty = default
    bool biasTee = false;    // rtlsdr
    int directSampling = 0;  // rtlsdr: 0 off, 1 I branch, 2 Q branch (HF on the RTL-SDR v3)
    int ppm = 0;             // rtlsdr frequency correction
    int bbAtten = -1;        // bb attenuator: -1 auto, 0-3 = 0, 10, 20, 30 dB
    int bbAmp = -1;          // bb amplifier: -1 auto, 0-3 (0 = lowest gain)
    bool extRef = false;     // bb: lock to a 10 MHz reference on port 1; skiq: the external reference clock
};

struct DeviceInfo {
    std::string driver;  // as in StreamConfig::driver
    std::string args;    // value for StreamConfig::args that selects this device
    std::string label;   // for display
    // The model ("B205mini", "X310", "BB60C", "X4", "RTL-SDR") and the serial
    // number, when the scan knows them. The configuration file's SDR profiles
    // match on them (issue #88).
    std::string product, serial;
};

struct GainRange {
    double min = 0, max = 50, step = 1;
    std::vector<double> steps;   // discrete gains if the device only supports those
    bool agc = true;             // device supports automatic gain
    const char* label = "Gain";  // the BB60 has a reference level instead
    const char* unit = "dB";
};

struct Range {
    double min = 0, max = 0;
    bool valid() const { return max > 0 && max >= min; }
};

// What an open device supports (issue #11: other USRPs). Empty / invalid
// fields are unknown; the UI then shows its defaults.
struct DeviceCaps {
    std::string product;                 // "B205mini", "X310", "N310", "X410"...
    bool usb = false;                    // USB (B2xx) or network transport
    int channels = 1;                    // receive channels
    std::vector<std::string> antennas;   // receive antennas of the open channel
    std::vector<std::string> refs;       // clock sources
    Range freq, rate, bandwidth;         // of the open channel
};

class SdrDevice {
public:
    using DataFn = std::function<void(const uint8_t* samples, int64_t count)>;
    // Samples lost before delivery; lost < 0 if the count is unknown.
    using GapFn = std::function<void(int64_t lost, const char* why)>;

    virtual ~SdrDevice() = default;

    virtual bool start(DataFn data, GapFn gap, std::string& err) = 0;
    virtual void stop() = 0;

    // Both may be called from the UI thread while streaming.
    virtual bool setCenterFreq(double hz, std::string& err) = 0;
    virtual bool setGain(bool agc, double db, std::string& err) = 0;

    // Actual values after the device coerced the request.
    virtual double centerFreq() const = 0;
    virtual double sampleRate() const = 0;
    virtual SampleFormat format() const = 0;
    virtual GainRange gainRange() const = 0;
    virtual std::string description() const = 0;  // e.g. "RTL-SDR (R820T) serial 00000001"
    virtual std::string notes() const { return {}; }  // non-fatal warnings to show the user
    virtual DeviceCaps caps() const { return {}; }
    // Set when streaming stopped by itself (e.g. the device was unplugged).
    virtual std::string fatalError() const { return {}; }

    // A device that restarts its stream to retune (the BB60) calls this hook
    // between the old samples and the new ones, so the retune is marked at the
    // right sample. Devices that don't call it return false from usesRetuneMark;
    // the engine then marks the retune after setCenterFreq returns.
    using RetuneFn = std::function<void(double fc)>;
    virtual void setRetuneMark(RetuneFn) {}  // NOLINT(performance-unnecessary-value-param): overrides move it
    virtual bool usesRetuneMark() const { return false; }
};

// Drivers compiled into this binary.
std::vector<std::string> sdrDrivers();
// Lists attached devices. UHD probes the network too, so this can take a second.
// `only`: list the devices of this driver only ("uhd", "bb", ...).
std::vector<DeviceInfo> enumerateDevices(const std::string& only = "");
// Opens and configures (rate, frequency, gain...) but does not start streaming.
std::unique_ptr<SdrDevice> openDevice(const StreamConfig& cfg, std::string& err);
// Checks a sample rate against what the driver supports. `warn` gets a
// non-fatal note (e.g. a rate that is known to drop samples). For a USRP the
// limit depends on the model, so only the absolute limit is checked here; the
// device reports the rate it can do when it opens.
bool checkSampleRate(const std::string& driver, double rate, std::string& err, std::string& warn);
// Drivers with discrete rates only (the BB60: 40 MS/s divided by a power of 2)
// list them, highest first; others return an empty list.
std::vector<double> supportedRates(const std::string& driver);
// The nearest supported rate (on a log scale), or `rate` for continuous drivers.
double snapSampleRate(const std::string& driver, double rate);
// The widest BB60 I/Q filter at this decimation (the API's table).
double bbMaxBandwidth(int decimation);

// Sidekiq helpers without SDK types (sdr_skiq.cpp uses them, skiq_check tests
// them). The gain in dB counts from the lowest gain index of the card.
int skiqGainIndex(double db, int indexMin, int indexMax, double dbPerStep);
double skiqGainDb(int index, int indexMin, double dbPerStep);
// Samples lost between two receive blocks: the RF timestamp counts samples, so
// a block that starts at `got` instead of `expected` lost the difference.
// Negative if the timestamp went back (after a retune on some cards). The
// counter wraps at 2^64.
int64_t skiqLostSamples(uint64_t expected, uint64_t got);
// Interleaved 16-bit I/Q from the card to ci16: `swap` exchanges each pair
// (Q,I -> I,Q), and the shift moves an N-bit sample (sign-extended in the low
// bits) to the top, so full scale is 0 dBFS for every resolution.
void skiqConvert(const int16_t* in, int16_t* out, size_t values, int resolution, bool swap);
// A Sidekiq receive port (issue #85): the SDK's port name ("J3"), the receive
// handle that it belongs to ("A2"), and whether it is a TRX port.
struct SkiqPort {
    std::string port, handle;
    bool trx = false;
};
// The name in the port list, the --antenna value and sessions: "J3 (RX A2)",
// "J1 (TRX A1)".
std::string skiqPortLabel(const SkiqPort& p);
// The index of the port with the name `name` (in any case), or -1. A port name
// alone ("J1", as before issue #85) gives the first port with that name.
int skiqFindPort(const std::vector<SkiqPort>& ports, const std::string& name);

// One line of a UHD device search ("type=x300,addr=192.168.40.2,name=,serial=
// 31A2B3C,product=X310") as a device list entry: network devices are selected
// by address, USB devices by serial.
DeviceInfo uhdDeviceInfo(const std::string& found);
// "a=1,b=2" -> {a: 1, b: 2}
std::vector<std::pair<std::string, std::string>> parseDeviceArgs(const std::string& args);

// Backends (each defined in its own sdr_*.cpp)
std::unique_ptr<SdrDevice> openSimDevice(const StreamConfig& cfg, std::string& err);
std::unique_ptr<SdrDevice> openSigGenDevice(const StreamConfig& cfg, std::string& err);
#ifdef WVT_HAVE_RTLSDR
std::unique_ptr<SdrDevice> openRtlSdrDevice(const StreamConfig& cfg, std::string& err);
void enumerateRtlSdr(std::vector<DeviceInfo>& out);
#endif
#ifdef WVT_HAVE_UHD
std::unique_ptr<SdrDevice> openUhdDevice(const StreamConfig& cfg, std::string& err);
void enumerateUhd(std::vector<DeviceInfo>& out);
#endif
#ifdef WVT_HAVE_BB
std::unique_ptr<SdrDevice> openBbDevice(const StreamConfig& cfg, std::string& err);
void enumerateBb(std::vector<DeviceInfo>& out);
#endif
#ifdef WVT_HAVE_SKIQ
std::unique_ptr<SdrDevice> openSkiqDevice(const StreamConfig& cfg, std::string& err);
void enumerateSkiq(std::vector<DeviceInfo>& out);
#endif
