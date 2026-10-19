#include "audio_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "log.h"
#include "ui_util.h"

namespace {
using clock_ = std::chrono::steady_clock;

int64_t blockFor(double fs) { return std::clamp<int64_t>((int64_t)(fs * 0.01), 1024, 1 << 20); }  // ~10 ms
}  // namespace

AudioEngine::AudioEngine(std::function<void()> wake) : wake_(std::move(wake)) {}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::startCommon(const AudioParams& p, double fs, const std::string& device, AudioOutput output,
                              std::string& err) {
    fs_ = fs;
    ring_ = std::make_shared<AudioRing>(kAudioRate);  // ~1.4 s
    target_ = (size_t)((live_ ? 0.15 : 0.1) * kAudioRate);
    {
        std::lock_guard<std::mutex> lk(m_);
        params_ = p;
        ++paramsGen_;
        paused_ = false;
        finished_ = false;
        map_.clear();
        error_.clear();
        resyncs_ = 0;
        ppm_ = load_ = busy_ = produced_ = 0;
        chanDb_ = -200;
    }
    LOG_INFO("audio", "%s audio: %s, %s wide at %+.0f Hz offset, input %s, output %s", live_ ? "live" : "file",
             audioModeName((AudioMode)p.mode), fmtSI(p.bw, "Hz", 4).c_str(), p.offset, fmtSI(fs, "S/s", 6).c_str(),
             output == AudioOutput::None ? "none" : device.empty() ? "default device" : device.c_str());
    LOG_DEBUG("audio", "agc %s, gain %.1f dB, de-emphasis %s, squelch %s (%.1f dBFS), BFO %.0f Hz", p.agc ? "on" : "off",
              p.gainDb, deemphasisName((Deemphasis)p.deemph), p.squelch ? "on" : "off", p.squelchDb, p.bfo);
    std::string oerr;
    if (output != AudioOutput::None && !out_.open(device, kAudioRate, ring_, oerr)) {
        LOG_DEBUG("audio", "output: %s", oerr.c_str());
        if (output == AudioOutput::Require) {
            err = oerr;
            return false;
        }
        std::lock_guard<std::mutex> lk(m_);
        error_ = oerr;
    } else if (out_.isOpen()) {
        LOG_DEBUG("audio", "playing on '%s', %d Hz, latency %.0f ms", out_.name().c_str(), kAudioRate, out_.latency() * 1e3);
    }
    out_.setPrime(target_);
    out_.setDraining(false);
    out_.setPaused(false);
    quit_ = false;
    th_ = std::thread([this] { live_ ? runLive() : runFile(); });
    return true;
}

bool AudioEngine::startFile(const Source& src, int64_t s0, int64_t s1, int64_t from, bool loop, const AudioParams& p,
                            const std::string& device, std::string& err) {
    stop();
    if (!src.file || !(src.fs > 0)) {
        err = "no file loaded";
        LOG_DEBUG("audio", "%s", err.c_str());
        return false;
    }
    if (s1 <= s0) {
        err = "the play range is empty";
        return false;
    }
    live_ = false;
    src_ = src;
    liveRing_.reset();
    {
        std::lock_guard<std::mutex> lk(m_);
        s0_ = s0;
        s1_ = s1;
        loop_ = loop;
        seekTo_ = std::clamp(from, s0, s1);
    }
    return startCommon(p, src.fs, device, AudioOutput::Require, err);
}

bool AudioEngine::startLive(std::shared_ptr<SampleRing> ring, SampleFormat fmt, double fs, const AudioParams& p,
                            const std::string& device, AudioOutput output, std::string& err) {
    stop();
    if (!ring || !(fs > 0)) {
        err = "no device is streaming";
        return false;
    }
    live_ = true;
    src_ = Source{};
    liveRing_ = std::move(ring);
    liveFmt_ = fmt;
    fresh_ = 0;
    return startCommon(p, fs, device, output, err);
}

