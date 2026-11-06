#pragma once
// Signal analysis of a file region (issue #111): the calculations of the
// Analysis window and of --analyze. Each one reads the channel output of a
// sample range (ChannelReader: the channel filter, or the file's samples)
// and has no ImGui, so the self-check (tools/analysis_check.cpp) and the
// command line use the same code as the window.
//
// The kinds that look at every sample (measurements, spectrum, pulses, the
// correlation search) read the range in blocks, so a range can be the whole
// file. The others hold the samples in memory, and use at most maxSamples of
// them (AnaRes::used says how many).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp/engine.h"

enum class AnaKind : int { Measure = 0, Spectrum, Histogram, Constellation, Pulses, Correlation, Symbols, Cyclo, Demod, Count };
constexpr int kAnaKinds = (int)AnaKind::Count;
// "measure", "spectrum", ... (the --analyze names), and the tab labels.
const char* anaKindName(AnaKind k);
const char* anaKindLabel(AnaKind k);
bool anaKindFromName(const std::string& s, AnaKind& out);

// The input: a sample range of a file and the channel that reads it.
struct AnaRegion {
    Source src;
    int64_t s0 = 0, s1 = 0;  // input samples [s0, s1)
    bool chanEnable = false;
    double chanOffset = 0, chanBw = 100e3;  // Hz
    int taps = 0, decim = 0;                // 0 = auto
    int64_t maxSamples = 16'000'000;        // outputs held in memory (the kinds that hold them)
    bool operator==(const AnaRegion&) const = default;
};

// --- measurements -------------------------------------------------------
struct MeasureParams {
    int nfft = 4096;
    int window = (int)WindowType::BlackmanHarris;
    bool operator==(const MeasureParams&) const = default;
};
struct MeasureRes {
    double meanDb = -200, peakDb = -200, paprDb = 0;  // dBFS (power), PAPR = peak - mean
    double dcI = 0, dcQ = 0, dcDb = -200;             // DC: mean I and Q, and its power over the total (dB)
    double gainImbDb = 0, phaseImbDeg = 0;            // I/Q imbalance (after the DC is removed)
    double carrierHz = 0;                             // the spectrum peak (absolute Hz, interpolated)
    double centroidHz = 0;                            // the power-weighted mean frequency (absolute Hz)
    double obwHz = 0, obwLo = 0, obwHi = 0;           // 99 % occupied bandwidth and its edges
    double bw26Hz = 0, bw26Lo = 0, bw26Hi = 0;        // -26 dB bandwidth (outermost bins) and its edges
    double noiseDb = -200;                            // noise floor: dBFS in one bin (the 20th percentile)
    double snrDb = 0;                                 // in the occupied bandwidth
    double rbwHz = 0;
    std::vector<double> f, psdDb;  // the spectrum it used (absolute Hz, dBFS in a bin)
};

// --- spectrum (Welch) ----------------------------------------------------
struct SpectrumParams {
    int nfft = 4096;  // issue #113: the larger sizes are a choice
    int window = (int)WindowType::BlackmanHarris;
    double overlap = 0.5;
    bool band = false;  // channel power and ACPR in [fLo, fHi]
    double fLo = 0, fHi = 0;  // absolute Hz
    bool operator==(const SpectrumParams&) const = default;
};
struct SpectrumRes {
    std::vector<double> f, avgDb, maxDb, minDb;  // absolute Hz; dBFS in a bin
    double rbwHz = 0;
    int64_t averages = 0;
    double bandDb = -200;               // the power in [fLo, fHi] (dBFS)
    double acprLoDb = 0, acprHiDb = 0;  // the bands of the same width below and above, relative to it (dBc)
    double totalDb = -200;              // the power of the whole spectrum (dBFS)
};

