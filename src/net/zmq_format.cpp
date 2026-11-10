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
#include <mutex>
#include <set>
#include <sstream>

#include "common/paths.h"
#include "net/zmq_format_proto.h"

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

void storeAt(uint64_t bits, int n, bool be, uint8_t* dst) {
    for (int i = 0; i < n; ++i) dst[be ? n - 1 - i : i] = (uint8_t)(bits >> (8 * i));
}

// The samples with I and Q, or the bytes of each value, in the other order.
// It is its own inverse, so the decoder and the encoder both use it.
void reorderSamples(const uint8_t* src, uint8_t* dst, size_t count, size_t bps, bool swapIQ, bool swapBytes) {
    const size_t half = bps / 2;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* in = src + i * bps;
        uint8_t* out = dst + i * bps;
        for (size_t c = 0; c < 2; ++c) {
            const uint8_t* from = in + (swapIQ ? 1 - c : c) * half;
            uint8_t* to = out + c * half;
            for (size_t b = 0; b < half; ++b) to[b] = from[swapBytes ? half - 1 - b : b];
        }
    }
}

// Writes `v` / the field's scale. False (why set) if it does not fit.
bool writeNumber(const FormatField& f, double v, const char* name, uint8_t* p, std::string& why) {
    const int n = typeBytes(f.type);
    const double x = v / f.scale;
    if (f.type == FieldType::F32) {
        const auto x32 = (float)x;
        uint32_t b = 0;
        memcpy(&b, &x32, sizeof b);
        storeAt(b, 4, f.bigEndian, p + f.offset);
        return true;
    }
    if (f.type == FieldType::F64) {
        uint64_t b = 0;
        memcpy(&b, &x, sizeof b);
        storeAt(b, 8, f.bigEndian, p + f.offset);
        return true;
    }
    const double r = std::nearbyint(x);
    const double top = std::ldexp(1.0, 8 * n - (typeIsSigned(f.type) ? 1 : 0));  // 2^bits or 2^(bits-1)
    const double lo = typeIsSigned(f.type) ? -top : 0;
    if (!std::isfinite(r) || r < lo || r >= top) {
        char buf[64];
        if (std::fabs(v) < 1e15 && v == std::floor(v)) snprintf(buf, sizeof buf, "%.0f", v);  // 2400000000, not 2.4e+09
        else snprintf(buf, sizeof buf, "%.9g", v);
        why = std::string(name) + " " + buf + " does not fit in " + fieldTypeName(f.type, f.bigEndian) +
              (f.scale != 1 ? " (with the scale)" : "");
        return false;
    }
    const uint64_t bits = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
    storeAt(bits, n, f.bigEndian, p + f.offset);
    return true;
}

