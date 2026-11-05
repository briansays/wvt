#include "net/zmq_format.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>

#include "common/paths.h"

namespace {

constexpr size_t kMaxHeaderBytes = 65536;
constexpr size_t kMinMaxBytes = 64;
constexpr size_t kMaxMaxBytes = 64u << 20;
constexpr int kMaxFrame = 16;
constexpr size_t kMaxGroup = 255;  // ZMQ_GROUP_MAX_LENGTH in libzmq 4.3.5

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// "text" -> text; other values unchanged.
std::string unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

// The line without its comment: "#" or ";" at the start or after a space,
// not inside quotes.
std::string stripComment(const std::string& line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == '#' || c == ';') && (i == 0 || std::isspace((unsigned char)line[i - 1])))
            return line.substr(0, i);
    }
    return line;
}

size_t distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t prev = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cur = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
            prev = cur;
        }
    }
    return row[b.size()];
}

// A number in base units: "2.4e6", "-20". No SI suffixes.
bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// A whole number, decimal or 0x hex; a leading "-" when `allowNegative`.
bool parseWhole(const std::string& s, bool allowNegative, uint64_t& mag, bool& negative) {
    negative = false;
    std::string t = s;
    if (!t.empty() && t[0] == '-') {
        if (!allowNegative) return false;
        negative = true;
        t = t.substr(1);
    }
    if (t.empty() || !std::isxdigit((unsigned char)t[0])) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 0);
    if (errno != 0 || end != t.c_str() + t.size()) return false;
    mag = v;
    return true;
}

int typeBytes(FieldType t) {
    switch (t) {
        case FieldType::U8:
        case FieldType::I8: return 1;
        case FieldType::U16:
        case FieldType::I16: return 2;
        case FieldType::U32:
        case FieldType::I32:
        case FieldType::F32: return 4;
        case FieldType::U64:
        case FieldType::I64:
        case FieldType::F64: return 8;
    }
    return 1;
}

bool typeIsFloat(FieldType t) { return t == FieldType::F32 || t == FieldType::F64; }
bool typeIsSigned(FieldType t) {
    return t == FieldType::I8 || t == FieldType::I16 || t == FieldType::I32 || t == FieldType::I64;
}

// "u32be" -> U32, big endian. u8 and i8 have no byte order.
bool parseType(const std::string& s, FieldType& t, bool& be) {
    static const std::pair<const char*, FieldType> kTypes[] = {
        {"u16", FieldType::U16}, {"i16", FieldType::I16}, {"u32", FieldType::U32}, {"i32", FieldType::I32},
        {"u64", FieldType::U64}, {"i64", FieldType::I64}, {"f32", FieldType::F32}, {"f64", FieldType::F64},
    };
    const std::string l = lower(s);
    be = false;
    if (l == "u8") {
        t = FieldType::U8;
        return true;
    }
    if (l == "i8") {
        t = FieldType::I8;
        return true;
    }
    if (l.size() != 5) return false;
    const std::string base = l.substr(0, 3), order = l.substr(3);
    if (order != "le" && order != "be") return false;
    for (const auto& [n, ty] : kTypes) {
        if (base == n) {
            t = ty;
            be = order == "be";
            return true;
        }
    }
    return false;
}

uint64_t loadBits(const uint8_t* p, int n, bool be) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) {
        const int shift = 8 * (be ? n - 1 - i : i);
        v |= (uint64_t)p[i] << shift;
    }
    return v;
}

void storeBits(uint64_t v, int n, bool be, std::string& out) {
    for (int i = 0; i < n; ++i) {
        const int shift = 8 * (be ? n - 1 - i : i);
        out += (char)(uint8_t)(v >> shift);
    }
}

// The value of a field as a double, and as a whole number when it is an
// integer type that is not negative.
struct FieldValue {
    double d = 0;
    uint64_t u = 0;
    bool negative = false;
};

