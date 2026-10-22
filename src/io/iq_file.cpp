#include "io/iq_file.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

#include "common/json.h"
#include "common/log.h"

const char* formatName(SampleFormat f) {
    switch (f) {
        case SampleFormat::CF32: return "cf32 (complex float32)";
        case SampleFormat::CI16: return "ci16 (complex int16)";
        case SampleFormat::CI8: return "ci8 (complex int8)";
        case SampleFormat::CU8: return "cu8 (complex uint8, RTL-SDR)";
        case SampleFormat::CF64: return "cf64 (complex float64)";
        case SampleFormat::CI32: return "ci32 (complex int32)";
        default: return "?";
    }
}

int formatBytesPerSample(SampleFormat f) {
    switch (f) {
        case SampleFormat::CF32: return 8;
        case SampleFormat::CI16: return 4;
        case SampleFormat::CI8: return 2;
        case SampleFormat::CU8: return 2;
        case SampleFormat::CF64: return 16;
        case SampleFormat::CI32: return 8;
        default: return 8;
    }
}

// ---------------------------------------------------------------------------
// SIGBUS guard for the mappings (issue #41)
// ---------------------------------------------------------------------------
// A table of the open mappings that the signal handler can read without a
// lock. A slot is in use while its file is open.
struct MapGuard {
    static constexpr int kSlots = 256;
    struct Slot {
        std::atomic<uintptr_t> base{0};
        std::atomic<size_t> len{0};
        std::atomic<IqFile*> file{nullptr};
    };
    static Slot slots[kSlots];
    static struct sigaction previous;

    static void handler(int sig, siginfo_t* info, void* ctx) {
        const uintptr_t a = (uintptr_t)info->si_addr;
        for (Slot& s : slots) {
            const uintptr_t b = s.base.load(std::memory_order_acquire);
            if (!b || a < b || a >= b + s.len.load(std::memory_order_relaxed)) continue;
            // The pages from the fault to the end of the mapping become zero
            // pages; the read then goes on with zeros. mmap() is a plain
            // system call here, as in other SIGBUS handlers of this kind.
            const uintptr_t page = (uintptr_t)getpagesize();
            const uintptr_t from = a & ~(page - 1);
            const uintptr_t end = b + s.len.load(std::memory_order_relaxed);
            const size_t len = (end - from + page - 1) & ~(page - 1);
            if (mmap((void*)from, len, PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) break;
            if (IqFile* f = s.file.load(std::memory_order_acquire)) f->truncated_.store(true, std::memory_order_relaxed);
            return;
        }
        // Not one of ours: do what would have happened without this handler.
        if (previous.sa_flags & SA_SIGINFO) {
            if (previous.sa_sigaction) previous.sa_sigaction(sig, info, ctx);
            return;
        }
        if (previous.sa_handler == SIG_IGN) return;
        if (previous.sa_handler != SIG_DFL && previous.sa_handler) {
            previous.sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);  // returning re-runs the access, which now stops the program
    }

    static void install() {
        static std::once_flag once;
        std::call_once(once, [] {
            struct sigaction sa{};
            sa.sa_sigaction = handler;
            sa.sa_flags = SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGBUS, &sa, &previous);
        });
    }

    static int add(IqFile* f) {
        install();
        for (int k = 0; k < kSlots; ++k) {
            IqFile* none = nullptr;
            if (!slots[k].file.compare_exchange_strong(none, f)) continue;
            slots[k].len.store(f->size_, std::memory_order_relaxed);
            slots[k].base.store((uintptr_t)f->data_, std::memory_order_release);
            return k;
        }
        LOG_WARN("file", "more than %d files open: a file that gets shorter can stop wvt", kSlots);
        return -1;
    }

    static void remove(int k) {
        if (k < 0) return;
        slots[k].base.store(0, std::memory_order_release);
        slots[k].len.store(0, std::memory_order_relaxed);
        slots[k].file.store(nullptr, std::memory_order_release);
    }
};
MapGuard::Slot MapGuard::slots[MapGuard::kSlots];
struct sigaction MapGuard::previous{};

std::shared_ptr<IqFile> IqFile::open(const std::string& path, std::string& err) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = "cannot open '" + path + "': " + errnoText(errno);
        return nullptr;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        err = "'" + path + "' is not a regular file";
        ::close(fd);
        return nullptr;
    }
    std::shared_ptr<IqFile> f(new IqFile());
    f->path_ = path;
    f->size_ = (uint64_t)st.st_size;
    if (f->size_ > 0) {
        // MAP_SHARED: wvt never writes, so it is the same as MAP_PRIVATE, except
        // after a truncation: macOS can go on reading stale pages of a private
        // mapping until they are evicted, then fault. Shared, both systems
        // fault at once, and the guard below handles it (issue #41).
        void* p = mmap(nullptr, f->size_, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            err = "mmap failed for '" + path + "': " + errnoText(errno);
            ::close(fd);
            return nullptr;
        }
        madvise(p, f->size_, MADV_RANDOM);
        f->data_ = (const uint8_t*)p;
        f->slot_ = MapGuard::add(f.get());
    }
    ::close(fd);
    LOG_DEBUG("file", "opened %s (%llu bytes)", path.c_str(), (unsigned long long)f->size_);
    return f;
}

