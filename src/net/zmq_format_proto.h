#pragma once
// Internal to net/zmq_format.cpp and net/zmq_format_proto.cpp: the Protobuf
// formats (issue #118) and the sample conversions that both encodings use.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/protobuf_wire.h"
#include "net/zmq_format.h"

constexpr size_t kMaxProtoPath = 16;  // field numbers in one path

// "sint32" -> Sint32.
bool parseProtoType(const std::string& s, ProtoType& t);
// The wire type of a value of type t (Len for bytes and string).
pb::Wire protoWire(ProtoType t);
bool protoIsFloat(ProtoType t);    // double, float
bool protoIsInteger(ProtoType t);  // int, uint, sint, fixed, sfixed (not bool, enum)
bool protoIsSigned(ProtoType t);   // int, sint, sfixed, enum
// The value of the number `text` as it is on the wire for type t (a match).
bool protoRawFromText(ProtoType t, const std::string& text, uint64_t& raw, std::string& err);

// `count` samples at `body`, laid out as fmt / bigEndian / swapIQ, in the
// ring format `ring`: m.samples points into body, or into scratch after a
// conversion.
void decodeSampleBody(SampleFormat fmt, bool bigEndian, bool swapIQ, SampleFormat ring, const uint8_t* body,
                      size_t count, StreamMessage& m, std::vector<uint8_t>& scratch);
// The reverse: `count` samples of the ring format `native` to `body`
// (count * formatBytesPerSample(fmt) bytes), laid out as fmt / bigEndian /
// swapIQ.
void encodeSampleBody(SampleFormat fmt, bool bigEndian, bool swapIQ, SampleFormat native, const uint8_t* samples,
                      size_t count, uint8_t* body);

// decodeStreamMessage, encodeStreamMessage and publishMaxSamples for a
// format with encoding = protobuf.
bool decodeProtoMessage(const StreamFormat& f, const uint8_t* p, size_t n, StreamMessage& m, FormatExtra& extra,
                        std::vector<uint8_t>& scratch, std::string& why);
bool encodeProtoMessage(const StreamFormat& f, const StreamHeader& h, uint64_t seq, SampleFormat native,
                        const uint8_t* samples, size_t count, std::vector<uint8_t>& out, std::string& why);
// The most samples in a message of at most `limit` bytes, and the size of
// such a message.
size_t protoMaxSamples(const StreamFormat& f, size_t limit, size_t* bytes = nullptr);
// The bits of a counter field of type t (a signed type: one less, so that
// the counter that wraps is never negative).
int protoCounterBits(ProtoType t);