FieldValue readField(const FormatField& f, const uint8_t* p) {
    FieldValue v;
    const int n = typeBytes(f.type);
    const uint64_t bits = loadBits(p + f.offset, n, f.bigEndian);
    switch (f.type) {
        case FieldType::F32: {
            const auto b32 = (uint32_t)bits;
            float x = 0;
            memcpy(&x, &b32, sizeof x);
            v.d = x;
            break;
        }
        case FieldType::F64: memcpy(&v.d, &bits, sizeof v.d); break;
        default:
            if (typeIsSigned(f.type)) {
                // Sign-extend from n bytes.
                const int shift = 64 - 8 * n;
                const auto s = (int64_t)(bits << shift) >> shift;
                v.d = (double)s;
                v.negative = s < 0;
                v.u = v.negative ? 0 : (uint64_t)s;
            } else {
                v.d = (double)bits;
                v.u = bits;
            }
            break;
    }
    return v;
}

struct Parser {
    Parser(const std::string& n, StreamFormat& fmt, std::vector<std::string>& w) : name(n), f(fmt), warnings(w) {}
    const std::string& name;
    StreamFormat& f;
    std::vector<std::string>& warnings;
    std::string err;
    int line = 0;
    std::string section;
    std::set<std::string> seen;
    bool haveSocket = false, haveTransport = false, haveMode = false, haveSamples = false;
    bool haveTopic = false, haveGroup = false, haveFixedRate = false;
    // The fields that are set, with their names, for the header size check.
    std::vector<std::pair<std::string, const FormatField*>> fields;

    std::string where() const { return name + ":" + std::to_string(line) + ": "; }
    void warn(const std::string& m) { warnings.push_back(where() + m); }
    bool fail(const std::string& m) {
        if (err.empty()) err = where() + m;
        return false;
    }

    void unknownKey(const std::string& key, std::initializer_list<const char*> known) {
        std::string best;
        size_t bestD = 3;
        for (const char* k : known) {
            const size_t d = distance(key, k);
            if (d < bestD) {
                bestD = d;
                best = k;
            }
        }
        warn("unknown key '" + key + "' in [" + section + "]" + (best.empty() ? "" : " (did you mean '" + best + "'?)"));
    }

    bool offsetOf(const std::string& s, uint32_t& out) {
        uint64_t v = 0;
        bool neg = false;
        if (!parseWhole(trim(s), false, v, neg) || v >= kMaxHeaderBytes) return fail("bad offset '" + trim(s) + "'");
        out = (uint32_t)v;
        return true;
    }

    // "TYPE @ OFFSET [* SCALE]"
    bool field(const std::string& key, const std::string& value, FormatField& out, bool scaled) {
        const size_t at = value.find('@');
        if (at == std::string::npos) return fail(key + ": write TYPE @ OFFSET, e.g. u32le @ 8");
        FormatField fld;
        if (!parseType(trim(value.substr(0, at)), fld.type, fld.bigEndian))
            return fail(key + ": unknown type '" + trim(value.substr(0, at)) + "' (u8, i8, u16le, u16be, ... f64be)");
        std::string rest = value.substr(at + 1);
        if (const size_t star = rest.find('*'); star != std::string::npos) {
            if (!scaled) return fail(key + ": a scale is allowed only for fc, rate, gain and time_ns");
            if (!parseNumber(trim(rest.substr(star + 1)), fld.scale) || fld.scale == 0)
                return fail(key + ": bad scale '" + trim(rest.substr(star + 1)) + "'");
            rest = rest.substr(0, star);
        }
        if (!offsetOf(rest, fld.offset)) return false;
        if (!scaled && typeIsFloat(fld.type)) return fail(key + ": must be an integer type (u8 ... u64be)");
        fld.set = true;
        out = fld;
        return true;
    }

