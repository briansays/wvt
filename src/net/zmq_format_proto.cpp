// Protobuf message formats (issue #118): the decoder for the subscriber and
// the encoder for the publisher. The format file names the fields by number
// (net/zmq_format.h); the parser is in net/zmq_format.cpp.
#include "net/zmq_format_proto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

using pb::Wire;

struct TypeName {
    const char* name;
    ProtoType type;
};
constexpr TypeName kTypes[] = {
    {"double", ProtoType::Double},     {"float", ProtoType::Float},       {"int32", ProtoType::Int32},
    {"int64", ProtoType::Int64},       {"uint32", ProtoType::Uint32},     {"uint64", ProtoType::Uint64},
    {"sint32", ProtoType::Sint32},     {"sint64", ProtoType::Sint64},     {"fixed32", ProtoType::Fixed32},
    {"fixed64", ProtoType::Fixed64},   {"sfixed32", ProtoType::Sfixed32}, {"sfixed64", ProtoType::Sfixed64},
    {"bool", ProtoType::Bool},         {"enum", ProtoType::Enum},         {"bytes", ProtoType::Bytes},
    {"string", ProtoType::String},
};

bool is32(ProtoType t) {
    return t == ProtoType::Int32 || t == ProtoType::Uint32 || t == ProtoType::Sint32 || t == ProtoType::Fixed32 ||
           t == ProtoType::Sfixed32 || t == ProtoType::Enum;
}

// A value read from the wire, by its type.
struct Value {
    double d = 0;
    uint64_t u = 0;         // the value when it is a whole number that is not negative
    bool negative = false;
};

Value valueOf(ProtoType t, uint64_t raw) {
    Value v;
    auto setSigned = [&](int64_t s) {
        v.d = (double)s;
        v.negative = s < 0;
        v.u = v.negative ? 0 : (uint64_t)s;
    };
    switch (t) {
        case ProtoType::Double: memcpy(&v.d, &raw, sizeof v.d); break;
        case ProtoType::Float: {
            const auto b = (uint32_t)raw;
            float x = 0;
            memcpy(&x, &b, sizeof x);
            v.d = x;
            break;
        }
        case ProtoType::Int32:
        case ProtoType::Enum:
        case ProtoType::Sfixed32: setSigned((int32_t)(uint32_t)raw); break;
        case ProtoType::Int64:
        case ProtoType::Sfixed64: setSigned((int64_t)raw); break;
        case ProtoType::Sint32: setSigned((int32_t)pb::unzigzag((uint32_t)raw)); break;
        case ProtoType::Sint64: setSigned(pb::unzigzag(raw)); break;
        case ProtoType::Uint32:
        case ProtoType::Fixed32:
            v.u = (uint32_t)raw;
            v.d = (double)v.u;
            break;
        case ProtoType::Bool:
            v.u = raw != 0;
            v.d = (double)v.u;
            break;
        default:  // Uint64, Fixed64
            v.u = raw;
            v.d = (double)raw;
            break;
    }
    return v;
}

// A whole number as it is on the wire for type t. False if it does not fit.
bool rawFromInteger(ProtoType t, bool negative, uint64_t mag, uint64_t& raw) {
    const bool b32 = is32(t);
    if (negative) {
        if (!protoIsSigned(t)) return false;
        if (mag > (b32 ? (1ULL << 31) : (1ULL << 63))) return false;
        const int64_t s = mag == (1ULL << 63) ? std::numeric_limits<int64_t>::min() : -(int64_t)mag;
        switch (t) {
            case ProtoType::Sint32: raw = (uint32_t)pb::zigzag(s); return true;
            case ProtoType::Sint64: raw = pb::zigzag(s); return true;
            case ProtoType::Sfixed32: raw = (uint32_t)(int32_t)s; return true;
            default: raw = (uint64_t)s; return true;  // int32, int64, enum: sign-extended to 64 bits; sfixed64
        }
    }
    const uint64_t max = t == ProtoType::Bool ? 1
                         : b32 ? (protoIsSigned(t) ? (1ULL << 31) - 1 : (1ULL << 32) - 1)
                               : (protoIsSigned(t) ? (1ULL << 63) - 1 : std::numeric_limits<uint64_t>::max());
    if (mag > max) return false;
    raw = t == ProtoType::Sint32 || t == ProtoType::Sint64 ? pb::zigzag((int64_t)mag) : mag;
    return true;
}

