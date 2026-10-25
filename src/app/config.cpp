#include "app/config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

#include "common/log.h"

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string expandTilde(const std::string& p) {
    if (p == "~" || p.rfind("~/", 0) == 0) {
        if (const char* home = envVar("HOME"); home && *home) return std::string(home) + p.substr(1);
    }
    return p;
}

// Edit distance, for "did you mean".
size_t distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] != b[j - 1])});
            diag = up;
        }
    }
    return row[b.size()];
}

std::string nearest(const std::string& key, const std::vector<std::string>& names) {
    std::string best;
    size_t bestD = std::max<size_t>(2, key.size() / 4) + 1;
    for (const auto& n : names)
        if (const size_t d = distance(key, n); d < bestD) {
            bestD = d;
            best = n;
        }
    return best;
}

// Joins the parts into one string.
template <class... T>
std::string cat(const T&... parts) {
    std::string s;
    (s += ... += parts);
    return s;
}

std::string fmtNum(double v) {
    char b[32];
    snprintf(b, sizeof b, "%g", v);
    return b;
}

// ---- values ------------------------------------------------------------------

enum class Kind { Folder, Name, Number, Whole, Choice, Bool, Text, Size, Bands };

struct Value {
    std::string text;  // as written (quotes removed); choices in lower case
    bool isAuto = false;
    double num = 0;
    long long whole = 0;
    int choice = 0;  // index into the choices
    bool flag = false;
    int w = 0, h = 0;
    std::vector<std::pair<double, double>> bands;
};