    // "TYPE @ OFFSET == VALUE" or "\"text\" @ OFFSET"
    bool match(const std::string& key, const std::string& value) {
        FormatMatch m;
        m.name = key.substr(6);
        if (m.name.empty()) return fail("match: write match.NAME = ...");
        const size_t at = value.rfind('@');
        const std::string left = trim(value.substr(0, at == std::string::npos ? 0 : at));
        if (at != std::string::npos && left.size() >= 2 && left.front() == '"' && left.back() == '"') {
            m.bytes = left.substr(1, left.size() - 2);
            if (m.bytes.empty()) return fail(key + ": the text is empty");
            if (!offsetOf(value.substr(at + 1), m.offset)) return false;
        } else {
            const size_t eq = value.find("==");
            const size_t at1 = value.find('@');
            if (at1 == std::string::npos || eq == std::string::npos || eq < at1)
                return fail(key + ": write TYPE @ OFFSET == VALUE, or \"text\" @ OFFSET");
            FieldType t;
            bool be = false;
            if (!parseType(trim(value.substr(0, at1)), t, be))
                return fail(key + ": unknown type '" + trim(value.substr(0, at1)) + "'");
            if (!offsetOf(value.substr(at1 + 1, eq - at1 - 1), m.offset)) return false;
            const std::string v = trim(value.substr(eq + 2));
            const int n = typeBytes(t);
            if (typeIsFloat(t)) {
                double d = 0;
                if (!parseNumber(v, d)) return fail(key + ": bad value '" + v + "'");
                uint64_t bits = 0;
                if (t == FieldType::F32) {
                    const auto x = (float)d;
                    uint32_t b32 = 0;
                    memcpy(&b32, &x, sizeof b32);
                    bits = b32;
                } else {
                    memcpy(&bits, &d, sizeof bits);
                }
                storeBits(bits, n, be, m.bytes);
            } else {
                uint64_t mag = 0;
                bool neg = false;
                if (!parseWhole(v, typeIsSigned(t), mag, neg)) return fail(key + ": bad value '" + v + "'");
                const int bits = 8 * n;
                const uint64_t max = bits == 64 ? std::numeric_limits<uint64_t>::max() : (1ULL << bits) - 1;
                const uint64_t smax = max >> 1;
                if (typeIsSigned(t) ? (neg ? mag > smax + 1 : mag > smax) : mag > max)
                    return fail(key + ": " + v + " does not fit in " + fieldTypeName(t, be));
                const uint64_t raw = neg ? (uint64_t)(-(int64_t)(mag - 1) - 1) : mag;
                storeBits(raw, n, be, m.bytes);
            }
        }
        if (std::any_of(f.matches.begin(), f.matches.end(), [&](const FormatMatch& x) { return x.name == m.name; }))
            return fail(key + " is set twice");
        f.matches.push_back(m);
        return true;
    }