// A counter: the low bits of `v` (it wraps, as the subscriber expects).
void writeCounter(const FormatField& f, uint64_t v, uint8_t* p) { storeAt(v, typeBytes(f.type), f.bigEndian, p + f.offset); }

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
    bool haveHeaderKeys = false, haveProtoKeys = false, haveEndian = false, haveOrder = false;
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

    // "6" or "6.1": field numbers, the second in the sub-message of the first.
    bool protoPath(const std::string& key, const std::string& text, std::vector<uint32_t>& path) {
        path.clear();
        const std::string t = trim(text);
        size_t at = 0;
        while (true) {
            const size_t dot = t.find('.', at);
            const std::string part = t.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
            errno = 0;
            const unsigned long long v = part.empty() || part.size() > 10 || part.find_first_not_of("0123456789") != std::string::npos
                                             ? 0
                                             : std::strtoull(part.c_str(), nullptr, 10);
            if (v < 1 || v > pb::kMaxField)
                return fail(key + ": bad field number '" + part + "' in '" + t +
                            "' (1 to 536870911; a field in a sub-message is 6.1)");
            path.push_back((uint32_t)v);
            if (path.size() > kMaxProtoPath) return fail(key + ": more than " + std::to_string(kMaxProtoPath) + " field numbers");
            if (dot == std::string::npos) return true;
            at = dot + 1;
        }
    }

    static std::vector<std::string> words(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        for (std::string w; in >> w;) out.push_back(w);
        return out;
    }

    // "TYPE PATH [* SCALE]"; for the arrays "[packed | unpacked] TYPE PATH".
    enum class Role { Number, Counter, Array, Bytes };  // Bytes: samples, which may be bytes
    bool protoField(const std::string& key, const std::string& value, ProtoField& out, Role role) {
        ProtoField fld;
        std::string v = value;
        if (const size_t star = v.find('*'); star != std::string::npos) {
            if (role != Role::Number) return fail(key + ": a scale is allowed only for fc, rate, gain and time_ns");
            if (!parseNumber(trim(v.substr(star + 1)), fld.scale) || fld.scale == 0)
                return fail(key + ": bad scale '" + trim(v.substr(star + 1)) + "'");
            v = v.substr(0, star);
        }
        std::vector<std::string> w = words(v);
        bool packing = false;
        if (!w.empty() && (role == Role::Array || role == Role::Bytes) && (lower(w[0]) == "packed" || lower(w[0]) == "unpacked")) {
            fld.packed = lower(w[0]) == "packed";
            packing = true;
            w.erase(w.begin());
        }
        if (w.size() != 2) return fail(key + ": write TYPE FIELD, e.g. double 3, or uint32 6.1 for a field in a sub-message");
        if (!parseProtoType(lower(w[0]), fld.type))
            return fail(key + ": unknown type '" + w[0] + "' (double, float, int32, int64, uint32, uint64, sint32, sint64, "
                              "fixed32, fixed64, sfixed32, sfixed64, bool, enum, bytes, string)");
        const ProtoType t = fld.type;
        switch (role) {
            case Role::Number:
                if (!protoIsFloat(t) && !protoIsInteger(t)) return fail(key + ": must be a number type, not " + w[0]);
                break;
            case Role::Counter:
                if (!protoIsInteger(t)) return fail(key + ": must be an integer type (int32 ... sfixed64), not " + w[0]);
                break;
            case Role::Array:
            case Role::Bytes:
                if (t == ProtoType::Bytes && role == Role::Bytes) {
                    if (packing) return fail(key + ": packed and unpacked are for number arrays, not bytes");
                    break;
                }
                if (!protoIsFloat(t) && !protoIsInteger(t))
                    return fail(key + ": the samples are " + (role == Role::Bytes ? "bytes, or " : "") +
                                "an array of float, double or an integer type, not " + w[0]);
                break;
        }
        if (!protoPath(key, w[1], fld.path)) return false;
        fld.set = true;
        out = fld;
        return true;
    }

    // "TYPE PATH == VALUE", "string PATH == \"text\"" or "present PATH".
    bool protoMatch(const std::string& key, const std::string& raw) {
        ProtoMatch m;
        m.name = key.substr(6);
        if (m.name.empty()) return fail("match: write match.NAME = ...");
        if (f.proto.matches.size() >= 32) return fail(key + ": at most 32 matches");
        const size_t eq = raw.find("==");
        const std::vector<std::string> w = words(eq == std::string::npos ? raw : raw.substr(0, eq));
        if (w.size() == 2 && lower(w[0]) == "present" && eq == std::string::npos) {
            m.kind = ProtoMatch::Kind::Present;
            if (!protoPath(key, w[1], m.path)) return false;
        } else {
            if (w.size() != 2 || eq == std::string::npos)
                return fail(key + ": write TYPE FIELD == VALUE, string FIELD == \"text\", or present FIELD");
            if (!parseProtoType(lower(w[0]), m.type)) return fail(key + ": unknown type '" + w[0] + "'");
            if (!protoPath(key, w[1], m.path)) return false;
            const std::string v = trim(raw.substr(eq + 2));
            if (m.type == ProtoType::String || m.type == ProtoType::Bytes) {
                if (v.size() < 2 || v.front() != '"' || v.back() != '"') return fail(key + ": write the text in quotes");
                m.kind = ProtoMatch::Kind::Text;
                m.text = v.substr(1, v.size() - 2);
            } else {
                m.kind = ProtoMatch::Kind::Value;
                std::string e;
                if (!protoRawFromText(m.type, v, m.raw, e)) return fail(key + ": " + e);
            }
        }
        if (std::any_of(f.proto.matches.begin(), f.proto.matches.end(), [&](const ProtoMatch& x) { return x.name == m.name; }))
            return fail(key + " is set twice");
        f.proto.matches.push_back(m);
        return true;
    }

    bool key(const std::string& k, const std::string& raw) {
        const std::string v = unquote(raw);
        const std::string lv = lower(v);
        if (!seen.insert(section + "." + k).second && k.rfind("match.", 0) != 0)
            warn(k + " is set twice in [" + section + "]; the last value is used");
        if (section == "format") {
            if (k == "name") {
                f.name = v;
            } else if (k == "description") {
                f.description = v;
            } else if (k == "encoding") {
                if (lv != "binary" && lv != "protobuf") return fail("encoding: '" + v + "' is not binary or protobuf");
                f.encoding = lv == "protobuf" ? FormatEncoding::Protobuf : FormatEncoding::Binary;
            } else {
                unknownKey(k, {"name", "description", "encoding"});
            }
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
            haveHeaderKeys = true;
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
                haveEndian = true;
                if (lv != "little" && lv != "big") return fail("endian: '" + v + "' is not little or big");
                f.bigEndian = lv == "big";
            } else if (k == "order") {
                haveOrder = true;
                if (lv != "iq" && lv != "qi") return fail("order: '" + v + "' is not iq or qi");
                f.swapIQ = lv == "qi";
            } else {
                unknownKey(k, {"format", "endian", "order"});
            }
        } else if (section == "protobuf") {
            haveProtoKeys = true;
            ProtoLayout& L = f.proto;
            if (k.rfind("match.", 0) == 0) return protoMatch(k, raw);
            if (k == "fc") return protoField(k, v, L.fc, Role::Number);
            if (k == "rate") return protoField(k, v, L.rate, Role::Number);
            if (k == "gain") return protoField(k, v, L.gain, Role::Number);
            if (k == "time_ns") return protoField(k, v, L.timeNs, Role::Number);
            if (k == "index") return protoField(k, v, L.index, Role::Counter);
            if (k == "seq") return protoField(k, v, L.seq, Role::Counter);
            if (k == "samples") return protoField(k, v, L.samples, Role::Bytes);
            if (k == "i") return protoField(k, v, L.i, Role::Array);
            if (k == "q") return protoField(k, v, L.q, Role::Array);
            if (k == "count") warn("count is not used: the length of the samples field gives the count");
            else unknownKey(k, {"fc", "rate", "gain", "time_ns", "index", "seq", "samples", "i", "q"});
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
                    section != "stream" && section != "protobuf") {
                    warn("unknown section [" + section + "] (format, socket, header, protobuf, samples, stream); its keys are ignored");
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

    // The checks of a protobuf format that need the whole file.
    template <class Fail>
    bool checkProto(Fail&& failFile) {
        ProtoLayout& L = f.proto;
        if (haveHeaderKeys) return failFile("[header] is for encoding = binary; a protobuf format uses [protobuf]");
        if (L.samples.set && (L.i.set || L.q.set)) return failFile("set samples, or i and q, in [protobuf], not both");
        if (!L.samples.set && !(L.i.set && L.q.set))
            return failFile("the samples are missing: set samples (I and Q in one field), or i and q, in [protobuf]");
        if (L.i.set && L.i.type != L.q.type) return failFile("i and q must have the same type");
        const ProtoType et = L.samples.set ? L.samples.type : L.i.type;
        if (et == ProtoType::Bytes) {
            if (!haveSamples) return failFile("[samples] format is missing (samples is bytes: [samples] gives their layout)");
        } else if (protoIsFloat(et)) {
            if (haveSamples && f.sampleFmt != SampleFormat::CF32 && f.sampleFmt != SampleFormat::CF64)
                warnings.push_back(name + ": [samples] format is not used: the samples are " + protoTypeName(et));
            f.sampleFmt = et == ProtoType::Float ? SampleFormat::CF32 : SampleFormat::CF64;
        } else {
            if (!haveSamples) f.sampleFmt = SampleFormat::CI16;
            if (f.sampleFmt != SampleFormat::CU8 && f.sampleFmt != SampleFormat::CI8 && f.sampleFmt != SampleFormat::CI16 &&
                f.sampleFmt != SampleFormat::CI32)
                return failFile(std::string("[samples] format: ") + protoTypeName(et) +
                                " samples are values of cu8, ci8, ci16 or ci32");
            if (!protoIsSigned(et) && f.sampleFmt != SampleFormat::CU8)
                return failFile(std::string(protoTypeName(et)) + " has no negative values: use a signed type (sint32), or format = cu8");
        }
        if (et != ProtoType::Bytes) {
            if (haveEndian) warnings.push_back(name + ": [samples] endian is not used: protobuf numbers are little endian");
            f.bigEndian = false;
        }
        if (L.i.set) {
            if (haveOrder) warnings.push_back(name + ": [samples] order is not used with i and q");
            f.swapIQ = false;
        }
        if (!L.rate.set && !haveFixedRate) return failFile("the sample rate is missing: set rate in [protobuf] or in [stream]");
        if (L.index.set && L.seq.set) return failFile("set index or seq in [protobuf], not both");

        // Each field once; only a "present" match may hold other fields.
        struct Use {
            std::string name;
            const std::vector<uint32_t>* path;
            bool present;
        };
        std::vector<Use> uses;
        const std::pair<const char*, const ProtoField*> fields[] = {
            {"fc", &L.fc}, {"rate", &L.rate}, {"gain", &L.gain}, {"time_ns", &L.timeNs}, {"index", &L.index},
            {"seq", &L.seq}, {"samples", &L.samples}, {"i", &L.i}, {"q", &L.q}};
        for (const auto& [n, fld] : fields)
            if (fld->set) uses.push_back({n, &fld->path, false});
        for (const ProtoMatch& m : L.matches)
            uses.push_back({"match." + m.name, &m.path, m.kind == ProtoMatch::Kind::Present});
        auto text = [](const std::vector<uint32_t>& p) {
            std::string s;
            for (uint32_t x : p) s += (s.empty() ? "" : ".") + std::to_string(x);
            return s;
        };
        for (size_t a = 0; a < uses.size(); ++a) {
            for (size_t b = 0; b < uses.size(); ++b) {
                if (a == b) continue;
                const auto& pa = *uses[a].path;
                const auto& pb2 = *uses[b].path;
                if (a < b && pa == pb2)
                    return failFile(uses[a].name + " and " + uses[b].name + " are both field " + text(pa));
                if (pa.size() < pb2.size() && std::equal(pa.begin(), pa.end(), pb2.begin()) && !uses[a].present)
                    return failFile(uses[b].name + " (field " + text(pb2) + ") is inside " + uses[a].name + " (field " +
                                    text(pa) + "), which is not a sub-message");
            }
        }
        if (f.name.empty()) f.name = f.id.empty() ? name : f.id;
        return true;
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
        if (f.encoding == FormatEncoding::Protobuf) return checkProto(failFile);
        if (haveProtoKeys) return failFile("[protobuf] is used only with encoding = protobuf in [format]");
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
    if (f.encoding == FormatEncoding::Protobuf) return decodeProtoMessage(f, p, n, m, extra, scratch, why);
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

    decodeSampleBody(f.sampleFmt, f.bigEndian, f.swapIQ, h.fmt, body, count, m, scratch);
    return true;
}

void decodeSampleBody(SampleFormat fmt, bool bigEndian, bool swapIQ, SampleFormat ring, const uint8_t* body,
                      size_t count, StreamMessage& m, std::vector<uint8_t>& scratch) {
    const auto bps = (size_t)formatBytesPerSample(fmt);
    if (ring == fmt && !swapIQ && (!bigEndian || bps == 2)) {
        // The ring's format as it is (cu8 has no byte order).
        m.samples = count ? body : nullptr;
        m.sampleBytes = count * bps;
        return;
    }
    if (ring == fmt) {
        // The same format, with the bytes or I and Q in another order.
        scratch.resize(count * bps);
        reorderSamples(body, scratch.data(), count, bps, swapIQ, bigEndian && bps > 2);
    } else {
        // Another format: to cf32.
        FormatParams fp;
        fp.fmt = fmt;
        fp.bigEndian = bigEndian;
        fp.swapIQ = swapIQ;
        thread_local std::vector<std::complex<float>> tmp;
        tmp.resize(count);
        decodeSamples(fp, body, (int64_t)count, tmp.data());
        scratch.resize(count * sizeof(std::complex<float>));
        if (count) memcpy(scratch.data(), tmp.data(), scratch.size());
    }
    m.samples = count ? scratch.data() : nullptr;
    m.sampleBytes = scratch.size();
}

void encodeSampleBody(SampleFormat fmt, bool bigEndian, bool swapIQ, SampleFormat native, const uint8_t* samples,
                      size_t count, uint8_t* body) {
    const auto bps = (size_t)formatBytesPerSample(fmt);
    const bool swapBytes = bigEndian && bps > 2;
    if (native == fmt && !swapIQ && !swapBytes) {
        memcpy(body, samples, count * bps);
    } else if (native == fmt) {
        reorderSamples(samples, body, count, bps, swapIQ, swapBytes);
    } else {
        // Through cf32, as the decoder does the other way.
        thread_local std::vector<std::complex<float>> tmp;
        thread_local std::vector<uint8_t> packed;
        tmp.resize(count);
        FormatParams fp;
        fp.fmt = native;
        decodeSamples(fp, samples, (int64_t)count, tmp.data());
        if (!swapIQ && !swapBytes) {
            encodeSamples(fmt, tmp.data(), (int64_t)count, body);
        } else {
            packed.resize(count * bps);
            encodeSamples(fmt, tmp.data(), (int64_t)count, packed.data());
            reorderSamples(packed.data(), body, count, bps, swapIQ, swapBytes);
        }
    }
}

namespace {
std::mutex gDirMutex;
std::string gDir;  // guarded by gDirMutex
}  // namespace

void setStreamFormatDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(gDirMutex);
    gDir = dir;
}