struct Spec {
    Kind kind;
    double lo = 0, hi = 0;          // Number, Whole
    const char* choices = nullptr;  // Choice: "a|b|c"
    bool autoOk = false;            // "auto" is a value
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string choiceList(const char* choices) {
    std::string s;
    for (const auto& c : split(choices, '|')) s += (s.empty() ? "" : ", ") + c;
    return s;
}

// Parses `text` as `spec`. False with err (the text after "key: ") on a bad value.
bool parseValue(const Spec& spec, const std::string& text, Value& v, std::string& err) {
    v.text = text;
    const std::string low = lower(text);
    if (spec.autoOk && low == "auto") {
        v.isAuto = true;
        return true;
    }
    const std::string orAuto = spec.autoOk ? " or auto" : "";
    switch (spec.kind) {
        case Kind::Folder: {
            if (text.empty()) {
                err = "wants a folder";
                return false;
            }
            if (expandTilde(text).front() != '/') {
                err = "'" + text + "' is not an absolute folder (start it with / or ~/)";
                return false;
            }
            return true;
        }
        case Kind::Name:
            if (text.empty() || text.find('/') != std::string::npos) {
                err = "wants a file name start without '/' (got '" + text + "')";
                return false;
            }
            return true;
        case Kind::Number: {
            std::string hint;
            if (!parseConfigNumber(text, v.num, hint)) {
                err = "'" + text + "' is not a number" + orAuto + (hint.empty() ? "" : "; " + hint);
                return false;
            }
            if (v.num < spec.lo || v.num > spec.hi) {
                err = "wants " + fmtNum(spec.lo) + " to " + fmtNum(spec.hi) + orAuto + " (got " + text + ")";
                return false;
            }
            return true;
        }
        case Kind::Whole: {
            char* end = nullptr;
            errno = 0;
            v.whole = strtoll(text.c_str(), &end, 10);
            if (text.empty() || *end || errno == ERANGE || v.whole < (long long)spec.lo || v.whole > (long long)spec.hi) {
                err = "wants a whole number from " + fmtNum(spec.lo) + " to " + fmtNum(spec.hi) + orAuto + " (got '" + text + "')";
                return false;
            }
            return true;
        }
        case Kind::Choice: {
            const auto cs = split(spec.choices, '|');
            for (size_t i = 0; i < cs.size(); ++i)
                if (low == cs[i]) {
                    v.choice = (int)i;
                    v.text = low;
                    return true;
                }
            err = "wants " + choiceList(spec.choices) + (spec.autoOk ? ", auto" : "") + " (got '" + text + "')";
            return false;
        }
        case Kind::Bool:
            if (low == "on" || low == "yes" || low == "true" || low == "1") v.flag = true;
            else if (low == "off" || low == "no" || low == "false" || low == "0") v.flag = false;
            else {
                err = "wants on or off (got '" + text + "')";
                return false;
            }
            return true;
        case Kind::Text:
            return true;
        case Kind::Size: {
            const auto p = split(low, 'x');
            char *e1 = nullptr, *e2 = nullptr;
            if (p.size() == 2) {
                v.w = (int)strtol(p[0].c_str(), &e1, 10);
                v.h = (int)strtol(p[1].c_str(), &e2, 10);
            }
            if (p.size() != 2 || p[0].empty() || p[1].empty() || *e1 || *e2 || v.w < spec.lo || v.w > spec.hi ||
                v.h < spec.lo || v.h > spec.hi) {
                err = "wants WxH, each from " + fmtNum(spec.lo) + " to " + fmtNum(spec.hi) + " (got '" + text + "')";
                return false;
            }
            return true;
        }
        case Kind::Bands: {
            for (const auto& part : split(text, ',')) {
                const auto lh = split(trim(part), ':');
                double lo = 0, hi = 0;
                std::string h1, h2;
                if (lh.size() != 2 || !parseConfigNumber(trim(lh[0]), lo, h1) || !parseConfigNumber(trim(lh[1]), hi, h2) ||
                    lo < 0 || hi <= lo) {
                    const std::string hint = !h1.empty() ? h1 : h2;
                    err = "wants bands LO:HI in Hz, separated by commas, e.g. 902e6:928e6 (got '" + trim(part) + "')" +
                          (hint.empty() ? "" : "; " + hint);
                    return false;
                }
                v.bands.emplace_back(lo, hi);
            }
            return true;
        }
    }
    return false;
}

// ---- the key table -----------------------------------------------------------

constexpr double kBig = 1e300;
const char* const kWindows = "rect|hann|hamming|blackman|blackman-harris|flat-top";
const char* const kWindowNames[] = {"rect", "hann", "hamming", "blackman", "blackman-harris", "flat-top"};

// A number as the file writes it: base units, scientific notation with an
// exponent that is a multiple of 3 from 1e4 up ("61.44e6", "1024", "-20").
std::string fmtValue(double v) {
    char b[48];
    const double a = std::fabs(v);
    if (a < 1e4 || !std::isfinite(v)) {
        snprintf(b, sizeof b, "%.12g", v);
        return b;
    }
    const int e = (int)std::floor(std::log10(a) / 3) * 3;
    snprintf(b, sizeof b, "%.12ge%d", v / std::pow(10.0, e), e);
    return b;
}

std::string onOff(bool b) { return b ? "on" : "off"; }

struct Key {
    const char* name;
    Spec spec;
    const char* help;  // the comment after the value in --write-config
    std::function<void(WvtConfig&, const Value&)> set;
    // The value as text, or "" when the key is not set (then --write-config
    // writes "# name = example" and --show-config writes `meaning`).
    std::function<std::string(const WvtConfig&)> get;
    const char* example = nullptr;
    const char* meaning = nullptr;
    const char* section = nullptr;  // a heading before this key in --write-config
};

const std::vector<Key>& keys() {
    static const std::vector<Key> k = {
        // folders
        {"recording.folder", {Kind::Folder}, "", [](WvtConfig& c, const Value& v) { c.recordingFolder = v.text; },
         [](const WvtConfig& c) { return c.recordingFolder; }, nullptr, nullptr, "folders"},
        {"recording.name", {Kind::Name}, "file names: <name>_<date>_<time>_<fc>_<rate>",
         [](WvtConfig& c, const Value& v) { c.recordingName = v.text; }, [](const WvtConfig& c) { return c.recordingName; }},
        {"snapshot.folder", {Kind::Folder}, "", [](WvtConfig& c, const Value& v) { c.snapshotFolder = v.text; },
         [](const WvtConfig& c) { return c.snapshotFolder; }},
        {"siggen.folder", {Kind::Folder}, "", [](WvtConfig& c, const Value& v) { c.siggenFolder = v.text; },
         [](const WvtConfig& c) { return c.siggenFolder; }},
        {"siggen.name", {Kind::Name}, "", [](WvtConfig& c, const Value& v) { c.siggenName = v.text; },
         [](const WvtConfig& c) { return c.siggenName; }},
        {"audio.folder", {Kind::Folder}, "", [](WvtConfig& c, const Value& v) { c.audioFolder = v.text; },
         [](const WvtConfig& c) { return c.audioFolder; }},
        // file formats
        {"recording.format", {Kind::Choice, 0, 0, "sigmf|raw"}, "sigmf | raw",
         [](WvtConfig& c, const Value& v) { c.recordingSigmf = v.choice == 0; },
         [](const WvtConfig& c) { return std::string(c.recordingSigmf ? "sigmf" : "raw"); }, nullptr, nullptr, "file formats"},
        {"recording.sample", {Kind::Choice, 0, 0, "native|cf32"}, "native (cu8 RTL-SDR, ci16 USRP/BB60/Sidekiq) | cf32",
         [](WvtConfig& c, const Value& v) { c.recordingCf32 = v.choice == 1; },
         [](const WvtConfig& c) { return std::string(c.recordingCf32 ? "cf32" : "native"); }},
        {"recording.max_seconds", {Kind::Number, 0, kBig}, "0 = no limit",
         [](WvtConfig& c, const Value& v) { c.recordingMaxSeconds = v.num; },
         [](const WvtConfig& c) { return fmtValue(c.recordingMaxSeconds); }},
        {"siggen.format", {Kind::Choice, 0, 0, "sigmf|raw"}, "sigmf | raw",
         [](WvtConfig& c, const Value& v) { c.siggenSigmf = v.choice == 0; },
         [](const WvtConfig& c) { return std::string(c.siggenSigmf ? "sigmf" : "raw"); }},
        {"siggen.sample", {Kind::Choice, 0, 0, "cf32|ci16|ci8|cu8"}, "cf32 | ci16 | ci8 | cu8",
         [](WvtConfig& c, const Value& v) {
             const SampleFormat f[] = {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CI8, SampleFormat::CU8};
             c.siggenSample = f[v.choice];
         },
         [](const WvtConfig& c) {
             return std::string(c.siggenSample == SampleFormat::CI16 ? "ci16"
                                : c.siggenSample == SampleFormat::CI8 ? "ci8"
                                : c.siggenSample == SampleFormat::CU8 ? "cu8"
                                                                       : "cf32");
         }},
        {"export.band_sample", {Kind::Choice, 0, 0, "cf32|ci16"}, "region export with a band filter: cf32 | ci16",
         [](WvtConfig& c, const Value& v) { c.exportBandCi16 = v.choice == 1; },
         [](const WvtConfig& c) { return std::string(c.exportBandCi16 ? "ci16" : "cf32"); }},
        // file waterfall
        {"fft.size", {Kind::Whole, 64, 65536}, "power of two, 64 to 65536", [](WvtConfig& c, const Value& v) { c.fftSize = (int)v.whole; },
         [](const WvtConfig& c) { return std::to_string(c.fftSize); }, nullptr, nullptr, "file waterfall FFT"},
        {"fft.window", {Kind::Choice, 0, 0, kWindows}, "rect | hann | hamming | blackman | blackman-harris | flat-top",
         [](WvtConfig& c, const Value& v) { c.fftWindow = (WindowType)v.choice; },
         [](const WvtConfig& c) { return std::string(kWindowNames[(int)c.fftWindow]); }},
        {"fft.overlap_pct", {Kind::Number, 0, 95}, "0 to 95", [](WvtConfig& c, const Value& v) { c.fftOverlapPct = v.num; },
         [](const WvtConfig& c) { return fmtValue(c.fftOverlapPct); }},
        {"display.colormap", {Kind::Text}, "an ImPlot colormap: viridis | plasma | hot | cool | jet | twilight | greys ...",
         [](WvtConfig& c, const Value& v) { c.colormap = lower(v.text); }, [](const WvtConfig& c) { return c.colormap; }},
        {"display.db_min", {Kind::Number, -400, 400, nullptr, true}, "", [](WvtConfig& c, const Value& v) { c.dbMin = v.isAuto ? NAN : v.num; },
         [](const WvtConfig& c) { return std::isnan(c.dbMin) ? std::string() : fmtValue(c.dbMin); }, "auto", "auto"},
        {"display.db_max", {Kind::Number, -400, 400, nullptr, true}, "", [](WvtConfig& c, const Value& v) { c.dbMax = v.isAuto ? NAN : v.num; },
         [](const WvtConfig& c) { return std::isnan(c.dbMax) ? std::string() : fmtValue(c.dbMax); }, "auto", "auto"},
        // live spectrum
        {"live.fft_size", {Kind::Whole, 64, 65536}, "power of two, 64 to 65536",
         [](WvtConfig& c, const Value& v) { c.liveFftSize = (int)v.whole; }, [](const WvtConfig& c) { return std::to_string(c.liveFftSize); },
         nullptr, nullptr, "live spectrum"},
        {"live.window", {Kind::Choice, 0, 0, kWindows}, "", [](WvtConfig& c, const Value& v) { c.liveWindow = (WindowType)v.choice; },
         [](const WvtConfig& c) { return std::string(kWindowNames[(int)c.liveWindow]); }},
        {"live.avg", {Kind::Whole, 1, 64, nullptr, true}, "auto (all new samples) | 1 to 64",
         [](WvtConfig& c, const Value& v) { c.liveAvg = v.isAuto ? 0 : (int)v.whole; },
         [](const WvtConfig& c) { return c.liveAvg > 0 ? std::to_string(c.liveAvg) : std::string("auto"); }},
        {"live.rows_per_sec", {Kind::Number, 1, 100}, "1 to 100", [](WvtConfig& c, const Value& v) { c.liveRowsPerSec = v.num; },
         [](const WvtConfig& c) { return fmtValue(c.liveRowsPerSec); }},
        {"live.max_columns", {Kind::Whole, 64, 8192}, "64 to 8192; more bins are max-pooled",
         [](WvtConfig& c, const Value& v) { c.liveMaxColumns = (int)v.whole; },
         [](const WvtConfig& c) { return std::to_string(c.liveMaxColumns); }},
        {"live.history_rows", {Kind::Whole, 64, 4096}, "64 to 4096", [](WvtConfig& c, const Value& v) { c.liveHistoryRows = (int)v.whole; },
         [](const WvtConfig& c) { return std::to_string(c.liveHistoryRows); }},
        // general
        {"gpu", {Kind::Choice, 0, 0, "auto|on|off"}, "auto | on | off   (env WVT_GPU)", [](WvtConfig& c, const Value& v) { c.gpu = v.text; },
         [](const WvtConfig& c) { return c.gpu; }, nullptr, nullptr, "general"},
        {"gpu.device", {Kind::Whole, 0, 63}, "", [](WvtConfig& c, const Value& v) { c.gpuDevice = (int)v.whole; },
         [](const WvtConfig& c) { return c.gpuDevice < 0 ? std::string() : std::to_string(c.gpuDevice); }, "0", "the best GPU"},
        {"ui.scale", {Kind::Number, 0.5, 4}, "0.5 to 4         (env WVT_SCALE)", [](WvtConfig& c, const Value& v) { c.uiScale = v.num; },
         [](const WvtConfig& c) { return fmtValue(c.uiScale); }},
        {"ui.geometry", {Kind::Size, 100, 16384}, "window size WxH",
         [](WvtConfig& c, const Value& v) {
             c.uiWidth = v.w;
             c.uiHeight = v.h;
         },
         [](const WvtConfig& c) { return std::to_string(c.uiWidth) + "x" + std::to_string(c.uiHeight); }},
        {"log.folder", {Kind::Folder}, "(env WVT_LOG_DIR)", [](WvtConfig& c, const Value& v) { c.logFolder = v.text; },
         [](const WvtConfig& c) { return c.logFolder; }, "~/wvt/logs/", "$WVT_LOG_DIR, then $TMPDIR, then /tmp"},
        // transmit limits
        {"tx.max_gain_db", {Kind::Number, -100, 200}, "", [](WvtConfig& c, const Value& v) { c.txMaxGainDb = v.num; },
         [](const WvtConfig& c) { return std::isnan(c.txMaxGainDb) ? std::string() : fmtValue(c.txMaxGainDb); }, "60", "no limit",
         "transmit limits (only stricter than the environment and the options)"},
        {"tx.allow", {Kind::Bands}, "", [](WvtConfig& c, const Value& v) { c.txAllow = v.bands; },
         [](const WvtConfig& c) {
             if (c.txNone) return std::string("(no frequency: the allowed bands do not overlap)");
             std::string s;
             for (const auto& [lo, hi] : c.txAllow) s += (s.empty() ? "" : ", ") + fmtValue(lo) + ":" + fmtValue(hi);
             return s;
         },
         "902e6:928e6, 2.4e9:2.5e9", "any frequency"},
        // SDRs
        {"sdr.default", {Kind::Text}, "", [](WvtConfig& c, const Value& v) { c.sdrDefault = lower(v.text); },
         [](const WvtConfig& c) { return c.sdrDefault; }, "b205", "the first device found", "SDRs"},
    };
    return k;
}

// sdr.N.<field>. drivers: the drivers the field applies to ("uhd,skiq"), or
// nullptr for all.
struct ProfileKey {
    const char* field;
    Spec spec;
    const char* drivers;
    std::function<void(SdrProfile&, const Value&)> set;
    std::function<std::string(const SdrProfile&)> get;  // "" = not set
    const char* help = "";
};

template <class T, class F>
std::string opt(const std::optional<T>& o, F f) {
    return o ? f(*o) : std::string();
}

const std::vector<ProfileKey>& profileKeys() {
    static const std::vector<ProfileKey> k = {
        {"type", {Kind::Text}, nullptr, [](SdrProfile& p, const Value& v) { p.type = lower(v.text); },
         [](const SdrProfile& p) { return p.type; }},
        {"serial", {Kind::Text}, nullptr, [](SdrProfile& p, const Value& v) { p.serial = v.text; },
         [](const SdrProfile& p) { return p.serial; }},
        {"args", {Kind::Text}, nullptr, [](SdrProfile& p, const Value& v) { p.args = v.text; },
         [](const SdrProfile& p) { return p.args; }},
        {"fc", {Kind::Number, 0, 1e12}, nullptr, [](SdrProfile& p, const Value& v) { p.fc = v.num; },
         [](const SdrProfile& p) { return opt(p.fc, fmtValue); }},
        {"rate", {Kind::Number, 1, 1e10}, nullptr, [](SdrProfile& p, const Value& v) { p.rate = v.num; },
         [](const SdrProfile& p) { return opt(p.rate, fmtValue); }},
        {"gain_db", {Kind::Number, -100, 200}, "uhd,skiq,rtlsdr,sim", [](SdrProfile& p, const Value& v) { p.gainDb = v.num; },
         [](const SdrProfile& p) { return opt(p.gainDb, fmtValue); }},
        {"ref_level_dbm", {Kind::Number, -130, 20}, "bb", [](SdrProfile& p, const Value& v) { p.refLevelDbm = v.num; },
         [](const SdrProfile& p) { return opt(p.refLevelDbm, fmtValue); }},
        {"agc", {Kind::Bool}, "uhd,skiq,rtlsdr,sim", [](SdrProfile& p, const Value& v) { p.agc = v.flag; },
         [](const SdrProfile& p) { return opt(p.agc, onOff); }},
        {"port", {Kind::Text}, "uhd,skiq", [](SdrProfile& p, const Value& v) { p.port = v.text; },
         [](const SdrProfile& p) { return p.port.value_or(""); }},
        {"bandwidth", {Kind::Number, 1, 1e10, nullptr, true}, "uhd,bb,skiq",
         [](SdrProfile& p, const Value& v) { p.bandwidth = v.isAuto ? 0 : v.num; },
         [](const SdrProfile& p) { return opt(p.bandwidth, [](double b) { return b > 0 ? fmtValue(b) : std::string("auto"); }); }},
        {"ref", {Kind::Choice, 0, 0, "internal|external|gpsdo"}, "uhd", [](SdrProfile& p, const Value& v) { p.ref = v.text; },
         [](const SdrProfile& p) { return p.ref.value_or(""); }},
        {"channel", {Kind::Whole, 0, 63}, "uhd", [](SdrProfile& p, const Value& v) { p.channel = (int)v.whole; },
         [](const SdrProfile& p) { return opt(p.channel, [](int c) { return std::to_string(c); }); }},
        {"atten", {Kind::Choice, 0, 0, "0|10|20|30", true}, "bb", [](SdrProfile& p, const Value& v) { p.atten = v.isAuto ? -1 : v.choice; },
         [](const SdrProfile& p) { return opt(p.atten, [](int a) { return a < 0 ? std::string("auto") : std::to_string(a * 10); }); }},
        {"amp", {Kind::Whole, 0, 3, nullptr, true}, "bb", [](SdrProfile& p, const Value& v) { p.amp = v.isAuto ? -1 : (int)v.whole; },
         [](const SdrProfile& p) { return opt(p.amp, [](int a) { return a < 0 ? std::string("auto") : std::to_string(a); }); }},
        {"ext_ref", {Kind::Bool}, "bb,skiq", [](SdrProfile& p, const Value& v) { p.extRef = v.flag; },
         [](const SdrProfile& p) { return opt(p.extRef, onOff); }},
        {"direct_sampling", {Kind::Choice, 0, 0, "off|i|q"}, "rtlsdr", [](SdrProfile& p, const Value& v) { p.directSampling = v.choice; },
         [](const SdrProfile& p) {
             return opt(p.directSampling, [](int d) { return std::string(d == 1 ? "i" : d == 2 ? "q" : "off"); });
         }},
        {"bias_tee", {Kind::Bool}, "rtlsdr", [](SdrProfile& p, const Value& v) { p.biasTee = v.flag; },
         [](const SdrProfile& p) { return opt(p.biasTee, onOff); }},
        {"ppm", {Kind::Whole, -1000, 1000}, "rtlsdr", [](SdrProfile& p, const Value& v) { p.ppm = (int)v.whole; },
         [](const SdrProfile& p) { return opt(p.ppm, [](int n) { return std::to_string(n); }); }},
    };
    return k;
}

bool appliesTo(const char* drivers, const std::string& driver) {
    if (!drivers) return true;
    for (const auto& d : split(drivers, ','))
        if (d == driver) return true;
    return false;
}

// ---- lines -------------------------------------------------------------------

// Splits one line into key and value. False with err when the line is not
// "key = value"; an empty or comment-only line gives an empty key.
bool splitLine(const std::string& line, std::string& key, std::string& value, std::string& err) {
    key.clear();
    value.clear();
    // "#" starts a comment at the line start or after a space or tab, outside quotes.
    auto commentAt = [&](size_t i) { return line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'); };
    size_t eq = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (commentAt(i)) {
            if (!trim(line.substr(0, i)).empty()) {
                err = "expected key = value";
                return false;
            }
            return true;  // comment line
        }
        if (line[i] == '=') {
            eq = i;
            break;
        }
    }
    if (eq == std::string::npos) {
        if (trim(line).empty()) return true;
        err = "expected key = value";
        return false;
    }
    key = lower(trim(line.substr(0, eq)));
    if (key.empty()) {
        err = "a value without a key";
        return false;
    }
    size_t i = eq + 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i < line.size() && line[i] == '"') {
        // A quoted value: \" and \\ are escapes; after the closing quote only a comment.
        for (++i; i < line.size() && line[i] != '"'; ++i) {
            if (line[i] == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) ++i;
            value += line[i];
        }
        if (i >= line.size()) {
            err = "the quote is not closed";
            return false;
        }
        const std::string rest = trim(line.substr(i + 1));
        if (!rest.empty() && rest[0] != '#') {
            err = "text after the closing quote";
            return false;
        }
        return true;
    }
    size_t end = line.size();
    for (size_t j = i; j < line.size(); ++j)
        if (commentAt(j)) {
            end = j;
            break;
        }
    value = trim(line.substr(i, end - i));
    return true;
}

