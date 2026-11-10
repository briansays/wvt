#pragma once
// The Protocol Buffers wire format (issue #118), without libprotobuf: a
// reader that walks the fields of a message, and the writer helpers that the
// publisher uses. wvt does not read .proto files; a format file names the
// fields by number (net/zmq_format.h).
//
// A message is a list of fields. Each field is a tag (a varint: the field
// number << 3 | the wire type), then the value:
//   0 varint  1 to 10 bytes, 7 bits each, low bits first
//   1 i64     8 bytes, little endian (fixed64, sfixed64, double)
//   2 len     a varint length, then that many bytes (bytes, string, a
//             sub-message, a packed repeated field)
//   5 i32     4 bytes, little endian (fixed32, sfixed32, float)
// The group wire types 3 and 4 (deprecated) are rejected.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pb {

enum class Wire : uint8_t { Varint = 0, I64 = 1, Len = 2, I32 = 5 };

constexpr uint32_t kMaxField = (1u << 29) - 1;
constexpr int kMaxVarintBytes = 10;

const char* wireName(Wire w);

// One field of a message. For Len, data/size are the bytes (inside the
// buffer that is read); for the others, value is the varint or the bits.
struct Field {
    uint32_t number = 0;
    Wire wire = Wire::Varint;
    uint64_t value = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// Reads the fields of one message, in order. Every length is checked
// against the bytes that are left.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    // The next field: true; the end of the message: false with `why` empty;
    // a bad message: false with `why` set.
    bool next(Field& f, std::string& why);
    bool atEnd() const { return p_ == end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

// A varint at p (at most `n` bytes). The bytes that it takes, or 0 if it is
// cut off or longer than 10 bytes.
size_t readVarint(const uint8_t* p, size_t n, uint64_t& v);

inline uint64_t zigzag(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
inline int64_t unzigzag(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

size_t varintSize(uint64_t v);
inline size_t tagSize(uint32_t number) { return varintSize((uint64_t)number << 3); }

void putVarint(std::vector<uint8_t>& out, uint64_t v);
void putTag(std::vector<uint8_t>& out, uint32_t number, Wire w);
void putFixed32(std::vector<uint8_t>& out, uint32_t v);
void putFixed64(std::vector<uint8_t>& out, uint64_t v);
// A len field: the tag, the length, the bytes.
void putLen(std::vector<uint8_t>& out, uint32_t number, const uint8_t* p, size_t n);

}  // namespace pb
