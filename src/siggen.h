#pragma once
// Signal generator: CW and pulsed waveforms, generated as a phase-continuous
// stream of complex float samples (full scale = 1.0).
//
// The carrier is a continuously running oscillator, so pulses are coherent
// (they are gated slices of one carrier). A pulsed waveform is a pulse
// template (edge shaping x intrapulse modulation) built once per parameter
// change and replayed on the PRI schedule.
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

enum class GenMode : int { CW = 0, Pulsed };
enum class EdgeShape : int { Rect = 0, Linear, RaisedCos, Gaussian, Count };
enum class PriPattern : int { Fixed = 0, Stagger, Jitter, Count };
enum class IntraMod : int { None = 0, LfmUp, LfmDown, LfmTriangle, Barker, Count };

const char* edgeName(EdgeShape e);
const char* priPatternName(PriPattern p);
const char* intraModName(IntraMod m);
// Barker code lengths that exist: 2, 3, 4, 5, 7, 11, 13.
const std::vector<int>& barkerLengths();

struct WaveformParams {
    GenMode mode = GenMode::CW;
    double rate = 2e6;         // S/s
    double fc = 100e6;         // RF center frequency (transmit tuning, file metadata)
    double offset = 100e3;     // carrier offset from fc, Hz
    double ampDb = -6;         // peak amplitude, dBFS
    double phaseDeg = 0;       // carrier phase at the first sample
    double driftHzPerSec = 0;  // CW: linear frequency drift

    // Pulsed. The pulse width is the whole on-time, edges included.
    double pri = 1e-3;
    double pw = 10e-6;
    EdgeShape edge = EdgeShape::Rect;
    double rise = 0, fall = 0;  // edge durations (ignored for Rect)
    PriPattern priPattern = PriPattern::Fixed;
    std::vector<double> stagger{1e-3, 1.25e-3};  // PRIs used in turn
    double jitterPct = 5;                        // PRI varies uniformly by +-jitterPct %
    uint32_t seed = 1;                           // jitter sequence (repeats for the same seed)
    int burstPulses = 0;                         // pulses per burst, 0 = continuous train
    double burstGap = 0;                         // extra dead time after each burst, s
    double startDelay = 0;                       // time before the first pulse, s
    IntraMod intra = IntraMod::None;
    double chirpBw = 1e6;  // LFM sweep, Hz
    int barkerLen = 13;

    // Impairments
    bool noise = false;
    double snrDb = 30;  // peak signal power / total noise power over the full band
    double dcI = 0, dcQ = 0;               // fraction of full scale
    double iqGainDb = 0, iqPhaseDeg = 0;   // Q branch gain and phase error
    double linewidth = 0;                  // Hz; carrier phase noise as a random walk

    bool operator==(const WaveformParams&) const = default;

    double minPri() const;   // shortest PRI the pattern can produce
    double meanPri() const;  // for duty cycle / average power
};

struct ParamCheck {
    std::vector<std::string> errors, warnings;
    bool ok() const { return errors.empty(); }
};
ParamCheck checkParams(const WaveformParams& p);

// One line summary, e.g. "pulsed, PRI 1 ms, PW 10 us, LFM up 1 MHz, -6 dBFS".
std::string describeParams(const WaveformParams& p);
// The parameters as a JSON object (for SigMF metadata).
std::string paramsJson(const WaveformParams& p);

class SigGen {
public:
    // tuneFc: the center frequency of the stream being generated. The signal
    // sits at p.fc + p.offset, so a receiver tuned elsewhere sees it shifted.
    // NaN = always p.fc (files, and a transmitter that follows p.fc).
    explicit SigGen(const WaveformParams& p, double tuneFc = NAN);

    // CW changes apply immediately with continuous phase. Pulsed changes apply
    // now if no pulse is playing, otherwise when the current pulse ends.
    void setParams(const WaveformParams& p);
    void setTuning(double fc);
    const WaveformParams& params() const { return p_; }

    void generate(std::complex<float>* out, int64_t n);
    int64_t position() const { return n_; }

    struct Pulse {
        int64_t start, len;
    };
    // Pulses that started during the last generate() call.
    const std::vector<Pulse>& lastPulses() const { return pulses_; }

private:
    void apply(const WaveformParams& p);
    void buildTemplate();
    double nextPri();
    void scheduleAfter(int64_t start);
    void carrier(std::complex<float>* out, const std::complex<float>* mod, int64_t m);
    void advanceCarrier(int64_t m);
    void impair(std::complex<float>* out, int64_t m);
    double gauss();

    WaveformParams p_, pending_;
    bool hasPending_ = false;
    double tuneFc_;
    int64_t n_ = 0;
    double ph_ = 0;  // carrier phase, radians
    double amp_ = 1;
    std::vector<std::complex<float>> tmpl_;

    // pulse schedule
    double nextStart_ = 0;     // fractional sample position of the next pulse
    int64_t curStart_ = -1;    // pulse playing: [curStart_, curStart_ + tmpl_.size())
    int64_t lastStart_ = -1;
    int burstCount_ = 0;
    size_t staggerIdx_ = 0;
    uint64_t jitterRng_ = 1;
    std::vector<Pulse> pulses_;

    uint64_t rng_ = 0x9e3779b97f4a7c15ull;
    bool haveSpare_ = false;
    double spare_ = 0;
};

// Shared, latest-wins parameter store: the generator window publishes, the
// streaming outputs (loopback device, transmitter) pick changes up.
class SigGenBus {
public:
    void publish(const WaveformParams& p);
    WaveformParams get(uint64_t* version = nullptr) const;
    uint64_t version() const { return ver_.load(std::memory_order_acquire); }

private:
    mutable std::mutex m_;
    WaveformParams p_;
    std::atomic<uint64_t> ver_{1};
};
SigGenBus& sigGenBus();

// A SigGen that follows the bus. The stream's own rate always wins over the
// published one (a device rate can't change while streaming).
class BusSigGen {
public:
    BusSigGen(double rate, double tuneFc);
    void generate(std::complex<float>* out, int64_t n);
    void setTuning(double fc) { gen_.setTuning(fc); }

private:
    WaveformParams fetch();
    double rate_;
    uint64_t ver_ = 0;
    SigGen gen_;
};
