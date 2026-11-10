#include "net/protobuf_wire.h"

namespace pb {

const char* wireName(Wire w) {
    switch (w) {
        case Wire::Varint: return "varint";
        case Wire::I64: return "i64";
        case Wire::Len: return "len";
        case Wire::I32: return "i32";
    }
    return "?";
}

size_t readVarint(const uint8_t* p, size_t n, uint64_t& v) {
    v = 0;
    const size_t max = n < (size_t)kMaxVarintBytes ? n : (size_t)kMaxVarintBytes;
    for (size_t i = 0; i < max; ++i) {
        v |= (uint64_t)(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) return i + 1;
    }
    return 0;
}

bool Reader::next(Field& f, std::string& why) {
    why.clear();
    if (p_ == end_) return false;
    uint64_t tag = 0;
    size_t k = readVarint(p_, (size_t)(end_ - p_), tag);
    if (k == 0) {
        why = "a field tag is cut off";
        return false;
    }
    p_ += k;
    const uint64_t number = tag >> 3;
    if (number == 0 || number > kMaxField) {
        why = "bad field number " + std::to_string(number);
        return false;
    }
    f = Field{};
    f.number = (uint32_t)number;
    const auto w = (unsigned)(tag & 7);
    const size_t left = (size_t)(end_ - p_);
    switch (w) {
        case 0:
            f.wire = Wire::Varint;
            k = readVarint(p_, left, f.value);
            if (k == 0) {
                why = "field " + std::to_string(number) + ": the varint is cut off";
                return false;
            }
            p_ += k;
            return true;
        case 1:
        case 5: {
            const size_t bytes = w == 1 ? 8 : 4;
            f.wire = w == 1 ? Wire::I64 : Wire::I32;
            if (left < bytes) {
                why = "field " + std::to_string(number) + ": the value is cut off";
                return false;
            }
            for (size_t i = 0; i < bytes; ++i) f.value |= (uint64_t)p_[i] << (8 * i);
            p_ += bytes;
            return true;
        }
        case 2: {
            f.wire = Wire::Len;
            uint64_t len = 0;
            k = readVarint(p_, left, len);
            if (k == 0 || len > left - k) {
                why = "field " + std::to_string(number) + ": the length is past the end of the message";
                return false;
            }
            f.data = p_ + k;
            f.size = (size_t)len;
            p_ += k + (size_t)len;
            return true;
        }
        default:
            why = "field " + std::to_string(number) + ": wire type " + std::to_string(w) +
                  (w == 3 || w == 4 ? " (group) is not supported" : " is not valid");
            return false;
    }
}

size_t varintSize(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

void putVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back((uint8_t)(v | 0x80));
        v >>= 7;
    }
    out.push_back((uint8_t)v);
}

void putTag(std::vector<uint8_t>& out, uint32_t number, Wire w) { putVarint(out, ((uint64_t)number << 3) | (uint64_t)w); }

void putFixed32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i)));
}

void putFixed64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (8 * i)));
}

void putLen(std::vector<uint8_t>& out, uint32_t number, const uint8_t* p, size_t n) {
    putTag(out, number, Wire::Len);
    putVarint(out, n);
    out.insert(out.end(), p, p + n);
}

}  // namespace pb