// A number (v / the field's scale) as it is on the wire for type t. False
// (why set) if it does not fit.
bool rawFromNumber(ProtoType t, double v, const char* name, uint64_t& raw, std::string& why) {
    if (t == ProtoType::Double) {
        memcpy(&raw, &v, sizeof raw);
        return true;
    }
    if (t == ProtoType::Float) {
        const auto x = (float)v;
        uint32_t b = 0;
        memcpy(&b, &x, sizeof b);
        raw = b;
        return true;
    }
    const double r = std::nearbyint(v);
    if (std::isfinite(r) && std::fabs(r) < 1.8e19 &&
        rawFromInteger(t, r < 0, r < 0 ? (uint64_t)-r : (uint64_t)r, raw))
        return true;
    char buf[64];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) snprintf(buf, sizeof buf, "%.0f", v);
    else snprintf(buf, sizeof buf, "%.9g", v);
    why = std::string(name) + " " + buf + " does not fit in " + protoTypeName(t);
    return false;
}

// A counter (index, seq): its low bits, never negative.
uint64_t rawFromCounter(ProtoType t, uint64_t v) {
    const int bits = protoCounterBits(t);
    if (bits < 64) v &= (1ULL << bits) - 1;
    uint64_t raw = 0;
    rawFromInteger(t, false, v, raw);
    return raw;
}

bool samePath(const std::vector<uint32_t>& path, const uint32_t* prefix, size_t depth) {
    return path.size() == depth && std::equal(path.begin(), path.end(), prefix);
}
bool underPath(const std::vector<uint32_t>& path, const uint32_t* prefix, size_t depth) {
    return path.size() > depth && std::equal(path.begin(), path.begin() + (ptrdiff_t)depth, prefix);
}

std::string pathText(const std::vector<uint32_t>& path) {
    std::string s;
    for (uint32_t n : path) s += (s.empty() ? "" : ".") + std::to_string(n);
    return s;
}

// What the decoder looks for in a message.
struct Scalar {
    const ProtoField* f = nullptr;
    bool seen = false;
    uint64_t raw = 0;
};
struct MatchState {
    const ProtoMatch* m = nullptr;
    bool seen = false;
    uint64_t raw = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};
// One run of array elements: a packed (or bytes) value, or one element of
// an unpacked repeated field.
struct Chunk {
    bool packed = true;
    const uint8_t* data = nullptr;  // packed: the bytes
    size_t size = 0;
    uint64_t raw = 0;               // unpacked: the element
};
struct Array {
    const ProtoField* f = nullptr;
    std::vector<Chunk>* chunks = nullptr;
};

struct Walk {
    Scalar scalars[6];
    int nScalars = 0;
    MatchState matches[32];
    int nMatches = 0;
    Array arrays[3];
    int nArrays = 0;
    uint32_t prefix[kMaxProtoPath] = {};

    bool wanted(size_t depth) const {  // a field under prefix[0..depth) is wanted
        for (int k = 0; k < nScalars; ++k)
            if (underPath(scalars[k].f->path, prefix, depth)) return true;
        for (int k = 0; k < nMatches; ++k)
            if (underPath(matches[k].m->path, prefix, depth)) return true;
        for (int k = 0; k < nArrays; ++k)
            if (underPath(arrays[k].f->path, prefix, depth)) return true;
        return false;
    }

    static bool wrongWire(const pb::Field& fd, Wire want, const std::vector<uint32_t>& path, ProtoType t,
                          std::string& why) {
        why = "field " + pathText(path) + " (" + protoTypeName(t) + ") has the wire type " + pb::wireName(fd.wire) +
              ", not " + pb::wireName(want);
        return false;
    }