    bool key(const std::string& k, const std::string& raw) {
        const std::string v = unquote(raw);
        const std::string lv = lower(v);
        if (!seen.insert(section + "." + k).second && k.rfind("match.", 0) != 0)
            warn(k + " is set twice in [" + section + "]; the last value is used");
        if (section == "format") {
            if (k == "name") f.name = v;
            else if (k == "description") f.description = v;
            else unknownKey(k, {"name", "description"});
        } else if (section == "socket") {
            if (k == "type") {
                haveSocket = true;
                if (lv == "dish") f.socket = FormatSocket::Dish;
                else if (lv == "sub") f.socket = FormatSocket::Sub;
                else if (lv == "pull") f.socket = FormatSocket::Pull;
                else return fail("type: '" + v + "' is not dish, sub or pull");
            } else if (k == "transport") {
                haveTransport = true;
                if (lv != "tcp" && lv != "udp") return fail("transport: '" + v + "' is not tcp or udp");
                f.tcp = lv == "tcp";
            } else if (k == "mode") {
                haveMode = true;
                if (lv != "bind" && lv != "connect") return fail("mode: '" + v + "' is not bind or connect");
                f.bind = lv == "bind";
            } else if (k == "address") {
                f.address = v;
            } else if (k == "topic") {
                haveTopic = true;
                f.topic = v;
            } else if (k == "group") {
                haveGroup = true;
                if (v.empty() || v.size() > kMaxGroup) return fail("group: 1 to 255 characters");
                f.group = v;
            } else if (k == "frames") {
                if (lv == "last") {
                    f.frame = 0;
                } else {
                    uint64_t n = 0;
                    bool neg = false;
                    if (!parseWhole(v, false, n, neg) || n < 1 || n > kMaxFrame)
                        return fail("frames: 'last' or a number from 1 to " + std::to_string(kMaxFrame));
                    f.frame = (int)n;
                }
            } else if (k == "max_bytes") {
                uint64_t n = 0;
                bool neg = false;
                if (!parseWhole(v, false, n, neg) || n < kMinMaxBytes || n > kMaxMaxBytes)
                    return fail("max_bytes: a number from " + std::to_string(kMinMaxBytes) + " to " + std::to_string(kMaxMaxBytes));
                f.maxBytes = (size_t)n;
            } else {
                unknownKey(k, {"type", "transport", "mode", "address", "topic", "group", "frames", "max_bytes"});
            }
        } else if (section == "header") {
            if (k == "bytes") {
                uint64_t n = 0;
                bool neg = false;
                if (!parseWhole(v, false, n, neg) || n > kMaxHeaderBytes)
                    return fail("bytes: a number from 0 to " + std::to_string(kMaxHeaderBytes));
                f.headerBytes = (size_t)n;
            } else if (k.rfind("match.", 0) == 0) {
                return match(k, raw);
            } else if (k == "fc") {
                return field(k, v, f.fc, true);
            } else if (k == "rate") {
                return field(k, v, f.rate, true);
            } else if (k == "gain") {
                return field(k, v, f.gain, true);
            } else if (k == "time_ns") {
                return field(k, v, f.timeNs, true);
            } else if (k == "index") {
                return field(k, v, f.index, false);
            } else if (k == "seq") {
                return field(k, v, f.seq, false);
            } else if (k == "count") {
                return field(k, v, f.count, false);
            } else {
                unknownKey(k, {"bytes", "fc", "rate", "gain", "time_ns", "index", "seq", "count"});
            }
        } else if (section == "samples") {
            if (k == "format") {
                haveSamples = true;
                static const std::pair<const char*, SampleFormat> kFmts[] = {
                    {"cu8", SampleFormat::CU8},   {"ci8", SampleFormat::CI8},   {"ci16", SampleFormat::CI16},
                    {"ci32", SampleFormat::CI32}, {"cf32", SampleFormat::CF32}, {"cf64", SampleFormat::CF64},
                };
                bool found = false;
                for (const auto& [n, sf] : kFmts) {
                    if (lv == n) {
                        f.sampleFmt = sf;
                        found = true;
                    }
                }
                if (!found) return fail("format: '" + v + "' is not cu8, ci8, ci16, ci32, cf32 or cf64");
            } else if (k == "endian") {
                if (lv != "little" && lv != "big") return fail("endian: '" + v + "' is not little or big");
                f.bigEndian = lv == "big";
            } else if (k == "order") {
                if (lv != "iq" && lv != "qi") return fail("order: '" + v + "' is not iq or qi");
                f.swapIQ = lv == "qi";
            } else {
                unknownKey(k, {"format", "endian", "order"});
            }
        } else if (section == "stream") {
            double d = 0;
            if (k != "rate" && k != "fc" && k != "gain") {
                unknownKey(k, {"rate", "fc", "gain"});
                return true;
            }
            if (!parseNumber(v, d)) return fail(k + ": '" + v + "' is not a number (write 2.4e6, not 2.4M)");
            if (k == "rate") {
                if (d <= 0 || d > 1e10) return fail("rate: from 0 to 1e10 S/s");
                f.fixedRate = d;
                haveFixedRate = true;
            } else if (k == "fc") {
                if (d < 0 || d > 1e12) return fail("fc: from 0 to 1e12 Hz");
                f.fixedFc = d;
            } else {
                if (std::fabs(d) > 1000) return fail("gain: from -1000 to 1000 dB");
                f.fixedGain = d;
            }
        }
        // Keys in an unknown section were reported with the section.
        return true;
    }

