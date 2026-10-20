#pragma once
// Signal generator: CW, pulsed, played-back and chained waveforms, generated as a
// phase-continuous stream of complex float samples (full scale = 1.0).
//
// The carrier is a continuously running oscillator, so pulses are coherent
// (they are gated slices of one carrier). A pulsed waveform is a pulse
// template (edge shaping x intrapulse modulation) built once per parameter
// change and replayed on the PRI schedule. Playback works the same way with
// a recording (SigMF or raw IQ) as the template, shifted by the offset. A
// chain plays several recordings in turn, each with its own offset and level.
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "iq_file.h"

enum class GenMode : int { CW = 0, Pulsed, Playback, Chain };
enum class EdgeShape : int { Rect = 0, Linear, RaisedCos, Gaussian, Count };
enum class PriPattern : int { Fixed = 0, Stagger, Jitter, Count };
enum class IntraMod : int { None = 0, LfmUp, LfmDown, LfmTriangle, Barker, Count };
// Sweeps (issue #28), for CW and pulsed signals. Ramp up goes From -> To,
// ramp down To -> From, a triangle From -> To -> From; each repeats every
// period. Stepped (frequency only) holds From, From + step, ... up to To for
// one dwell each. A CW tone follows the sweep continuously; each pulse takes
// the frequency and level the sweep has when it starts, and keeps them.
enum class SweepShape : int { Off = 0, RampUp, RampDown, Triangle, Stepped, Count };

const char* edgeName(EdgeShape e);
const char* priPatternName(PriPattern p);
const char* intraModName(IntraMod m);
const char* sweepShapeName(SweepShape s);
// Barker code lengths that exist: 2, 3, 4, 5, 7, 11, 13.
const std::vector<int>& barkerLengths();

// One recording in a chain (issue #27).
struct ChainItem {
    std::string path;        // data file (a .sigmf-data, or raw samples)
    FormatParams fp;         // how to read it
    double fileRate = 0;     // its sample rate; resampled to the output rate if different (0 = same)
    double start = 0, len = 0;  // segment to play, s (len 0 = to the end)
    double offset = 0;       // its baseband lands at fc + offset, Hz
    bool keepLevel = true;   // play the samples as recorded; otherwise scale the peak to ampDb
    double ampDb = -6;       // dBFS
    int repeats = 1;         // plays in a row
    double gapAfter = 0;     // silence after each play, s
    bool operator==(const ChainItem&) const = default;
};

struct WaveformParams {
    GenMode mode = GenMode::CW;
    double rate = 2e6;         // S/s
    double fc = 100e6;         // RF center frequency (transmit tuning, file metadata)
    double offset = 100e3;     // carrier offset from fc, Hz
    double ampDb = -6;         // peak amplitude, dBFS
    double phaseDeg = 0;       // carrier phase at the first sample
    double driftHzPerSec = 0;  // CW: linear frequency drift (ignored while the frequency sweeps)

    // Sweeps (CW and pulsed). Both run over one period and repeat until the output stops.
    SweepShape fSweep = SweepShape::Off;  // replaces `offset`
    double fFrom = -400e3, fTo = 400e3;   // offsets from fc, Hz
    double sweepPeriod = 10e-3;           // s (a stepped sweep derives it from the step and dwell)
    double fStep = 10e3, fDwell = 1e-3;   // stepped sweep
    SweepShape aSweep = SweepShape::Off;  // replaces `ampDb`; no Stepped
    double aFrom = -40, aTo = -6;         // dBFS

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

    // Playback of a recording. Its baseband (0 Hz) lands at fc + offset, and
    // its peak is scaled to ampDb.
    std::string playPath;      // data file (a .sigmf-data, or raw samples)
    FormatParams playFp;       // how to read it
    double playRate = 0;       // the file's sample rate; resampled to `rate` if different (0 = same)
    double playStart = 0;      // segment of the file to play, s
    double playLen = 0;        // 0 = to the end
    bool playLoop = true;      // repeat; otherwise play once (after startDelay)
    double playGap = 0;        // silence between repeats, s

    // Chain: the items play in order, `chainLoops` times (0 = until stopped),
    // after startDelay.
    std::vector<ChainItem> chain;
    int chainLoops = 0;

