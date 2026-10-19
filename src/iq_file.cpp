#include "iq_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

#include "log.h"

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

std::shared_ptr<IqFile> IqFile::open(const std::string& path, std::string& err) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = "cannot open '" + path + "': " + strerror(errno);
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
        void* p = mmap(nullptr, f->size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            err = "mmap failed for '" + path + "': " + strerror(errno);
            ::close(fd);
            return nullptr;
        }
        madvise(p, f->size_, MADV_RANDOM);
        f->data_ = (const uint8_t*)p;
    }
    ::close(fd);
    LOG_DEBUG("file", "opened %s (%llu bytes)", path.c_str(), (unsigned long long)f->size_);
    return f;
}

IqFile::~IqFile() {
    if (data_) munmap((void*)data_, size_);
}

int64_t IqFile::numSamples(const FormatParams& fp) const {
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

// Finds `"key"` in a JSON-ish text and returns the raw value token after ':'.
static bool jsonValue(const std::string& text, const std::string& key, std::string& out) {
    size_t p = text.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = text.find(':', p + key.size() + 2);
    if (p == std::string::npos) return false;
    ++p;
    while (p < text.size() && std::isspace((unsigned char)text[p])) ++p;
    if (p >= text.size()) return false;
    if (text[p] == '"') {
        size_t e = text.find('"', p + 1);
        if (e == std::string::npos) return false;
        out = text.substr(p + 1, e - p - 1);
    } else {
        size_t e = p;
        while (e < text.size() && (std::isdigit((unsigned char)text[e]) || strchr("+-.eE", text[e]))) ++e;
        out = text.substr(p, e - p);
    }
    return !out.empty();
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
            std::string text = ss.str(), v;
            if (jsonValue(text, "core:datatype", v) && parseFormatName(v, g.fmt, g.bigEndian)) g.haveFmt = true;
            if (jsonValue(text, "core:sample_rate", v)) { g.rate = atof(v.c_str()); g.haveRate = g.rate > 0; }
            if (jsonValue(text, "core:frequency", v)) { g.fc = atof(v.c_str()); g.haveFc = true; }
            g.source = "SigMF metadata";
            return g;
        }
    }

    std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);

    // gqrx: gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw
    {
        static const std::regex re(R"(gqrx_\d{8}_\d{6}_(\d+)_(\d+)_fc\.raw)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(base, m, re)) {
            g.fc = atof(m[1].str().c_str());
            g.rate = atof(m[2].str().c_str());
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
            g.rate = atof(m[1].str().c_str()) * mult(m[2].str());
            g.haveRate = g.rate > 0;
        }
        if (std::regex_search(base, m, fcRe)) {
            g.fc = atof(m[1].str().c_str()) * mult(m[2].str());
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

FileMetaGuess guessMeta(const std::string& path) {
    FileMetaGuess g = guessMetaImpl(path);
    char rate[32] = "?", fc[32] = "?";
    if (g.haveRate) snprintf(rate, sizeof rate, "%.9g S/s", g.rate);
    if (g.haveFc) snprintf(fc, sizeof fc, "%.12g Hz", g.fc);
    LOG_DEBUG("file", "%s: format %s, rate %s, fc %s (from %s)", path.c_str(), g.haveFmt ? formatName(g.fmt) : "?", rate, fc,
              g.source.empty() ? "nothing" : g.source.c_str());
    return g;
}