std::string streamFormatDir() {
    std::lock_guard<std::mutex> lk(gDirMutex);
    return gDir;
}

bool isBuiltinFormatId(const std::string& id) { return id.empty() || lower(id) == "wvt"; }

std::string streamFormatSummary(const StreamFormat& f) {
    std::string s = f.socket == FormatSocket::Dish ? "dish" : f.socket == FormatSocket::Sub ? "sub" : "pull";
    s += f.tcp ? ", tcp " : ", udp ";
    s += f.bind ? "bind" : "connect";
    if (!f.address.empty()) {
        s += ' ';
        s += f.address;
    }
    if (f.builtin) return s + ", the wvt header";
    if (f.encoding == FormatEncoding::Protobuf) {
        // "protobuf, float samples in field 10", "protobuf, sint32 i and q in fields 4.1 and 4.2 (ci16)"
        const ProtoLayout& L = f.proto;
        auto path = [](const ProtoField& x) {
            std::string t;
            for (uint32_t n : x.path) t += (t.empty() ? "" : ".") + std::to_string(n);
            return t;
        };
        const ProtoField& a = L.samples.set ? L.samples : L.i;
        s += ", protobuf, ";
        s += protoTypeName(a.type);
        s += L.samples.set ? " samples in field " + path(a) : " i and q in fields " + path(L.i) + " and " + path(L.q);
        if (!protoIsFloat(a.type)) {
            s += " (";
            s += formatName(f.sampleFmt);
            if (f.bigEndian) s += " big endian";
            s += ")";
        }
        if (f.swapIQ) s += " (Q, I)";
        return s;
    }
    s += ", ";
    s += formatName(f.sampleFmt);
    if (f.bigEndian) s += " big endian";
    if (f.swapIQ) s += " (Q, I)";
    return s;
}

