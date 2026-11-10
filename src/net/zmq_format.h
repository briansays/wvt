#pragma once
// ZMQ message formats from configuration files (issue #101,
// docs/design/zmq-formats-plan.md), without libzmq. One file
// (<config folder>/zmq_*.ini) describes one message structure: the socket,
// a fixed-size header with fields at fixed offsets, the samples after it,
// and the values that the header does not give. The subscriber decodes each
// message with it into the same StreamMessage that parseStreamMessage gives.
//
// The built-in format "wvt" is the wvt wire format (net/zmq_wire.h). It is
// always there, also without a config folder.
//
// The file format (INI; "#" or ";" starts a comment at the line start or
// after a space; numbers as in wvt.conf, e.g. 2.4e6):
//
//   [format]   name, description
//   [socket]   type = dish | sub | pull, transport = udp | tcp,
//              mode = bind | connect, address, topic (sub), group (dish),
//              frames = last | N, max_bytes
//   [header]   bytes, match.NAME = TYPE @ OFFSET == VALUE | "text" @ OFFSET,
//              fc | rate | gain | time_ns | index | seq | count = TYPE @ OFFSET [* SCALE]
//   [samples]  format = cu8 | ci8 | ci16 | ci32 | cf32 | cf64,
//              endian = little | big, order = iq | qi
//   [stream]   rate, fc, gain: the values that the header does not give
//
// TYPE: u8, i8, u16le, u16be, i16le, ... u64be, i64be, f32le, f32be, f64le, f64be.
//
// A Protobuf format (issue #118, [format] encoding = protobuf) has a
// [protobuf] section in place of [header]. Each field is named by its number
// in the .proto file; a dotted path reaches a field in a sub-message:
//
//   [protobuf] proto = FILE, message = NAME   (WVT_WITH_PROTOBUF: field names)
//              match.NAME = PTYPE PATH == VALUE | string PATH == "text" | present PATH,
//              fc | rate | gain | time_ns | index | seq = PTYPE PATH [* SCALE],
//              samples = [packed | unpacked] ETYPE PATH  (I and Q interleaved),
//              or i = ... and q = ... (two arrays)
//
// PTYPE: double, float, int32, int64, uint32, uint64, sint32, sint64,
// fixed32, fixed64, sfixed32, sfixed64, bool, enum. ETYPE: the numeric
// PTYPEs but bool and enum, or bytes (the samples as [samples] describes
// them). Integer elements are values of [samples] format (cu8, ci8, ci16,
// ci32; default ci16). With proto = FILE, PATH can use the field names
// (iq.rate_hz), TYPE can be left out (the .proto file gives it), and an enum
// match can use the name of the value.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io/iq_file.h"
#include "net/zmq_wire.h"

enum class FormatSocket : uint8_t { Dish, Sub, Pull };

enum class FieldType : uint8_t { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64 };

enum class FormatEncoding : uint8_t { Binary, Protobuf };

// The scalar types of Protobuf (issue #118). The wire format alone does not
// tell them apart (int32 and sint32 are both varints), so the file names one.
enum class ProtoType : uint8_t {
    Double, Float, Int32, Int64, Uint32, Uint64, Sint32, Sint64,
    Fixed32, Fixed64, Sfixed32, Sfixed64, Bool, Enum, Bytes, String
};

// A field of a Protobuf message: its type and the path of field numbers
// (one number, or more for a field in a sub-message).
struct ProtoField {
    bool set = false;
    ProtoType type = ProtoType::Double;
    std::vector<uint32_t> path;
    std::string name;     // the names of the path, with proto = FILE ("iq.rate_hz"); else empty
    double scale = 1;     // fc, rate, gain, time_ns only
    bool packed = true;   // samples, i, q: how the publisher writes them (both are read)
};

// A value that each message must have, else it is not of this format.
struct ProtoMatch {
    enum class Kind : uint8_t { Value, Text, Present };
    std::string name;
    Kind kind = Kind::Value;
    ProtoType type = ProtoType::Uint32;
    std::vector<uint32_t> path;
    uint64_t raw = 0;  // Value: the value as it is on the wire (the varint, or the bits)
    std::string text;  // Text: the bytes
};

struct ProtoLayout {
    std::string schemaFile;     // proto = FILE (phase 2), as in the format file; empty: numbers only
    std::string schemaMessage;  // the full name of the message in it
    std::vector<ProtoMatch> matches;
    ProtoField fc, rate, gain, timeNs, index, seq;
    ProtoField samples;  // I and Q interleaved; or
    ProtoField i, q;     // two arrays of the same length
};

