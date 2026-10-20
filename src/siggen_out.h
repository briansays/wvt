#pragma once
// Outputs of the signal generator:
//  - FileGenJob writes a waveform to a SigMF or raw file as fast as the disk
//    allows, through the same Recorder the live view uses.
//  - TxEngine transmits the waveform published on the SigGenBus through an
//    SdrTx and follows parameter changes while it runs.
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine.h"
#include "recorder.h"
#include "sdr_tx.h"
#include "siggen.h"

// What the generator's events are called: "pulse", "play" or "sweep".
const char* eventName(const WaveformParams& p);

struct FileGenConfig {
    RecordConfig rec;  // prefix, SigMF or raw (cf32 is ignored: `fmt` is the output format)
    SampleFormat fmt = SampleFormat::CF32;
    double seconds = 1;   // length, unless pulses > 0
    int64_t pulses = 0;   // pulsed: write exactly this many PRIs; playback: this many repeats; sweep: periods
    bool annotatePulses = false;
};

class FileGenJob {
public:
    struct Status {
        bool active = false;
        double progress = 0;  // 0..1
        std::string error;    // set if the job failed
        Recorder::Status rec;
    };

    FileGenJob() = default;
    ~FileGenJob() { cancel(); }
    bool start(const WaveformParams& p, const FileGenConfig& cfg, std::string& err);
    void cancel();  // stops early; the file keeps what was written
    Status status() const;

private:
    void run();

    WaveformParams p_;
    FileGenConfig cfg_;
    std::shared_ptr<SampleRing> ring_;
    Recorder rec_;
    std::thread th_;
    std::atomic<bool> active_{false}, cancel_{false};
    std::atomic<double> progress_{0};
    mutable std::mutex m_;
    std::string error_;
};

class TxEngine {
public:
    struct Stats {
        bool running = false;
        std::string description, notes, error;
        double fc = 0, rate = 0, gain = 0;
        GainRange range;
        SdrTx::Stats tx;
        bool finished = false;  // a finite chain has ended: stop the transmitter
        int chainPasses = 0;
    };

    TxEngine() = default;
    ~TxEngine() { stop(); }
    // Starts transmitting the waveform currently on the bus.
    bool start(const TxConfig& cfg, std::string& err);
    void stop();
    bool running() const { return tx_ != nullptr; }
    bool setCenterFreq(double hz, std::string& err);
    bool setGain(double db, std::string& err);
    Stats stats() const;

private:
    std::unique_ptr<SdrTx> tx_;
    std::shared_ptr<BusSigGen> gen_;
};

// Data for the generator window's preview plots.
struct SigPreviewReq {
    WaveformParams p;
    bool train = false;  // pulsed: show several PRIs instead of one pulse
    bool operator==(const SigPreviewReq&) const = default;
};
struct SigPreviewRes {
    SigPreviewReq req;
    std::vector<double> t, mag, i, q;  // time from the preview start (s); i/q only when `iq`
    bool iq = false;
    std::vector<double> sweepF, sweepDb;  // sweep: frequency (Hz) and level (dBFS) at each t, over one period
    std::vector<double> f, db;  // absolute frequency, power per bin in dBFS
    double rbw = 0;             // spectrum bin width, Hz
    bool hold = false;          // the spectrum is a max-hold (a sweep), not an average
    // playback
    std::string error;  // the recording could not be loaded (nothing else is set)
    double playSeconds = 0, fileSeconds = 0, playPeakDb = 0;
    double playOutPeakDb = 0;  // the output's peak, dBFS (above ampDb only through resampling overshoot)
    // chain: each item as loaded (or why not), and the plays of one pass
    struct ChainInfo {
        double seconds = 0, fileSeconds = 0;  // the segment as played, and the whole file
        double peakDb = 0;                    // recorded peak, dBFS
        double outPeakDb = 0;                 // played peak, dBFS (resampling overshoot, level)
        int64_t samples = 0;
        std::string error;
    };
    std::vector<ChainInfo> chainItems;
    std::vector<SigGen::Pulse> chainPlays;  // one pass, from its start
    double passSeconds = 0;
    int64_t chainSamples = 0;  // all items, loaded
};
std::shared_ptr<SigPreviewRes> computeSigPreview(const SigPreviewReq& r, const CancelFn& cancelled);
