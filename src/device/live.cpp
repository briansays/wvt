#include "device/live.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "gpu/gpu.h"
#include "common/log.h"
#include "common/ui_util.h"

namespace {
constexpr float kEps = 1e-20f;
constexpr int kMaxPendingRows = 4096;  // if the UI stops polling, keep only the newest
// Auto averaging analyzes at most this many samples per second (bounds CPU
// use). The GPU takes more (issue #3).
constexpr double kMaxAnalyzeRate = 20e6;
}  // namespace

LiveEngine::LiveEngine(std::function<void()> wake) : wake_(std::move(wake)) {}

LiveEngine::~LiveEngine() { stop(); }

bool LiveEngine::start(const StreamConfig& cfg, std::string& err) {
    stop();
    auto dev = openDevice(cfg, err);
    if (!dev) return false;
    cfg_ = cfg;
    // About 2 s of samples, between 16 MB and 1 GB (a network USRP at 250 MS/s
    // fills 1 GB in a second).
    const int bps = formatBytesPerSample(dev->format());
    const int64_t bytes = std::clamp<int64_t>((int64_t)(2.0 * dev->sampleRate()) * bps, 16 << 20, 1 << 30);
    ring_ = std::make_shared<SampleRing>(bytes / bps, bps);
    auto ring = ring_;
    // A device that restarts its stream to retune marks the boundary itself,
    // between the last old sample and the first new one.
    dev->setRetuneMark([this, ring](double fc) {
        freshFrom_ = ring->written();
        rec_.noteRetune(fc);
    });
    if (!dev->start([ring](const uint8_t* p, int64_t n) { ring->push(p, n); },
                    [ring](int64_t lost, const char* why) { ring->noteGap(lost, why); }, err)) {
        LOG_DEBUG("live", "stream start failed: %s", err.c_str());
        ring_.reset();
        return false;
    }
    LOG_DEBUG("live", "stream started");
    LOG_DEBUG("live", "sample ring %s (%s)", fmtSI((double)bytes, "B", 4).c_str(),
              fmtSI((double)(bytes / bps) / dev->sampleRate(), "s", 3).c_str());
    dev_ = std::move(dev);
    {
        std::lock_guard<std::mutex> lk(m_);
        ++gen_;
        pending_ = LiveRows{};
        analyzed_ = 1;
        rowPeriod_ = 0;
    }
    freshFrom_ = 0;
    quit_ = false;
    th_ = std::thread([this] { spectrumLoop(); });
    return true;
}

void LiveEngine::stop() {
    rec_.stop();
    if (th_.joinable()) {
        quit_ = true;
        cv_.notify_all();
        th_.join();
    }
    if (dev_) {
        dev_->stop();
        LOG_INFO("live", "stream stopped: %s received, %lld samples lost in %lld gaps%s%s",
                 fmtSI((double)ring_->written(), "S", 4).c_str(), (long long)ring_->droppedDevice(),
                 (long long)ring_->deviceGapEvents(), dev_->fatalError().empty() ? "" : ", error: ",
                 dev_->fatalError().c_str());
    }
    dev_.reset();
}

GainRange LiveEngine::gainRange() const { return dev_ ? dev_->gainRange() : GainRange{}; }

DeviceCaps LiveEngine::caps() const { return dev_ ? dev_->caps() : DeviceCaps{}; }

bool LiveEngine::setCenterFreq(double hz, std::string& err) {
    if (!dev_) return false;
    if (!dev_->setCenterFreq(hz, err)) {
        LOG_DEBUG("live", "retune to %s failed: %s", fmtSI(hz, "Hz", 9).c_str(), err.c_str());
        return false;
    }
    LOG_INFO("live", "tuned to %s", fmtSI(dev_->centerFreq(), "Hz", 9).c_str());
    cfg_.fc = hz;
    if (!dev_->usesRetuneMark()) {
        freshFrom_ = ring_->written();
        rec_.noteRetune(dev_->centerFreq());
    }
    std::lock_guard<std::mutex> lk(m_);
    ++gen_;
    return true;
}

bool LiveEngine::setGain(bool agc, double db, std::string& err) {
    if (!dev_) return false;
    if (!dev_->setGain(agc, db, err)) {
        LOG_DEBUG("live", "gain change failed: %s", err.c_str());
        return false;
    }
    LOG_INFO("live", "gain %s", agc ? "auto" : fmtSI(db, "dB", 4).c_str());
    cfg_.agc = agc;
    cfg_.gain = db;
    return true;
}