    bool run(const std::string& text) {
        std::istringstream in(text);
        std::string raw;
        while (std::getline(in, raw)) {
            ++line;
            const std::string l = trim(stripComment(raw));
            if (l.empty()) continue;
            if (l.front() == '[') {
                if (l.back() != ']') return fail("a section is [name]");
                section = lower(trim(l.substr(1, l.size() - 2)));
                if (section != "format" && section != "socket" && section != "header" && section != "samples" &&
                    section != "stream") {
                    warn("unknown section [" + section + "] (format, socket, header, samples, stream); its keys are ignored");
                }
                continue;
            }
            const size_t eq = l.find('=');
            if (eq == std::string::npos) return fail("not 'key = value': " + l);
            const std::string k = lower(trim(l.substr(0, eq)));
            if (section.empty()) return fail("'" + k + "' is before the first [section]");
            if (!key(k, trim(l.substr(eq + 1)))) return false;
        }
        line = 0;
        return check();
    }

    // The checks that need the whole file.
    bool check() {
        auto failFile = [&](const std::string& m) {
            if (err.empty()) err = name + ": " + m;
            return false;
        };
        if (!haveSocket) return failFile("[socket] type is missing (dish, sub or pull)");
        if (!haveTransport) f.tcp = f.socket != FormatSocket::Dish;
        if (!haveMode) f.bind = f.socket == FormatSocket::Dish;
        if (f.socket == FormatSocket::Dish && f.tcp) return failFile("type = dish works only with transport = udp");
        if (f.socket != FormatSocket::Dish && !f.tcp)
            return failFile("transport = udp works only with type = dish (libzmq sends sub and pull on tcp)");
        if (f.socket == FormatSocket::Dish && !f.bind) return failFile("type = dish receives with mode = bind");
        if (haveTopic && f.socket != FormatSocket::Sub) warnings.push_back(name + ": topic is used only with type = sub");
        if (haveGroup && f.socket != FormatSocket::Dish) warnings.push_back(name + ": group is used only with type = dish");
        if (!haveSamples) return failFile("[samples] format is missing");
        if (!f.rate.set && !haveFixedRate)
            return failFile("the sample rate is missing: set rate in [header] or in [stream]");
        if (f.index.set && f.seq.set) return failFile("set index or seq in [header], not both");
        if (f.headerBytes >= f.maxBytes) return failFile("[header] bytes must be less than max_bytes");
        fields = {{"fc", &f.fc},       {"rate", &f.rate},   {"gain", &f.gain}, {"time_ns", &f.timeNs},
                  {"index", &f.index}, {"seq", &f.seq},     {"count", &f.count}};
        for (const auto& [n, fld] : fields) {
            if (fld->set && fld->offset + (size_t)typeBytes(fld->type) > f.headerBytes)
                return failFile(n + " (bytes " + std::to_string(fld->offset) + " to " +
                                std::to_string(fld->offset + typeBytes(fld->type) - 1) +
                                ") is outside the header ([header] bytes = " + std::to_string(f.headerBytes) + ")");
        }
        for (const FormatMatch& m : f.matches) {
            if (m.offset + m.bytes.size() > f.headerBytes)
                return failFile("match." + m.name + " is outside the header ([header] bytes = " +
                                std::to_string(f.headerBytes) + ")");
        }
        if (f.name.empty()) f.name = f.id.empty() ? name : f.id;
        return true;
    }
};

std::string fileName(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// "zmq_gnuradio.ini" -> "gnuradio"
std::string idOf(const std::string& file) {
    std::string id = file;
    if (id.rfind("zmq_", 0) == 0) id = id.substr(4);
    if (id.size() >= 4 && id.compare(id.size() - 4, 4, ".ini") == 0) id = id.substr(0, id.size() - 4);
    return id;
}

}  // namespace

const char* fieldTypeName(FieldType t, bool bigEndian) {
    switch (t) {
        case FieldType::U8: return "u8";
        case FieldType::I8: return "i8";
        case FieldType::U16: return bigEndian ? "u16be" : "u16le";
        case FieldType::I16: return bigEndian ? "i16be" : "i16le";
        case FieldType::U32: return bigEndian ? "u32be" : "u32le";
        case FieldType::I32: return bigEndian ? "i32be" : "i32le";
        case FieldType::U64: return bigEndian ? "u64be" : "u64le";
        case FieldType::I64: return bigEndian ? "i64be" : "i64le";
        case FieldType::F32: return bigEndian ? "f32be" : "f32le";
        case FieldType::F64: return bigEndian ? "f64be" : "f64le";
    }
    return "?";
}