    bool run(const uint8_t* p, size_t n, size_t depth, std::string& why) {
        pb::Reader r(p, n);
        pb::Field fd;
        while (r.next(fd, why)) {
            if (depth >= kMaxProtoPath) continue;  // no path is this long
            prefix[depth] = fd.number;
            const size_t d = depth + 1;
            for (int k = 0; k < nScalars; ++k) {
                Scalar& s = scalars[k];
                if (!samePath(s.f->path, prefix, d)) continue;
                const Wire w = protoWire(s.f->type);
                if (fd.wire != w) return wrongWire(fd, w, s.f->path, s.f->type, why);
                s.seen = true;
                s.raw = fd.value;  // the last one wins, as in Protobuf
            }
            for (int k = 0; k < nMatches; ++k) {
                MatchState& s = matches[k];
                if (!samePath(s.m->path, prefix, d)) continue;
                s.seen = true;
                if (s.m->kind == ProtoMatch::Kind::Present) continue;
                const Wire w = protoWire(s.m->type);
                if (fd.wire != w) return wrongWire(fd, w, s.m->path, s.m->type, why);
                s.raw = fd.value;
                s.data = fd.data;
                s.size = fd.size;
            }
            for (int k = 0; k < nArrays; ++k) {
                Array& a = arrays[k];
                if (!samePath(a.f->path, prefix, d)) continue;
                const Wire w = protoWire(a.f->type);
                if (fd.wire == Wire::Len) a.chunks->push_back({true, fd.data, fd.size, 0});
                else if (fd.wire == w) a.chunks->push_back({false, nullptr, 0, fd.value});
                else return wrongWire(fd, Wire::Len, a.f->path, a.f->type, why);
            }
            if (wanted(d)) {
                if (fd.wire != Wire::Len) {
                    why = "field " + pathText(std::vector<uint32_t>(prefix, prefix + d)) +
                          " is not a sub-message (wire type " + pb::wireName(fd.wire) + ")";
                    return false;
                }
                if (!run(fd.data, fd.size, d, why)) {
                    if (why.empty()) why = "a sub-message is not valid";
                    return false;
                }
            }
        }
        return why.empty();
    }
};

// The elements of an array, as raw wire values, in order. False (why set)
// for a packed run that is not whole elements.
template <class Fn>
bool forEachElement(const Array& a, Fn&& fn, std::string& why) {
    const Wire w = protoWire(a.f->type);
    for (const Chunk& c : *a.chunks) {
        if (!c.packed) {
            fn(c.raw);
            continue;
        }
        const uint8_t* p = c.data;
        size_t left = c.size;
        if (w == Wire::Varint) {
            while (left) {
                uint64_t v = 0;
                const size_t k = pb::readVarint(p, left, v);
                if (k == 0) {
                    why = "field " + pathText(a.f->path) + ": a packed varint is cut off";
                    return false;
                }
                fn(v);
                p += k;
                left -= k;
            }
        } else {
            const size_t bytes = w == Wire::I64 ? 8 : 4;
            if (left % bytes) {
                why = "field " + pathText(a.f->path) + ": " + std::to_string(left) + " bytes are not whole " +
                      protoTypeName(a.f->type) + " values";
                return false;
            }
            for (; left; left -= bytes, p += bytes) {
                uint64_t v = 0;
                for (size_t i = 0; i < bytes; ++i) v |= (uint64_t)p[i] << (8 * i);
                fn(v);
            }
        }
    }
    return true;
}

// The range of the values of an integer sample format.
void sampleRange(SampleFormat f, int64_t& lo, int64_t& hi) {
    switch (f) {
        case SampleFormat::CU8: lo = 0, hi = 255; break;
        case SampleFormat::CI8: lo = -128, hi = 127; break;
        case SampleFormat::CI32: lo = std::numeric_limits<int32_t>::min(), hi = std::numeric_limits<int32_t>::max(); break;
        default: lo = -32768, hi = 32767; break;
    }
}

// The array elements to `buf` at positions start, start + step, ...: as
// float (float, double), or as values of `fmt` (integer types). `n` is the
// number of elements. False (why set) for a value out of range.
bool putElements(const Array& a, SampleFormat fmt, uint8_t* buf, size_t start, size_t step, size_t n, std::string& why) {
    const ProtoType t = a.f->type;
    size_t pos = start, k = 0;
    if (protoIsFloat(t)) {
        return forEachElement(a, [&](uint64_t raw) {
            if (k++ >= n) return;
            const auto x = (float)valueOf(t, raw).d;
            memcpy(buf + 4 * pos, &x, 4);
            pos += step;
        }, why);
    }
    int64_t lo = 0, hi = 0;
    sampleRange(fmt, lo, hi);
    const auto bytes = (size_t)formatBytesPerSample(fmt) / 2;
    bool range = true;
    int64_t bad = 0;
    const bool ok = forEachElement(a, [&](uint64_t raw) {
        if (k++ >= n) return;
        const Value v = valueOf(t, raw);
        const int64_t s = v.negative ? (int64_t)v.d : (v.u > (uint64_t)hi ? hi + 1 : (int64_t)v.u);
        if (s < lo || s > hi) {
            if (range) bad = s;
            range = false;
        }
        const auto x = (uint64_t)s;
        uint8_t* o = buf + pos * bytes;
        for (size_t b = 0; b < bytes; ++b) o[b] = (uint8_t)(x >> (8 * b));
        pos += step;
    }, why);
    if (!ok) return false;
    if (!range) {
        why = "the sample value " + std::to_string(bad) + " does not fit in " + formatName(fmt);
        return false;
    }
    return true;
}

