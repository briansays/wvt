#pragma once
// Background processing: each kind of computation runs on its own worker
// thread with "latest request wins" semantics. A newer request cancels the one
// in flight, so the UI stays responsive while the user drags/zooms.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "dsp.h"
#include "iq_file.h"
#include "log.h"

using CancelFn = std::function<bool()>;

template <class Req, class Res>
class Worker {
public:
    using Fn = std::function<std::shared_ptr<Res>(const Req&, const CancelFn&)>;

    // `name` tags the log lines of slow jobs.
    Worker(Fn fn, std::function<void()> onDone, const char* name = "worker")
        : fn_(std::move(fn)), onDone_(std::move(onDone)), name_(name) {
        th_ = std::thread([this] { loop(); });
    }
    ~Worker() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
            ++gen_;
        }
        cv_.notify_all();
        th_.join();
    }

    // Submits a request unless it equals the last one submitted.
    void submit(const Req& r) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (last_ && *last_ == r) return;
            last_ = r;
            pending_ = r;
            ++gen_;
        }
        cv_.notify_all();
    }
    void invalidate() {
        std::lock_guard<std::mutex> lk(m_);
        last_.reset();
    }
    // Returns a freshly completed result (once), or nullptr.
    std::shared_ptr<Res> take() {
        std::lock_guard<std::mutex> lk(m_);
        if (!fresh_) return nullptr;
        fresh_ = false;
        return result_;
    }
    bool busy() const { return busy_.load(); }

private:
    void loop() {
        for (;;) {
            Req req;
            uint64_t myGen;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return quit_ || pending_.has_value(); });
                if (quit_) return;
                req = std::move(*pending_);
                pending_.reset();
                myGen = gen_;
                busy_ = true;
            }
            // A superseded job keeps running for a short grace period so that
            // continuous dragging still produces progressive updates.
            const auto start = std::chrono::steady_clock::now();
            CancelFn cancelled = [this, myGen, start] {
                if (quit_) return true;
                return gen_.load() != myGen && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(150);
            };
            std::shared_ptr<Res> res = fn_(req, cancelled);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            // Only slow jobs: logging every recompute while panning would flood the file.
            if (secs >= 0.25) LOG_DEBUG("engine", "%s job took %.0f ms%s", name_, secs * 1e3, res ? "" : " (cancelled)");
            {
                std::lock_guard<std::mutex> lk(m_);
                if (res && !quit_) {
                    result_ = res;
                    fresh_ = true;
                }
                busy_ = pending_.has_value();
            }
            if (onDone_) onDone_();
        }
    }

    Fn fn_;
    std::function<void()> onDone_;
    const char* name_;
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::optional<Req> pending_, last_;
    std::atomic<uint64_t> gen_{0};
    std::atomic<bool> busy_{false};
    std::shared_ptr<Res> result_;
    bool fresh_ = false;
    std::atomic<bool> quit_{false};
};

// ---------------------------------------------------------------------------
// Source description shared by all requests
// ---------------------------------------------------------------------------
struct Source {
    std::shared_ptr<IqFile> file;
    FormatParams fp;
    double fs = 1e6;
    double fc = 0;
    bool operator==(const Source&) const = default;
};

// ---------------------------------------------------------------------------
// Waterfall / spectrum
// ---------------------------------------------------------------------------
struct WaterfallReq {
    Source src;
    int64_t s0 = 0, s1 = 0;  // sample range covered by the image
    int nfft = 1024;
    int window = (int)WindowType::BlackmanHarris;
    double overlap = 0.5;  // 0..0.95
    int maxRows = 1024;
    int minRows = 128;
    int maxCols = 2048;
    int avgPerRow = 1;  // minimum FFTs per row; zoomed out, rows use as many as cover their slice
    bool operator==(const WaterfallReq&) const = default;
};