IqFile::~IqFile() {
    MapGuard::remove(slot_);
    if (data_) munmap((void*)data_, size_);
}

int64_t IqFile::numSamples(const FormatParams& fp) const {
    // A negative header would put sample 0 before the mapping (issue #39).
    if (fp.headerBytes < 0) return 0;
    int64_t avail = (int64_t)size_ - fp.headerBytes;
    if (avail <= 0) return 0;
    return avail / formatBytesPerSample(fp.fmt);
}

namespace {
template <typename T>
inline T loadSwapped(const uint8_t* p, bool swap) {
    T v;
    if (!swap) {
        memcpy(&v, p, sizeof(T));
    } else {
        uint8_t b[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) b[i] = p[sizeof(T) - 1 - i];
        memcpy(&v, b, sizeof(T));
    }
    return v;
}
}  // namespace

void decodeSamples(const FormatParams& fp, const uint8_t* src, int64_t count, std::complex<float>* out) {
    const int bps = formatBytesPerSample(fp.fmt);
    const int half = bps / 2;
    const bool sw = fp.bigEndian;
    for (int64_t i = 0; i < count; ++i) {
        const uint8_t* p = src + i * bps;
        float re, im;
        switch (fp.fmt) {
            case SampleFormat::CF32:
                re = loadSwapped<float>(p, sw);
                im = loadSwapped<float>(p + half, sw);
                break;
            case SampleFormat::CF64:
                re = (float)loadSwapped<double>(p, sw);
                im = (float)loadSwapped<double>(p + half, sw);
                break;
            case SampleFormat::CI16:
                re = loadSwapped<int16_t>(p, sw) * (1.f / 32768.f);
                im = loadSwapped<int16_t>(p + half, sw) * (1.f / 32768.f);
                break;
            case SampleFormat::CI32:
                re = (float)(loadSwapped<int32_t>(p, sw) * (1.0 / 2147483648.0));
                im = (float)(loadSwapped<int32_t>(p + half, sw) * (1.0 / 2147483648.0));
                break;
            case SampleFormat::CI8:
                re = (int8_t)p[0] * (1.f / 128.f);
                im = (int8_t)p[1] * (1.f / 128.f);
                break;
            case SampleFormat::CU8:
                re = (p[0] - 127.5f) * (1.f / 127.5f);
                im = (p[1] - 127.5f) * (1.f / 127.5f);
                break;
            default: re = im = 0; break;
        }
        out[i] = fp.swapIQ ? std::complex<float>(im, re) : std::complex<float>(re, im);
    }
}

void encodeSamples(SampleFormat fmt, const std::complex<float>* in, int64_t count, uint8_t* dst) {
    auto q = [](float v, float scale, float lo, float hi) { return std::clamp(std::nearbyint(v * scale), lo, hi); };
    for (int64_t i = 0; i < count; ++i) {
        const float re = in[i].real(), im = in[i].imag();
        switch (fmt) {
            case SampleFormat::CF32: {
                float v[2] = {re, im};
                memcpy(dst + i * 8, v, 8);
                break;
            }
            case SampleFormat::CF64: {
                double v[2] = {re, im};
                memcpy(dst + i * 16, v, 16);
                break;
            }
            case SampleFormat::CI16: {
                int16_t v[2] = {(int16_t)q(re, 32768.f, -32768.f, 32767.f), (int16_t)q(im, 32768.f, -32768.f, 32767.f)};
                memcpy(dst + i * 4, v, 4);
                break;
            }
            case SampleFormat::CI32: {
                int32_t v[2] = {(int32_t)std::clamp(std::nearbyint(re * 2147483648.0), -2147483648.0, 2147483647.0),
                                (int32_t)std::clamp(std::nearbyint(im * 2147483648.0), -2147483648.0, 2147483647.0)};
                memcpy(dst + i * 8, v, 8);
                break;
            }
            case SampleFormat::CI8:
                dst[i * 2] = (uint8_t)(int8_t)q(re, 128.f, -128.f, 127.f);
                dst[i * 2 + 1] = (uint8_t)(int8_t)q(im, 128.f, -128.f, 127.f);
                break;
            case SampleFormat::CU8:
                dst[i * 2] = (uint8_t)std::clamp(std::nearbyint(re * 127.5f + 127.5f), 0.f, 255.f);
                dst[i * 2 + 1] = (uint8_t)std::clamp(std::nearbyint(im * 127.5f + 127.5f), 0.f, 255.f);
                break;
            default: break;
        }
    }
}