    // Impairments
    bool noise = false;
    double snrDb = 30;  // peak signal power / total noise power over the full band
    double dcI = 0, dcQ = 0;               // fraction of full scale
    double iqGainDb = 0, iqPhaseDeg = 0;   // Q branch gain and phase error
    double linewidth = 0;                  // Hz; carrier phase noise as a random walk

    bool operator==(const WaveformParams&) const = default;

    double minPri() const;   // shortest PRI the pattern can produce
    double meanPri() const;  // for duty cycle / average power
    bool sweeping() const {
        return (mode == GenMode::CW || mode == GenMode::Pulsed) && (fSweep != SweepShape::Off || aSweep != SweepShape::Off);
    }
    int sweepSteps() const;            // stepped sweep: number of frequencies
    int64_t sweepSamples() const;      // one sweep period, in samples (at least 2)
    // Frequency offset (Hz) and level (dBFS) at sample k of a period, k in [0, sweepSamples()).
    double sweepOffsetAt(int64_t k) const;
    double sweepLevelAt(int64_t k) const;
};

// The playback parameters that load chain item i (for loadPlayback / cachedPlayback).
WaveformParams chainItemParams(const WaveformParams& p, size_t i);

// A chain item for a recording, from its metadata (format, rate, frequency).
// The offset puts it at its recorded frequency relative to p.fc, or at 0 when
// that is outside the band. note: where the settings came from, and anything
// that didn't fit.
ChainItem makeChainItem(const std::string& path, const WaveformParams& p, std::string* note = nullptr);

// Chain files (*.wvtchain.json): the items, the loop count, the start delay,
// and the generator's rate and center frequency. Recordings in the chain
// file's folder (or below it) are stored relative to it, so the folder can
// move. loadChain sets chain, chainLoops, startDelay, rate and fc.
bool saveChain(const std::string& path, const WaveformParams& p, std::string& err);
bool loadChain(const std::string& path, WaveformParams& p, std::string& err);

struct ParamCheck {
    std::vector<std::string> errors, warnings;
    bool ok() const { return errors.empty(); }
};
ParamCheck checkParams(const WaveformParams& p);

// A recording segment loaded for playback, resampled to the output rate.
struct PlaybackWave {
    std::vector<std::complex<float>> s;
    double peak = 0;          // largest |sample| in the segment as recorded: it plays at ampDb
    double outPeak = 0;       // largest |sample| after resampling (filter ringing can add a little)
    int64_t fileSamples = 0;  // whole file, at its own rate
    double fileRate = 0;
};
// Loads the segment `p` selects. The last waveform loaded stays cached, so
// the preview, loopback, transmitter and file output share one copy. `cancelled`
// (optional) aborts a long load.
std::shared_ptr<const PlaybackWave> loadPlayback(const WaveformParams& p, std::string& err,
                                                 const std::function<bool()>& cancelled = {});
// The cached waveform for `p` if it's already loaded, else null. Never blocks on a load.
std::shared_ptr<const PlaybackWave> cachedPlayback(const WaveformParams& p);
// Drops the cache, so the next load rereads the file.
void forgetPlayback();
constexpr int64_t kMaxPlaySamples = int64_t(1) << 25;  // 32 Mi samples (256 MB as floats)
// The cache keeps every loaded waveform up to this many samples in all (1 GiB
// as floats). Over it, the least recently used ones that nothing else holds go.
constexpr int64_t kPlayCacheBudget = int64_t(1) << 27;
int64_t playbackCacheSamples();
void setPlaybackCacheBudget(int64_t samples);  // for tests; <= 0 restores the default

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
    // Playback: why the recording could not be loaded (the output is silent).
    const std::string& error() const { return err_; }

    void generate(std::complex<float>* out, int64_t n);
    int64_t position() const { return n_; }
    // Sweep: jump to sample k of the period (the preview samples a long sweep
    // this way). Pulsed: the next pulse starts pulseIn samples from now, at k.
    void seekSweep(int64_t k, int64_t pulseIn = 0);

    struct Pulse {
        int64_t start, len;
        int item = -1, rep = 0;  // chain: which item, and which of its repeats
    };
    // Pulses (playback repeats, sweep periods, chain items) that started during the last generate() call.
    const std::vector<Pulse>& lastPulses() const { return pulses_; }

    // Chain: passes started so far (the one playing included), and where a finite chain ends (the
    // sample after the last gap; -1 while it doesn't end). The output is
    // silent from there on.
    int chainPasses() const { return chainPass_; }
    int64_t endSample() const { return chainEnd_ ? chainEndAt_ : -1; }
    bool finished() const { return chainEnd_ && n_ >= chainEndAt_; }