StreamFormat builtinStreamFormat() {
    StreamFormat f;
    f.id = "wvt";
    f.name = "wvt";
    f.description = "The wvt stream: RADIO/DISH on UDP, a 64-byte header with the frequency, rate, format and gain";
    f.builtin = true;
    f.socket = FormatSocket::Dish;
    f.tcp = false;
    f.bind = true;
    f.maxBytes = 8192;
    return f;
}

bool parseStreamFormat(const std::string& text, const std::string& name, StreamFormat& f,
                       std::vector<std::string>& warnings, std::string& err) {
    Parser p(name, f, warnings);
    const bool ok = p.run(text);
    if (!ok) err = p.err;
    return ok;
}

bool loadStreamFormat(const std::string& path, StreamFormat& f, std::vector<std::string>& warnings, std::string& err) {
    const std::string file = fileName(path);
    f = StreamFormat{};
    f.id = idOf(file);
    f.path = path;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot read " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseStreamFormat(ss.str(), file, f, warnings, err);
}

std::vector<StreamFormatEntry> scanStreamFormats(const std::string& dir) {
    std::vector<StreamFormatEntry> out;
    out.push_back({builtinStreamFormat(), {}, {}});
    if (dir.empty()) return out;
    for (const std::string& file : listFiles(dir, "zmq_", ".ini")) {
        StreamFormatEntry e;
        std::string path = dir;
        if (path.back() != '/') path += '/';
        path += file;
        if (!loadStreamFormat(path, e.format, e.warnings, e.error)) {
            e.format.id = idOf(file);
            e.format.path = path;
            if (e.format.name.empty()) e.format.name = e.format.id;
        }
        if (e.ok() && findStreamFormat(out, e.format.id))
            e.error = file + ": the name '" + e.format.id + "' is taken (the built-in format or another file); rename the file";
        if (e.ok()) {
            for (const StreamFormatEntry& o : out)
                if (o.ok() && o.format.name == e.format.name)
                    e.warnings.push_back(file + ": another format has the name '" + e.format.name + "'");
        }
        out.push_back(std::move(e));
    }
    return out;
}

const StreamFormatEntry* findStreamFormat(const std::vector<StreamFormatEntry>& list, const std::string& id) {
    const std::string l = lower(id);
    for (const StreamFormatEntry& e : list)
        if (lower(e.format.id) == l) return &e;
    return nullptr;
}

SampleFormat streamRingFormat(const StreamFormat& f) {
    if (f.builtin) return SampleFormat::CI16;  // not used: the header gives it
    switch (f.sampleFmt) {
        case SampleFormat::CU8:
        case SampleFormat::CI16:
        case SampleFormat::CF32: return f.sampleFmt;
        default: return SampleFormat::CF32;
    }
}