// A number in the header.
struct FormatField {
    bool set = false;
    FieldType type = FieldType::U8;
    bool bigEndian = false;
    uint32_t offset = 0;
    double scale = 1;  // the value is multiplied by this (fc, rate, gain, time_ns only)
};

// Bytes that must be at an offset, else the message is not of this format.
struct FormatMatch {
    std::string name;   // NAME of match.NAME
    uint32_t offset = 0;
    std::string bytes;  // the bytes to compare
};

struct StreamFormat {
    std::string id;           // "wvt", or the file name without "zmq_" and ".ini"
    std::string name;         // the text in the drop-down
    std::string description;  // the tooltip
    std::string path;         // the file; empty for the built-in format
    bool builtin = false;
    FormatEncoding encoding = FormatEncoding::Binary;  // [format] encoding

    // [socket]
    FormatSocket socket = FormatSocket::Dish;
    bool tcp = false;   // else udp
    bool bind = true;   // else connect
    std::string address;  // the default for the Address field; empty = wvt's default
    std::string topic;    // sub: the topic prefix; empty = all messages
    std::string group = kStreamGroup;  // dish: the group to join
    int frame = 0;        // the frame with the data: 0 = the last, else 1, 2, ...
    size_t maxBytes = 1 << 20;

    // [header]
    size_t headerBytes = 0;
    std::vector<FormatMatch> matches;
    FormatField fc, rate, gain, timeNs, index, seq, count;

    // [samples]
    SampleFormat sampleFmt = SampleFormat::CF32;
    bool bigEndian = false;
    bool swapIQ = false;  // order = qi

    // [stream]: used when the header has no field for the value
    double fixedRate = 0, fixedFc = 0, fixedGain = 0;

    // [protobuf]: encoding = protobuf only
    ProtoLayout proto;
};

// The built-in wvt wire format.
StreamFormat builtinStreamFormat();

// Parses the text of a format file. `name` (the file name) goes in the
// messages ("zmq_x.ini:12: ..."). Problems that make the format unusable give
// false and err; the others go to warnings.
bool parseStreamFormat(const std::string& text, const std::string& name, StreamFormat& f,
                       std::vector<std::string>& warnings, std::string& err);
// Reads and parses a file; the id comes from the file name.
bool loadStreamFormat(const std::string& path, StreamFormat& f, std::vector<std::string>& warnings, std::string& err);

// One entry of the format list: a format that loaded, or a file with an error.
struct StreamFormatEntry {
    StreamFormat format;  // format.id, .name and .path are set also on error
    std::string error;    // empty: usable
    std::vector<std::string> warnings;
    bool ok() const { return error.empty(); }
};
// The built-in format first, then the zmq_*.ini files of `dir`, sorted by
// file name. `dir` may be empty (no config folder).
std::vector<StreamFormatEntry> scanStreamFormats(const std::string& dir);
// The entry with this id (any case), or nullptr.
const StreamFormatEntry* findStreamFormat(const std::vector<StreamFormatEntry>& list, const std::string& id);

// The sample format that the decoder delivers for `f`: the format of the
// file when the ring carries it (cu8, ci16, cf32), else cf32.
SampleFormat streamRingFormat(const StreamFormat& f);

// Extra values of a decoded message that StreamHeader has no place for.
struct FormatExtra {
    bool hasIndex = false;  // h.index is from the message
    bool hasSeq = false;
    uint64_t seq = 0;       // the message counter
};

// Decodes one message (one frame) with format f. On success m has kind Data,
// the samples in streamRingFormat(f) (in the input, or in `scratch` after a
// conversion), and the values from the header or the file. False (why set)
// for a message that does not match or is not valid. The built-in format
// uses parseStreamMessage, so info and end messages pass through.
bool decodeStreamMessage(const StreamFormat& f, const uint8_t* p, size_t n, StreamMessage& m, FormatExtra& extra,
                         std::vector<uint8_t>& scratch, std::string& why);

// The text of a field type ("u32be"), for messages and checks.
const char* fieldTypeName(FieldType t, bool bigEndian);

// The config folder where the subscriber finds the format files. main sets
// it at start (configFindDir); empty = no folder: only the built-in format.
void setStreamFormatDir(const std::string& dir);
std::string streamFormatDir();

// True for the id of the built-in format: "" or "wvt" (any case).
bool isBuiltinFormatId(const std::string& id);