// "sdr.12.rate" -> 12, "rate". False when the key is not of that form.
bool profileKey(const std::string& key, int& index, std::string& field) {
    if (key.rfind("sdr.", 0) != 0) return false;
    const size_t dot = key.find('.', 4);
    if (dot == std::string::npos || dot == 4 || dot - 4 > 2) return false;
    for (size_t i = 4; i < dot; ++i)
        if (!std::isdigit((unsigned char)key[i])) return false;
    index = (int)std::strtol(key.substr(4, dot - 4).c_str(), nullptr, 10);  // 1 or 2 digits, checked above
    field = key.substr(dot + 1);
    return true;
}

SdrProfile& profileFor(WvtConfig& cfg, int index, int line) {
    auto it = std::lower_bound(cfg.sdrs.begin(), cfg.sdrs.end(), index,
                               [](const SdrProfile& p, int i) { return p.index < i; });
    if (it == cfg.sdrs.end() || it->index != index) {
        SdrProfile p;
        p.index = index;
        p.line = line;
        it = cfg.sdrs.insert(it, p);
    }
    return *it;
}

bool isPow2(long long n) { return n > 0 && (n & (n - 1)) == 0; }

}  // namespace

const std::vector<std::pair<const char*, const char*>>& configSdrTypes() {
    static const std::vector<std::pair<const char*, const char*>> t = {
        {"b200", "uhd"},   {"b205", "uhd"},   {"b210", "uhd"},     {"x310", "uhd"}, {"n310", "uhd"},
        {"x410", "uhd"},   {"usrp", "uhd"},   {"bb60", "bb"},      {"mpcie", "skiq"}, {"m2", "skiq"},
        {"m2-2280", "skiq"}, {"z3u", "skiq"}, {"x2", "skiq"},      {"x4", "skiq"},  {"nv100", "skiq"},
        {"sidekiq", "skiq"}, {"rtlsdr", "rtlsdr"}, {"sim", "sim"},
    };
    return t;
}

