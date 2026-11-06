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
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io/iq_file.h"
#include "net/zmq_wire.h"

enum class FormatSocket : uint8_t { Dish, Sub, Pull };

enum class FieldType : uint8_t { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64 };

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