void AudioEngine::stop() {
    if (th_.joinable()) {
        quit_ = true;
        cv_.notify_all();
        th_.join();
        int64_t resyncs;
        {
            std::lock_guard<std::mutex> lk(m_);
            resyncs = resyncs_;
        }
        LOG_INFO("audio", "audio stopped: %lld underruns, %lld resyncs", (long long)out_.underruns(), (long long)resyncs);
    }
    out_.close();
    stopWav();
}

void AudioEngine::setParams(const AudioParams& p) {
    std::lock_guard<std::mutex> lk(m_);
    if (p == params_) return;
    // Sliders change the other fields every frame; only mode changes are worth a line.
    if (p.mode != params_.mode) LOG_DEBUG("audio", "mode %s", audioModeName((AudioMode)p.mode));
    params_ = p;
    ++paramsGen_;
}

void AudioEngine::setVolume(double db, bool mute) {
    out_.setVolume((float)std::pow(10.0, db / 20));
    out_.setMute(mute);
}

void AudioEngine::setPaused(bool p) {
    {
        std::lock_guard<std::mutex> lk(m_);
        paused_ = p;
    }
    out_.setPaused(p);
    cv_.notify_all();
}

void AudioEngine::setLoop(bool loop) {
    std::lock_guard<std::mutex> lk(m_);
    loop_ = loop;
    if (loop) finished_ = false;
}

void AudioEngine::seek(int64_t sample) {
    {
        std::lock_guard<std::mutex> lk(m_);
        seekTo_ = std::max<int64_t>(0, sample);
    }
    cv_.notify_all();
}

bool AudioEngine::startWav(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(wavM_);
    wavError_.clear();
    if (!wav_.open(path, kAudioRate, err)) {
        LOG_DEBUG("audio", "WAV: %s", err.c_str());
        return false;
    }
    LOG_INFO("audio", "writing WAV %s", wav_.path().c_str());
    return true;
}

void AudioEngine::stopWav() {
    std::lock_guard<std::mutex> lk(wavM_);
    if (wav_.isOpen()) LOG_INFO("audio", "WAV closed: %s (%.3f s)", wav_.path().c_str(), wav_.seconds());
    wav_.close();
}

// Input sample being heard now (call with m_ held).
double AudioEngine::heardPos() {
    const int64_t r = ring_ ? ring_->readPos() - (int64_t)(out_.latency() * kAudioRate) : 0;
    while (map_.size() >= 2 && map_[1].first <= r) map_.pop_front();
    if (map_.empty()) return -1;
    return map_.front().second + (double)(r - map_.front().first) * fs_ / kAudioRate;
}

AudioEngine::Stats AudioEngine::stats() {
    Stats s;
    s.running = running();
    if (!s.running) return s;
    s.live = live_;
    s.hasOutput = out_.isOpen();
    s.device = out_.name();
    s.underruns = out_.underruns();
    const float pk = out_.takePeak();
    s.peakDb = pk > 0 ? 20.f * std::log10(pk) : -200.f;
    {
        std::lock_guard<std::mutex> lk(m_);
        s.paused = paused_;
        s.finished = finished_;
        s.error = error_;
        s.pos = heardPos();
        s.chanDb = chanDb_;
        s.squelchOpen = sqOpen_;
        s.resyncs = resyncs_;
        s.ppm = ppm_;
        s.load = load_;
        s.ifRate = ifRate_;
    }
    s.latency = (double)ring_->fill() / kAudioRate + out_.latency();
    std::lock_guard<std::mutex> lk(wavM_);
    s.recording = wav_.isOpen();
    s.wavPath = wav_.path();
    s.wavSeconds = wav_.seconds();
    s.wavError = wavError_;
    return s;
}