// --- histograms and CCDF -------------------------------------------------
enum class HistQty : int { I = 0, Q, AM, FM, PM, Count };
const char* histQtyName(HistQty q);
struct HistogramParams {
    int bins = 200;
    bool operator==(const HistogramParams&) const = default;
};
struct Histogram {
    double lo = 0, hi = 1;  // the edges of the bins
    std::vector<double> counts;
    double mean = 0, stdev = 0;
};
struct HistogramRes {
    Histogram h[(int)HistQty::Count];
    // CCDF of the power: P(power > mean + x dB), x from 0 in steps of ccdfStepDb.
    double ccdfStepDb = 0.05;
    std::vector<double> ccdfDb, ccdfProb;
    double papr1e2 = 0, papr1e3 = 0, papr1e4 = 0;  // x at a probability of 1 %, 0.1 %, 0.01 %
    double peakDb = 0;                             // the largest x
};

// --- constellation -------------------------------------------------------
enum class CarrierFix : int { None = 0, Manual, MeanFreq, PowerN, Count };
const char* carrierFixName(CarrierFix c);
struct ConstellationParams {
    int carrier = (int)CarrierFix::PowerN;
    double manualHz = 0;  // with Manual: the offset to remove (Hz from the channel center)
    int power = 4;        // with PowerN: x^power (2, 4 or 8)
    bool normalize = true;  // to an RMS magnitude of 1
    int bins = 160;
    int maxPoints = 20000;
    double symbolRate = 0;  // > 0: one point per symbol, at symbolT0 + k / symbolRate
    double symbolT0 = 0;    // s (file time)
    bool operator==(const ConstellationParams&) const = default;
};
struct ConstellationRes {
    double removedHz = 0;   // the frequency offset that was removed
    double scale = 1;       // the samples were multiplied by this (normalize)
    double extent = 1;      // the density grid covers [-extent, extent] on both axes
    int bins = 0;
    std::vector<float> density;  // bins x bins counts, row 0 = top (largest Q)
    std::vector<double> xs, ys;  // the points of the scatter
    double rmsMag = 0, magStdev = 0;  // of the points (after the scale)
};

// --- pulses ----------------------------------------------------------------
struct PulseParams {
    bool autoThreshold = true;
    double thresholdDb = 13;  // auto: dB over the noise floor; else dBFS
    double hysteresisDb = 3;
    double minWidthS = 0;  // shorter pulses are ignored
    int smooth = 1;        // moving-average samples of the power
    int maxPulses = 100000;
    int overlayPulses = 200;
    bool operator==(const PulseParams&) const = default;
};
struct Pulse {
    double toa = 0;    // 50 % crossing of the leading edge (s, file time)
    double width = 0;  // 50 % to 50 % (s)
    double pri = NAN;  // from the previous pulse (s); NaN for the first
    double peakDb = 0, meanDb = 0;  // dBFS
    double freqHz = 0;              // mean frequency in the pulse (absolute Hz)
    double chirpHzPerS = 0;         // slope of the frequency in the pulse
    double rise = NAN, fall = NAN;  // 10 % to 90 % (s)
};
struct PulseRes {
    std::vector<Pulse> pulses;
    bool truncated = false;  // more than maxPulses
    double noiseDb = -200, thresholdDb = -200;  // dBFS
    // PRI groups: the PRIs within 1 % of each other. One group: a fixed PRI;
    // a few: stagger; many: jitter.
    std::vector<double> priGroups;  // the mean of each group (s)
    double meanWidth = 0, meanPri = 0, dutyCycle = 0;
    // Overlay: the envelope of up to overlayPulses pulses, aligned on their TOA.
    std::vector<double> overlayT;                // s from the TOA
    std::vector<std::vector<float>> overlayDb;   // one row per pulse
};

// --- correlation -------------------------------------------------------------
struct CorrelationParams {
    bool search = false;     // false: autocorrelation of the region; true: find the template in [searchS0, searchS1)
    int64_t tmplS0 = 0, tmplS1 = 0;      // the template (input samples)
    int64_t searchS0 = 0, searchS1 = 0;  // where to look
    double threshold = 0.5;  // normalized correlation of a hit (0 to 1)
    int maxHits = 10000;
    bool operator==(const CorrelationParams&) const = default;
};
struct CorrPeak {
    double x = 0;  // lag (s) or time (s, file time)
    double v = 0;  // normalized correlation
};
struct CorrelationRes {
    bool search = false;
    std::vector<double> x, y;     // autocorrelation: lag (s) and |R|/R(0); search: time and score (max per pixel)
    std::vector<CorrPeak> peaks;  // autocorrelation: the strongest lags; search: the hits, in time order
    double tmplLen = 0;           // s
    bool truncated = false;       // more than maxHits
    std::string err;
};