bool resolveStreamFormat(const std::string& id, StreamFormat& f, std::string& err) {
    if (isBuiltinFormatId(id)) {
        f = builtinStreamFormat();
        return true;
    }
    const std::string dir = streamFormatDir();
    const auto list = scanStreamFormats(dir);
    const StreamFormatEntry* e = findStreamFormat(list, id);
    if (!e) {
        err = "no ZMQ format '" + id + "' (" + (dir.empty() ? "no config folder" : "no zmq_" + id + ".ini in " + dir) + ")";
        return false;
    }
    if (!e->ok()) {
        err = "ZMQ format '" + id + "': " + e->error;
        return false;
    }
    f = e->format;
    return true;
}

bool formatEndpoint(const StreamFormat& f, const std::string& typed, FormatEndpoint& out, std::string& err) {
    out = FormatEndpoint{};
    std::string t = trim(typed.empty() ? f.address : typed);
    const bool dish = f.socket == FormatSocket::Dish;
    for (const char* scheme : {"udp://", "tcp://"}) {
        if (t.rfind(scheme, 0) == 0) {
            const bool udp = scheme[0] == 'u';
            if (udp != dish) {
                err = std::string("the format '") + f.name + "' receives on " + (dish ? "udp" : "tcp") + ", not " +
                      (udp ? "udp" : "tcp");
                return false;
            }
            t = t.substr(6);
        }
    }
    if (dish) {
        if (t.empty()) t = defaultStreamAddress();
        if (!parseStreamAddress(t, out.udp, err)) return false;
        out.endpoint = out.udp.endpoint();
        out.text = out.udp.text();
        out.port = out.udp.port;
        return true;
    }
    if (t.empty()) {
        err = "no address: type host:port in the Address field" +
              (f.path.empty() ? std::string() : ", or set address in [socket] of " + f.path);
        return false;
    }
    const size_t colon = t.rfind(':');
    const std::string host = colon == std::string::npos ? t : t.substr(0, colon);
    const std::string port = colon == std::string::npos ? "" : t.substr(colon + 1);
    uint64_t p = 0;
    bool neg = false;
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos || !parseWhole(port, false, p, neg) ||
        p < 1 || p > 65535) {
        err = "'" + t + "': write host:port, e.g. 127.0.0.1:5555";
        return false;
    }
    if (host.empty() || host.find_first_of(" \t;/") != std::string::npos) {
        err = "'" + t + "': bad host '" + host + "'";
        return false;
    }
    if (host == "*" && !f.bind) {
        err = "'" + t + "': * (all interfaces) is for mode = bind; to connect, type the source's address";
        return false;
    }
    out.port = (int)p;
    out.text = host + ":" + std::to_string(p);
    out.endpoint = "tcp://" + out.text;
    return true;
}