void LiveEngine::setParams(const LiveParams& p) {
    std::lock_guard<std::mutex> lk(m_);
    if (p == params_) return;
    LOG_DEBUG("live", "spectrum: FFT %d, avg %d, %.0f rows/s, window %d, %d columns", p.nfft, p.avg, p.rowsPerSec, p.window,
              p.maxCols);
    if (p.nfft != params_.nfft || p.maxCols != params_.maxCols) ++gen_;
    params_ = p;
    cv_.notify_all();
}

void LiveEngine::takeRows(LiveRows& out) {
    std::lock_guard<std::mutex> lk(m_);
    out = std::move(pending_);
    pending_ = LiveRows{};
    pending_.gen = gen_;
    if (out.gen != gen_) {
        // Rows from before the latest change are stale.
        out = LiveRows{};
        out.gen = gen_;
    }
}

LiveEngine::Stats LiveEngine::stats() const {
    Stats s;
    if (!dev_) return s;
    s.running = true;
    s.description = dev_->description();
    s.notes = dev_->notes();
    s.error = dev_->fatalError();
    s.fc = dev_->centerFreq();
    s.rate = dev_->sampleRate();
    s.fmt = dev_->format();
    s.received = ring_->written();
    s.dropped = ring_->droppedDevice();
    s.gapEvents = ring_->deviceGapEvents();
    std::lock_guard<std::mutex> lk(m_);
    s.analyzed = analyzed_;
    s.gpu = onGpu_;
    s.rowPeriod = rowPeriod_;
    return s;
}

bool LiveEngine::startRecording(const RecordConfig& cfg, std::string& err) {
    if (!dev_) {
        err = "no device is streaming";
        return false;
    }
    RecordSource src;
    src.fmt = dev_->format();
    src.rate = dev_->sampleRate();
    src.fc = dev_->centerFreq();
    src.hw = dev_->description();
    return rec_.start(ring_, src, cfg, err);
}