// --- symbols -------------------------------------------------------------------
// The detector of the symbol clock: the power (shaped PSK, QAM), the size of
// its changes (OOK, ASK), or the size of the frequency changes (FSK, PSK with
// sharp edges).
enum class SymMethod : int { Envelope = 0, EnvChange, Transitions, Count };
enum class SymTrace : int { I = 0, Q, AM, FM, PM, Count };
const char* symMethodName(SymMethod m);
const char* symTraceName(SymTrace t);
struct SymbolParams {
    int method = (int)SymMethod::Transitions;
    double minRate = 0, maxRate = 0;  // search range (Hz); 0 = automatic
    double rate = 0;                  // > 0: use this rate in place of the estimate
    bool haveT0 = false;              // t0 is set; else the estimate
    double t0 = 0;                    // the time of a symbol center (s, file time)
    int trace = (int)SymTrace::FM;    // the trace for the cursors and the eye
    int eyeSymbols = 2;
    int eyeBins = 128;
    int power = 4;  // the x^power spectrum
    bool operator==(const SymbolParams&) const = default;
};
struct SymbolRes {
    double estRate = 0, estT0 = 0;  // the estimate
    double rate = 0, t0 = 0;        // what was used
    double lineSnrDb = 0;           // how far the symbol line is over the median of the search range
    std::vector<double> lineF, lineDb;    // spectrum of the detector, 0 to rate/2
    std::vector<double> powF, powDb;      // spectrum of x^power (Hz from the channel center)
    double powPeakHz = 0;                 // its peak
    double powCarrierHz = 0;              // peak / power: the carrier offset (Hz from the channel center)
    double powPeakDb = 0;                 // the peak over the median (dB)
    std::vector<float> traceT, traceY;    // the trace (s from AnaRes::t0), for the cursors
    std::vector<double> symT, symV;       // the trace at each symbol center
    int eyeW = 0, eyeH = 0;
    double eyeLo = 0, eyeHi = 1;          // the value range of the eye
    std::vector<float> eye;               // eyeH x eyeW counts, row 0 = top
};

// --- cyclostationary (FAM) ---------------------------------------------------
struct CycloParams {
    int np = 64;      // channelizer FFT size
    int maxP = 1024;  // the largest second FFT
    bool operator==(const CycloParams&) const = default;
};
struct CycloRes {
    int na = 0, nf = 0;          // alpha columns, frequency rows
    double aLo = 0, aHi = 0;     // alpha extent (Hz)
    double fLo = 0, fHi = 0;     // frequency extent (Hz from the channel center)
    std::vector<float> db;       // nf x na, row 0 = top (largest f); dB over the alpha = 0 peak
    std::vector<double> alpha, profileDb;  // max over f for each alpha (full resolution)
    std::vector<CorrPeak> peaks;           // the strongest alphas (x = alpha Hz, v = dB)
    int np = 0, p = 0, l = 0;
    int64_t samples = 0;
};

// --- digital demodulation ---------------------------------------------------
enum class DemodMode : int { OOK = 0, FSK, BPSK, QPSK, Count };
const char* demodModeName(DemodMode m);
struct DigitalParams {
    int mode = (int)DemodMode::FSK;
    double rate = 0;      // symbol rate (Hz); 0 = estimate it
    bool autoThreshold = true;
    double threshold = 0;  // OOK: linear amplitude; FSK: Hz from the channel center
    bool differential = false;  // BPSK, QPSK: a symbol is the phase change
    double loopGain = 0.02;     // timing loop (0 = fixed timing)
    int maxSymbols = 1'000'000;
    bool operator==(const DigitalParams&) const = default;
};
struct DigitalRes {
    double rate = 0;           // used
    double carrierHz = 0;      // PSK: the removed offset (Hz from the channel center)
    double threshold = 0;      // OOK, FSK: used
    std::vector<double> symT;  // s (file time)
    std::vector<float> soft, softQ;  // the decision values (PSK: I and Q after the carrier)
    std::vector<uint8_t> bits;       // one per byte, 0 or 1
    int bitsPerSymbol = 1;
    bool truncated = false;
};