const char* configSdrDriver(const std::string& type) {
    const std::string t = lower(type);
    for (const auto& [name, driver] : configSdrTypes())
        if (t == name) return driver;
    return nullptr;
}

bool parseConfigNumber(const std::string& s, double& out, std::string& hint) {
    hint.clear();
    const std::string t = trim(s);
    char* end = nullptr;
    errno = 0;
    out = t.empty() ? 0 : strtod(t.c_str(), &end);
    if (!t.empty() && end && *end == 0 && errno != ERANGE && std::isfinite(out)) {
        // strtod also takes hex floats, "inf" and "nan"; the file takes decimal only.
        if (t.find_first_of("xXiInN") == std::string::npos) return true;
    }
    // Explain the SI suffix form: "61.44M" or "61.44 MHz" -> "use 61.44e6".
    if (end && end != t.c_str()) {
        std::string rest = trim(end);
        const char* units[] = {"hz", "s/s", "sps", "dbm", "db", "s"};
        int exp = 0;
        bool known = true;
        if (!rest.empty()) {
            switch (rest[0]) {
                case 'k': exp = 3; break;
                case 'M': exp = 6; break;
                case 'G': exp = 9; break;
                case 'T': exp = 12; break;
                case 'm': exp = -3; break;
                case 'u': exp = -6; break;
                case 'n': exp = -9; break;
                default: known = false;
            }
            if (known) rest = rest.substr(1);
        }
        bool unit = rest.empty();
        for (const char* u : units) unit = unit || lower(rest) == u;
        if (unit) {
            const std::string mant = trim(t.substr(0, (size_t)(end - t.c_str())));
            hint = "use " + mant + (exp ? "e" + std::to_string(exp) : "") + " (base units, no suffix)";
        }
    }
    return false;
}

