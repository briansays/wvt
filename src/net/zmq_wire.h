#pragma once
// The wire format of the wvt network stream (issue #96), without libzmq: the
// message header, the stream address, and the sample index tracker. The
// publisher and the subscriber use it; zmq_check and fuzz_zmq_wire test it.
//
// One ZMQ message is one UDP datagram (RADIO / DISH, group "wvt"). Each
// message starts with a 64-byte header, all values little-endian:
//
//   0  char[4]  magic "WVTZ"
//   4  u8       version (1)
//   5  u8       kind: 1 data, 2 info
//   6  u8       sample format: 1 cu8, 2 ci16, 3 cf32
//   7  u8       flags: bit 0 AGC, bit 1 retune at the first sample, bit 2 end of stream
//   8  u32      stream ID, random for each start of the source
//  12  u32      samples in this message (info: 0)
//  16  u64      index of the first sample since the start of the stream
//  24  f64      center frequency, Hz
//  32  f64      sample rate, S/s
//  40  f64      gain, dB (BB60: reference level, dBm)
//  48  u64      time of the first sample, ns since 1970 (UTC); 0 if unknown
//  56  u8[8]    reserved: 0 when sent, ignored when read
//
// A data message has the samples after the header. An info message (one
// each second) has two strings after the header: the driver and the device
// description, each a u16 length and the bytes.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io/iq_file.h"

constexpr size_t kStreamHeaderBytes = 64;
// libzmq sends one datagram of at most 8192 bytes, the group name included.
constexpr size_t kStreamMaxPayload = 8000;
constexpr size_t kStreamMaxInfoText = 1000;  // each info string is cut to this
constexpr uint8_t kStreamVersion = 1;
constexpr const char* kStreamGroup = "wvt";
constexpr int kStreamDefaultPort = 5600;

enum class StreamKind : uint8_t { Data = 1, Info = 2 };

constexpr uint8_t kStreamFlagAgc = 1;
constexpr uint8_t kStreamFlagRetune = 2;
constexpr uint8_t kStreamFlagEnd = 4;

struct StreamHeader {
    StreamKind kind = StreamKind::Data;
    SampleFormat fmt = SampleFormat::CI16;
    uint8_t flags = 0;
    uint32_t streamId = 0;
    uint32_t count = 0;  // samples in the message
    uint64_t index = 0;  // of the first sample
    double fc = 0, rate = 0, gain = 0;
    uint64_t timeNs = 0;
};

struct StreamInfo {
    std::string driver, description;
};

// A received message. `samples` points into the buffer that was parsed.
struct StreamMessage {
    StreamHeader h;
    const uint8_t* samples = nullptr;
    size_t sampleBytes = 0;
    StreamInfo info;  // info messages only
};

// The formats that the stream carries (the native formats of the devices):
// the bytes of one complex sample, or 0 for other formats.
int streamBytesPerSample(SampleFormat f);
// The most samples of format `f` in one data message.
uint32_t streamMaxSamples(SampleFormat f);

// Writes the header to out[0..kStreamHeaderBytes). The caller puts the
// samples (h.count of them) after it.
void packStreamHeader(const StreamHeader& h, uint8_t* out);
// A complete info message.
std::vector<uint8_t> packStreamInfo(const StreamHeader& h, const StreamInfo& info);
// Checks and reads a message. On failure, `why` tells the reason.
bool parseStreamMessage(const uint8_t* p, size_t n, StreamMessage& m, std::string& why);

// A stream address: "a.b.c.d[:port]", or "iface;a.b.c.d[:port]" for a
// multicast group on one interface (the udp:// form of libzmq). Unicast is
// where the subscriber receives: an address of the subscriber's computer.
// Multicast (224.0.0.0 to 239.255.255.255) is the group that both sides use.
struct StreamAddress {
    std::string iface;  // multicast only; empty = the default interface
    std::string ip;     // dotted IPv4
    int port = kStreamDefaultPort;
    bool multicast = false;
    std::string text() const;      // "iface;a.b.c.d:port"
    std::string endpoint() const;  // "udp://" + text()
};
bool parseStreamAddress(const std::string& s, StreamAddress& a, std::string& err);

// An IPv4 interface address, for the choice of the default address.
struct StreamIface {
    std::string name;
    std::string ip;  // dotted IPv4
    bool up = false, running = false, loopback = false, pointToPoint = false;
};
// The default address: the address of the best interface (up and running,
// not the loopback, not point-to-point such as a VPN, not link-local
// 169.254.x.x if there is another), with kStreamDefaultPort; else
// 127.0.0.1:5600.
std::string pickStreamAddress(const std::vector<StreamIface>& ifaces);
// The interfaces of this computer (getifaddrs).
std::vector<StreamIface> streamInterfaces();
std::string defaultStreamAddress();

// Follows the sample index of the data messages of a stream, for the
// subscriber. UDP can lose, repeat and (rarely) reorder messages.
class StreamTracker {
public:
    enum class Event {
        First,      // the first message: the stream starts here
        InOrder,    // the next samples
        Lost,       // `lost` samples are missing before this message
        Late,       // the message is before the expected index (late or repeated): drop it
        NewStream,  // another stream ID: the source started again
    };
    struct Result {
        Event event = Event::First;
        uint64_t lost = 0;
    };

    Result next(const StreamHeader& h);
    void reset() { started_ = false; }
    bool started() const { return started_; }
    uint64_t expected() const { return expected_; }  // index of the next sample
    uint32_t streamId() const { return id_; }

private:
    bool started_ = false;
    uint32_t id_ = 0;
    uint64_t expected_ = 0;
};