size_t countElements(const Array& a, std::string& why) {
    if (protoWire(a.f->type) != Wire::Varint) {
        // Fixed size: no need to read them.
        const size_t bytes = protoWire(a.f->type) == Wire::I64 ? 8 : 4;
        size_t n = 0;
        for (const Chunk& c : *a.chunks) {
            if (!c.packed) {
                ++n;
                continue;
            }
            if (c.size % bytes) {
                why = "field " + pathText(a.f->path) + ": " + std::to_string(c.size) + " bytes are not whole " +
                      protoTypeName(a.f->type) + " values";
                return SIZE_MAX;
            }
            n += c.size / bytes;
        }
        return n;
    }
    size_t n = 0;
    if (!forEachElement(a, [&](uint64_t) { ++n; }, why)) return SIZE_MAX;
    return n;
}

// The size of one element on the wire, at most, for the publisher.
size_t elementBytes(const ProtoField& f, SampleFormat fmt) {
    switch (protoWire(f.type)) {
        case Wire::I32: return 4;
        case Wire::I64: return 8;
        default: break;
    }
    int64_t lo = 0, hi = 0;
    sampleRange(fmt, lo, hi);
    if (f.type == ProtoType::Sint32 || f.type == ProtoType::Sint64)
        return std::max(pb::varintSize(pb::zigzag(lo)), pb::varintSize(pb::zigzag(hi)));
    if (lo < 0) return 10;  // int32, int64: a negative value takes 10 bytes
    return pb::varintSize((uint64_t)hi);
}

// One thing that the encoder writes: a scalar, a match, or the bytes of an
// array (with its tags).
struct Item {
    const std::vector<uint32_t>* path = nullptr;
    enum class Kind : uint8_t { Raw, Bytes, Present, Verbatim } kind = Kind::Raw;
    Wire wire = Wire::Varint;
    uint64_t raw = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

void putRaw(std::vector<uint8_t>& out, Wire w, uint64_t raw) {
    switch (w) {
        case Wire::Varint: pb::putVarint(out, raw); break;
        case Wire::I32: pb::putFixed32(out, (uint32_t)raw); break;
        case Wire::I64: pb::putFixed64(out, raw); break;
        case Wire::Len: break;
    }
}

// Writes the items under prefix[0..depth), in the order of the field
// numbers.
void encodeLevel(const std::vector<Item>& items, std::vector<uint32_t>& prefix, std::vector<uint8_t>& out) {
    const size_t depth = prefix.size();
    std::vector<uint32_t> numbers;
    for (const Item& it : items)
        if (underPath(*it.path, prefix.data(), depth)) numbers.push_back((*it.path)[depth]);
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    for (uint32_t num : numbers) {
        prefix.push_back(num);
        bool deeper = false, present = false;
        for (const Item& it : items) {
            if (!samePath(*it.path, prefix.data(), depth + 1)) {
                deeper = deeper || underPath(*it.path, prefix.data(), depth + 1);
                continue;
            }
            switch (it.kind) {
                case Item::Kind::Raw:
                    pb::putTag(out, num, it.wire);
                    putRaw(out, it.wire, it.raw);
                    break;
                case Item::Kind::Bytes: pb::putLen(out, num, it.data, it.size); break;
                case Item::Kind::Verbatim: out.insert(out.end(), it.data, it.data + it.size); break;
                case Item::Kind::Present: present = true; break;
            }
        }
        if (deeper) {
            std::vector<uint8_t> sub;
            encodeLevel(items, prefix, sub);
            pb::putLen(out, num, sub.data(), sub.size());
        } else if (present) {
            pb::putLen(out, num, nullptr, 0);  // an empty sub-message (or string)
        }
        prefix.pop_back();
    }
}

// The elements of an array to `out`, with the tags: packed (one len field)
// or unpacked (a tag for each element).
void putArray(const ProtoField& f, const std::vector<uint64_t>& raws, std::vector<uint8_t>& out) {
    const Wire w = protoWire(f.type);
    const uint32_t num = f.path.back();
    if (!f.packed) {
        for (uint64_t r : raws) {
            pb::putTag(out, num, w);
            putRaw(out, w, r);
        }
        return;
    }
    thread_local std::vector<uint8_t> body;
    body.clear();
    for (uint64_t r : raws) putRaw(body, w, r);
    pb::putLen(out, num, body.data(), body.size());
}

}  // namespace