std::vector<std::string> configKeyNames() {
    std::vector<std::string> n;
    for (const auto& k : keys()) n.push_back(k.name);
    for (const auto& k : profileKeys()) n.push_back(std::string("sdr.N.") + k.field);
    return n;
}

void parseConfig(const std::string& text, const std::string& name, WvtConfig& cfg, std::vector<std::string>& warnings) {
    std::map<std::string, int> seen;  // key -> line
    const size_t first = warnings.size();  // the messages of this text start here
    std::vector<std::string> plainNames;
    for (const auto& k : keys()) plainNames.push_back(k.name);
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    auto warn = [&](int ln, const std::string& msg) { warnings.push_back(name + ":" + std::to_string(ln) + ": " + msg); };
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string key, value, err;
        if (!splitLine(line, key, value, err)) {
            warn(lineNo, err + ": '" + trim(line) + "'");
            continue;
        }
        if (key.empty()) continue;

        int index = -1;
        std::string field;
        const Key* plain = nullptr;
        const ProfileKey* prof = nullptr;
        if (profileKey(key, index, field)) {
            for (const auto& k : profileKeys())
                if (field == k.field) prof = &k;
            if (!prof) {
                std::vector<std::string> fields;
                for (const auto& k : profileKeys()) fields.push_back(std::string("sdr.") + std::to_string(index) + "." + k.field);
                const std::string near = nearest(key, fields);
                warn(lineNo, "unknown key '" + key + "'" + (near.empty() ? "" : " (did you mean '" + near + "'?)"));
                continue;
            }
        } else {
            for (const auto& k : keys())
                if (key == k.name) plain = &k;
            if (!plain) {
                std::vector<std::string> all = plainNames;
                if (key.rfind("sdr.", 0) == 0)
                    for (const auto& k : profileKeys()) all.push_back(std::string("sdr.0.") + k.field);
                const std::string near = nearest(key, all);
                warn(lineNo, "unknown key '" + key + "'" + (near.empty() ? "" : " (did you mean '" + near + "'?)"));
                continue;
            }
        }

        Value v;
        if (!parseValue(plain ? plain->spec : prof->spec, value, v, err)) {
            warn(lineNo, cat(key, ": ", err, "; the line is not used"));
            continue;
        }
        if ((key == "fft.size" || key == "live.fft_size") && !isPow2(v.whole)) {
            warn(lineNo, cat(key, ": wants a power of two (got ", value, "); the line is not used"));
            continue;
        }
        if (prof && field == "type" && !configSdrDriver(v.text)) {
            std::string types;
            for (const auto& t : configSdrTypes()) types += (types.empty() ? "" : ", ") + std::string(t.first);
            warn(lineNo, cat(key, ": unknown type '", value, "' (types: ", types, "); the line is not used"));
            continue;
        }
        if (auto it = seen.find(key); it != seen.end())
            warn(lineNo, key + " is set again; this line replaces line " + std::to_string(it->second));
        seen[key] = lineNo;
        cfg.origin[key] = name + ":" + std::to_string(lineNo);
        if (plain) plain->set(cfg, v);
        else prof->set(profileFor(cfg, index, lineNo), v);
    }

    // Checks across lines: the profiles and sdr.default. Their messages go
    // with the others in line order.
    std::vector<std::pair<int, std::string>> late;
    auto warnLate = [&](int ln, const std::string& msg) { late.emplace_back(ln, msg); };
    for (const SdrProfile& p : cfg.sdrs) {
        const std::string pre = "sdr." + std::to_string(p.index) + ".";
        if (p.type.empty()) {
            warnLate(p.line, "sdr." + std::to_string(p.index) + " has no " + pre + "type; the profile is not used");
            continue;
        }
        const std::string driver = configSdrDriver(p.type);
        for (const auto& k : profileKeys()) {
            const auto it = seen.find(pre + k.field);
            if (it != seen.end() && !appliesTo(k.drivers, driver))
                warnLate(it->second, pre + k.field + " does not apply to a " + p.type + "; it is not used");
        }
    }
    if (!std::isnan(cfg.dbMin) && !std::isnan(cfg.dbMax) && cfg.dbMin >= cfg.dbMax) {
        warnLate(seen["display.db_max"], "display.db_min (" + fmtNum(cfg.dbMin) + ") is not below display.db_max (" +
                                             fmtNum(cfg.dbMax) + "); the automatic range is used");
        cfg.dbMin = cfg.dbMax = NAN;
    }
    if (!cfg.sdrDefault.empty()) {
        const int ln = seen.count("sdr.default") ? seen["sdr.default"] : 0;
        const std::string& d = cfg.sdrDefault;
        if (std::all_of(d.begin(), d.end(), [](unsigned char c) { return std::isdigit(c); })) {
            const int i = (int)std::strtol(d.c_str(), nullptr, 10);  // digits only
            const bool found = std::any_of(cfg.sdrs.begin(), cfg.sdrs.end(), [&](const SdrProfile& p) { return p.index == i; });
            if (!found) warnLate(ln, "sdr.default: there is no profile sdr." + d + ".*");
        } else if (!configSdrDriver(d)) {
            warnLate(ln, "sdr.default: '" + d + "' is not a profile index or an SDR type");
        }
    }
    // Merge into the per-line messages, which are already in line order.
    auto lineOf = [&](const std::string& w) { return (int)std::strtol(w.c_str() + name.size() + 1, nullptr, 10); };
    std::stable_sort(late.begin(), late.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [ln, msg] : late) {
        const std::string text = cat(name, ":", std::to_string(ln), ": ", msg);
        auto pos = std::upper_bound(warnings.begin() + (std::ptrdiff_t)first, warnings.end(), ln,
                                    [&](int l, const std::string& w) { return l < lineOf(w); });
        warnings.insert(pos, text);
    }
}