bool decodeStreamMessage(const StreamFormat& f, const uint8_t* p, size_t n, StreamMessage& m, FormatExtra& extra,
                         std::vector<uint8_t>& scratch, std::string& why) {
    extra = FormatExtra{};
    if (f.builtin) {
        if (!parseStreamMessage(p, n, m, why)) return false;
        extra.hasIndex = true;
        return true;
    }
    m = StreamMessage{};
    if (n > f.maxBytes) {
        why = "too large (" + std::to_string(n) + " bytes; max_bytes = " + std::to_string(f.maxBytes) + ")";
        return false;
    }
    if (n < f.headerBytes) {
        why = "too short (" + std::to_string(n) + " bytes, the header is " + std::to_string(f.headerBytes) + ")";
        return false;
    }
    for (const FormatMatch& mt : f.matches) {
        if (memcmp(p + mt.offset, mt.bytes.data(), mt.bytes.size()) != 0) {
            why = "match." + mt.name + " does not match";
            return false;
        }
    }
    StreamHeader& h = m.h;
    h.kind = StreamKind::Data;
    h.fmt = streamRingFormat(f);
    h.fc = f.fc.set ? readField(f.fc, p).d * f.fc.scale : f.fixedFc;
    h.rate = f.rate.set ? readField(f.rate, p).d * f.rate.scale : f.fixedRate;
    h.gain = f.gain.set ? readField(f.gain, p).d * f.gain.scale : f.fixedGain;
    if (!std::isfinite(h.rate) || h.rate <= 0 || h.rate > 1e10) {
        why = "bad sample rate";
        return false;
    }
    if (!std::isfinite(h.fc) || h.fc < 0 || h.fc > 1e12) {
        why = "bad center frequency";
        return false;
    }
    if (!std::isfinite(h.gain) || std::fabs(h.gain) > 1000) {
        why = "bad gain";
        return false;
    }
    if (f.timeNs.set) {
        const FieldValue v = readField(f.timeNs, p);
        if (!typeIsFloat(f.timeNs.type) && f.timeNs.scale == 1) {
            h.timeNs = v.u;  // exact: a double has only 53 bits
        } else {
            const double t = v.d * f.timeNs.scale;
            h.timeNs = std::isfinite(t) && t > 0 && t < 1.8e19 ? (uint64_t)t : 0;  // 0: unknown
        }
    }
    if (f.index.set) {
        const FieldValue v = readField(f.index, p);
        if (v.negative) {
            why = "negative sample index";
            return false;
        }
        h.index = v.u;
        extra.hasIndex = true;
    }
    if (f.seq.set) {
        const FieldValue v = readField(f.seq, p);
        if (v.negative) {
            why = "negative message counter";
            return false;
        }
        extra.seq = v.u;
        extra.hasSeq = true;
    }

    const uint8_t* body = p + f.headerBytes;
    const size_t rest = n - f.headerBytes;
    const auto bps = (size_t)formatBytesPerSample(f.sampleFmt);
    size_t count = 0;
    if (f.count.set) {
        const FieldValue v = readField(f.count, p);
        if (v.negative || v.u > rest / bps) {
            why = "the sample count does not fit in the message";
            return false;
        }
        count = (size_t)v.u;
    } else {
        if (rest % bps != 0) {
            why = std::to_string(rest) + " bytes after the header are not whole samples of " + std::to_string(bps) + " bytes";
            return false;
        }
        count = rest / bps;
    }
    h.count = (uint32_t)count;  // at most kMaxMaxBytes / 2

    const SampleFormat out = h.fmt;
    if (out == f.sampleFmt && !f.swapIQ && (!f.bigEndian || bps == 2)) {
        // The ring's format as it is (cu8 has no byte order).
        m.samples = count ? body : nullptr;
        m.sampleBytes = count * bps;
        return true;
    }
    if (out == f.sampleFmt) {
        // The same format, with the bytes or I and Q in another order.
        const size_t half = bps / 2;
        const bool swapBytes = f.bigEndian && half > 1;
        scratch.resize(count * bps);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* src = body + i * bps;
            uint8_t* dst = scratch.data() + i * bps;
            for (size_t c = 0; c < 2; ++c) {
                const uint8_t* s = src + (f.swapIQ ? 1 - c : c) * half;
                uint8_t* d = dst + c * half;
                for (size_t b = 0; b < half; ++b) d[b] = s[swapBytes ? half - 1 - b : b];
            }
        }
    } else {
        // Another format: to cf32.
        FormatParams fp;
        fp.fmt = f.sampleFmt;
        fp.bigEndian = f.bigEndian;
        fp.swapIQ = f.swapIQ;
        thread_local std::vector<std::complex<float>> tmp;
        tmp.resize(count);
        decodeSamples(fp, body, (int64_t)count, tmp.data());
        scratch.resize(count * sizeof(std::complex<float>));
        if (count) memcpy(scratch.data(), tmp.data(), scratch.size());
    }
    m.samples = count ? scratch.data() : nullptr;
    m.sampleBytes = scratch.size();
    return true;
}