void LiveEngine::spectrumLoop() {
    using clock = std::chrono::steady_clock;
    FormatParams fp;
    fp.fmt = dev_->format();
    const int bps = ring_->bytesPerSample();
    std::vector<uint8_t> raw;
    std::vector<cf> buf, seg;
    std::vector<float> pw, win;
    LiveParams p;
    int winFor = -1, winType = -1;
    int64_t lastEnd = ring_->written();  // newest sample used by the previous row
    auto lastRow = clock::now();
    auto next = clock::now();
    // Device gaps are counted in the ring (the RX callback must not log);
    // report new ones from here, at most every 5 s.
    int64_t gapsLogged = ring_->deviceGapEvents(), lostLogged = ring_->droppedDevice();
    auto lastGapLog = clock::now() - std::chrono::seconds(5);
    bool fatalLogged = false;
    GpuLiveSpectrum gpu;  // used while gpuLiveUsable()
    int side = -1;        // 1 = the last row ran on the GPU, 0 = on the CPU; logged when it changes

    while (!quit_) {
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_until(lk, next, [&] { return quit_.load(); });
            p = params_;
        }
        if (quit_) break;
        if (const int64_t g = ring_->deviceGapEvents(); g != gapsLogged && clock::now() - lastGapLog >= std::chrono::seconds(5)) {
            const int64_t lost = ring_->droppedDevice();
            LOG_WARN("live", "device dropped samples: %lld gaps, %lld samples since the last report", (long long)(g - gapsLogged),
                     (long long)(lost - lostLogged));
            gapsLogged = g;
            lostLogged = lost;
            lastGapLog = clock::now();
        }
        if (!fatalLogged && !dev_->fatalError().empty()) {
            LOG_INFO("live", "device stopped streaming: %s", dev_->fatalError().c_str());
            fatalLogged = true;
        }
        const auto period = std::chrono::duration<double>(1.0 / std::clamp(p.rowsPerSec, 1.0, 200.0));
        next = std::max(next + std::chrono::duration_cast<clock::duration>(period), clock::now());

        const int n = isPow2(p.nfft) ? p.nfft : 2048;
        const int64_t w = ring_->written();
        // Wait for at least one FFT's worth of new samples.
        if (w - lastEnd < n) continue;
        // Auto: average FFTs over all samples since the previous row.
        // Auto averaging covers every new sample, up to the CPU's or the GPU's rate.
        const GpuLimits gl = gpuLimits();
        auto framesFor = [&](double maxRate) {
            const int64_t maxFrames = std::max<int64_t>(1, (int64_t)(maxRate / p.rowsPerSec / n));
            const int64_t f = p.avg > 0 ? std::min(p.avg, 64) : std::clamp<int64_t>((w - lastEnd) / n, 1, maxFrames);
            return std::min(f, (w - freshFrom_.load()) / n);  // nothing from before a retune
        };
        bool tryGpu = gpuLiveUsable();
        const char* why = tryGpu ? "the GPU call failed" : !gpuEnabled() ? "the GPU is off or not available" : "waiting after a GPU error";
        int64_t frames = framesFor(tryGpu ? gl.liveMaxRate : kMaxAnalyzeRate);
        if (tryGpu && (double)frames * n < gl.liveMinInput) {
            tryGpu = false;  // a small row is faster on the CPU
            why = "row below the GPU minimum";
            frames = framesFor(kMaxAnalyzeRate);
        }
        if (frames < 1) continue;
        const int avg = (int)frames;
        const int64_t need = (int64_t)n * avg;

        raw.resize((size_t)need * bps);
        if (!ring_->read(w - need, need, raw.data())) continue;

        if (winFor != n || winType != p.window) {
            win = makeWindow((WindowType)p.window, n);
            winFor = n;
            winType = p.window;
        }
        double wsum = 0;
        for (float v : win) wsum += v;
        const float norm = (float)(1.0 / (wsum * wsum)) / avg;
        pw.resize(n);
        const bool onGpu = tryGpu && gpu.run(raw.data(), fp, n, avg, win.data(), pw.data()) == GpuStatus::Ok;
        if (!onGpu) {
            // After a GPU failure this one row may be larger than the CPU cap; the next uses the CPU cap.
            buf.resize(need);
            decodeSamples(fp, raw.data(), need, buf.data());
            auto fft = getFFT(n);
            pw.assign(n, 0.f);
            seg.resize(n);
            for (int a = 0; a < avg; ++a) {
                const cf* s = &buf[(size_t)a * n];
                for (int i = 0; i < n; ++i) seg[i] = s[i] * win[i];
                fft->forward(seg.data());
                for (int i = 0; i < n; ++i) pw[i] += std::norm(seg[(i + n / 2) & (n - 1)]);  // fftshift
            }
        }
        if (side != (int)onGpu) {
            if (onGpu) LOG_DEBUG("live", "spectrum rows on the GPU (FFT %d, %d FFTs per row)", n, avg);
            else LOG_DEBUG("live", "spectrum rows on the CPU: %s (FFT %d, %d FFTs per row)", why, n, avg);
            side = onGpu;
        }
        onGpu_ = onGpu;

        const int cols = std::min(n, std::max(16, p.maxCols));
        const int perCol = n / cols;
        const double rate = dev_->sampleRate(), fc = dev_->centerFreq();
        const double binHz = rate / n;
        auto now = clock::now();
        {
            std::lock_guard<std::mutex> lk(m_);
            // Fraction of new samples that this row actually analyzed.
            const double fresh = (double)std::min<int64_t>(need, w - lastEnd);
            const double frac = fresh / (double)(w - lastEnd);
            analyzed_ = analyzed_ * 0.9 + frac * 0.1;
            const double dt = std::chrono::duration<double>(now - lastRow).count();
            rowPeriod_ = rowPeriod_ > 0 ? rowPeriod_ * 0.9 + dt * 0.1 : dt;

            LiveRows& o = pending_;
            if (o.gen != gen_ || o.cols != cols) {
                o = LiveRows{};
                o.gen = gen_;
                o.cols = cols;
            }
            o.f0 = fc + (-n / 2 - 0.5) * binHz;
            o.f1 = fc + (n / 2 - 0.5) * binHz;
            if (o.n >= kMaxPendingRows) {
                o.db.erase(o.db.begin(), o.db.begin() + cols);
                o.lin.erase(o.lin.begin(), o.lin.begin() + cols);
                --o.n;
            }
            for (int c = 0; c < cols; ++c) {
                float mx = 0.f;
                for (int b = 0; b < perCol; ++b) mx = std::max(mx, pw[(size_t)c * perCol + b]);
                mx *= norm;
                o.lin.push_back(mx);
                o.db.push_back(10.f * std::log10(mx + kEps));
            }
            ++o.n;
        }
        lastEnd = w;
        lastRow = now;
        if (wake_) wake_();
    }
}