bool loadConfigFile(const std::string& path, WvtConfig& cfg, std::vector<std::string>& warnings, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot read the configuration file " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string name = path;
    if (const size_t s = name.rfind('/'); s != std::string::npos) name = name.substr(s + 1);
    parseConfig(ss.str(), name, cfg, warnings);
    return true;
}

std::string configPath(const std::string& option, bool& required) {
    required = true;
    if (!option.empty()) return expandTilde(option);
    if (const char* e = envVar("WVT_CONFIG"); e && *e) return expandTilde(e);
    required = false;
    if (const char* x = envVar("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/wvt/wvt.conf";
    return expandTilde("~/.config/wvt/wvt.conf");
}

bool configTakeArgs(std::vector<char*>& args, std::string& path, bool& noConfig, std::string& err) {
    std::vector<char*> keep;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string a = args[i] ? args[i] : "";
        if (i > 0 && a == "--no-config") {
            noConfig = true;
        } else if (i > 0 && a == "--config") {
            if (i + 1 >= args.size() || !args[i + 1]) {
                err = "--config wants a file";
                return false;
            }
            path = args[++i];
        } else if (i > 0 && a.rfind("--config=", 0) == 0) {
            path = a.substr(9);
            if (path.empty()) {
                err = "--config wants a file";
                return false;
            }
        } else {
            keep.push_back(args[i]);
        }
    }
    args = std::move(keep);
    return true;
}

std::string configJoin(const std::string& folder, const std::string& name) {
    if (folder.empty()) return name;
    return folder.back() == '/' ? folder + name : cat(folder, "/", name);
}

namespace {
// "M.2-2280" -> "m2-2280", "B205mini" -> "b205mini"
std::string modelKey(const std::string& product) {
    std::string k;
    for (unsigned char c : product)
        if (c != '.' && c != ' ') k += (char)std::tolower(c);
    return k;
}

// Each comma-separated item of `want` ("addr=1.2.3.4", "#0") is an item of `have`.
bool argsContain(const std::string& have, const std::string& want) {
    const auto items = split(have, ',');
    for (const auto& w : split(want, ',')) {
        const std::string t = trim(w);
        if (!t.empty() && std::find(items.begin(), items.end(), t) == items.end()) return false;
    }
    return true;
}

bool sameText(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
}  // namespace

bool configTypeFits(const std::string& type, const DeviceInfo& d) {
    const char* driver = configSdrDriver(type);
    if (!driver || d.driver != driver) return false;
    if (d.product.empty() && d.args.empty()) return false;  // "none detected"
    const std::string t = lower(type);
    if (t == "usrp" || t == "bb60" || t == "sidekiq" || t == "rtlsdr" || t == "sim") return true;
    const std::string m = modelKey(d.product);
    if (d.driver == "skiq") return m == t;
    return m.rfind(t, 0) == 0;  // USRP: "b205" fits "b205mini"
}

const SdrProfile* configProfileFor(const WvtConfig& cfg, const DeviceInfo& d) {
    for (const SdrProfile& p : cfg.sdrs) {
        if (p.type.empty() || !configTypeFits(p.type, d)) continue;
        if (!p.serial.empty() && !sameText(p.serial, d.serial) && !argsContain(d.args, "serial=" + p.serial) &&
            !sameText(p.serial, d.args))
            continue;
        if (!p.args.empty() && !argsContain(d.args, p.args)) continue;
        return &p;
    }
    return nullptr;
}

bool configSameDevice(const DeviceInfo& listed, const std::string& driver, const std::string& args) {
    if (listed.driver != driver || (listed.product.empty() && listed.args.empty())) return false;
    return args.empty() || listed.args == args || argsContain(listed.args, args) ||
           (!listed.serial.empty() && (sameText(args, listed.serial) || sameText(args, "serial=" + listed.serial)));
}

int configDefaultDevice(const WvtConfig& cfg, const std::vector<DeviceInfo>& devices) {
    const std::string& want = cfg.sdrDefault;
    if (want.empty()) return -1;
    const bool index = std::all_of(want.begin(), want.end(), [](unsigned char c) { return std::isdigit(c); });
    const SdrProfile* prof = nullptr;
    if (index) {
        const int n = (int)std::strtol(want.c_str(), nullptr, 10);
        for (const SdrProfile& p : cfg.sdrs)
            if (p.index == n && !p.type.empty()) prof = &p;
        if (!prof) return -1;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        if (index ? configProfileFor(cfg, devices[i]) == prof : configTypeFits(want, devices[i])) return (int)i;
    }
    return -1;
}

void configApplyProfile(const SdrProfile& p, StreamConfig& c, const std::vector<std::string>& keep) {
    // Not a field that the options set, and one that applies to this driver
    // (the parse warned about the others).
    auto use = [&](const char* field) {
        if (std::find(keep.begin(), keep.end(), field) != keep.end()) return false;
        for (const auto& k : profileKeys())
            if (std::string(k.field) == field) return appliesTo(k.drivers, c.driver);
        return true;
    };
    if (p.fc && use("fc")) c.fc = *p.fc;
    if (p.rate && use("rate")) c.rate = *p.rate;
    if (p.gainDb && use("gain_db") && use("gain")) c.gain = *p.gainDb;
    if (p.refLevelDbm && use("ref_level_dbm") && use("gain")) c.gain = *p.refLevelDbm;
    if (p.agc && use("agc") && use("gain")) c.agc = *p.agc;
    if (p.port && use("port")) c.antenna = *p.port;
    if (p.bandwidth && use("bandwidth")) c.bandwidth = *p.bandwidth;
    if (p.ref && use("ref")) c.ref = *p.ref;
    if (p.channel && use("channel")) c.channel = *p.channel;
    if (p.atten && use("atten")) c.bbAtten = *p.atten;
    if (p.amp && use("amp")) c.bbAmp = *p.amp;
    if (p.extRef && use("ext_ref")) c.extRef = *p.extRef;
    if (p.biasTee && use("bias_tee")) c.biasTee = *p.biasTee;
    if (p.ppm && use("ppm")) c.ppm = *p.ppm;
    if (p.directSampling && use("direct_sampling")) c.directSampling = *p.directSampling;
}

namespace {
// A value as the file writes it: in quotes when it has "#", a quote, a
// backslash or spaces at an end.
std::string quoted(const std::string& v) {
    const bool need = v.find_first_of("#\"\\") != std::string::npos ||
                      (!v.empty() && (std::isspace((unsigned char)v.front()) || std::isspace((unsigned char)v.back())));
    if (!need) return v;
    std::string q = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q + "\"";
}

// "name = value", the value from column 21, then "# comment" from column 42.
std::string fileLine(const std::string& name, const std::string& value, const std::string& comment, bool commented) {
    std::string s = (commented ? "# " : "") + name;
    s.resize(std::max<size_t>(s.size() + 1, 20), ' ');
    s += "= " + value;
    if (!comment.empty()) {
        s.resize(std::max<size_t>(s.size() + 1, 41), ' ');
        s += "# " + comment;
    }
    return s + "\n";
}

std::string sectionLine(const std::string& title) {
    std::string s = "\n# ---- " + title + " ";
    s.resize(std::max<size_t>(s.size(), 80), '-');
    return s + "\n";
}
}  // namespace

std::string configFileText(const WvtConfig& cfg) {
    std::string out =
        "# wvt configuration file (issue #88)\n"
        "# One \"key = value\" per line. \"#\" starts a comment (at the line start or after a space).\n"
        "# Numbers are in base units (Hz, S/s, s, dB), in scientific notation: 61.44e6 = 61.44 MS/s.\n"
        "# \"~\" is the home folder.\n"
        "# Order: built-in default < this file < environment variable < command-line option.\n";
    for (const Key& k : keys()) {
        if (k.section) out += sectionLine(k.section);
        if (std::string(k.name) == "sdr.default")
            out += "# sdr.default: a type (the first profile and the first device of that type)\n"
                   "# or a profile index. Not set: the first device that the scan finds.\n";
        const std::string v = k.get(cfg);
        out += v.empty() ? fileLine(k.name, k.example ? k.example : "", k.help, true) : fileLine(k.name, quoted(v), k.help, false);
    }
    std::string types;
    for (const auto& t : configSdrTypes()) types += (types.empty() ? "" : " ") + std::string(t.first);
    out += "\n# Profiles: sdr.N.<field>, N from 0 to 99. A device uses the first profile whose type\n"
           "# fits it and whose serial and args, where set, match it.\n# Types: " + types + ".\n";
    for (const SdrProfile& p : cfg.sdrs) {
        out += "\n# Profile " + std::to_string(p.index) + "\n";
        for (const ProfileKey& f : profileKeys())
            if (const std::string v = f.get(p); !v.empty())
                out += fileLine("sdr." + std::to_string(p.index) + "." + f.field, quoted(v), "", false);
    }
    if (cfg.sdrs.empty())
        out += "\n# Profile 0, an example:\n" +
               fileLine("sdr.0.type", "b205", "", true) + fileLine("sdr.0.serial", "31DDAD7", "only this unit", true) +
               fileLine("sdr.0.fc", "100e6", "", true) + fileLine("sdr.0.rate", "61.44e6", "", true) +
               fileLine("sdr.0.gain_db", "0", "BB60: sdr.0.ref_level_dbm", true) + fileLine("sdr.0.agc", "off", "", true) +
               fileLine("sdr.0.port", "TX/RX", "USRP antenna or Sidekiq RF port", true) +
               fileLine("sdr.0.bandwidth", "auto", "", true) + fileLine("sdr.0.ref", "internal", "USRP: internal | external | gpsdo", true) +
               fileLine("sdr.0.channel", "0", "USRP", true) +
               "# Also: atten, amp (BB60), ext_ref (BB60, Sidekiq), direct_sampling, bias_tee, ppm (RTL-SDR).\n";
    return out;
}

std::string configShowText(const WvtConfig& cfg, const std::map<std::string, std::string>& sources) {
    auto from = [&](const std::string& key) {
        if (auto it = sources.find(key); it != sources.end()) return it->second;
        if (auto it = cfg.origin.find(key); it != cfg.origin.end()) return it->second;
        return std::string("default");
    };
    auto line = [](const std::string& key, const std::string& value, const std::string& src) {
        std::string s = key;
        s.resize(std::max<size_t>(s.size(), 22), ' ');
        s += "= " + value;
        s.resize(std::max<size_t>(s.size() + 2, 56), ' ');
        return s + src + "\n";
    };
    std::string out;
    for (const Key& k : keys()) {
        const std::string v = k.get(cfg);
        out += line(k.name, v.empty() ? std::string("(") + (k.meaning ? k.meaning : "not set") + ")" : v, from(k.name));
    }
    for (const SdrProfile& p : cfg.sdrs)
        for (const ProfileKey& f : profileKeys()) {
            const std::string key = "sdr." + std::to_string(p.index) + "." + f.field;
            if (const std::string v = f.get(p); !v.empty()) out += line(key, v, from(key));
        }
    return out;
}
