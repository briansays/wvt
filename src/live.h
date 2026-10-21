#pragma once
// Live device streaming: owns the open SdrDevice, the SampleRing it fills,
// the Recorder, and a spectrum thread that turns the newest samples into
// waterfall rows for the UI.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "recorder.h"
#include "sample_ring.h"
#include "sdr_device.h"

struct LiveParams {
    int nfft = 2048;
    int window = (int)WindowType::BlackmanHarris;
    int avg = 0;             // FFTs averaged per row; 0 = all new samples (up to kMaxAnalyzeRate)
    int maxCols = 2048;      // bins beyond this are max-pooled
    double rowsPerSec = 30;  // target row rate
    bool operator==(const LiveParams&) const = default;
};

// Rows produced since the UI last asked. `gen` changes whenever the row
// geometry or meaning changes (FFT size, columns, frequency, rate); the UI
// then discards its history.
struct LiveRows {
    uint64_t gen = 0;
    int cols = 0;
    double f0 = 0, f1 = 0;  // absolute frequency extent of the columns
    int n = 0;              // rows in db
    std::vector<float> db;  // n * cols, oldest first
    std::vector<float> lin; // n * cols, linear power (for spectrum averaging)
};

class LiveEngine {
public:
    struct Stats {
        bool running = false;
        std::string description, notes, error;
        double fc = 0, rate = 0;
        SampleFormat fmt = SampleFormat::CI16;
        double analyzed = 1;   // fraction of samples that went into FFTs
        bool gpu = false;      // the last row was computed on the GPU
        double rowPeriod = 0;  // measured seconds between rows
        int64_t received = 0;  // samples received from the device
        int64_t dropped = 0;   // known lost samples (device overflows)
        int64_t gapEvents = 0; // overflow events (count may be unknown)
    };

    explicit LiveEngine(std::function<void()> wake);
    ~LiveEngine();

    // Opens the device, starts streaming and the spectrum thread.
    bool start(const StreamConfig& cfg, std::string& err);
    void stop();
    bool running() const { return dev_ != nullptr; }
    const StreamConfig& config() const { return cfg_; }
    GainRange gainRange() const;
    DeviceCaps caps() const;  // of the open device

    bool setCenterFreq(double hz, std::string& err);
    bool setGain(bool agc, double db, std::string& err);
    void setParams(const LiveParams& p);

    void takeRows(LiveRows& out);
    // For extra readers (audio): the ring, its format, and the first sample of
    // the current tuning.
    std::shared_ptr<SampleRing> ring() const { return ring_; }
    SampleFormat format() const { return dev_ ? dev_->format() : SampleFormat::CI16; }
    double sampleRate() const { return dev_ ? dev_->sampleRate() : 0; }
    int64_t freshFrom() const { return freshFrom_.load(); }
    Stats stats() const;

    bool startRecording(const RecordConfig& cfg, std::string& err);
    void stopRecording() { rec_.stop(); }
    Recorder& recorder() { return rec_; }

private:
    void spectrumLoop();

    std::function<void()> wake_;
    StreamConfig cfg_;
    std::unique_ptr<SdrDevice> dev_;
    std::shared_ptr<SampleRing> ring_;
    Recorder rec_;

    std::thread th_;
    std::atomic<bool> quit_{false};
    mutable std::mutex m_;  // guards params_, gen_, pending_, stats below
    std::condition_variable cv_;
    LiveParams params_;
    uint64_t gen_ = 1;
    LiveRows pending_;
    double analyzed_ = 1, rowPeriod_ = 0;
    std::atomic<bool> onGpu_{false};
    std::atomic<int64_t> freshFrom_{0};  // rows use only samples from here on (set on retune)
};