void AudioEngine::produce(AudioDemod& d, const cf* in, size_t n, double inputEnd) {
    const auto t0 = clock_::now();
    audio_.clear();
    d.process(in, n, audio_);
    const double dt = std::chrono::duration<double>(clock_::now() - t0).count();
    if (out_.isOpen()) ring_->push(audio_.data(), audio_.size());
    {
        std::lock_guard<std::mutex> lk(m_);
        map_.emplace_back(ring_->written(), inputEnd);
        while (map_.size() > 4096) map_.pop_front();
        chanDb_ = d.channelDb();
        sqOpen_ = d.squelchOpen();
        ifRate_ = d.ifRate();
        busy_ += dt;
        produced_ += (double)n / fs_;
        if (produced_ >= 0.5) {
            load_ = busy_ / produced_;
            busy_ = produced_ = 0;
        }
    }
    std::lock_guard<std::mutex> lk(wavM_);
    if (wav_.isOpen() && !audio_.empty()) {
        std::string e;
        if (!wav_.write(audio_.data(), audio_.size(), e)) {
            LOG_DEBUG("audio", "WAV write failed: %s", e.c_str());
            wavError_ = e;
            wav_.close();
        }
    }
}

void AudioEngine::runFile() {
    AudioDemod d(fs_, kAudioRate);
    uint64_t gen = 0;
    int64_t cur = 0;
    const int64_t block = blockFor(fs_);
    std::vector<cf> in;
    bool done = false;
    while (!quit_) {
        bool paused, loop;
        int64_t s0, s1, seek;
        {
            std::unique_lock<std::mutex> lk(m_);
            if (paramsGen_ != gen) {
                gen = paramsGen_;
                d.setParams(params_);
            }
            paused = paused_;
            loop = loop_;
            s0 = s0_;
            s1 = s1_;
            seek = seekTo_;
            seekTo_ = -1;
            if (seek >= 0) {
                map_.clear();
                finished_ = false;
            }
        }
        if (seek >= 0) {
            cur = std::clamp(seek, s0, s1);
            d.reset();
            ring_->skipTo(ring_->written());
            {
                std::lock_guard<std::mutex> lk(m_);
                map_.emplace_back(ring_->written(), (double)cur);
            }
            out_.setDraining(false);
            out_.reprime();
            done = false;
        }
        const bool atEnd = cur >= s1 && !loop;
        if (paused || atEnd || ring_->fill() >= target_) {
            if (atEnd) {
                out_.setDraining(true);
                if (!done && ring_->fill() == 0) {
                    done = true;
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        finished_ = true;
                    }
                    if (wake_) wake_();
                }
            }
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(5), [&] { return quit_.load() || seekTo_ >= 0; });
            continue;
        }
        if (cur >= s1) cur = s0;  // loop
        const int64_t n = std::min(block, s1 - cur);
        in.resize((size_t)n);
        src_.file->read(src_.fp, cur, n, in.data());
        cur += n;
        produce(d, in.data(), (size_t)n, (double)cur);
    }
}

