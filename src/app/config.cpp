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

struct Key {
    const char* name;
    Spec spec;
    std::function<void(WvtConfig&, const Value&)> set;
};

const std::vector<Key>& keys() {
    static const std::vector<Key> k = {
        // folders
        {"recording.folder", {Kind::Folder}, [](WvtConfig& c, const Value& v) { c.recordingFolder = v.text; }},
        {"recording.name", {Kind::Name}, [](WvtConfig& c, const Value& v) { c.recordingName = v.text; }},
        {"snapshot.folder", {Kind::Folder}, [](WvtConfig& c, const Value& v) { c.snapshotFolder = v.text; }},
        {"siggen.folder", {Kind::Folder}, [](WvtConfig& c, const Value& v) { c.siggenFolder = v.text; }},
        {"siggen.name", {Kind::Name}, [](WvtConfig& c, const Value& v) { c.siggenName = v.text; }},
        {"audio.folder", {Kind::Folder}, [](WvtConfig& c, const Value& v) { c.audioFolder = v.text; }},
        // file formats
        {"recording.format", {Kind::Choice, 0, 0, "sigmf|raw"}, [](WvtConfig& c, const Value& v) { c.recordingSigmf = v.choice == 0; }},
        {"recording.sample", {Kind::Choice, 0, 0, "native|cf32"}, [](WvtConfig& c, const Value& v) { c.recordingCf32 = v.choice == 1; }},
        {"recording.max_seconds", {Kind::Number, 0, kBig}, [](WvtConfig& c, const Value& v) { c.recordingMaxSeconds = v.num; }},
        {"siggen.format", {Kind::Choice, 0, 0, "sigmf|raw"}, [](WvtConfig& c, const Value& v) { c.siggenSigmf = v.choice == 0; }},
        {"siggen.sample", {Kind::Choice, 0, 0, "cf32|ci16|ci8|cu8"},
         [](WvtConfig& c, const Value& v) {
             const SampleFormat f[] = {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CI8, SampleFormat::CU8};
             c.siggenSample = f[v.choice];
         }},
        {"export.band_sample", {Kind::Choice, 0, 0, "cf32|ci16"}, [](WvtConfig& c, const Value& v) { c.exportBandCi16 = v.choice == 1; }},
        // file waterfall
        {"fft.size", {Kind::Whole, 64, 65536}, [](WvtConfig& c, const Value& v) { c.fftSize = (int)v.whole; }},
        {"fft.window", {Kind::Choice, 0, 0, kWindows}, [](WvtConfig& c, const Value& v) { c.fftWindow = (WindowType)v.choice; }},
        {"fft.overlap_pct", {Kind::Number, 0, 95}, [](WvtConfig& c, const Value& v) { c.fftOverlapPct = v.num; }},
        {"display.colormap", {Kind::Text}, [](WvtConfig& c, const Value& v) { c.colormap = lower(v.text); }},
        {"display.db_min", {Kind::Number, -400, 400, nullptr, true},
         [](WvtConfig& c, const Value& v) { c.dbMin = v.isAuto ? NAN : v.num; }},
        {"display.db_max", {Kind::Number, -400, 400, nullptr, true},
         [](WvtConfig& c, const Value& v) { c.dbMax = v.isAuto ? NAN : v.num; }},
        // live spectrum
        {"live.fft_size", {Kind::Whole, 64, 65536}, [](WvtConfig& c, const Value& v) { c.liveFftSize = (int)v.whole; }},
        {"live.window", {Kind::Choice, 0, 0, kWindows}, [](WvtConfig& c, const Value& v) { c.liveWindow = (WindowType)v.choice; }},
        {"live.avg", {Kind::Whole, 1, 64, nullptr, true}, [](WvtConfig& c, const Value& v) { c.liveAvg = v.isAuto ? 0 : (int)v.whole; }},
        {"live.rows_per_sec", {Kind::Number, 1, 100}, [](WvtConfig& c, const Value& v) { c.liveRowsPerSec = v.num; }},
        {"live.max_columns", {Kind::Whole, 64, 8192}, [](WvtConfig& c, const Value& v) { c.liveMaxColumns = (int)v.whole; }},
        {"live.history_rows", {Kind::Whole, 64, 4096}, [](WvtConfig& c, const Value& v) { c.liveHistoryRows = (int)v.whole; }},
        // general
        {"gpu", {Kind::Choice, 0, 0, "auto|on|off"}, [](WvtConfig& c, const Value& v) { c.gpu = v.text; }},
        {"gpu.device", {Kind::Whole, 0, 63}, [](WvtConfig& c, const Value& v) { c.gpuDevice = (int)v.whole; }},
        {"ui.scale", {Kind::Number, 0.5, 4}, [](WvtConfig& c, const Value& v) { c.uiScale = v.num; }},
        {"ui.geometry", {Kind::Size, 100, 16384},
         [](WvtConfig& c, const Value& v) {
             c.uiWidth = v.w;
             c.uiHeight = v.h;
         }},
        {"log.folder", {Kind::Folder}, [](WvtConfig& c, const Value& v) { c.logFolder = v.text; }},
        // transmit limits
        {"tx.max_gain_db", {Kind::Number, -100, 200}, [](WvtConfig& c, const Value& v) { c.txMaxGainDb = v.num; }},
        {"tx.allow", {Kind::Bands}, [](WvtConfig& c, const Value& v) { c.txAllow = v.bands; }},
        // SDRs
        {"sdr.default", {Kind::Text}, [](WvtConfig& c, const Value& v) { c.sdrDefault = lower(v.text); }},
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
};

const std::vector<ProfileKey>& profileKeys() {
    static const std::vector<ProfileKey> k = {
        {"type", {Kind::Text}, nullptr, [](SdrProfile& p, const Value& v) { p.type = lower(v.text); }},
        {"serial", {Kind::Text}, nullptr, [](SdrProfile& p, const Value& v) { p.serial = v.text; }},
        {"args", {Kind::Text}, nullptr, [](SdrProfile& p, const Value& v) { p.args = v.text; }},
        {"fc", {Kind::Number, 0, 1e12}, nullptr, [](SdrProfile& p, const Value& v) { p.fc = v.num; }},
        {"rate", {Kind::Number, 1, 1e10}, nullptr, [](SdrProfile& p, const Value& v) { p.rate = v.num; }},
        {"gain_db", {Kind::Number, -100, 200}, "uhd,skiq,rtlsdr,sim", [](SdrProfile& p, const Value& v) { p.gainDb = v.num; }},
        {"ref_level_dbm", {Kind::Number, -130, 20}, "bb", [](SdrProfile& p, const Value& v) { p.refLevelDbm = v.num; }},
        {"agc", {Kind::Bool}, "uhd,skiq,rtlsdr,sim", [](SdrProfile& p, const Value& v) { p.agc = v.flag; }},
        {"port", {Kind::Text}, "uhd,skiq", [](SdrProfile& p, const Value& v) { p.port = v.text; }},
        {"bandwidth", {Kind::Number, 1, 1e10, nullptr, true}, "uhd,bb,skiq",
         [](SdrProfile& p, const Value& v) { p.bandwidth = v.isAuto ? 0 : v.num; }},
        {"ref", {Kind::Choice, 0, 0, "internal|external|gpsdo"}, "uhd", [](SdrProfile& p, const Value& v) { p.ref = v.text; }},
        {"channel", {Kind::Whole, 0, 63}, "uhd", [](SdrProfile& p, const Value& v) { p.channel = (int)v.whole; }},
        {"atten", {Kind::Choice, 0, 0, "0|10|20|30", true}, "bb",
         [](SdrProfile& p, const Value& v) { p.atten = v.isAuto ? -1 : v.choice; }},
        {"amp", {Kind::Whole, 0, 3, nullptr, true}, "bb", [](SdrProfile& p, const Value& v) { p.amp = v.isAuto ? -1 : (int)v.whole; }},
        {"ext_ref", {Kind::Bool}, "bb,skiq", [](SdrProfile& p, const Value& v) { p.extRef = v.flag; }},
        {"direct_sampling", {Kind::Choice, 0, 0, "off|i|q"}, "rtlsdr",
         [](SdrProfile& p, const Value& v) { p.directSampling = v.choice; }},
        {"bias_tee", {Kind::Bool}, "rtlsdr", [](SdrProfile& p, const Value& v) { p.biasTee = v.flag; }},
        {"ppm", {Kind::Whole, -1000, 1000}, "rtlsdr", [](SdrProfile& p, const Value& v) { p.ppm = (int)v.whole; }},
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