void IqFile::read(const FormatParams& fp, int64_t start, int64_t count, std::complex<float>* out) const {
    // Samples outside [0, n) are zero-filled; the rest are decoded in one run.
    const int64_t n = numSamples(fp);
    const int64_t lo = std::max<int64_t>(start, 0), hi = std::min<int64_t>(start + count, n);
    if (hi <= lo) {
        std::fill(out, out + count, std::complex<float>(0.f, 0.f));
        return;
    }
    std::fill(out, out + (lo - start), std::complex<float>(0.f, 0.f));
    decodeSamples(fp, data_ + fp.headerBytes + lo * formatBytesPerSample(fp.fmt), hi - lo, out + (lo - start));
    std::fill(out + (hi - start), out + count, std::complex<float>(0.f, 0.f));
}

// ---------------------------------------------------------------------------
// Metadata guessing
// ---------------------------------------------------------------------------

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool parseFormatName(const std::string& in, SampleFormat& fmt, bool& bigEndian) {
    std::string s = lower(in);
    bigEndian = endsWith(s, "_be");
    if (s.rfind("cf32", 0) == 0 || s == "fc32" || s == "f32" || s == "float" || s == "cfile") fmt = SampleFormat::CF32;
    else if (s.rfind("cf64", 0) == 0 || s == "fc64" || s == "f64" || s == "double") fmt = SampleFormat::CF64;
    else if (s.rfind("ci16", 0) == 0 || s.rfind("cs16", 0) == 0 || s == "sc16" || s == "s16" || s == "i16" || s == "int16") fmt = SampleFormat::CI16;
    else if (s.rfind("ci32", 0) == 0 || s.rfind("cs32", 0) == 0 || s == "sc32" || s == "s32" || s == "i32" || s == "int32") fmt = SampleFormat::CI32;
    else if (s.rfind("ci8", 0) == 0 || s.rfind("cs8", 0) == 0 || s == "sc8" || s == "s8" || s == "i8" || s == "int8") fmt = SampleFormat::CI8;
    else if (s.rfind("cu8", 0) == 0 || s == "u8" || s == "uint8" || s == "rtl") fmt = SampleFormat::CU8;
    else return false;
    if (s == "s8" || s == "u8" || s == "i8") bigEndian = false;
    return true;
}

// "2026-10-20T13:50:22.125Z" (SigMF core:datetime, UTC) -> seconds since
// 1970; 0 when it does not parse.
double parseIsoTime(const std::string& s) {
    int y, mo, d, h, mi;
    double sec;
    // The count checks the format; a field too long for an int only gives a wrong time.
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;  // NOLINT(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
    struct tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    const time_t t = timegm(&tm);
    return t == (time_t)-1 ? 0 : (double)t + sec;
}

