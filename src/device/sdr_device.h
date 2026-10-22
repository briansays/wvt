#pragma once
// SDR receiver abstraction. One open device delivers a continuous stream of
// samples in its native packed format (RTL-SDR: cu8, USRP: ci16, BB60: ci16,
// simulator: ci16) to a callback on the device's own thread.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "io/iq_file.h"

struct StreamConfig {
    std::string driver = "sim";  // "sim" | "siggen" | "rtlsdr" | "uhd" | "bb"
    std::string args;            // rtlsdr: index or serial; uhd: device args ("serial=...", "addr=192.168.10.2"); bb: serial
    double fc = 100e6;
    double rate = 2.4e6;
    bool agc = false;
    double gain = 30;        // dB; bb: the reference level in dBm
    std::string antenna;     // uhd: "RX2", "TX/RX", "RX1"...; empty = device default
    double bandwidth = 0;    // uhd analog bandwidth, bb I/Q filter bandwidth; 0 = auto
    int channel = 0;         // uhd: receive channel (X310 / N310 / X410 have several)
    std::string ref;         // uhd: clock and time source ("internal", "external", "gpsdo"); empty = default
    bool biasTee = false;    // rtlsdr
    int directSampling = 0;  // rtlsdr: 0 off, 1 I branch, 2 Q branch (HF on the RTL-SDR v3)
    int ppm = 0;             // rtlsdr frequency correction
    int bbAtten = -1;        // bb attenuator: -1 auto, 0-3 = 0, 10, 20, 30 dB
    int bbAmp = -1;          // bb amplifier: -1 auto, 0-3 (0 = lowest gain)
    bool extRef = false;     // bb: lock to a 10 MHz reference on port 1
};

struct DeviceInfo {
    std::string driver;  // as in StreamConfig::driver
    std::string args;    // value for StreamConfig::args that selects this device
    std::string label;   // for display
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
std::vector<DeviceInfo> enumerateDevices();
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