// One line about the socket and the samples, for lists and tooltips:
// "sub, tcp connect 127.0.0.1:5555, cf32".
std::string streamFormatSummary(const StreamFormat& f);

// The format with this id ("" = "wvt") from streamFormatDir(). False (err
// set) when there is no such format or its file has an error.
bool resolveStreamFormat(const std::string& id, StreamFormat& f, std::string& err);

// Where the subscriber receives, for format f: `typed` (the Address field),
// else the format's address, else (dish only) the default address.
//   dish:     "a.b.c.d[:port]" or "iface;group[:port]" (parseStreamAddress)
//   sub/pull: "host:port"; "*:port" binds on all interfaces (mode = bind)
// A "udp://" or "tcp://" before it must fit the transport.
struct FormatEndpoint {
    std::string endpoint;  // for zmq_bind / zmq_connect: "udp://...", "tcp://..."
    std::string text;      // for messages: "127.0.0.1:5555"
    int port = 0;
    StreamAddress udp;     // dish only
};
bool formatEndpoint(const StreamFormat& f, const std::string& typed, FormatEndpoint& out, std::string& err);

// Follows a message counter ([header] seq) of `bits` bits, which wraps. A
// counter that jumps far, or goes back by more than a few messages, is a
// source that started again.
class MessageCounter {
public:
    enum class Event { First, InOrder, Lost, Late, Restart };
    struct Result {
        Event event = Event::First;
        uint64_t lost = 0;  // messages, for Lost
    };
    explicit MessageCounter(int bits = 64);
    Result next(uint64_t seq);
    void reset() { started_ = false; }

    static constexpr uint64_t kMaxLost = 1000000;  // a larger jump forward is a restart
    static constexpr uint64_t kMaxLate = 1000;     // a larger jump back is a restart

private:
    uint64_t mask_;
    bool started_ = false;
    uint64_t expected_ = 0;
};

// The bits of a field type (for MessageCounter).
int fieldTypeBits(FieldType t);
// The bits of the message counter ([header] or [protobuf] seq) of f.
int streamSeqBits(const StreamFormat& f);
// True when the sample rate comes from each message (a rate field), not
// from [stream].
bool streamRateInMessage(const StreamFormat& f);

// The name of a Protobuf type ("sint32").
const char* protoTypeName(ProtoType t);

// ---------------------------------------------------------------------------
// Publishing with a format file (phase 6): the reverse of the subscriber.
// The publisher's socket fits the subscriber's: dish -> RADIO, sub -> PUB,
// pull -> PUSH. Its mode is the other one: a subscriber that connects needs
// a publisher that binds.

// True when wvt can publish with format f, else false and err. Every header
// field has a value that wvt knows, so the checks are about the frames: a
// sub format with a topic needs frames = last or 2 (the topic, then the
// data), the others frames = last or 1.
bool checkPublishFormat(const StreamFormat& f, std::string& err);

// The most samples in one message of format f: a datagram (dish) holds at
// most 8064 bytes, a tcp message at most max_bytes and 256 KiB, and a count
// field limits it too. 0 if not even one sample fits.
size_t publishMaxSamples(const StreamFormat& f);

// The largest message that the publisher sends with format f.
size_t publishMaxBytes(const StreamFormat& f);

// Where the publisher sends with format f: as formatEndpoint, from the side
// of the publisher (sub and pull formats: the other mode).
bool publishEndpoint(const StreamFormat& f, const std::string& typed, FormatEndpoint& out, std::string& err);

// Writes one data message of format f (not the built-in format) in `out`:
// the header with h.fc, h.rate, h.gain, h.timeNs, h.index, `seq` and the
// count, the matches, then `count` samples converted from `native` (cu8, ci16
// or cf32, as in the ring). False (why set) when a value does not fit in its
// field, e.g. fc = 2.4e9 in a u16 field.
bool encodeStreamMessage(const StreamFormat& f, const StreamHeader& h, uint64_t seq, SampleFormat native,
                         const uint8_t* samples, size_t count, std::vector<uint8_t>& out, std::string& why);

// True when a publisher with format `tx` on `txAddress` sends to the
// subscriber with format `rx` on `rxAddress`: the stream would come back to
// the wvt that sends it. The sockets must fit (RADIO to DISH in the same
// group, PUB to SUB, PUSH to PULL) and the endpoints must be the same port
// on the same host (or "*" on one side).
bool publishReachesSubscriber(const StreamFormat& rx, const std::string& rxAddress, const StreamFormat& tx,
                              const std::string& txAddress);