// --- the request -----------------------------------------------------------
struct AnaReq {
    AnaKind kind = AnaKind::Measure;
    AnaRegion reg;
    MeasureParams measure;
    SpectrumParams spectrum;
    HistogramParams hist;
    ConstellationParams cons;
    PulseParams pulse;
    CorrelationParams corr;
    SymbolParams sym;
    CycloParams cyclo;
    DigitalParams digital;
    bool operator==(const AnaReq&) const = default;
};

struct AnaRes {
    AnaReq req;
    std::string err;  // why there is no result
    double rate = 0;  // channel output rate
    double center = 0;  // channel center (absolute Hz)
    int64_t wanted = 0, used = 0;  // outputs in the region, and outputs the kind used
    double t0 = 0;                 // file time of the first output used
    double seconds = 0;            // time taken
    MeasureRes measure;
    SpectrumRes spectrum;
    HistogramRes hist;
    ConstellationRes cons;
    PulseRes pulse;
    CorrelationRes corr;
    SymbolRes sym;
    CycloRes cyclo;
    DigitalRes digital;
};

// r with only the settings of r.kind (the others at their defaults), so that
// a change on one tab does not make the result of another tab old.
AnaReq anaOnlyKind(const AnaReq& r);

std::shared_ptr<AnaRes> computeAnalysis(const AnaReq& r, const CancelFn& cancelled, Progress* progress = nullptr);

// --analyze-opt: sets one setting of r from "key" and "value" (times in
// seconds of file time, at r.reg.src.fs). False with err on a bad key or
// value. analysisOptionHelp() lists the keys.
bool anaSetOption(AnaReq& r, const std::string& key, const std::string& value, std::string& err);
// "key=value,key=value": each one through anaSetOption.
bool anaSetOptions(AnaReq& r, const std::string& list, std::string& err);
const char* analysisOptionHelp();

// The result as text (--analyze --analyze-out): a header of "# " lines and
// CSV rows.
std::string analysisReport(const AnaRes& r);

// Building blocks (also for the self-check) -----------------------------------
// Welch average of n samples: nfft-point FFTs with the window, at a hop of
// nfft * (1 - overlap). Powers per bin are fftshifted and scaled so that a
// full-scale tone in one bin reads 1 (0 dBFS). avg, mx, mn get nfft values.
// Returns the number of FFTs (0 when n < nfft: then one zero-padded FFT).
int64_t welchPsd(const cf* x, int64_t n, int nfft, int window, double overlap, std::vector<double>& avg,
                 std::vector<double>& mx, std::vector<double>& mn);
// The equivalent noise bandwidth of a window, in bins.
double windowEnbw(int window, int n);
// The strongest spectral line of v (real) between rates lo and hi (Hz, at
// sample rate fs), interpolated. The spectrum (0 to fs/2, dB) goes to f, db.
double strongestLine(const std::vector<float>& v, double fs, double lo, double hi, std::vector<double>* f = nullptr,
                     std::vector<double>* db = nullptr, double* snrDb = nullptr, std::complex<double>* phasor = nullptr);
int nextPow2(int64_t n);

// Symbol cursors in a plot that shows [lo, hi] (s): the symbol centers
// t0 + k / rate in it (none when more than maxCursors), S0 (the first one
// after the left 2 % of the plot) and the rate handle, n symbols after S0
// (80 % of the way across).
struct SymbolCursors {
    std::vector<double> xs;
    double s0 = 0, handle = 0;
    int n = 0;
};
SymbolCursors symbolCursors(double rate, double t0, double lo, double hi, int maxCursors = 2000);
// The rate that puts the handle, n symbols after s0, at x. 0 when x is not after s0.
inline double rateFromHandle(double s0, double x, int n) { return n > 0 && x > s0 ? n / (x - s0) : 0; }
