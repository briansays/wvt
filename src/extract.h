#pragma once
// Exports a time range of a file, and optionally a frequency band, to a new
// SigMF or raw file (issue #18, docs/design/region-export-plan.md).
//
// Full band: the raw bytes of the range are copied unchanged (same format,
// rate and center frequency). With a band: the band center is mixed to 0 Hz
// (phase from the absolute sample index), low-pass filtered with the band as
// the passband and decimated to fs / D. The filter runs on the GPU when one
// is usable (gpuChannelize), else on the CPU (channelizeCpu).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "recorder.h"

struct ExtractConfig {
    Source src;                       // file, format, rate, center frequency
    int64_t s0 = 0, s1 = 0;           // sample range [s0, s1)
    int64_t limit0 = 0, limit1 = -1;  // allowed range (the loaded segment); -1 = the file end
    bool fullBand = true;
    double fLo = 0, fHi = 0;  // band in absolute Hz (without fullBand)
    bool ci16 = false;        // band output as ci16 instead of cf32
    bool sigmf = true;
    std::string path;        // output path; a known extension is removed
    bool overwrite = false;  // replace existing output files
    double sourceStart = 0;  // time of the file's sample 0 (s since 1970), 0 = unknown
    bool operator==(const ExtractConfig&) const = default;
};

// What an export would write. err is empty when it can run.
struct ExtractPlan {
    std::string err;
    double bw = 0, center = 0;  // band width, and the output center frequency (Hz)
    double offset = 0;          // band center - fc (Hz)
    int decim = 1, taps = 0;
    double cutoff = 0;  // filter cutoff, cycles per input sample
    double rate = 0;    // output sample rate
    SampleFormat fmt = SampleFormat::CF32;
    int64_t samples = 0;  // output samples
    double bytes = 0;     // output data bytes
    std::string base, dataPath, metaPath;
    bool exists = false;  // an output file exists already
    double freeBytes = -1;  // free space where it goes (-1 = unknown)
};

// The checks and the numbers, without writing anything (stat calls only).
ExtractPlan planExtract(const ExtractConfig& c);
// "<dir>/<source name>_<t0>s_<fc>MHz_<rate>Sps": a name that guessMeta can
// read back. rate and fc are the output values.
std::string defaultExtractPath(const std::string& sourcePath, double t0, double fc, double rate);

class ExtractJob {
public:
    struct Status {
        bool active = false;
        bool done = false;       // finished and the files are complete
        bool cancelled = false;  // stopped by cancel(); the files were deleted
        double progress = 0;     // 0..1
        std::string error, dataPath, metaPath;
        int64_t samples = 0;
        uint64_t bytes = 0;
        int64_t clipped = 0;  // ci16 values that were clipped
        double seconds = 0;   // time taken
        bool gpu = false;     // the filter ran on the GPU
    };
    ExtractJob() = default;
    ~ExtractJob() { cancel(); }
    ExtractJob(const ExtractJob&) = delete;
    ExtractJob& operator=(const ExtractJob&) = delete;

    bool start(const ExtractConfig& c, std::string& err);
    // Stops the job and deletes what it wrote. Waits for the thread.
    void cancel();
    Status status() const;

private:
    void run();
    bool pushAll(const uint8_t* raw, int64_t n);

    ExtractConfig cfg_;
    ExtractPlan plan_;
    std::shared_ptr<SampleRing> ring_;
    Recorder rec_;
    std::thread th_;
    std::atomic<bool> cancel_{false}, active_{false};
    std::atomic<double> progress_{0};
    std::atomic<int64_t> clipped_{0};
    std::atomic<bool> gpu_{false};
    mutable std::mutex m_;
    std::string error_;
    bool done_ = false, cancelled_ = false;
    double seconds_ = 0;
    Recorder::Status recStatus_;
};