private:
    void apply(const WaveformParams& p);
    void applyChain(const WaveformParams& old, const WaveformParams& p);
    bool canApplyNow() const;
    void startChainItem();
    void setItemLevel();
    void buildTemplate();
    int64_t tmplLen() const { return tmpl_ ? (int64_t)tmpl_->size() : 0; }
    double nextPri();
    void scheduleAfter(double start);
    void carrier(std::complex<float>* out, const std::complex<float>* mod, int64_t m);
    void sweep(std::complex<float>* out, int64_t m);
    void sweepPulse();
    void advanceCarrier(int64_t m);
    void impair(std::complex<float>* out, int64_t m);
    double gauss();

    WaveformParams p_, pending_;
    bool hasPending_ = false;
    double tuneFc_;
    int64_t n_ = 0;
    double ph_ = 0;  // carrier phase, radians
    double amp_ = 1;
    double offset_ = 0;    // carrier offset in use (a chain item's or a swept pulse's own)
    double noiseAmp_ = 1;  // the level the SNR refers to
    std::shared_ptr<const std::vector<std::complex<float>>> tmpl_;  // pulse, or the playback recording
    std::shared_ptr<const PlaybackWave> wave_;
    std::string err_;

    // pulse schedule
    double nextStart_ = 0;     // fractional sample position of the next pulse
    int64_t curStart_ = -1;    // pulse playing: [curStart_, curStart_ + tmpl_.size())
    int64_t lastStart_ = -1;
    double lastStartF_ = 0;    // where the last pulse was due, unrounded (keeps fractional PRIs exact)
    int burstCount_ = 0;
    size_t staggerIdx_ = 0;
    uint64_t jitterRng_ = 1;
    std::vector<Pulse> pulses_;
    int64_t sweepN_ = 0;   // CW: position in the sweep period
    int64_t sweepT0_ = 0;  // pulsed: the sample where the sweep period started

    // chain: the item that plays at nextStart_, and the one playing now
    std::vector<std::shared_ptr<const PlaybackWave>> waves_;
    size_t chainIdx_ = 0;
    int chainRep_ = 0;
    int chainPass_ = 0;  // passes started
    int chainDone_ = 0;  // passes whose last item has started (the schedule is past them)
    int curItem_ = -1;
    bool chainEnd_ = false;   // the last pass is scheduled: no more items
    int64_t chainEndAt_ = 0;
    double chainResume_ = 0;  // where the next item would have started (if the loop count grows)

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

    // Chain progress of the stream that follows the bus (the loopback shows it).
    void reportChain(int passes, bool finished) {
        chainPasses_.store(passes, std::memory_order_relaxed);
        chainFinished_.store(finished, std::memory_order_relaxed);
    }
    int chainPasses() const { return chainPasses_.load(std::memory_order_relaxed); }
    bool chainFinished() const { return chainFinished_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex m_;
    WaveformParams p_;
    std::atomic<uint64_t> ver_{1};
    std::atomic<int> chainPasses_{0};
    std::atomic<bool> chainFinished_{false};
};
SigGenBus& sigGenBus();

// A SigGen that follows the bus. The stream's own rate always wins over the
// published one (a device rate can't change while streaming).
class BusSigGen {
public:
    BusSigGen(double rate, double tuneFc);
    void generate(std::complex<float>* out, int64_t n);
    void setTuning(double fc) { gen_.setTuning(fc); }
    // Chain progress, readable from other threads.
    int chainPasses() const { return passes_.load(std::memory_order_relaxed); }
    bool finished() const { return finished_.load(std::memory_order_relaxed); }

private:
    WaveformParams fetch();
    double rate_;
    uint64_t ver_ = 0;
    SigGen gen_;
    std::atomic<int> passes_{0};
    std::atomic<bool> finished_{false};
};