int fieldTypeBits(FieldType t) { return 8 * typeBytes(t); }

int streamSeqBits(const StreamFormat& f) {
    return f.encoding == FormatEncoding::Protobuf ? protoCounterBits(f.proto.seq.type) : fieldTypeBits(f.seq.type);
}

bool streamRateInMessage(const StreamFormat& f) {
    return f.encoding == FormatEncoding::Protobuf ? f.proto.rate.set : f.rate.set;
}

MessageCounter::MessageCounter(int bits) : mask_(bits >= 64 ? ~0ULL : (1ULL << bits) - 1) {}

MessageCounter::Result MessageCounter::next(uint64_t seq) {
    Result r;
    seq &= mask_;
    if (!started_) {
        started_ = true;
        expected_ = (seq + 1) & mask_;
        r.event = Event::First;
        return r;
    }
    const uint64_t ahead = (seq - expected_) & mask_;  // 0 = in order
    const uint64_t behind = (expected_ - seq) & mask_;  // 1 = a repeat of the last message
    if (ahead == 0) {
        r.event = Event::InOrder;
    } else if (behind <= kMaxLate && behind <= mask_ / 2) {
        r.event = Event::Late;
        return r;  // expected_ stays
    } else if (ahead <= kMaxLost && ahead <= mask_ / 2) {
        r.event = Event::Lost;
        r.lost = ahead;
    } else {
        r.event = Event::Restart;
    }
    expected_ = (seq + 1) & mask_;
    return r;
}

