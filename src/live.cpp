#include "live.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
constexpr float kEps = 1e-20f;
constexpr int kMaxPendingRows = 4096;  // if the UI stops polling, keep only the newest
// Auto averaging analyzes at most this many samples per second (bounds CPU use).
constexpr double kMaxAnalyzeRate = 20e6;
}  // namespace

LiveEngine::LiveEngine(std::function<void()> wake) : wake_(std::move(wake)) {}

LiveEngine::~LiveEngine() { stop(); }

bool LiveEngine::start(const StreamConfig& cfg, std::string& err) {
    stop();
    auto dev = openDevice(cfg, err);
    if (!dev) return false;
    cfg_ = cfg;
    // About 2 s of samples, between 16 MB and 256 MB.
    const int bps = formatBytesPerSample(dev->format());
    const int64_t bytes = std::clamp<int64_t>((int64_t)(2.0 * dev->sampleRate()) * bps, 16 << 20, 256 << 20);
    ring_ = std::make_shared<SampleRing>(bytes / bps, bps);
    auto ring = ring_;
    if (!dev->start([ring](const uint8_t* p, int64_t n) { ring->push(p, n); },
                    [ring](int64_t lost, const char* why) { ring->noteGap(lost, why); }, err)) {
        ring_.reset();
        return false;
    }
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
    if (dev_) dev_->stop();
    dev_.reset();
}

GainRange LiveEngine::gainRange() const { return dev_ ? dev_->gainRange() : GainRange{}; }

bool LiveEngine::setCenterFreq(double hz, std::string& err) {
    if (!dev_) return false;
    if (!dev_->setCenterFreq(hz, err)) return false;
    cfg_.fc = hz;
    freshFrom_ = ring_->written();
    rec_.noteRetune(dev_->centerFreq());
    std::lock_guard<std::mutex> lk(m_);
    ++gen_;
    return true;
}

bool LiveEngine::setGain(bool agc, double db, std::string& err) {
    if (!dev_) return false;
    if (!dev_->setGain(agc, db, err)) return false;
    cfg_.agc = agc;
    cfg_.gain = db;
    return true;
}

void LiveEngine::setParams(const LiveParams& p) {
    std::lock_guard<std::mutex> lk(m_);
    if (p == params_) return;
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

    while (!quit_) {
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_until(lk, next, [&] { return quit_.load(); });
            p = params_;
        }
        if (quit_) break;
        const auto period = std::chrono::duration<double>(1.0 / std::clamp(p.rowsPerSec, 1.0, 200.0));
        next = std::max(next + std::chrono::duration_cast<clock::duration>(period), clock::now());

        const int n = isPow2(p.nfft) ? p.nfft : 2048;
        const int64_t w = ring_->written();
        // Wait for at least one FFT's worth of new samples.
        if (w - lastEnd < n) continue;
        // Auto: average FFTs over all samples since the previous row.
        const int64_t maxFrames = std::max<int64_t>(1, (int64_t)(kMaxAnalyzeRate / p.rowsPerSec / n));
        int64_t frames = p.avg > 0 ? std::min(p.avg, 64) : std::clamp<int64_t>((w - lastEnd) / n, 1, maxFrames);
        frames = std::min(frames, (w - freshFrom_.load()) / n);  // nothing from before a retune
        if (frames < 1) continue;
        const int avg = (int)frames;
        const int64_t need = (int64_t)n * avg;

        raw.resize((size_t)need * bps);
        if (!ring_->read(w - need, need, raw.data())) continue;
        buf.resize(need);
        decodeSamples(fp, raw.data(), need, buf.data());

        if (winFor != n || winType != p.window) {
            win = makeWindow((WindowType)p.window, n);
            winFor = n;
            winType = p.window;
        }
        double wsum = 0;
        for (float v : win) wsum += v;
        const float norm = (float)(1.0 / (wsum * wsum)) / avg;
        auto fft = getFFT(n);
        pw.assign(n, 0.f);
        seg.resize(n);
        for (int a = 0; a < avg; ++a) {
            const cf* s = &buf[(size_t)a * n];
            for (int i = 0; i < n; ++i) seg[i] = s[i] * win[i];
            fft->forward(seg.data());
            for (int i = 0; i < n; ++i) pw[i] += std::norm(seg[(i + n / 2) & (n - 1)]);  // fftshift
        }

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
