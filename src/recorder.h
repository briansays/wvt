#pragma once
// Writes a live sample stream to disk as SigMF (.sigmf-data + .sigmf-meta) or
// as a bare raw file whose name carries the rate and frequency so wvt can
// reopen it without options. A dedicated thread drains the SampleRing through
// its lossless gate; samples lost upstream become SigMF annotations.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iq_file.h"
#include "sample_ring.h"

struct RecordConfig {
    // Path prefix, e.g. "~/wvt-recordings/wvt". "_YYYYMMDD_HHMMSS_<fc>_<rate>"
    // and the extension are appended. Missing directories are created.
    std::string prefix = "~/wvt-recordings/wvt";
    bool sigmf = true;
    bool cf32 = false;      // convert to complex float32 instead of the native format
    double maxSeconds = 0;  // 0 = no limit
    double maxBytes = 0;    // 0 = no limit
    std::string description;
    // Instead of the prefix, time, frequency and rate: the output path without
    // its extension (a file export picks its own name).
    std::string exactPath;
    // Start time of the first sample for core:datetime (seconds since 1970,
    // UTC); 0 = now, negative = unknown (no core:datetime).
    double startTime = 0;
    // Extra "global" members for the SigMF metadata, as JSON text:
    // "\"ns:key\": value" pairs separated by commas (no trailing comma).
    std::string extraGlobal;
};

struct RecordSource {
    SampleFormat fmt = SampleFormat::CI16;  // native format of the ring
    double rate = 0;
    double fc = 0;
    std::string hw;  // device description for SigMF core:hw
};

class Recorder {
public:
    struct Status {
        bool active = false;
        std::string dataPath, metaPath;
        std::string stopReason;  // why the last recording ended (limit, error...)
        bool failed = false;     // stopped because of an error
        int64_t samples = 0;
        uint64_t bytes = 0;
        double seconds = 0;
        int64_t dropped = 0;  // known number of lost samples
        int gaps = 0;         // number of gaps (annotations)
        float ringFill = 0;   // 0..1, how far the writer lags behind
    };

    Recorder() = default;
    ~Recorder() { stop(); }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool start(std::shared_ptr<SampleRing> ring, const RecordSource& src, const RecordConfig& cfg, std::string& err);
    // Writes everything received up to now, then closes the files.
    void stop();
    bool active() const { return active_.load(); }
    bool sigmf() const { return cfg_.sigmf; }
    // Starts a new SigMF capture segment at the current stream position.
    void noteRetune(double fc);
    // Adds a SigMF annotation at ring position `at` (ignored for raw files and
    // outside the recording). Annotations beyond `kMaxAnnotations` are dropped.
    void annotate(int64_t at, int64_t count, const std::string& label);
    static constexpr int kMaxAnnotations = 10000;
    Status status() const;

private:
    void run();
    void handleGaps(bool all);
    bool writeMeta(std::string& err);

    struct Capture {
        int64_t start;
        double fc;
        std::string datetime;
    };
    struct Annotation {
        int64_t start;
        std::string comment;
        int64_t count = 0;  // 0 = point annotation
        std::string label;
    };

    std::shared_ptr<SampleRing> ring_;
    RecordSource src_;
    RecordConfig cfg_;
    std::thread th_;
    std::atomic<bool> active_{false}, stopReq_{false};
    std::atomic<int64_t> stopAt_{0};
    int fd_ = -1;
    int64_t start_ = 0, pos_ = 0, maxSamples_ = 0;
    size_t gapCursor_ = 0;
    int outBps_ = 0;
    double startWall_ = 0;

    mutable std::mutex m_;  // guards everything below
    std::string dataPath_, metaPath_, stopReason_;
    bool failed_ = false;
    std::vector<Capture> captures_;
    std::vector<Annotation> annotations_;
    int extraAnnotations_ = 0;  // annotate() calls, for the cap
    bool metaDirty_ = false;
    int64_t samples_ = 0, dropped_ = 0;
    uint64_t bytes_ = 0;
    int gapsLogged_ = 0;  // gaps logged one by one, up to kMaxGapLogs
    static constexpr int kMaxGapLogs = 50;
};

// "~/x" -> "$HOME/x"
std::string expandHome(const std::string& path);
// mkdir -p
bool makeDirs(const std::string& dir, std::string& err);
// Extension of a raw recording in format f, e.g. ".cs16"
const char* rawFileExt(SampleFormat f);
