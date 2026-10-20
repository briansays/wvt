#include "siggen_out.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "dsp.h"
#include "log.h"
#include "ui_util.h"

// ---------------------------------------------------------------------------
// File generation
// ---------------------------------------------------------------------------
const char* eventName(const WaveformParams& p) {
    return p.mode == GenMode::Playback ? "play" : p.mode == GenMode::CW ? "sweep" : "pulse";
}

bool FileGenJob::start(const WaveformParams& p, const FileGenConfig& cfg, std::string& err) {
    cancel();
    ParamCheck chk = checkParams(p);
    if (!chk.ok()) {
        err = chk.errors.front();
        LOG_DEBUG("siggen", "file generation rejected: %s", err.c_str());
        return false;
    }
    if (cfg.pulses > 0 ? p.mode == GenMode::CW && !p.sweeping() : !(cfg.seconds > 0)) {
        err = cfg.pulses > 0 ? "a pulse count needs pulsed mode, playback or a sweep" : "length must be positive";
        LOG_DEBUG("siggen", "file generation rejected: %s", err.c_str());
        return false;
    }
    p_ = p;
    cfg_ = cfg;
    if (p.mode == GenMode::Playback) {
        auto w = loadPlayback(p, err);
        if (!w) {
            LOG_DEBUG("siggen", "playback file: %s", err.c_str());
            return false;
        }
        if (cfg.pulses > 0 && !p.playLoop) {  // played once: the file ends with the recording
            cfg_.pulses = 0;
            cfg_.seconds = ((double)std::llround(p.startDelay * p.rate) + (double)w->s.size()) / p.rate;
        }
    }
    const int bps = formatBytesPerSample(cfg.fmt);
    ring_ = std::make_shared<SampleRing>((32 << 20) / bps, bps);

    RecordSource src;
    src.fmt = cfg.fmt;
    src.rate = p.rate;
    src.fc = p.fc;
    src.hw = "wvt signal generator";
    RecordConfig rc = cfg.rec;
    rc.cf32 = false;
    if (rc.description.empty()) rc.description = "wvt signal generator: " + describeParams(p);
    rc.extraGlobal = "\"wvt:siggen\": " + paramsJson(p);
    if (!rec_.start(ring_, src, rc, err)) return false;
    LOG_INFO("siggen", "generating %s at %s, %s to a file, format %s", describeParams(p).c_str(), fmtSI(p.rate, "S/s", 6).c_str(),
             cfg_.pulses > 0 ? (std::to_string(cfg_.pulses) + " " + eventName(p) + "s").c_str()
                             : fmtSI(cfg_.seconds, "s", 5).c_str(),
             formatName(cfg.fmt));

    {
        std::lock_guard<std::mutex> lk(m_);
        error_.clear();
    }
    progress_ = 0;
    cancel_ = false;
    active_ = true;
    th_ = std::thread([this] { run(); });
    return true;
}

void FileGenJob::cancel() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
}

FileGenJob::Status FileGenJob::status() const {
    Status s;
    s.active = active_;
    s.progress = progress_;
    s.rec = rec_.status();
    std::lock_guard<std::mutex> lk(m_);
    s.error = error_;
    return s;
}