struct WaterfallRes {
    WaterfallReq req;
    int rows = 0, cols = 0;
    std::vector<float> db;  // rows*cols, row 0 = earliest time
    double t0 = 0, t1 = 0;  // time extent of the image (s)
    double f0 = 0, f1 = 0;  // frequency extent (Hz, absolute)
    std::vector<double> specF, specAvg, specPeak;  // per column (dB)
    float p5 = -100, p50 = -80, p999 = 0;          // percentiles for auto-range
    int fftsPerRow = 1;
    double coverage = 1;  // fraction of the view's samples inside an FFT (< 1 only past the work budget)
};

std::shared_ptr<WaterfallRes> computeWaterfall(const WaterfallReq& r, const CancelFn& cancelled);

// ---------------------------------------------------------------------------
// Channelizer + AM / FM / PM demodulation
// ---------------------------------------------------------------------------
enum class AmMode : int { Linear = 0, Decibel, Normalized };
enum class PmMode : int { Wrapped = 0, Unwrapped, Detrended };

struct DemodReq {
    Source src;
    int64_t s0 = 0, s1 = 0;
    // channel selection
    bool chanEnable = false;
    double chanOffset = 0;  // Hz relative to fc
    double chanBw = 100e3;  // Hz
    int taps = 0;           // 0 = auto
    int decim = 0;          // 0 = auto
    // AM
    int amMode = (int)AmMode::Linear;
    int amSmooth = 1;
    // FM
    int fmSmooth = 1;
    bool fmRelative = false;  // relative to channel center instead of fc
    // PM
    int pmMode = (int)PmMode::Unwrapped;
    int pmSmooth = 1;
    bool pmDegrees = false;
    // squelch: mask FM/PM where power is below threshold
    bool squelch = false;
    double squelchDb = -60;
    // limits
    int64_t maxDense = 4'000'000;  // max demodulated samples before sparse mode
    int displayPoints = 3000;
    bool operator==(const DemodReq&) const = default;
};

struct Series {
    std::vector<double> t, y;
};
struct Stats {
    double min = 0, max = 0, mean = 0, rms = 0;  // rms about the mean
    int64_t n = 0;
};

struct DemodRes {
    DemodReq req;
    Series i, q, am, fm, pm, pwr;
    Stats sI, sAm, sFm, sPm, sPwr;
    double amIndex = 0;  // (max-min)/(max+min) of linear envelope
    double outRate = 0;
    int decim = 1, taps = 0;
    bool sparse = false;
    int64_t nOut = 0;
};

std::shared_ptr<DemodRes> computeDemod(const DemodReq& r, const CancelFn& cancelled);

// ---------------------------------------------------------------------------
// Whole-file power overview (for the navigation strip)
// ---------------------------------------------------------------------------
struct OverviewReq {
    Source src;
    int64_t s0 = 0, s1 = -1;  // sample range; s1 < 0 = to the end of the file
    int points = 2000;
    bool operator==(const OverviewReq&) const = default;
};
struct OverviewRes {
    std::vector<double> t, mean, peak;  // dBFS
};
std::shared_ptr<OverviewRes> computeOverview(const OverviewReq& r, const CancelFn& cancelled);

// Effective channel parameters (shared by UI readouts and the engine)
struct ChanDesign {
    int decim = 1, taps = 0;
};
ChanDesign chanDesign(const DemodReq& r);

// ---------------------------------------------------------------------------
// Time segment of a file (issue #16)
// ---------------------------------------------------------------------------
// Turns a segment typed in seconds into samples [s0, s1) of a file with n
// samples at rate fs. A negative t0 means the file start, a negative t1 the
// file end. On a bad segment, err says why and [s0, s1) is the whole file.
// An end past the file end is clamped, and warn says so.
struct SegmentRange {
    int64_t s0 = 0, s1 = 0;
    std::string err, warn;
    bool whole(int64_t n) const { return s0 == 0 && s1 == n; }
};
SegmentRange segmentRange(double t0, double t1, double fs, int64_t n);