void AudioEngine::runLive() {
    AudioDemod d(fs_, kAudioRate);
    uint64_t gen = 0;
    const int64_t block = blockFor(fs_);
    const int bps = liveRing_->bytesPerSample();
    FormatParams fp;
    fp.fmt = liveFmt_;
    std::vector<uint8_t> raw;
    std::vector<cf> in;
    int64_t cur = liveRing_->written();
    double fillEma = (double)target_, integ = 0;
    auto resync = [&](int64_t to) {
        cur = to;
        d.reset();
        std::lock_guard<std::mutex> lk(m_);
        ++resyncs_;
    };
    while (!quit_) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (paramsGen_ != gen) {
                gen = paramsGen_;
                d.setParams(params_);
            }
        }
        const int64_t w = liveRing_->written();
        const int64_t fresh = fresh_.load();
        if (cur < fresh) {
            cur = fresh;  // retuned: skip samples of the old frequency
            d.reset();
        }
        // Fell behind (slow DSP, or a stall): jump to the newest samples.
        if (w - cur > std::max<int64_t>((int64_t)(fs_ * 0.5), 4 * block) || w - cur > liveRing_->capacity() * 3 / 4)
            resync(w - block);
        if (w - cur < block) {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(2), [&] { return quit_.load(); });
            continue;
        }
        raw.resize((size_t)block * bps);
        if (!liveRing_->read(cur, block, raw.data())) {
            resync(liveRing_->written() - block);
            continue;
        }
        in.resize((size_t)block);
        decodeSamples(fp, raw.data(), block, in.data());
        cur += block;
        produce(d, in.data(), (size_t)block, (double)cur);

        if (out_.isOpen()) {
            // The device and the sound card run from different clocks. A PI
            // loop on the buffer level trims the resampling ratio.
            const double dt = block / fs_;
            const double fill = (double)ring_->fill();
            fillEma += (fill - fillEma) * std::min(1.0, dt / 1.0);
            const double e = (fillEma - (double)target_) / kAudioRate;  // seconds
            integ = std::clamp(integ + e * dt, -3.0, 3.0);
            const double trim = std::clamp(-(0.02 * e + 0.002 * integ), -0.005, 0.005);
            d.setRatioTrim(trim);
            if (fill > (double)target_ + 0.3 * kAudioRate) {
                ring_->skipTo(ring_->written() - (int64_t)target_);
                fillEma = (double)target_;
                std::lock_guard<std::mutex> lk(m_);
                ++resyncs_;
            }
            std::lock_guard<std::mutex> lk(m_);
            ppm_ = trim * 1e6;
        }
    }
}

// ---------------------------------------------------------------------------
// WAV export
// ---------------------------------------------------------------------------
bool WavExportJob::start(const WavExportConfig& cfg, std::string& err) {
    cancel();
    if (!cfg.src.file || !(cfg.src.fs > 0) || cfg.s1 <= cfg.s0) {
        err = "nothing to export";
        LOG_DEBUG("audio", "WAV export: %s", err.c_str());
        return false;
    }
    if (!wav_.open(cfg.path, kAudioRate, err)) {
        LOG_DEBUG("audio", "WAV export: %s", err.c_str());
        return false;
    }
    LOG_INFO("audio", "exporting %s audio of samples %lld-%lld to %s", audioModeName((AudioMode)cfg.params.mode),
             (long long)cfg.s0, (long long)cfg.s1, cfg.path.c_str());
    cfg_ = cfg;
    {
        std::lock_guard<std::mutex> lk(m_);
        error_.clear();
    }
    progress_ = 0;
    seconds_ = 0;
    cancel_ = false;
    active_ = true;
    th_ = std::thread([this] { run(); });
    return true;
}

void WavExportJob::cancel() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
}

WavExportJob::Status WavExportJob::status() const {
    Status s;
    s.active = active_.load();
    s.progress = progress_.load();
    s.seconds = seconds_.load();
    s.path = cfg_.path;
    std::lock_guard<std::mutex> lk(m_);
    s.error = error_;
    return s;
}

void WavExportJob::run() {
    AudioDemod d(cfg_.src.fs, kAudioRate);
    d.setParams(cfg_.params);
    const int64_t block = std::clamp<int64_t>((int64_t)(cfg_.src.fs * 0.05), 4096, 1 << 21);
    std::vector<cf> in;
    std::vector<float> out;
    std::string err;
    for (int64_t cur = cfg_.s0; cur < cfg_.s1 && !cancel_;) {
        const int64_t n = std::min(block, cfg_.s1 - cur);
        in.resize((size_t)n);
        cfg_.src.file->read(cfg_.src.fp, cur, n, in.data());
        out.clear();
        d.process(in.data(), (size_t)n, out);
        if (!wav_.write(out.data(), out.size(), err)) break;
        cur += n;
        progress_ = (double)(cur - cfg_.s0) / (double)(cfg_.s1 - cfg_.s0);
        seconds_ = wav_.seconds();
    }
    wav_.close();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!err.empty()) error_ = err;
        else if (cancel_) error_ = "cancelled";
        LOG_INFO("audio", "WAV export %s: %.3f s", error_.empty() ? "done" : error_.c_str(), seconds_.load());
    }
    active_ = false;
}