const char* protoTypeName(ProtoType t) {
    for (const TypeName& n : kTypes)
        if (n.type == t) return n.name;
    return "?";
}

bool parseProtoType(const std::string& s, ProtoType& t) {
    for (const TypeName& n : kTypes) {
        if (s == n.name) {
            t = n.type;
            return true;
        }
    }
    return false;
}

pb::Wire protoWire(ProtoType t) {
    switch (t) {
        case ProtoType::Double:
        case ProtoType::Fixed64:
        case ProtoType::Sfixed64: return Wire::I64;
        case ProtoType::Float:
        case ProtoType::Fixed32:
        case ProtoType::Sfixed32: return Wire::I32;
        case ProtoType::Bytes:
        case ProtoType::String: return Wire::Len;
        default: return Wire::Varint;
    }
}

bool protoIsFloat(ProtoType t) { return t == ProtoType::Double || t == ProtoType::Float; }

bool protoIsInteger(ProtoType t) {
    return !protoIsFloat(t) && t != ProtoType::Bool && t != ProtoType::Enum && t != ProtoType::Bytes &&
           t != ProtoType::String;
}

bool protoIsSigned(ProtoType t) {
    return t == ProtoType::Int32 || t == ProtoType::Int64 || t == ProtoType::Sint32 || t == ProtoType::Sint64 ||
           t == ProtoType::Sfixed32 || t == ProtoType::Sfixed64 || t == ProtoType::Enum;
}

int protoCounterBits(ProtoType t) {
    const int bits = is32(t) ? 32 : 64;
    return protoIsSigned(t) ? bits - 1 : bits;
}

bool protoRawFromText(ProtoType t, const std::string& text, uint64_t& raw, std::string& err) {
    if (protoIsFloat(t)) {
        errno = 0;
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (text.empty() || errno != 0 || end != text.c_str() + text.size() || !std::isfinite(d)) {
            err = "bad value '" + text + "'";
            return false;
        }
        std::string why;
        return rawFromNumber(t, d, "", raw, why);
    }
    if (t == ProtoType::Bool && (text == "true" || text == "false")) {
        raw = text == "true";
        return true;
    }
    std::string s = text;
    const bool negative = !s.empty() && s[0] == '-';
    if (negative) s = s.substr(1);
    errno = 0;
    char* end = nullptr;
    const unsigned long long mag = s.empty() || !std::isxdigit((unsigned char)s[0]) ? 0 : std::strtoull(s.c_str(), &end, 0);
    if (s.empty() || !std::isxdigit((unsigned char)s[0]) || errno != 0 || end != s.c_str() + s.size()) {
        err = "bad value '" + text + "'";
        return false;
    }
    if (!rawFromInteger(t, negative, mag, raw)) {
        err = text + " does not fit in " + protoTypeName(t);
        return false;
    }
    return true;
}

