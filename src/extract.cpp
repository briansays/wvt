#include "extract.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <regex>

#include "gpu.h"
#include "log.h"
#include "ui_util.h"

namespace {
// Each output chunk reads about this many input samples (plus the filter length).
constexpr int64_t kChunkInput = 4 << 20;

std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// "x.sigmf-data", "x.cs16", ... -> "x"
std::string stripExt(const std::string& path) {
    static const char* exts[] = {".sigmf-data", ".sigmf-meta", ".sigmf", ".cf32", ".fc32", ".cf64", ".fc64", ".cs16",
                                 ".ci16",       ".sc16",       ".cs8",   ".ci8",  ".sc8",  ".cu8",  ".u8",   ".cs32",
                                 ".ci32",       ".sc32",       ".raw",   ".iq",   ".bin",  ".dat",  ".cfile"};
    const std::string lp = lower(path);
    for (const char* e : exts) {
        const size_t n = strlen(e);
        if (lp.size() > n && lp.compare(lp.size() - n, n, e) == 0) return path.substr(0, path.size() - n);
    }
    return path;
}

std::string num(double v, const char* f = "%.17g") {
    char b[48];
    snprintf(b, sizeof b, f, v);
    return b;
}

std::string jsonText(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        if ((unsigned char)ch < 0x20) {
            o += ' ';
            continue;
        }
        o += ch;
    }
    return o + "\"";
}

bool sameFile(const std::string& a, const std::string& b) {
    struct stat sa, sb;
    return stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

bool exists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0;
}

// Free bytes in the folder of `path`, or in the nearest folder above it that exists.
double freeSpace(const std::string& path) {
    std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
    if (dir.empty()) dir = path.find('/') == 0 ? "/" : ".";
    struct statvfs v;
    while (statvfs(dir.c_str(), &v) != 0) {
        const size_t sl = dir.find_last_of('/');
        if (sl == std::string::npos || dir == "/") return -1;
        dir = sl == 0 ? "/" : dir.substr(0, sl);
    }
    return (double)v.f_bavail * (double)v.f_frsize;
}

// "%.6g" without a trailing "." or zeros.
std::string shortNum(double v) {
    std::string s = num(v, "%.6f");
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}
}  // namespace

std::string defaultExtractPath(const std::string& sourcePath, double t0, double fc, double rate) {
    const size_t sl = sourcePath.find_last_of('/');
    const std::string dir = sl == std::string::npos ? "." : sourcePath.substr(0, sl);
    std::string name = stripExt(sl == std::string::npos ? sourcePath : sourcePath.substr(sl + 1));
    // guessMeta takes the first rate and frequency in a name, so drop the source's.
    static const std::regex rateRe(R"(_?\d+(?:\.\d+)?\s*[kKmMgG]?\s*(?:sps|SPS|Sps|S/s|msps|Msps|MSps|ksps|kSps))");
    static const std::regex fcRe(R"(_?\d+(?:\.\d+)?\s*[kKmMgG][hH][zZ])");
    name = std::regex_replace(std::regex_replace(name, rateRe, ""), fcRe, "");
    if (name.empty()) name = "extract";
    const std::string r = rate >= 1e6 ? shortNum(rate / 1e6) + "MSps" : shortNum(rate / 1e3) + "kSps";
    return dir + "/" + name + "_" + shortNum(t0) + "s_" + shortNum(fc / 1e6) + "MHz_" + r;
}