static FileMetaGuess guessMetaImpl(const std::string& path) {
    FileMetaGuess g;
    g.dataPath = path;
    std::string lp = lower(path);

    // SigMF
    std::string metaPath;
    if (endsWith(lp, ".sigmf-meta")) {
        metaPath = path;
        g.dataPath = path.substr(0, path.size() - 4) + "data";
    } else if (endsWith(lp, ".sigmf-data")) {
        metaPath = path.substr(0, path.size() - 4) + "meta";
    } else if (endsWith(lp, ".sigmf")) {
        // archive form not supported; fall through
    }
    if (!metaPath.empty()) {
        std::ifstream in(metaPath);
        if (!in) LOG_DEBUG("file", "cannot read SigMF metadata %s; guessing from the name", metaPath.c_str());
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            JsonValue meta;
            std::string err;
            if (parseJson(ss.str(), meta, err)) {
                // SigMF puts the datatype and the rate in "global", the frequency and the
                // time in each capture; the first capture describes sample 0. A key in the
                // other place is accepted too.
                const JsonValue* caps = meta.get("captures");
                const JsonValue* cap0 = caps && !caps->arr.empty() ? &caps->arr[0] : nullptr;
                const JsonValue* global = meta.get("global");
                auto find = [&](const char* key, JsonValue::Type type) -> const JsonValue* {
                    for (const JsonValue* o : {global, cap0}) {
                        const JsonValue* v = o ? o->get(key) : nullptr;
                        if (v && v->type == type) return v;
                    }
                    return nullptr;
                };
                using T = JsonValue::Type;
                if (const JsonValue* v = find("core:datatype", T::String)) {
                    // Only complex types: wvt has no real-sample formats (ri16_le, rf32_le, ...).
                    const bool complex = !v->str.empty() && (v->str[0] == 'c' || v->str[0] == 'C');
                    if (complex && parseFormatName(v->str, g.fmt, g.bigEndian)) g.haveFmt = true;
                    else g.badType = v->str;
                }
                if (const JsonValue* v = find("core:sample_rate", T::Number)) {
                    g.rate = v->num;
                    g.haveRate = std::isfinite(g.rate) && g.rate > 0;
                }
                if (const JsonValue* v = find("core:frequency", T::Number); v && std::isfinite(v->num)) {
                    g.fc = v->num;
                    g.haveFc = true;
                }
                if (const JsonValue* v = find("core:datetime", T::String)) g.startTime = parseIsoTime(v->str);
                g.source = "SigMF metadata";
                if (!g.haveFmt && g.badType.empty()) {
                    LOG_WARN("file", "SigMF metadata %s has no core:datatype; reading it as cf32", metaPath.c_str());
                    g.source += " (no datatype: cf32 assumed)";
                }
                return g;
            }
            LOG_WARN("file", "SigMF metadata %s is not valid JSON (%s); guessing from the name", metaPath.c_str(), err.c_str());
        }
    }

    std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);

    // gqrx: gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw
    {
        static const std::regex re(R"(gqrx_\d{8}_\d{6}_(\d+)_(\d+)_fc\.raw)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(base, m, re)) {
            g.fc = strtod(m[1].str().c_str(), nullptr);
            g.rate = strtod(m[2].str().c_str(), nullptr);
            g.haveFc = g.haveRate = true;
            g.haveFmt = true;
            g.fmt = SampleFormat::CF32;
            g.source = "gqrx filename";
            return g;
        }
    }

    // Generic hints embedded in names, e.g. "capture_2.4MSps_915MHz.cs16"
    {
        static const std::regex rateRe(R"((\d+(?:\.\d+)?)\s*([kKmMgG]?)\s*(?:sps|SPS|Sps|S/s|msps|Msps|MSps|ksps|kSps))");
        static const std::regex fcRe(R"((\d+(?:\.\d+)?)\s*([kKmMgG])[hH][zZ])");
        std::smatch m;
        auto mult = [](const std::string& p) {
            if (p == "k" || p == "K") return 1e3;
            if (p == "m" || p == "M") return 1e6;
            if (p == "g" || p == "G") return 1e9;
            return 1.0;
        };
        if (std::regex_search(base, m, rateRe)) {
            g.rate = strtod(m[1].str().c_str(), nullptr) * mult(m[2].str());
            g.haveRate = g.rate > 0;
        }
        if (std::regex_search(base, m, fcRe)) {
            g.fc = strtod(m[1].str().c_str(), nullptr) * mult(m[2].str());
            g.haveFc = true;
        }
    }

    // Extension
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = lower(base.substr(dot + 1));
        if (ext == "cu8" || ext == "u8") { g.fmt = SampleFormat::CU8; g.haveFmt = true; }
        else if (ext == "cs8" || ext == "ci8" || ext == "s8" || ext == "sc8") { g.fmt = SampleFormat::CI8; g.haveFmt = true; }
        else if (ext == "cs16" || ext == "ci16" || ext == "sc16" || ext == "s16") { g.fmt = SampleFormat::CI16; g.haveFmt = true; }
        else if (ext == "cs32" || ext == "ci32" || ext == "sc32") { g.fmt = SampleFormat::CI32; g.haveFmt = true; }
        else if (ext == "cf32" || ext == "fc32" || ext == "cfile" || ext == "raw" || ext == "iq" || ext == "bin" || ext == "dat") { g.fmt = SampleFormat::CF32; g.haveFmt = true; }
        else if (ext == "cf64" || ext == "fc64") { g.fmt = SampleFormat::CF64; g.haveFmt = true; }
        if (g.haveFmt) g.source = "file extension";
    }
    if (g.haveRate || g.haveFc) g.source = g.haveFmt ? "file name and extension" : "file name";
    return g;
}

bool metaReadable(const FileMetaGuess& g, std::string& err) {
    if (g.badType.empty()) return true;
    err = g.dataPath + ": SigMF datatype '" + g.badType +
          "' is not supported (wvt reads complex cf64, cf32, ci32, ci16, ci8 and cu8, _le or _be)";
    return false;
}

FileMetaGuess guessMeta(const std::string& path) {
    FileMetaGuess g = guessMetaImpl(path);
    char rate[32] = "?", fc[32] = "?";
    if (g.haveRate) snprintf(rate, sizeof rate, "%.9g S/s", g.rate);
    if (g.haveFc) snprintf(fc, sizeof fc, "%.12g Hz", g.fc);
    LOG_DEBUG("file", "%s: format %s, rate %s, fc %s (from %s)", path.c_str(), g.haveFmt ? formatName(g.fmt) : "?", rate, fc,
              g.source.empty() ? "nothing" : g.source.c_str());
    return g;
}
