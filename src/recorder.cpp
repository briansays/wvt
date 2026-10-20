#include "recorder.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "log.h"

namespace {

double wallNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ISO 8601 UTC with milliseconds, as SigMF core:datetime wants.
std::string isoTime(double t) {
    time_t s = (time_t)std::floor(t);
    int ms = std::min(999, (int)((t - (double)s) * 1000));
    struct tm tm{};
    gmtime_r(&s, &tm);
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string num(double v) {
    char b[40];
    snprintf(b, sizeof b, "%.17g", v);
    return b;
}

// "%.6f" without trailing zeros, keeping at least `keep` decimals.
std::string trimmed(double v, int keep) {
    char b[48];
    snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    size_t dot = s.find('.');
    while (s.size() > dot + 1 + keep && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

bool mkdirs(const std::string& dir, std::string& err) {
    if (dir.empty()) return true;
    std::string cur;
    size_t i = 0;
    while (i <= dir.size()) {
        size_t j = dir.find('/', i);
        if (j == std::string::npos) j = dir.size();
        cur = dir.substr(0, j);
        if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            err = "cannot create directory '" + cur + "': " + strerror(errno);
            return false;
        }
        i = j + 1;
    }
    return true;
}

bool writeAll(int fd, const void* data, size_t n) {
    const char* p = (const char*)data;
    while (n > 0) {
        ssize_t r = ::write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

const char* sigmfType(SampleFormat f) {
    switch (f) {
        case SampleFormat::CU8: return "cu8";
        case SampleFormat::CI8: return "ci8";
        case SampleFormat::CI16: return "ci16_le";
        case SampleFormat::CI32: return "ci32_le";
        case SampleFormat::CF64: return "cf64_le";
        default: return "cf32_le";
    }
}

const char* rawExt(SampleFormat f) {
    switch (f) {
        case SampleFormat::CU8: return ".cu8";
        case SampleFormat::CI8: return ".cs8";
        case SampleFormat::CI16: return ".cs16";
        case SampleFormat::CI32: return ".cs32";
        case SampleFormat::CF64: return ".cf64";
        default: return ".cf32";
    }
}

}  // namespace

const char* rawFileExt(SampleFormat f) { return rawExt(f); }

bool makeDirs(const std::string& dir, std::string& err) { return mkdirs(dir, err); }

std::string expandHome(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char* home = getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

bool Recorder::start(std::shared_ptr<SampleRing> ring, const RecordSource& src, const RecordConfig& cfg,
                     std::string& err) {
    stop();
    ring_ = std::move(ring);
    src_ = src;
    cfg_ = cfg;
    const SampleFormat outFmt = cfg.cf32 ? SampleFormat::CF32 : src.fmt;
    outBps_ = formatBytesPerSample(outFmt);

    // <prefix>_YYYYMMDD_HHMMSS_<fc>MHz_<rate>MSps  (the name wvt's guessMeta parses)
    startWall_ = wallNow();
    time_t now = (time_t)startWall_;
    struct tm tm{};
    gmtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
    std::string rate = src.rate >= 1e6 ? trimmed(src.rate / 1e6, 0) + "MSps" : trimmed(src.rate / 1e3, 0) + "kSps";
    std::string prefix = expandHome(cfg.prefix.empty() ? "wvt" : cfg.prefix);
    std::string base = cfg.exactPath.empty() ? prefix + "_" + stamp + "_" + trimmed(src.fc / 1e6, 3) + "MHz_" + rate
                                             : expandHome(cfg.exactPath);
    size_t sl = base.find_last_of('/');
    if (sl != std::string::npos && !mkdirs(base.substr(0, sl), err)) {
        LOG_DEBUG("record", "start failed: %s", err.c_str());
        return false;
    }

    std::string dataPath = cfg.sigmf ? base + ".sigmf-data" : base + rawExt(outFmt);
    fd_ = ::open(dataPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd_ < 0) {
        err = "cannot create '" + dataPath + "': " + strerror(errno);
        LOG_DEBUG("record", "start failed: %s", err.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        dataPath_ = dataPath;
        metaPath_ = cfg.sigmf ? base + ".sigmf-meta" : "";
        stopReason_.clear();
        failed_ = false;
        captures_ = {{0, src.fc, cfg.startTime < 0 ? "" : isoTime(cfg.startTime > 0 ? cfg.startTime : startWall_)}};
        annotations_.clear();
        extraAnnotations_ = 0;
        metaDirty_ = false;
        samples_ = dropped_ = 0;
        bytes_ = 0;
    }
    if (cfg.sigmf && !writeMeta(err)) {
        LOG_DEBUG("record", "start failed: %s", err.c_str());
        ::close(fd_);
        fd_ = -1;
        unlink(dataPath.c_str());
        return false;
    }

    int64_t lim = 0;
    if (cfg.maxSeconds > 0) lim = (int64_t)std::llround(cfg.maxSeconds * src.rate);
    if (cfg.maxBytes > 0) {
        int64_t b = (int64_t)(cfg.maxBytes / outBps_);
        lim = lim > 0 ? std::min(lim, b) : b;
    }
    maxSamples_ = lim;
    LOG_INFO("record", "record start: %s, %s%s%s, %s", cfg.sigmf ? "SigMF" : "raw", formatName(outFmt),
             cfg.maxSeconds > 0 ? (", stop after " + trimmed(cfg.maxSeconds, 3) + " s").c_str() : "",
             cfg.maxBytes > 0 ? (", stop after " + trimmed(cfg.maxBytes, 0) + " B").c_str() : "", dataPath.c_str());

    start_ = pos_ = ring_->written();
    gapCursor_ = ring_->gapCount();
    ring_->openGate(start_);
    stopReq_ = false;
    active_ = true;
    th_ = std::thread([this] { run(); });
    return true;
}

void Recorder::stop() {
    if (th_.joinable()) {
        stopAt_ = ring_->written();
        stopReq_ = true;
        th_.join();
    }
}

void Recorder::noteRetune(double fc) {
    if (!active_ || !cfg_.sigmf) return;
    std::lock_guard<std::mutex> lk(m_);
    // Approximate: the retune takes effect a few USB buffers after this point.
    int64_t at = std::max<int64_t>(0, ring_->written() - start_);
    if (!captures_.empty() && captures_.back().start >= at) captures_.pop_back();
    captures_.push_back({at, fc, isoTime(wallNow())});
    metaDirty_ = true;
    LOG_DEBUG("record", "retune to %.0f Hz at sample %lld: new capture segment", fc, (long long)at);
}

void Recorder::annotate(int64_t at, int64_t count, const std::string& label) {
    if (!active_ || !cfg_.sigmf) return;
    std::lock_guard<std::mutex> lk(m_);
    if (at < start_ || ++extraAnnotations_ > kMaxAnnotations) return;
    annotations_.push_back({at - start_, "", count, label});
    metaDirty_ = true;
}

Recorder::Status Recorder::status() const {
    std::lock_guard<std::mutex> lk(m_);
    Status s;
    s.active = active_;
    s.dataPath = dataPath_;
    s.metaPath = metaPath_;
    s.stopReason = stopReason_;
    s.failed = failed_;
    s.samples = samples_;
    s.bytes = bytes_;
    s.seconds = src_.rate > 0 ? samples_ / src_.rate : 0;
    s.dropped = dropped_;
    s.gaps = (int)annotations_.size() - std::min(extraAnnotations_, kMaxAnnotations);
    if (active_ && ring_) s.ringFill = (float)(ring_->written() - (start_ + samples_)) / (float)ring_->capacity();
    return s;
}

void Recorder::handleGaps(bool all) {
    auto gaps = ring_->takeGaps(gapCursor_, all);
    if (gaps.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& g : gaps) {
        if (g.at < start_) continue;
        int64_t at = g.at - start_;
        if (maxSamples_ > 0 && at >= maxSamples_) continue;
        std::string c = g.lost >= 0 ? std::to_string(g.lost) + " samples dropped: " + g.why
                                    : "samples dropped (count unknown): " + g.why;
        annotations_.push_back({at, c, 0, {}});
        if (++gapsLogged_ <= kMaxGapLogs)
            LOG_DEBUG("record", "gap at sample %lld: %s%s", (long long)at, c.c_str(),
                      gapsLogged_ == kMaxGapLogs ? " (further gaps are not logged one by one)" : "");
        if (g.lost > 0) dropped_ += g.lost;
        metaDirty_ = true;
    }
}

void Recorder::run() {
    using clock = std::chrono::steady_clock;
    const int bps = ring_->bytesPerSample();
    const int64_t chunk = std::max<int64_t>(4096, (4 << 20) / bps);
    std::vector<uint8_t> raw((size_t)chunk * bps);
    std::vector<std::complex<float>> fl(cfg_.cf32 ? chunk : 0);
    FormatParams fp;
    fp.fmt = src_.fmt;
    auto lastMeta = clock::now();
    std::string reason, err;
    bool failed = false, metaWarned = false;
    float maxFill = 0;
    gapsLogged_ = 0;

    for (;;) {
        const bool stopping = stopReq_;
        int64_t end = ring_->written();
        if (stopping) end = std::min(end, stopAt_.load());
        if (maxSamples_ > 0) end = std::min(end, start_ + maxSamples_);
        const int64_t n = std::min(end - pos_, chunk);
        if (n <= 0) {
            if (maxSamples_ > 0 && pos_ >= start_ + maxSamples_) {
                reason = "limit reached";
                break;
            }
            if (stopping) {
                reason = "stopped";
                break;
            }
            handleGaps(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
            if (!ring_->read(pos_, n, raw.data())) {
                reason = "internal error: samples overwritten before they were written to disk";
                failed = true;
                break;
            }
            bool ok;
            if (cfg_.cf32) {
                decodeSamples(fp, raw.data(), n, fl.data());
                ok = writeAll(fd_, fl.data(), (size_t)n * sizeof(std::complex<float>));
            } else {
                ok = writeAll(fd_, raw.data(), (size_t)n * bps);
            }
            if (!ok) {
                reason = std::string("write failed: ") + strerror(errno);
                failed = true;
                break;
            }
            maxFill = std::max(maxFill, (float)(ring_->written() - pos_) / (float)ring_->capacity());
            pos_ += n;
            ring_->advanceGate(pos_);
            {
                std::lock_guard<std::mutex> lk(m_);
                samples_ = pos_ - start_;
                bytes_ = (uint64_t)samples_ * outBps_;
            }
            handleGaps(false);
        }
        bool dirty;
        {
            std::lock_guard<std::mutex> lk(m_);
            dirty = metaDirty_;
        }
        if (cfg_.sigmf && dirty && clock::now() - lastMeta > std::chrono::seconds(2)) {
            if (!writeMeta(err) && !metaWarned) {
                LOG_WARN("record", "metadata update failed (retrying): %s", err.c_str());
                metaWarned = true;
            }
            lastMeta = clock::now();
        }
    }

    ring_->closeGate();
    handleGaps(true);
    if (::close(fd_) != 0 && !failed) {
        reason = std::string("close failed: ") + strerror(errno);
        failed = true;
    }
    fd_ = -1;
    if (cfg_.sigmf && !writeMeta(err) && !failed) {
        reason = err;
        failed = true;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        stopReason_ = reason;
        failed_ = failed;
        const double secs = src_.rate > 0 ? samples_ / src_.rate : 0;
        const int gaps = (int)annotations_.size() - std::min(extraAnnotations_, kMaxAnnotations);
        LOG_INFO("record", "recording %s%s: %s, %.3f s, %llu bytes, %d gaps, %lld samples lost", failed ? "failed: " : "",
                 reason.c_str(), dataPath_.c_str(), secs, (unsigned long long)bytes_, gaps, (long long)dropped_);
    }
    LOG_DEBUG("record", "writer lag high-water mark: %.1f%% of the ring", maxFill * 100);
    active_ = false;
}

bool Recorder::writeMeta(std::string& err) {
    std::string j, metaPath;
    {
        std::lock_guard<std::mutex> lk(m_);
        metaPath = metaPath_;
        j += "{\n  \"global\": {\n";
        j += "    \"core:datatype\": \"" + std::string(sigmfType(cfg_.cf32 ? SampleFormat::CF32 : src_.fmt)) + "\",\n";
        j += "    \"core:sample_rate\": " + num(src_.rate) + ",\n";
        j += "    \"core:version\": \"1.0.0\",\n";
        j += "    \"core:recorder\": \"wvt\",\n";
        if (!src_.hw.empty()) j += "    \"core:hw\": \"" + jsonEscape(src_.hw) + "\",\n";
        if (!cfg_.description.empty()) j += "    \"core:description\": \"" + jsonEscape(cfg_.description) + "\",\n";
        if (!cfg_.extraGlobal.empty()) j += "    " + cfg_.extraGlobal + ",\n";
        j += "    \"core:num_channels\": 1\n  },\n  \"captures\": [";
        for (size_t i = 0; i < captures_.size(); ++i) {
            const auto& c = captures_[i];
            j += std::string(i ? "," : "") + "\n    {\"core:sample_start\": " + std::to_string(c.start) +
                 ", \"core:frequency\": " + num(c.fc);
            if (!c.datetime.empty()) j += ", \"core:datetime\": \"" + c.datetime + "\"";
            j += "}";
        }
        j += "\n  ],\n  \"annotations\": [";
        // SigMF wants annotations ordered by sample_start.
        std::stable_sort(annotations_.begin(), annotations_.end(),
                         [](const Annotation& a, const Annotation& b) { return a.start < b.start; });
        for (size_t i = 0; i < annotations_.size(); ++i) {
            const auto& a = annotations_[i];
            j += std::string(i ? "," : "") + "\n    {\"core:sample_start\": " + std::to_string(a.start);
            if (a.count > 0) j += ", \"core:sample_count\": " + std::to_string(a.count);
            if (!a.label.empty()) j += ", \"core:label\": \"" + jsonEscape(a.label) + "\"";
            if (!a.comment.empty()) j += ", \"core:comment\": \"" + jsonEscape(a.comment) + "\"";
            j += "}";
        }
        j += annotations_.empty() ? "]\n}\n" : "\n  ]\n}\n";
        metaDirty_ = false;
    }
    // Write to a temporary file and rename, so the metadata is never half-written.
    std::string tmp = metaPath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot write '" + tmp + "': " + strerror(errno);
        return false;
    }
    bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp.c_str(), metaPath.c_str()) != 0) {
        err = "cannot write '" + metaPath + "': " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}