ExtractPlan planExtract(const ExtractConfig& c) {
    ExtractPlan p;
    const Source& s = c.src;
    if (!s.file) {
        p.err = "no file is open";
        return p;
    }
    const double fs = s.fs;
    if (!(fs > 0)) {
        p.err = "the sample rate is not set";
        return p;
    }
    const int64_t n = s.file->numSamples(s.fp);
    const int64_t lim0 = std::clamp<int64_t>(c.limit0, 0, n), lim1 = c.limit1 < 0 ? n : std::clamp<int64_t>(c.limit1, lim0, n);
    if (c.s1 <= c.s0) {
        p.err = "the end must be after the start";
        return p;
    }
    if (c.s0 < lim0 || c.s1 > lim1) {
        const bool seg = lim0 > 0 || lim1 < n;
        p.err = "the range must be inside the " + std::string(seg ? "loaded segment" : "file") + " (" +
                fmtSI(lim0 / fs, "s", 6) + " to " + fmtSI(lim1 / fs, "s", 6) + ")";
        return p;
    }
    if (c.fullBand) {
        p.bw = fs;
        p.center = s.fc;
        p.rate = fs;
        p.fmt = s.fp.fmt;
        p.samples = c.s1 - c.s0;
    } else {
        const double lo = std::min(c.fLo, c.fHi), hi = std::max(c.fLo, c.fHi);
        p.bw = hi - lo;
        const double tol = fs * 1e-9;
        if (!(p.bw > 0)) {
            p.err = "the band needs a width";
            return p;
        }
        if (lo < s.fc - fs / 2 - tol || hi > s.fc + fs / 2 + tol) {
            p.err = "the band must be inside the capture (" + fmtSI(s.fc - fs / 2, "Hz", 9) + " to " +
                    fmtSI(s.fc + fs / 2, "Hz", 9) + ")";
            return p;
        }
        if (p.bw > 0.8 * fs) {
            p.err = "the band is wider than 80 % of the sample rate; use Full band";
            return p;
        }
        if (p.bw < fs / 1e5) {
            p.err = "the band must be at least " + fmtSI(fs / 1e5, "Hz", 4) + " wide (fs / 100 000)";
            return p;
        }
        // The band is the passband; 25 % of it on each side is the transition band.
        // fs / D >= 1.25 BW keeps the aliased transition band out of the passband.
        const double transition = 0.25 * p.bw;
        p.decim = std::max(1, (int)std::floor(fs / (1.25 * p.bw)));
        p.taps = (int)std::ceil(5.5 * fs / transition) | 1;  // Blackman: transition ~ 5.5 fs / N
        p.cutoff = (0.5 * p.bw + 0.5 * transition) / fs;
        p.center = 0.5 * (lo + hi);
        p.offset = p.center - s.fc;
        p.rate = fs / p.decim;
        p.fmt = c.ci16 ? SampleFormat::CI16 : SampleFormat::CF32;
        p.samples = (c.s1 - c.s0 + p.decim - 1) / p.decim;
    }
    p.bytes = (double)p.samples * formatBytesPerSample(p.fmt);

    p.base = stripExt(expandHome(c.path));
    if (p.base.empty() || p.base.back() == '/') {
        p.err = "choose an output file name";
        return p;
    }
    p.dataPath = c.sigmf ? p.base + ".sigmf-data" : p.base + rawFileExt(p.fmt);
    p.metaPath = c.sigmf ? p.base + ".sigmf-meta" : "";
    if (sameFile(p.dataPath, s.file->path())) {
        p.err = "the output would replace the source file";
        return p;
    }
    p.exists = exists(p.dataPath) || exists(p.metaPath);
    p.freeBytes = freeSpace(p.base);
    if (p.freeBytes >= 0 && p.bytes + 1e6 > p.freeBytes) {
        p.err = "not enough disk space: the export needs " + fmtSI(p.bytes, "B", 3) + ", " + fmtSI(p.freeBytes, "B", 3) +
                " is free";
        return p;
    }
    return p;
}