// ---------------------------------------------------------------------------
// Publishing (phase 6)

namespace {
constexpr size_t kDatagramBytes = kStreamHeaderBytes + kStreamMaxPayload;  // as the built-in format
constexpr size_t kTcpMessageBytes = 256u << 10;

bool isAnyHost(const std::string& h) { return h == "*" || h == "0.0.0.0"; }
bool isLoopback(const std::string& h) { return h == "localhost" || h.rfind("127.", 0) == 0; }
}  // namespace

bool checkPublishFormat(const StreamFormat& f, std::string& err) {
    if (f.builtin) return true;
    const bool topic = f.socket == FormatSocket::Sub && !f.topic.empty();
    if (topic && f.frame != 0 && f.frame != 2) {
        err = "the format '" + f.name + "' has a topic: wvt sends it as frame 1 and the data as frame 2, so set frames = last or 2";
        return false;
    }
    if (!topic && f.frame > 1) {
        err = "the format '" + f.name + "' reads frame " + std::to_string(f.frame) +
              ": wvt sends one frame (two with a topic), so set frames = last or 1";
        return false;
    }
    if (publishMaxSamples(f) == 0) {
        err = "the format '" + f.name + "': no sample fits in a message" +
              (f.encoding == FormatEncoding::Protobuf ? std::string(" (max_bytes)")
                                                      : " after the header of " + std::to_string(f.headerBytes) + " bytes");
        return false;
    }
    return true;
}