bool decodeProtoMessage(const StreamFormat& f, const uint8_t* p, size_t n, StreamMessage& m, FormatExtra& extra,
                        std::vector<uint8_t>& scratch, std::string& why) {
    const ProtoLayout& L = f.proto;
    thread_local std::vector<Chunk> chunks[3];
    Walk w;
    for (const ProtoField* fld : {&L.fc, &L.rate, &L.gain, &L.timeNs, &L.index, &L.seq})
        if (fld->set) w.scalars[w.nScalars++].f = fld;
    for (const ProtoMatch& mt : L.matches) {
        if (w.nMatches == (int)(sizeof w.matches / sizeof w.matches[0])) break;  // the parser limits them
        w.matches[w.nMatches++].m = &mt;
    }
    for (const ProtoField* fld : {&L.samples, &L.i, &L.q}) {
        if (!fld->set) continue;
        chunks[w.nArrays].clear();
        w.arrays[w.nArrays] = {fld, &chunks[w.nArrays]};
        ++w.nArrays;
    }
    if (!w.run(p, n, 0, why)) {
        if (why.empty()) why = "not a valid protobuf message";
        return false;
    }

    // A field that is not in the message has the value 0 (proto3 leaves it out).
    for (int k = 0; k < w.nMatches; ++k) {
        const MatchState& s = w.matches[k];
        bool ok = true;
        switch (s.m->kind) {
            case ProtoMatch::Kind::Present: ok = s.seen; break;
            case ProtoMatch::Kind::Value: ok = (s.seen ? s.raw : 0) == s.m->raw; break;
            case ProtoMatch::Kind::Text:
                ok = s.size == s.m->text.size() && (s.size == 0 || memcmp(s.data, s.m->text.data(), s.size) == 0);
                break;
        }
        if (!ok) {
            why = "match." + s.m->name + " does not match";
            return false;
        }
    }
    auto scalar = [&](const ProtoField& fld) -> const Scalar* {
        for (int k = 0; k < w.nScalars; ++k)
            if (w.scalars[k].f == &fld) return &w.scalars[k];
        return nullptr;
    };
    StreamHeader& h = m.h;
    h.kind = StreamKind::Data;
    h.fmt = streamRingFormat(f);
    auto number = [&](const ProtoField& fld, double fixed) {
        const Scalar* s = scalar(fld);
        return s && s->seen ? valueOf(fld.type, s->raw).d * fld.scale : fixed;
    };
    h.fc = number(L.fc, f.fixedFc);
    h.rate = number(L.rate, f.fixedRate);
    h.gain = number(L.gain, f.fixedGain);
    if (!std::isfinite(h.rate) || h.rate <= 0 || h.rate > 1e10) {
        why = scalar(L.rate) && !scalar(L.rate)->seen ? "no rate in the message (field " + pathText(L.rate.path) + ")"
                                                      : "bad sample rate";
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
    if (const Scalar* s = scalar(L.timeNs); s && s->seen) {
        const Value v = valueOf(L.timeNs.type, s->raw);
        if (!protoIsFloat(L.timeNs.type) && L.timeNs.scale == 1) {
            h.timeNs = v.u;  // exact: a double has only 53 bits
        } else {
            const double t = v.d * L.timeNs.scale;
            h.timeNs = std::isfinite(t) && t > 0 && t < 1.8e19 ? (uint64_t)t : 0;
        }
    }
    if (const Scalar* s = scalar(L.index)) {
        const Value v = valueOf(L.index.type, s->seen ? s->raw : 0);
        if (v.negative) {
            why = "negative sample index";
            return false;
        }
        h.index = v.u;
        extra.hasIndex = true;
    }
    if (const Scalar* s = scalar(L.seq)) {
        const Value v = valueOf(L.seq.type, s->seen ? s->raw : 0);
        if (v.negative) {
            why = "negative message counter";
            return false;
        }
        extra.seq = v.u;
        extra.hasSeq = true;
    }

    // The samples.
    thread_local std::vector<uint8_t> raw;
    if (L.samples.set && L.samples.type == ProtoType::Bytes) {
        const Array& a = w.arrays[0];
        const uint8_t* body = nullptr;
        size_t bytes = 0;
        if (a.chunks->size() == 1) {
            body = a.chunks->front().data;
            bytes = a.chunks->front().size;
        } else {
            raw.clear();
            for (const Chunk& c : *a.chunks) raw.insert(raw.end(), c.data, c.data + c.size);
            body = raw.data();
            bytes = raw.size();
        }
        const auto bps = (size_t)formatBytesPerSample(f.sampleFmt);
        if (bytes % bps) {
            why = "field " + pathText(L.samples.path) + ": " + std::to_string(bytes) + " bytes are not whole samples of " +
                  std::to_string(bps) + " bytes";
            return false;
        }
        const size_t count = bytes / bps;
        h.count = (uint32_t)count;
        decodeSampleBody(f.sampleFmt, f.bigEndian, f.swapIQ, h.fmt, body, count, m, scratch);
        if (m.samples == raw.data() && count) {  // into scratch, where the caller expects it
            scratch.assign(raw.begin(), raw.begin() + (ptrdiff_t)m.sampleBytes);
            m.samples = scratch.data();
        }
        return true;
    }

    const ProtoType et = L.samples.set ? L.samples.type : L.i.type;
    const SampleFormat elemFmt = protoIsFloat(et) ? SampleFormat::CF32 : f.sampleFmt;
    const auto bps = (size_t)formatBytesPerSample(elemFmt);
    size_t count = 0;
    if (L.samples.set) {
        const Array& a = w.arrays[0];
        const size_t values = countElements(a, why);
        if (values == SIZE_MAX) return false;
        if (values % 2) {
            why = "field " + pathText(L.samples.path) + " has " + std::to_string(values) + " values: not I and Q pairs";
            return false;
        }
        count = values / 2;
        // Packed float in one run, I first: the samples are cf32 in the message.
        if (et == ProtoType::Float && !f.swapIQ && a.chunks->size() == 1 && a.chunks->front().packed) {
            h.count = (uint32_t)count;
            m.samples = count ? a.chunks->front().data : nullptr;
            m.sampleBytes = count * 8;
            return true;
        }
        raw.assign(count * bps, 0);
        if (!putElements(a, elemFmt, raw.data(), 0, 1, 2 * count, why)) return false;
    } else {
        const Array& ai = w.arrays[0];
        const Array& aq = w.arrays[1];
        const size_t ni = countElements(ai, why);
        const size_t nq = ni == SIZE_MAX ? 0 : countElements(aq, why);
        if (ni == SIZE_MAX || nq == SIZE_MAX) return false;
        if (ni != nq) {
            why = "i has " + std::to_string(ni) + " values, q has " + std::to_string(nq);
            return false;
        }
        count = ni;
        raw.assign(count * bps, 0);
        if (!putElements(ai, elemFmt, raw.data(), 0, 2, count, why) ||
            !putElements(aq, elemFmt, raw.data(), 1, 2, count, why))
            return false;
    }
    h.count = (uint32_t)count;
    decodeSampleBody(elemFmt, false, f.swapIQ && L.samples.set, h.fmt, raw.data(), count, m, scratch);
    if (count && m.samples == raw.data()) {
        scratch.swap(raw);
        m.samples = scratch.data();
    }
    if (!count) m.samples = nullptr;
    return true;
}

bool encodeProtoMessage(const StreamFormat& f, const StreamHeader& h, uint64_t seq, SampleFormat native,
                        const uint8_t* samples, size_t count, std::vector<uint8_t>& out, std::string& why) {
    const ProtoLayout& L = f.proto;
    std::vector<Item> items;
    items.reserve(16);
    auto number = [&](const ProtoField& fld, double v, const char* name) {
        if (!fld.set) return true;
        Item it;
        it.path = &fld.path;
        it.wire = protoWire(fld.type);
        if (!rawFromNumber(fld.type, v / fld.scale, name, it.raw, why)) return false;
        items.push_back(it);
        return true;
    };
    auto counter = [&](const ProtoField& fld, uint64_t v) {
        if (!fld.set) return;
        Item it;
        it.path = &fld.path;
        it.wire = protoWire(fld.type);
        it.raw = rawFromCounter(fld.type, v);
        items.push_back(it);
    };
    if (!number(L.fc, h.fc, "fc") || !number(L.rate, h.rate, "rate") || !number(L.gain, h.gain, "gain")) return false;
    if (L.timeNs.set) {
        if (!protoIsFloat(L.timeNs.type) && L.timeNs.scale == 1 && !is32(L.timeNs.type)) counter(L.timeNs, h.timeNs);
        else if (!number(L.timeNs, (double)h.timeNs, "time_ns")) return false;
    }
    counter(L.index, h.index);
    counter(L.seq, seq);
    for (const ProtoMatch& mt : L.matches) {
        Item it;
        it.path = &mt.path;
        switch (mt.kind) {
            case ProtoMatch::Kind::Value:
                it.wire = protoWire(mt.type);
                it.raw = mt.raw;
                break;
            case ProtoMatch::Kind::Text:
                it.kind = Item::Kind::Bytes;
                it.data = (const uint8_t*)mt.text.data();
                it.size = mt.text.size();
                break;
            case ProtoMatch::Kind::Present: it.kind = Item::Kind::Present; break;
        }
        items.push_back(it);
    }

    // The samples.
    thread_local std::vector<uint8_t> arr[2];
    arr[0].clear();
    arr[1].clear();
    const ProtoType et = L.samples.set ? L.samples.type : L.i.type;
    if (et == ProtoType::Bytes) {
        thread_local std::vector<uint8_t> body;
        body.resize(count * (size_t)formatBytesPerSample(f.sampleFmt));
        if (count) encodeSampleBody(f.sampleFmt, f.bigEndian, f.swapIQ, native, samples, count, body.data());
        pb::putLen(arr[0], L.samples.path.back(), body.data(), body.size());
    } else {
        // The samples as cf32 (float, double) or as values of [samples] format.
        const SampleFormat elemFmt = protoIsFloat(et) ? SampleFormat::CF32 : f.sampleFmt;
        thread_local std::vector<uint8_t> buf;
        buf.resize(count * (size_t)formatBytesPerSample(elemFmt));
        if (count) encodeSampleBody(elemFmt, false, f.swapIQ && L.samples.set, native, samples, count, buf.data());
        const size_t half = (size_t)formatBytesPerSample(elemFmt) / 2;
        auto element = [&](size_t k) -> uint64_t {  // value k (I, Q, I, Q ...) on the wire
            const uint8_t* q = buf.data() + k * half;
            if (protoIsFloat(et)) {
                float x = 0;
                memcpy(&x, q, 4);
                std::string ignored;
                uint64_t r = 0;
                rawFromNumber(et, x, "", r, ignored);
                return r;
            }
            int64_t v = 0;
            switch (elemFmt) {
                case SampleFormat::CU8: v = q[0]; break;
                case SampleFormat::CI8: v = q[0] < 128 ? (int64_t)q[0] : (int64_t)q[0] - 256; break;
                case SampleFormat::CI32: {
                    int32_t x = 0;
                    memcpy(&x, q, 4);
                    v = x;
                    break;
                }
                default: {
                    int16_t x = 0;
                    memcpy(&x, q, 2);
                    v = x;
                    break;
                }
            }
            uint64_t r = 0;
            rawFromInteger(et, v < 0, v < 0 ? (uint64_t)-v : (uint64_t)v, r);  // the parser checks the range
            return r;
        };
        thread_local std::vector<uint64_t> raws;
        if (L.samples.set) {
            raws.resize(2 * count);
            for (size_t k = 0; k < 2 * count; ++k) raws[k] = element(k);
            putArray(L.samples, raws, arr[0]);
        } else {
            raws.resize(count);
            for (size_t k = 0; k < count; ++k) raws[k] = element(2 * k);
            putArray(L.i, raws, arr[0]);
            for (size_t k = 0; k < count; ++k) raws[k] = element(2 * k + 1);
            putArray(L.q, raws, arr[1]);
        }
    }
    const ProtoField* arrays[2] = {L.samples.set ? &L.samples : &L.i, L.samples.set ? nullptr : &L.q};
    for (int k = 0; k < 2; ++k) {
        if (!arrays[k]) continue;
        Item it;
        it.path = &arrays[k]->path;
        it.kind = Item::Kind::Verbatim;
        it.data = arr[k].data();
        it.size = arr[k].size();
        items.push_back(it);
    }
    out.clear();
    std::vector<uint32_t> prefix;
    encodeLevel(items, prefix, out);
    return true;
}

size_t protoMaxSamples(const StreamFormat& f, size_t limit, size_t* bytes) {
    const ProtoLayout& L = f.proto;
    // The fixed part: each scalar and match at its largest, and each
    // sub-message's tag and length.
    size_t fixed = 0;
    std::vector<std::vector<uint32_t>> parents;
    auto addPath = [&](const std::vector<uint32_t>& path) {
        for (size_t d = 1; d < path.size(); ++d) {
            std::vector<uint32_t> pre(path.begin(), path.begin() + (ptrdiff_t)d);
            if (std::find(parents.begin(), parents.end(), pre) == parents.end()) {
                fixed += pb::tagSize(pre.back()) + 5;
                parents.push_back(pre);
            }
        }
    };
    for (const ProtoField* fld : {&L.fc, &L.rate, &L.gain, &L.timeNs, &L.index, &L.seq}) {
        if (!fld->set) continue;
        const Wire w = protoWire(fld->type);
        fixed += pb::tagSize(fld->path.back()) + (w == Wire::I32 ? 4 : w == Wire::I64 ? 8 : 10);
        addPath(fld->path);
    }
    for (const ProtoMatch& mt : L.matches) {
        fixed += pb::tagSize(mt.path.back());
        switch (mt.kind) {
            case ProtoMatch::Kind::Value: fixed += 10; break;
            case ProtoMatch::Kind::Text: fixed += 5 + mt.text.size(); break;
            case ProtoMatch::Kind::Present: fixed += 1; break;
        }
        addPath(mt.path);
    }
    size_t perSample = 0;
    for (const ProtoField* fld : {&L.samples, &L.i, &L.q}) {
        if (!fld->set) continue;
        addPath(fld->path);
        fixed += pb::tagSize(fld->path.back()) + 5;  // packed or bytes: one tag and length
        if (fld->type == ProtoType::Bytes) {
            perSample += (size_t)formatBytesPerSample(f.sampleFmt);
            continue;
        }
        const size_t values = fld == &L.samples ? 2 : 1;
        const size_t elem = elementBytes(*fld, f.sampleFmt) + (fld->packed ? 0 : pb::tagSize(fld->path.back()));
        perSample += values * elem;
    }
    const size_t n = limit > fixed && perSample ? (limit - fixed) / perSample : 0;
    if (bytes) *bytes = n ? fixed + n * perSample : 0;
    return n;
}