void FileGenJob::run() {
    SigGen gen(p_);
    const int64_t chunk = 1 << 16;
    std::vector<std::complex<float>> buf(chunk);
    std::vector<uint8_t> raw((size_t)chunk * ring_->bytesPerSample());
    const int64_t total = cfg_.pulses > 0 ? -1 : std::max<int64_t>(1, std::llround(cfg_.seconds * p_.rate));
    int64_t done = 0, pulses = 0;
    bool finished = false;

    while (!finished && !cancel_) {
        if (!rec_.active()) break;  // the recorder failed (disk full...)
        if (ring_->gateRoom() < chunk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        int64_t n = total > 0 ? std::min(chunk, total - done) : chunk;
        gen.generate(buf.data(), n);
        for (const auto& pl : gen.lastPulses()) {
            if (cfg_.pulses > 0 && pulses == cfg_.pulses) {
                // The pulse after the last one marks the end of the last PRI.
                n = pl.start - done;
                finished = true;
                break;
            }
            ++pulses;
            if (cfg_.annotatePulses)
                rec_.annotate(pl.start, pl.len, std::string(p_.mode == GenMode::Playback ? "playback" : eventName(p_)) + " " +
                                                    std::to_string(pulses));
        }
        encodeSamples(cfg_.fmt, buf.data(), n, raw.data());
        ring_->push(raw.data(), n);
        done += n;
        if (total > 0) {
            progress_ = (double)done / (double)total;
            finished = done >= total;
        } else {
            progress_ = (double)pulses / (double)cfg_.pulses;
        }
    }
    rec_.stop();  // writes what was pushed, then the final metadata
    auto rs = rec_.status();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (rs.failed) error_ = rs.stopReason;
        else if (cancel_ && !finished) error_ = "cancelled";
        LOG_INFO("siggen", "file generation %s: %lld samples, %lld pulses", error_.empty() ? "done" : error_.c_str(),
                 (long long)done, (long long)pulses);
    }
    active_ = false;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------
bool TxEngine::start(const TxConfig& cfg, std::string& err) {
    stop();
    WaveformParams p = sigGenBus().get();
    p.rate = cfg.rate;
    ParamCheck chk = checkParams(p);
    if (!chk.ok()) {
        err = chk.errors.front();
        LOG_DEBUG("tx", "rejected: %s", err.c_str());
        return false;
    }
    // Load the recording now, not in the transmit thread where it would underflow.
    if (p.mode == GenMode::Playback && !loadPlayback(p, err)) {
        LOG_DEBUG("tx", "playback file: %s", err.c_str());
        return false;
    }
    auto tx = openTx(cfg, err);
    if (!tx) {
        LOG_DEBUG("tx", "open failed: %s", err.c_str());
        return false;
    }
    // The transmitter is tuned to the generator's fc, so the generator needs no extra shift.
    auto gen = std::make_shared<BusSigGen>(tx->sampleRate(), NAN);
    if (!tx->start([gen](std::complex<float>* out, int64_t n) { gen->generate(out, n); }, err)) {
        LOG_DEBUG("tx", "start failed: %s", err.c_str());
        return false;
    }
    LOG_INFO("tx", "transmitting %s on %s at %s, %s, gain %.2f dB", describeParams(p).c_str(), tx->description().c_str(),
             fmtSI(tx->centerFreq(), "Hz", 9).c_str(), fmtSI(tx->sampleRate(), "S/s", 6).c_str(), tx->gain());
    if (!tx->notes().empty()) LOG_WARN("tx", "%s", tx->notes().c_str());
    gen_ = gen;
    tx_ = std::move(tx);
    return true;
}

void TxEngine::stop() {
    if (tx_) {
        tx_->stop();
        auto st = tx_->stats();
        const std::string fatal = tx_->fatalError();
        LOG_INFO("tx", "transmitter stopped: %lld samples sent, %lld underflows, %lld lost packets, %lld time errors%s%s",
                 (long long)st.sent, (long long)st.underflows, (long long)st.seqErrors, (long long)st.timeErrors,
                 fatal.empty() ? "" : "; error: ", fatal.c_str());
    }
    tx_.reset();
    gen_.reset();
}

bool TxEngine::setCenterFreq(double hz, std::string& err) {
    if (!tx_) return false;
    if (!tx_->setCenterFreq(hz, err)) {
        LOG_DEBUG("tx", "retune to %s failed: %s", fmtSI(hz, "Hz", 9).c_str(), err.c_str());
        return false;
    }
    LOG_INFO("tx", "transmitter tuned to %s", fmtSI(tx_->centerFreq(), "Hz", 9).c_str());
    return true;
}

bool TxEngine::setGain(double db, std::string& err) {
    if (!tx_) return false;
    if (!tx_->setGain(db, err)) {
        LOG_DEBUG("tx", "gain change failed: %s", err.c_str());
        return false;
    }
    LOG_INFO("tx", "transmit gain %.2f dB", tx_->gain());
    return true;
}

TxEngine::Stats TxEngine::stats() const {
    Stats s;
    if (!tx_) return s;
    s.running = true;
    s.description = tx_->description();
    s.notes = tx_->notes();
    s.error = tx_->fatalError();
    s.fc = tx_->centerFreq();
    s.rate = tx_->sampleRate();
    s.gain = tx_->gain();
    s.range = tx_->gainRange();
    s.tx = tx_->stats();
    return s;
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------
std::shared_ptr<SigPreviewRes> computeSigPreview(const SigPreviewReq& r, const CancelFn& cancelled) {
    auto res = std::make_shared<SigPreviewRes>();
    res->req = r;
    const double rate = r.p.rate;
    constexpr int64_t kMaxPoints = 2000;

    WaveformParams p = r.p;
    int64_t playLen = 0;
    if (p.mode == GenMode::Playback) {
        auto w = loadPlayback(p, res->error, cancelled);
        if (!w) {
            if (cancelled()) return nullptr;
            LOG_DEBUG("siggen", "preview: %s", res->error.c_str());
            return res;
        }
        playLen = (int64_t)w->s.size();
        res->playSeconds = (double)playLen / rate;
        res->fileSeconds = (double)w->fileSamples / w->fileRate;
        res->playPeakDb = 20 * std::log10(w->peak);
        res->playOutPeakDb = p.ampDb + 20 * std::log10(w->outPeak / w->peak);
    }

    // --- time domain ---
    int64_t n = 0;
    if (p.sweeping()) {
        // One period of the sweep, from the parameters: a long sweep costs nothing.
        const int64_t P = p.sweepSamples();
        const int pts = (int)std::min<int64_t>(P, kMaxPoints);
        for (int j = 0; j <= pts; ++j) {
            const int64_t k = std::min(P - 1, (int64_t)((double)j * (double)P / pts));
            res->t.push_back((double)k / rate);
            res->sweepF.push_back(p.fc + p.sweepOffsetAt(k));
            res->sweepDb.push_back(p.sweepLevelAt(k));
        }
    } else if (p.mode == GenMode::CW) {
        n = p.offset != 0 ? std::llround(10 * rate / std::fabs(p.offset)) : 1024;  // about 10 cycles
        n = std::clamp<int64_t>(n, 64, 4096);
    } else if (p.mode == GenMode::Playback) {
        // The whole recording, and the gap before it repeats.
        p.startDelay = 0;
        n = playLen + (p.playLoop ? std::llround(p.playGap * rate) : playLen / 20);
    } else if (!r.train) {
        p.startDelay = 0.2 * p.pw;  // one pulse with a little silence around it
        n = std::max<int64_t>(16, std::llround(1.4 * p.pw * rate));
    } else {
        p.startDelay = 0;
        const double span = p.burstPulses > 0 ? 2 * (p.burstPulses * p.meanPri() + p.burstGap) : 8 * p.meanPri();
        n = std::max<int64_t>(16, std::llround(span * rate));
    }
    n = std::min<int64_t>(n, p.mode == GenMode::Playback ? kMaxPlaySamples + (8 << 20) : 4 << 20);
    if (n > 0) {
        SigGen g(p);
        const int64_t per = std::max<int64_t>(1, (n + kMaxPoints - 1) / kMaxPoints);
        res->iq = per == 1;
        std::vector<cf> buf(65536);
        double peak = 0;
        int64_t inBucket = 0;
        for (int64_t done = 0; done < n;) {
            if (cancelled()) return nullptr;
            const int64_t m = std::min<int64_t>((int64_t)buf.size(), n - done);
            g.generate(buf.data(), m);
            for (int64_t k = 0; k < m; ++k) {
                const cf x = buf[(size_t)k];
                if (res->iq) {
                    res->t.push_back((double)(done + k) / rate);
                    res->i.push_back(x.real());
                    res->q.push_back(x.imag());
                    res->mag.push_back(std::abs(x));
                    continue;
                }
                // Many samples per point: keep the envelope's maximum so short pulses stay visible.
                peak = std::max(peak, (double)std::abs(x));
                if (++inBucket == per) {
                    res->t.push_back((double)(done + k + 1 - per) / rate);
                    res->mag.push_back(peak);
                    peak = 0;
                    inBucket = 0;
                }
            }
            done += m;
        }
    }

    // --- spectrum (Welch average; a sweep: max-hold of blocks spread over one period) ---
    p = r.p;
    p.startDelay = 0;
    const int nfft = 4096;
    int64_t total = 32 * nfft;
    if (p.mode == GenMode::Pulsed) total = std::max<int64_t>(total, std::llround(16 * p.meanPri() * rate));
    if (p.mode == GenMode::Playback) total = std::max(total, playLen);
    total = std::min<int64_t>(total, 2 << 20);
    const bool hold = p.sweeping();
    // Up to 1024 blocks: a sweep across the whole band then moves at most 4 bins between blocks,
    // inside the window's main lobe, so the band shows filled.
    const int segs = hold ? (int)std::clamp<int64_t>((p.sweepSamples() + nfft - 1) / nfft, 64, 1024)
                          : (int)std::max<int64_t>(1, total / nfft);
    auto fft = getFFT(nfft);
    auto win = makeWindow(WindowType::BlackmanHarris, nfft);
    double wsum = 0;
    for (float w : win) wsum += w;
    std::vector<double> acc(nfft, 0.0);
    std::vector<cf> seg(nfft);
    SigGen g(p);
    for (int s = 0; s < segs; ++s) {
        if (cancelled()) return nullptr;
        if (hold) g.seekSweep((int64_t)((double)s * (double)p.sweepSamples() / segs));
        g.generate(seg.data(), nfft);
        for (int k = 0; k < nfft; ++k) seg[k] *= win[k];
        fft->forward(seg.data());
        for (int k = 0; k < nfft; ++k) acc[k] = hold ? std::max(acc[k], (double)std::norm(seg[k])) : acc[k] + std::norm(seg[k]);
    }
    res->f.resize(nfft);
    res->db.resize(nfft);
    const double norm = 1.0 / ((hold ? 1 : segs) * wsum * wsum);  // a full-scale tone reads 0 dBFS
    for (int j = 0; j < nfft; ++j) {
        const int k = (j + nfft / 2) % nfft;
        res->f[j] = p.fc + (j - nfft / 2) * rate / nfft;
        res->db[j] = 10 * std::log10(acc[k] * norm + 1e-20);
    }
    res->rbw = rate / nfft;
    res->hold = hold;
    return res;
}