bool ExtractJob::start(const ExtractConfig& c, std::string& err) {
    cancel();
    cfg_ = c;
    plan_ = planExtract(c);
    if (!plan_.err.empty()) {
        err = plan_.err;
        return false;
    }
    if (plan_.exists) {
        if (!c.overwrite) {
            err = plan_.dataPath + " exists";
            return false;
        }
        unlink(plan_.dataPath.c_str());
        if (!plan_.metaPath.empty()) unlink(plan_.metaPath.c_str());
    }
    const Source& s = c.src;
    const int bps = formatBytesPerSample(plan_.fmt);
    ring_ = std::make_shared<SampleRing>((32 << 20) / bps, bps);

    RecordSource rs;
    rs.fmt = plan_.fmt;
    rs.rate = plan_.rate;
    rs.fc = plan_.center;
    rs.hw = "wvt extract";
    RecordConfig rc;
    rc.sigmf = c.sigmf;
    rc.exactPath = plan_.base;
    rc.startTime = c.sourceStart > 0 ? c.sourceStart + c.s0 / s.fs : -1;
    const std::string name = s.file->path().substr(s.file->path().find_last_of('/') + 1);
    std::string band = c.fullBand ? "full band"
                                  : fmtSI(std::min(c.fLo, c.fHi), "Hz", 9) + " to " + fmtSI(std::max(c.fLo, c.fHi), "Hz", 9);
    rc.description = "Extract of " + name + ": " + fmtSI(c.s0 / s.fs, "s", 9) + " to " + fmtSI(c.s1 / s.fs, "s", 9) + ", " + band;
    rc.extraGlobal = "\"wvt:extract\": {\"source\": " + jsonText(s.file->path()) + ", \"sample_start\": " + std::to_string(c.s0) +
                     ", \"sample_count\": " + std::to_string(c.s1 - c.s0) + ", \"source_sample_rate\": " + num(s.fs) +
                     ", \"source_frequency\": " + num(s.fc);
    if (!c.fullBand)
        rc.extraGlobal += ", \"band\": [" + num(std::min(c.fLo, c.fHi)) + ", " + num(std::max(c.fLo, c.fHi)) +
                          "], \"decimation\": " + std::to_string(plan_.decim) + ", \"taps\": " + std::to_string(plan_.taps) +
                          ", \"cutoff\": " + num(plan_.cutoff) + ", \"filter\": \"Blackman windowed sinc\"";
    rc.extraGlobal += "}";
    if (!rec_.start(ring_, rs, rc, err)) return false;

    LOG_INFO("extract", "export %s, %s, %s -> %s (%s, %s at %s, %s%s)", name.c_str(),
             (fmtSI(c.s0 / s.fs, "s", 9) + " to " + fmtSI(c.s1 / s.fs, "s", 9)).c_str(), band.c_str(), plan_.dataPath.c_str(),
             formatName(plan_.fmt), fmtSI(plan_.rate, "S/s", 6).c_str(), fmtSI(plan_.center, "Hz", 9).c_str(),
             fmtSI(plan_.bytes, "B", 4).c_str(),
             c.fullBand ? ", copy" : (", decimation " + std::to_string(plan_.decim) + ", " + std::to_string(plan_.taps) + " taps").c_str());
    {
        std::lock_guard<std::mutex> lk(m_);
        error_.clear();
        done_ = cancelled_ = false;
        seconds_ = 0;
        recStatus_ = Recorder::Status{};
    }
    cancel_ = false;
    progress_ = 0;
    clipped_ = 0;
    gpu_ = false;
    active_ = true;
    th_ = std::thread([this] { run(); });
    return true;
}

void ExtractJob::cancel() {
    if (!th_.joinable()) return;
    cancel_ = true;
    th_.join();
}

ExtractJob::Status ExtractJob::status() const {
    Status st;
    st.active = active_.load();
    st.progress = progress_.load();
    st.clipped = clipped_.load();
    st.gpu = gpu_.load();
    st.dataPath = plan_.dataPath;
    st.metaPath = plan_.metaPath;
    std::lock_guard<std::mutex> lk(m_);
    st.done = done_;
    st.cancelled = cancelled_;
    st.error = error_;
    st.seconds = seconds_;
    st.samples = recStatus_.samples;
    st.bytes = recStatus_.bytes;
    return st;
}

// Pushes n samples into the ring, waiting while the writer catches up.
bool ExtractJob::pushAll(const uint8_t* raw, int64_t n) {
    const int bps = ring_->bytesPerSample();
    const int64_t piece = std::max<int64_t>(1, ring_->capacity() / 4);
    while (n > 0) {
        if (cancel_ || !rec_.active()) return false;
        const int64_t m = std::min(n, piece);
        if (ring_->gateRoom() < m) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ring_->push(raw, m);
        raw += m * bps;
        n -= m;
    }
    return true;
}