size_t publishMaxBytes(const StreamFormat& f) {
    if (f.builtin) return kDatagramBytes;
    const size_t limit = f.socket == FormatSocket::Dish ? kDatagramBytes : std::min(f.maxBytes, kTcpMessageBytes);
    if (f.encoding == FormatEncoding::Protobuf) {
        size_t bytes = 0;
        protoMaxSamples(f, limit, &bytes);
        return bytes;
    }
    return f.headerBytes + publishMaxSamples(f) * (size_t)formatBytesPerSample(f.sampleFmt);
}

size_t publishMaxSamples(const StreamFormat& f) {
    const size_t limit = f.socket == FormatSocket::Dish ? kDatagramBytes : std::min(f.maxBytes, kTcpMessageBytes);
    if (f.encoding == FormatEncoding::Protobuf) return protoMaxSamples(f, std::min(limit, f.maxBytes));
    const auto bps = (size_t)formatBytesPerSample(f.sampleFmt);
    size_t n = limit > f.headerBytes ? (limit - f.headerBytes) / bps : 0;
    if (f.count.set) {
        const int bits = 8 * typeBytes(f.count.type) - (typeIsSigned(f.count.type) ? 1 : 0);
        if (bits < 64) n = std::min<size_t>(n, (size_t)((1ULL << bits) - 1));
    }
    return n;
}

bool publishEndpoint(const StreamFormat& f, const std::string& typed, FormatEndpoint& out, std::string& err) {
    StreamFormat p = f;
    if (f.socket != FormatSocket::Dish) p.bind = !f.bind;  // a dish binds; its RADIO sends to the address
    return formatEndpoint(p, typed, out, err);
}

bool encodeStreamMessage(const StreamFormat& f, const StreamHeader& h, uint64_t seq, SampleFormat native,
                         const uint8_t* samples, size_t count, std::vector<uint8_t>& out, std::string& why) {
    if (f.encoding == FormatEncoding::Protobuf) return encodeProtoMessage(f, h, seq, native, samples, count, out, why);
    const auto bps = (size_t)formatBytesPerSample(f.sampleFmt);
    out.assign(f.headerBytes + count * bps, 0);
    uint8_t* p = out.data();
    if (f.fc.set && !writeNumber(f.fc, h.fc, "fc", p, why)) return false;
    if (f.rate.set && !writeNumber(f.rate, h.rate, "rate", p, why)) return false;
    if (f.gain.set && !writeNumber(f.gain, h.gain, "gain", p, why)) return false;
    if (f.timeNs.set) {
        if (!typeIsFloat(f.timeNs.type) && f.timeNs.scale == 1 && typeBytes(f.timeNs.type) == 8)
            writeCounter(f.timeNs, h.timeNs, p);  // exact: a double has only 53 bits
        else if (!writeNumber(f.timeNs, (double)h.timeNs, "time_ns", p, why))
            return false;
    }
    if (f.index.set) writeCounter(f.index, h.index, p);
    if (f.seq.set) writeCounter(f.seq, seq, p);
    if (f.count.set && !writeNumber(f.count, (double)count, "count", p, why)) return false;
    for (const FormatMatch& m : f.matches) std::copy(m.bytes.begin(), m.bytes.end(), p + m.offset);

    if (count) encodeSampleBody(f.sampleFmt, f.bigEndian, f.swapIQ, native, samples, count, p + f.headerBytes);
    return true;
}

bool publishReachesSubscriber(const StreamFormat& rx, const std::string& rxAddress, const StreamFormat& tx,
                              const std::string& txAddress) {
    if (rx.socket != tx.socket) return false;  // RADIO-DISH, PUB-SUB, PUSH-PULL only
    FormatEndpoint a, b;
    std::string err;
    if (!formatEndpoint(rx, rxAddress, a, err) || !publishEndpoint(tx, txAddress, b, err)) return false;
    if (rx.socket == FormatSocket::Dish) return rx.group == tx.group && a.udp.ip == b.udp.ip && a.udp.port == b.udp.port;
    // The publisher has the other mode than its file: it meets a subscriber of the same mode.
    if (rx.bind != tx.bind || a.port != b.port) return false;
    const std::string ha = a.text.substr(0, a.text.rfind(':')), hb = b.text.substr(0, b.text.rfind(':'));
    return ha == hb || isAnyHost(ha) || isAnyHost(hb) || (isLoopback(ha) && isLoopback(hb));
}