void ExtractJob::run() {
    const auto t0 = std::chrono::steady_clock::now();
    const ExtractConfig& c = cfg_;
    const Source& s = c.src;
    const int bps = formatBytesPerSample(plan_.fmt);
    bool ok = true;
    if (c.fullBand) {
        // Copy the bytes. Big-endian or swapped I/Q sources are put in the standard
        // order (byte and I/Q swaps only, so nothing is lost).
        const uint8_t* base = s.file->data() + s.fp.headerBytes;
        const bool fix = s.fp.bigEndian || s.fp.swapIQ;
        const int half = bps / 2;
        std::vector<uint8_t> tmp;
        const int64_t block = 1 << 20;
        for (int64_t pos = c.s0; ok && pos < c.s1; pos += block) {
            const int64_t m = std::min(block, c.s1 - pos);
            const uint8_t* src = base + pos * bps;
            if (fix) {
                tmp.assign(src, src + m * bps);
                for (int64_t k = 0; k < m; ++k) {
                    uint8_t* v = &tmp[(size_t)k * bps];
                    if (s.fp.bigEndian && half > 1) {
                        std::reverse(v, v + half);
                        std::reverse(v + half, v + bps);
                    }
                    if (s.fp.swapIQ) std::swap_ranges(v, v + half, v + half);
                }
                src = tmp.data();
            }
            ok = pushAll(src, m);
            progress_ = (double)(pos + m - c.s0) / (double)(c.s1 - c.s0);
        }
    } else {
        const auto h = designLowpass(plan_.taps, plan_.cutoff);
        ChanJob job;
        job.file = s.file.get();
        job.fp = s.fp;
        job.decim = plan_.decim;
        job.frac = plan_.offset / s.fs;
        job.h = h.data();
        job.taps = (int)h.size();
        const int64_t chunk = std::max<int64_t>(1, kChunkInput / plan_.decim);
        std::vector<cf> y((size_t)std::min(chunk, plan_.samples));
        std::vector<uint8_t> raw(y.size() * bps);
        CancelFn cancelled = [this] { return cancel_.load(); };
        for (int64_t j0 = 0; ok && j0 < plan_.samples; j0 += chunk) {
            const int64_t m = std::min(chunk, plan_.samples - j0);
            // Output j is centered on input sample s0 + j * D.
            job.inStart = c.s0 + j0 * plan_.decim - job.taps / 2;
            job.outLen = m;
            GpuStatus st = gpuChannelUsable() ? gpuChannelize(job, reinterpret_cast<float*>(y.data()), cancelled)
                                              : GpuStatus::Unavailable;
            if (st == GpuStatus::Ok) gpu_ = true;
            else if (st == GpuStatus::Cancelled || !channelizeCpu(job, y.data(), cancelled)) ok = false;
            if (!ok) break;
            if (plan_.fmt == SampleFormat::CI16) {
                int64_t clip = 0;
                for (int64_t k = 0; k < m; ++k)
                    clip += (std::fabs(y[k].real()) * 32768.f > 32767.5f) + (std::fabs(y[k].imag()) * 32768.f > 32767.5f);
                clipped_ += clip;
            }
            encodeSamples(plan_.fmt, y.data(), m, raw.data());
            ok = pushAll(raw.data(), m);
            progress_ = (double)(j0 + m) / (double)plan_.samples;
        }
    }
    rec_.stop();
    const Recorder::Status rs = rec_.status();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lk(m_);
    recStatus_ = rs;
    seconds_ = secs;
    if (cancel_) {
        unlink(plan_.dataPath.c_str());
        if (!plan_.metaPath.empty()) unlink(plan_.metaPath.c_str());
        cancelled_ = true;
        LOG_INFO("extract", "export cancelled; %s deleted", plan_.dataPath.c_str());
    } else if (rs.failed || !ok) {
        error_ = rs.stopReason.empty() ? "the export stopped" : rs.stopReason;
        LOG_DEBUG("extract", "export failed: %s", error_.c_str());  // the caller shows and logs it
    } else {
        done_ = true;
        LOG_INFO("extract", "export done: %s, %lld samples, %s in %.2f s%s", plan_.dataPath.c_str(), (long long)rs.samples,
                 fmtSI((double)rs.bytes, "B", 4).c_str(), secs, gpu_ ? " (filter on the GPU)" : "");
        if (clipped_ > 0)
            LOG_WARN("extract", "%lld ci16 values were clipped; use cf32 to keep them", (long long)clipped_.load());
    }
    active_ = false;
}
