// ZMQ streaming self-test (issue #96).
//  - The wire format (src/net/zmq_wire): the header, the info message, the
//    rejection of bad messages, the stream address, the default address and
//    the sample index tracker. These need no libzmq and run in every build.
//  - With WVT_HAVE_ZMQ: the libzmq that wvt links has the draft RADIO and
//    DISH sockets, and one message of the largest size that wvt sends goes
//    through udp:// on the loopback.
//  - With WVT_HAVE_ZMQ: the publisher (src/net/zmq_pub) streams a sample
//    ring to a DISH on the loopback: every sample, the retune, the gain, the
//    info and end messages; and with a ring that overflows, the samples that
//    it skips are the samples that the subscriber sees as lost.
//  - With WVT_HAVE_ZMQ: a live engine with the simulator publishes, and a
//    second live engine receives with the zmq device: the parameters, the
//    source's retune and gain, a SigMF recording on the subscriber, no loss
//    on the loopback, and the errors (another rate, the source stops, the
//    port in use, an address of another computer, no stream).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/sample_ring.h"
#include "net/zmq_pub.h"
#include "net/zmq_wire.h"

#ifdef WVT_HAVE_ZMQ
#include <unistd.h>
#include <zmq.h>

#include <cstdlib>
#include <functional>

#include "common/json.h"
#include "device/live.h"
#endif

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

StreamHeader sampleHeader() {
    StreamHeader h;
    h.kind = StreamKind::Data;
    h.fmt = SampleFormat::CI16;
    h.flags = kStreamFlagAgc | kStreamFlagRetune;
    h.streamId = 0xA1B2C3D4;
    h.count = 3;
    h.index = 0x0102030405060708ULL;
    h.fc = 915.25e6;
    h.rate = 2.4e6;
    h.gain = -20.5;
    h.timeNs = 1793750400123456789ULL;
    return h;
}

std::vector<uint8_t> dataMessage(const StreamHeader& h, uint8_t fill = 0x5A) {
    std::vector<uint8_t> m(kStreamHeaderBytes + (size_t)h.count * streamBytesPerSample(h.fmt), fill);
    packStreamHeader(h, m.data());
    return m;
}

bool parses(const std::vector<uint8_t>& m, std::string* whyOut = nullptr) {
    StreamMessage r;
    std::string why;
    const bool ok = parseStreamMessage(m.data(), m.size(), r, why);
    if (whyOut) *whyOut = why;
    return ok;
}

void checkHeader() {
    printf("header\n");
    const StreamHeader h = sampleHeader();
    std::vector<uint8_t> m = dataMessage(h);
    check(m.size() == 64 + 12, "3 ci16 samples: 64 + 12 bytes");
    check(memcmp(m.data(), "WVTZ", 4) == 0 && m[4] == 1 && m[5] == 1 && m[6] == 2 && m[7] == 3,
          "magic, version 1, kind data, format code 2 (ci16), flags");
    check(m[8] == 0xD4 && m[11] == 0xA1 && m[16] == 0x08 && m[23] == 0x01, "little-endian stream ID and index");
    bool reservedZero = true;
    for (int i = 56; i < 64; ++i) reservedZero = reservedZero && m[i] == 0;
    check(reservedZero, "reserved bytes are 0");

    StreamMessage r;
    std::string why;
    check(parseStreamMessage(m.data(), m.size(), r, why), "a data message parses" + (why.empty() ? "" : ": " + why));
    check(r.h.kind == h.kind && r.h.fmt == h.fmt && r.h.flags == h.flags && r.h.streamId == h.streamId &&
              r.h.count == h.count && r.h.index == h.index && r.h.fc == h.fc && r.h.rate == h.rate &&
              r.h.gain == h.gain && r.h.timeNs == h.timeNs,
          "all header fields read back");
    check(r.samples == m.data() + 64 && r.sampleBytes == 12, "the samples follow the header");

    const std::pair<SampleFormat, const char*> formats[] = {
        {SampleFormat::CU8, "cu8"}, {SampleFormat::CI16, "ci16"}, {SampleFormat::CF32, "cf32"}};
    for (const auto& [f, name] : formats) {
        StreamHeader hf = h;
        hf.fmt = f;
        hf.count = streamMaxSamples(f);
        const auto mf = dataMessage(hf);
        StreamMessage rf;
        check(mf.size() == 64 + 8000 && parseStreamMessage(mf.data(), mf.size(), rf, why) && rf.h.fmt == f,
              std::string(name) + ": " + std::to_string(hf.count) + " samples fill 8000 bytes");
    }
    check(streamMaxSamples(SampleFormat::CI16) == 2000 && streamMaxSamples(SampleFormat::CU8) == 4000 &&
              streamMaxSamples(SampleFormat::CF32) == 1000,
          "2000 ci16, 4000 cu8 or 1000 cf32 samples in a message");
    check(streamBytesPerSample(SampleFormat::CF64) == 0, "cf64 is not a stream format");

    StreamHeader end = h;
    end.count = 0;
    end.flags = kStreamFlagEnd;
    check(parses(dataMessage(end)), "an end-of-stream message without samples parses");
    StreamHeader zero = h;
    zero.fc = 0;
    check(parses(dataMessage(zero)), "center frequency 0 Hz parses");
    std::vector<uint8_t> future = m;
    future[60] = 0xEE;
    future[7] |= 0x80;
    check(parses(future), "reserved bytes and unknown flags are ignored");
}

void checkInfo() {
    printf("info message\n");
    StreamHeader h = sampleHeader();
    const auto m = packStreamInfo(h, {"uhd", "USRP B205mini serial 31A2B3C"});
    StreamMessage r;
    std::string why;
    check(parseStreamMessage(m.data(), m.size(), r, why) && r.h.kind == StreamKind::Info && r.h.count == 0,
          "an info message parses as kind info, 0 samples" + (why.empty() ? "" : ": " + why));
    check(r.info.driver == "uhd" && r.info.description == "USRP B205mini serial 31A2B3C", "driver and description");
    check(r.h.rate == h.rate && r.h.fc == h.fc && r.h.streamId == h.streamId, "with the stream parameters");

    const auto e = packStreamInfo(h, {"", ""});
    check(parseStreamMessage(e.data(), e.size(), r, why) && r.info.driver.empty() && r.info.description.empty(),
          "empty strings");

    std::string longText(3000, 'x');
    longText.replace(998, 3, "\xE2\x82\xAC");  // a 3-byte character over the cut at 1000
    const auto l = packStreamInfo(h, {"sim", longText});
    check(parseStreamMessage(l.data(), l.size(), r, why) && r.info.description.size() == 998,
          "a long description is cut to 1000 bytes, before a whole UTF-8 character");

    const auto c = packStreamInfo(h, {"sim", std::string("a\nb\x1b[31mc", 9)});
    check(parseStreamMessage(c.data(), c.size(), r, why) && r.info.description == "a?b?[31mc",
          "control characters become '?'");
}

void checkRejects() {
    printf("bad messages are rejected\n");
    const StreamHeader h = sampleHeader();
    const auto good = dataMessage(h);
    std::string why;

    auto bad = [&](const std::string& what, std::vector<uint8_t> m, const char* expect) {
        const bool ok = parses(m, &why);
        check(!ok && why.find(expect) != std::string::npos, what + " (" + why + ")");
    };
    bad("empty", {}, "too short");
    bad("63 bytes", std::vector<uint8_t>(good.begin(), good.begin() + 63), "too short");
    auto m = good;
    m[0] = 'X';
    bad("wrong magic", m, "not a wvt");
    m = good;
    m[4] = 2;
    bad("version 2", m, "version 2");
    m = good;
    m[5] = 3;
    bad("kind 3", m, "kind");
    m = good;
    m[6] = 0;
    bad("format 0", m, "format");
    m = good;
    m[6] = 9;
    bad("format 9", m, "format");
    m = good;
    m.pop_back();
    bad("one byte missing", m, "need 12 bytes");
    m = good;
    m.push_back(0);
    bad("one byte extra", m, "need 12 bytes");
    m = good;
    m[15] = 0xFF;  // count 0xFF000003
    bad("a huge count", m, "need");

    auto withHeader = [&](StreamHeader x) { return dataMessage(x); };
    StreamHeader x = h;
    x.rate = 0;
    bad("rate 0", withHeader(x), "rate");
    x.rate = -1;
    bad("negative rate", withHeader(x), "rate");
    x.rate = NAN;
    bad("NaN rate", withHeader(x), "rate");
    x.rate = INFINITY;
    bad("infinite rate", withHeader(x), "rate");
    x = h;
    x.fc = -1;
    bad("negative frequency", withHeader(x), "frequency");
    x.fc = NAN;
    bad("NaN frequency", withHeader(x), "frequency");
    x = h;
    x.gain = NAN;
    bad("NaN gain", withHeader(x), "gain");

    auto info = packStreamInfo(h, {"uhd", "B205"});
    m = info;
    m[12] = 1;  // count 1
    bad("info with samples", m, "with samples");
    m = info;
    m.push_back('!');
    bad("info with extra bytes", m, "extra bytes");
    m = info;
    m.resize(m.size() - 2);
    bad("info cut short", m, "cut short");
    m = info;
    m.resize(kStreamHeaderBytes + 1);
    bad("info without the length", m, "cut short");
    m = std::move(info);
    m[kStreamHeaderBytes] = 0xFF;
    m[kStreamHeaderBytes + 1] = 0xFF;
    bad("info length 65535", m, "too long");
}

void checkAddress() {
    printf("stream address\n");
    StreamAddress a;
    std::string err;
    check(parseStreamAddress("127.0.0.1:5600", a, err) && a.ip == "127.0.0.1" && a.port == 5600 && !a.multicast &&
              a.endpoint() == "udp://127.0.0.1:5600",
          "127.0.0.1:5600: unicast, udp://127.0.0.1:5600");
    check(parseStreamAddress("  192.168.1.20  ", a, err) && a.port == 5600 && a.text() == "192.168.1.20:5600",
          "no port: 5600; spaces are removed");
    check(parseStreamAddress("239.10.0.1:6000", a, err) && a.multicast && a.port == 6000, "239.10.0.1: multicast");
    check(parseStreamAddress("224.0.0.1:1", a, err) && a.multicast && a.port == 1, "224.0.0.1:1: multicast, port 1");
    check(parseStreamAddress("en0;239.10.0.1:5600", a, err) && a.iface == "en0" &&
              a.endpoint() == "udp://en0;239.10.0.1:5600",
          "en0;239.10.0.1:5600: a group on one interface");
    check(parseStreamAddress("192.168.1.2;239.10.0.1:5600", a, err) && a.iface == "192.168.1.2",
          "the interface can be an address");
    check(parseStreamAddress("010.001.002.003:65535", a, err) && a.ip == "10.1.2.3" && a.port == 65535,
          "leading zeros are removed; port 65535");

    auto bad = [&](const char* s, const char* expect) {
        const bool ok = parseStreamAddress(s, a, err);
        check(!ok && err.find(expect) != std::string::npos, std::string("'") + s + "' (" + err + ")");
    };
    bad("", "no address");
    bad("   ", "no address");
    bad("localhost:5600", "not an IPv4");
    bad("1.2.3:5600", "not an IPv4");
    bad("1.2.3.4.5:5600", "not an IPv4");
    bad("256.1.1.1:5600", "not an IPv4");
    bad("1.2.3.-4:5600", "not an IPv4");
    bad("1.2.3.4:0", "bad port");
    bad("1.2.3.4:65536", "bad port");
    bad("1.2.3.4:", "bad port");
    bad("1.2.3.4:56a", "bad port");
    bad("1.2.3.4:999999999999", "bad port");
    bad("0.0.0.0:5600", "not an address");
    bad("255.255.255.255:5600", "broadcast");
    bad("240.0.0.1:5600", "reserved");
    bad("en0;192.168.1.20:5600", "multicast group only");
    bad(";239.10.0.1:5600", "interface");
    bad("e n0;239.10.0.1:5600", "interface");
    bad("[::1]:5600", "not an IPv4");
}

void checkDefaultAddress() {
    printf("default address\n");
    auto f = [](const char* name, const char* ip, bool running = true, bool loopback = false, bool p2p = false) {
        StreamIface i;
        i.name = name;
        i.ip = ip;
        i.up = true;
        i.running = running;
        i.loopback = loopback;
        i.pointToPoint = p2p;
        return i;
    };
    check(pickStreamAddress({}) == "127.0.0.1:5600", "no interfaces: 127.0.0.1:5600");
    check(pickStreamAddress({f("lo0", "127.0.0.1", true, true)}) == "127.0.0.1:5600", "the loopback only: 127.0.0.1:5600");
    check(pickStreamAddress({f("lo0", "127.0.0.1", true, true), f("en0", "192.168.1.20")}) == "192.168.1.20:5600",
          "the loopback and en0: en0");
    check(pickStreamAddress({f("utun3", "10.8.0.2", true, false, true), f("en0", "192.168.1.20")}) == "192.168.1.20:5600",
          "a VPN (point-to-point) is not used");
    check(pickStreamAddress({f("docker0", "172.17.0.1", false), f("eth0", "10.0.0.5")}) == "10.0.0.5:5600",
          "an interface that runs before one that does not");
    check(pickStreamAddress({f("en5", "169.254.10.1"), f("en0", "192.168.1.20")}) == "192.168.1.20:5600",
          "a link-local address only if there is no other");
    check(pickStreamAddress({f("en5", "169.254.10.1")}) == "169.254.10.1:5600", "a link-local address alone is used");
    check(pickStreamAddress({f("en0", "192.168.1.20"), f("en1", "192.168.2.30")}) == "192.168.1.20:5600",
          "two equal interfaces: the first");
    StreamIface down = f("en0", "192.168.1.20");
    down.up = false;
    check(pickStreamAddress({down}) == "127.0.0.1:5600", "an interface that is down is not used");

    const std::string d = defaultStreamAddress();
    StreamAddress a;
    std::string err;
    check(parseStreamAddress(d, a, err) && !a.multicast && a.port == 5600,
          "this computer: " + d + " is a unicast address with port 5600");
}

void checkTracker() {
    printf("sample index tracker\n");
    using E = StreamTracker::Event;
    StreamTracker t;
    StreamHeader h;
    h.streamId = 7;
    h.count = 100;
    auto at = [&](uint64_t index, uint32_t id = 7, uint32_t count = 100) {
        h.index = index;
        h.streamId = id;
        h.count = count;
        return t.next(h);
    };
    StreamTracker::Result r = at(5000);
    check(r.event == E::First && t.expected() == 5100, "the first message starts the stream (a late join)");
    r = at(5100);
    check(r.event == E::InOrder && t.expected() == 5200, "the next message: in order");
    r = at(5500);
    check(r.event == E::Lost && r.lost == 300 && t.expected() == 5600, "a jump of 300: 300 lost");
    r = at(5200);
    check(r.event == E::Late && t.expected() == 5600, "an earlier message: late, dropped");
    r = at(5500);
    check(r.event == E::Late, "a repeated message: late, dropped");
    r = at(5550);
    check(r.event == E::Late && t.expected() == 5600, "a message over the expected index: dropped");
    r = at(5600, 7, 0);
    check(r.event == E::InOrder && t.expected() == 5600, "a message without samples changes nothing");
    r = at(0, 8);
    check(r.event == E::NewStream && t.expected() == 100 && t.streamId() == 8, "another stream ID: a new stream");
    r = at(100, 8);
    check(r.event == E::InOrder, "then in order on the new stream");
    t.reset();
    r = at(100, 8);
    check(r.event == E::First, "after reset: first again");
}

#ifdef WVT_HAVE_ZMQ
void checkSockets() {
    printf("libzmq (RADIO / DISH on udp://)\n");
    int major = 0, minor = 0, patch = 0;
    zmq_version(&major, &minor, &patch);
    printf("  libzmq %d.%d.%d\n", major, minor, patch);
    check(zmq_has("draft") != 0, "libzmq has the draft API");

    void* ctx = zmq_ctx_new();
    void* dish = zmq_socket(ctx, ZMQ_DISH);
    void* radio = zmq_socket(ctx, ZMQ_RADIO);
    check(dish && radio, "RADIO and DISH sockets");
    if (!dish || !radio) return;

    // A free port: the subscriber binds, so try a few.
    std::string endpoint;
    for (int port = 15600; port < 15700 && endpoint.empty(); ++port) {
        const std::string e = "udp://127.0.0.1:" + std::to_string(port);
        if (zmq_bind(dish, e.c_str()) == 0) endpoint = e;
    }
    check(!endpoint.empty(), "DISH binds on the loopback (" + endpoint + ")");
    check(zmq_join(dish, kStreamGroup) == 0, "DISH joins the group");
    check(!endpoint.empty() && zmq_connect(radio, endpoint.c_str()) == 0, "RADIO connects to the DISH");

    const int timeoutMs = 100;
    zmq_setsockopt(dish, ZMQ_RCVTIMEO, &timeoutMs, sizeof timeoutMs);

    // The largest message: a full header and 8000 bytes of samples.
    StreamHeader h = sampleHeader();
    h.count = streamMaxSamples(h.fmt);
    std::vector<uint8_t> sent = dataMessage(h);
    for (size_t i = kStreamHeaderBytes; i < sent.size(); ++i) sent[i] = (uint8_t)(i * 7 + 3);

    // UDP has no handshake: send until the DISH receives one (the RADIO can
    // drop messages until its connection is up), for at most 3 s.
    bool got = false, same = false, group = false, parsed = false;
    size_t size = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!got && std::chrono::steady_clock::now() < end) {
        zmq_msg_t m;
        zmq_msg_init_size(&m, sent.size());
        memcpy(zmq_msg_data(&m), sent.data(), sent.size());
        zmq_msg_set_group(&m, kStreamGroup);
        if (zmq_msg_send(&m, radio, 0) < 0) zmq_msg_close(&m);

        zmq_msg_t r;
        zmq_msg_init(&r);
        if (zmq_msg_recv(&r, dish, 0) >= 0) {
            got = true;
            size = zmq_msg_size(&r);
            same = size == sent.size() && memcmp(zmq_msg_data(&r), sent.data(), size) == 0;
            group = zmq_msg_group(&r) && strcmp(zmq_msg_group(&r), kStreamGroup) == 0;
            StreamMessage sm;
            std::string why;
            parsed = parseStreamMessage((const uint8_t*)zmq_msg_data(&r), size, sm, why) && sm.h.count == h.count;
        }
        zmq_msg_close(&r);
    }
    check(got, "a message arrives through udp:// on the loopback");
    check(same, "it has the " + std::to_string(sent.size()) + " bytes that were sent (got " + std::to_string(size) + ")");
    check(group, "it has the group name");
    check(parsed, "it parses as a data message with 2000 ci16 samples");

    const int linger = 0;
    zmq_setsockopt(radio, ZMQ_LINGER, &linger, sizeof linger);
    zmq_setsockopt(dish, ZMQ_LINGER, &linger, sizeof linger);
    zmq_close(radio);
    zmq_close(dish);
    zmq_ctx_term(ctx);
}

// The test pattern: sample k of the ring is (k & 0x7fff, (k >> 15) & 0x7fff).
void fillPattern(std::vector<int16_t>& buf, int64_t from, int64_t n) {
    buf.resize((size_t)n * 2);
    for (int64_t k = 0; k < n; ++k) {
        buf[(size_t)k * 2] = (int16_t)((from + k) & 0x7fff);
        buf[(size_t)k * 2 + 1] = (int16_t)(((from + k) >> 15) & 0x7fff);
    }
}

bool matchesPattern(const StreamMessage& m) {
    for (uint32_t k = 0; k < m.h.count; ++k) {
        const int64_t i = (int64_t)m.h.index + k;
        int16_t iq[2];
        memcpy(iq, m.samples + (size_t)k * 4, 4);
        if (iq[0] != (int16_t)(i & 0x7fff) || iq[1] != (int16_t)((i >> 15) & 0x7fff)) return false;
    }
    return true;
}

// A DISH on a free loopback port, and a thread that keeps every message
// until it has seen `ends` end-of-stream messages or 10 s passed.
struct Receiver {
    void* ctx = zmq_ctx_new();
    void* dish = zmq_socket(ctx, ZMQ_DISH);
    std::string address;
    std::vector<std::vector<uint8_t>> msgs;
    std::thread th;

    bool open() {
        const int hwm = 100000, rcvbuf = 8 << 20, timeoutMs = 100;
        zmq_setsockopt(dish, ZMQ_RCVHWM, &hwm, sizeof hwm);
        zmq_setsockopt(dish, ZMQ_RCVBUF, &rcvbuf, sizeof rcvbuf);
        zmq_setsockopt(dish, ZMQ_RCVTIMEO, &timeoutMs, sizeof timeoutMs);
        for (int port = 15700; port < 15800 && address.empty(); ++port) {
            const std::string a = "127.0.0.1:" + std::to_string(port);
            if (zmq_bind(dish, ("udp://" + a).c_str()) == 0) address = a;
        }
        return !address.empty() && zmq_join(dish, kStreamGroup) == 0;
    }
    void run(int ends) {
        th = std::thread([this, ends] {
            int seen = 0;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (seen < ends && std::chrono::steady_clock::now() < end) {
                zmq_msg_t r;
                zmq_msg_init(&r);
                if (zmq_msg_recv(&r, dish, 0) >= 0) {
                    const auto* p = (const uint8_t*)zmq_msg_data(&r);
                    msgs.emplace_back(p, p + zmq_msg_size(&r));
                    StreamMessage m;
                    std::string why;
                    if (parseStreamMessage(p, zmq_msg_size(&r), m, why) && (m.h.flags & kStreamFlagEnd)) ++seen;
                }
                zmq_msg_close(&r);
            }
        });
    }
    void join() {
        if (th.joinable()) th.join();
    }
    ~Receiver() {
        join();
        const int linger = 0;
        zmq_setsockopt(dish, ZMQ_LINGER, &linger, sizeof linger);
        zmq_close(dish);
        zmq_ctx_term(ctx);
    }
};

void checkPublisher() {
    printf("publisher\n");
    Receiver rx;
    check(rx.open(), "a DISH listens on " + rx.address);
    rx.run(2);

    // 1. A normal stream: 100 000 ci16 samples at 1 MS/s, a retune and a
    //    gain change half way.
    const int64_t total = 100000, half = 50000, chunk = 5000;
    auto ring = std::make_shared<SampleRing>(1 << 18, 4);
    PublishSource src;
    src.fmt = SampleFormat::CI16;
    src.rate = 1e6;
    src.fc = 100e6;
    src.gain = 30;
    src.driver = "sim";
    src.description = "zmq_check source";
    StreamPublisher pub;
    std::string err;
    check(pub.start(ring, src, {rx.address, 1}, err), "the publisher starts (" + (err.empty() ? rx.address : err) + ")");
    std::vector<int16_t> buf;
    for (int64_t at = 0; at < total; at += chunk) {
        if (at == half) {
            pub.noteRetune(half, 101e6);
            pub.setGain(40, true);
        }
        fillPattern(buf, at, chunk);
        ring->push((const uint8_t*)buf.data(), chunk);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const uint32_t id1 = pub.status().streamId;
    pub.stop();
    const StreamPublisher::Status s1 = pub.status();
    check(!s1.active && s1.error.empty(), "it stops without an error");
    check(s1.samples == total && s1.skipped == 0, "it sent " + std::to_string(s1.samples) + " samples, skipped " +
                                                      std::to_string(s1.skipped));

    // 2. A ring of 4096 samples and 200 000 samples at once: the publisher
    //    falls behind and skips.
    const int64_t total2 = 200000;
    auto small = std::make_shared<SampleRing>(4096, 4);
    check(pub.start(small, src, {rx.address, 1}, err), "the publisher starts again on a small ring");
    const uint32_t id2 = pub.status().streamId;
    fillPattern(buf, 0, total2);
    small->push((const uint8_t*)buf.data(), total2);
    pub.stop();
    const StreamPublisher::Status s2 = pub.status();
    check(id2 != id1, "a new stream ID for the new start");
    check(s2.samples + s2.skipped == total2,
          "small ring: " + std::to_string(s2.samples) + " sent + " + std::to_string(s2.skipped) + " skipped = 200000");

    rx.join();

    // Check what arrived.
    int infos = 0, ends = 0, data1 = 0, bad = 0, crossing = 0, outOfOrder = 0;
    bool infoOk = true, patternOk = true, retuneOk = true, gainOk = true, timeOk = true, sizeOk = true;
    int64_t got1 = 0, got2 = 0, lost2 = 0, end1 = -1, end2 = -1;
    bool sawRetune = false;
    uint64_t t0 = 0;
    StreamTracker tr;
    for (const auto& raw : rx.msgs) {
        StreamMessage m;
        std::string why;
        if (!parseStreamMessage(raw.data(), raw.size(), m, why)) {
            ++bad;
            continue;
        }
        const bool first = m.h.streamId == id1;
        if (m.h.kind == StreamKind::Info) {
            ++infos;
            infoOk = infoOk && m.info.driver == "sim" && m.info.description == "zmq_check source" && m.h.rate == 1e6;
            continue;
        }
        if (m.h.flags & kStreamFlagEnd) {
            ++ends;
            (first ? end1 : end2) = (int64_t)m.h.index;
            continue;
        }
        const StreamTracker::Result r = tr.next(m.h);
        if (r.event == StreamTracker::Event::Late) ++outOfOrder;
        sizeOk = sizeOk && m.h.count >= 1 && m.h.count <= 2000;
        patternOk = patternOk && matchesPattern(m);
        if (first) {
            ++data1;
            got1 += m.h.count;
            if (r.event == StreamTracker::Event::Lost) ++outOfOrder;
            const bool after = m.h.index >= (uint64_t)half;
            if (m.h.index < (uint64_t)half && m.h.index + m.h.count > (uint64_t)half) ++crossing;
            retuneOk = retuneOk && m.h.fc == (after ? 101e6 : 100e6) &&
                       ((m.h.flags & kStreamFlagRetune) != 0) == (m.h.index == (uint64_t)half);
            sawRetune = sawRetune || (m.h.flags & kStreamFlagRetune);
            if (after) gainOk = gainOk && m.h.gain == 40 && (m.h.flags & kStreamFlagAgc);
            if (m.h.index == 0) t0 = m.h.timeNs;
            else if (t0) timeOk = timeOk && std::llabs((long long)(m.h.timeNs - t0) - (long long)(m.h.index * 1000)) <= 1;
        } else {
            got2 += m.h.count;
            // Before the first message that arrived, and the gaps after it.
            if (r.event == StreamTracker::Event::NewStream) lost2 += (int64_t)m.h.index;
            if (r.event == StreamTracker::Event::Lost) lost2 += (int64_t)r.lost;
        }
    }
    printf("  %zu messages: %d info, %d data in the first stream, %d end\n", rx.msgs.size(), infos, data1, ends);
    check(bad == 0, "all messages parse");
    check(infos >= 2 && infoOk, "an info message for each start, with the driver, the description and the rate");
    check(sizeOk, "1 to 2000 samples in each data message");
    check(got1 == total && outOfOrder == 0, "stream 1: all 100000 samples arrive in order (" + std::to_string(got1) + ")");
    check(patternOk, "every sample has its value");
    check(sawRetune && retuneOk && crossing == 0,
          "the retune: the message at sample 50000 has the flag and 101 MHz, no message crosses it");
    check(gainOk, "after the change: gain 40 dB and AGC");
    check(t0 != 0 && timeOk, "the time of each message is the start time + index / rate");
    check(ends == 2 && end1 == total && end2 == total2, "an end message for each stream, at its last sample");
    check(got2 == s2.samples && lost2 == s2.skipped,
          "small ring: the subscriber lost " + std::to_string(lost2) + " samples, the publisher skipped " +
              std::to_string(s2.skipped));
    printf("  (the small ring made the publisher skip %lld of 200000 samples)\n", (long long)s2.skipped);
}

bool waitFor(const std::function<bool()>& ok, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!ok()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

void checkSubscriber() {
    printf("subscriber (the zmq device in a live engine)\n");
    std::string err;
    LiveEngine src({}), sub({});
    StreamConfig sc;
    sc.driver = "sim";
    sc.fc = 100e6;
    sc.rate = 1e6;
    sc.gain = 30;
    check(src.start(sc, err), "the source (simulator, 1 MS/s, 100 MHz) starts" + (err.empty() ? "" : ": " + err));

    // A free port: the subscriber binds, so try a few.
    StreamConfig rc;
    rc.driver = "zmq";
    std::string addr;
    for (int port = 15800; port < 15820; ++port) {
        addr = "127.0.0.1:" + std::to_string(port);
        if (!src.startPublishing({addr, 1}, err)) break;
        rc.args = addr;
        if (sub.start(rc, err)) break;
        src.stopPublishing();
        if (!contains(err, "in use")) break;
    }
    check(sub.running(), "the subscriber opens on " + addr + (err.empty() ? "" : " (" + err + ")"));
    if (!sub.running()) return;
    LiveEngine::Stats st = sub.stats();
    check(st.rate == 1e6 && st.fc == 100e6 && st.fmt == SampleFormat::CI16,
          "it has the rate, the frequency and the format of the source");
    check(waitFor([&] { return contains(sub.stats().description, "wvt simulator"); }, 2.5),
          "its description has the source's: " + sub.stats().description);

    // A recording on the subscriber, over a retune of the source.
    const char* tmp = getenv("TMPDIR");
    RecordConfig recCfg;
    recCfg.prefix = std::string(tmp && *tmp ? tmp : "/tmp") + "/zmq_check_" + std::to_string(getpid());
    check(sub.startRecording(recCfg, err), "a SigMF recording starts on the subscriber" + (err.empty() ? "" : ": " + err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(src.setCenterFreq(101e6, err), "the source tunes to 101 MHz");
    check(waitFor([&] { return sub.stats().fc == 101e6; }, 2), "the subscriber follows to 101 MHz");
    check(!sub.setCenterFreq(102e6, err) && contains(err, "source sets"), "the subscriber cannot tune: " + err);
    check(src.setGain(false, 40, err), "the source sets 40 dB");
    check(waitFor([&] { return sub.gainRange().min == 40; }, 2), "the subscriber shows the source's gain");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sub.stopRecording();
    const Recorder::Status rs = sub.recorder().status();
    JsonValue meta;
    std::string text;
    if (FILE* f = fopen(rs.metaPath.c_str(), "rb")) {
        char b[4096];
        size_t n = 0;
        while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
        fclose(f);
    }
    bool capsOk = false;
    if (parseJson(text, meta, err)) {
        const JsonValue* caps = meta.get("captures");
        capsOk = caps && caps->arr.size() == 2 && caps->arr[0].number("core:frequency") == 100e6 &&
                 caps->arr[1].number("core:frequency") == 101e6 && caps->arr[1].number("core:sample_start") > 0;
    }
    check(rs.samples > 500000 && rs.dropped == 0 && rs.gaps == 0,
          "the recording has " + std::to_string(rs.samples) + " samples, none lost");
    check(capsOk, "its metadata has two captures: 100 MHz, then 101 MHz");
    remove(rs.dataPath.c_str());
    remove(rs.metaPath.c_str());

    st = sub.stats();
    const int64_t sent = src.publisher().status().samples;
    check(st.dropped == 0 && st.gapEvents == 0, "no samples lost on the loopback");
    check(st.received > 1000000 && st.received <= sent && st.received >= sent - 100000,
          "received " + std::to_string(st.received) + " of the " + std::to_string(sent) + " samples sent");

    // A second subscriber on the same port.
    LiveEngine sub2({});
    check(!sub2.start(rc, err) && contains(err, "in use"), "a second subscriber on the port: " + err);

    // Another stream with another rate to the same address.
    StreamPublisher other;
    auto ring = std::make_shared<SampleRing>(1 << 16, 4);
    PublishSource os;
    os.rate = 2e6;
    os.fc = 100e6;
    check(other.start(ring, os, {addr, 1}, err), "a second source at 2 MS/s sends to the same address");
    std::vector<int16_t> zeros(2 * 2000);
    for (int i = 0; i < 20; ++i) {
        ring->push((const uint8_t*)zeros.data(), 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(waitFor([&] { return contains(sub.stats().error, "another rate"); }, 2),
          "the subscriber stops: " + sub.stats().error);
    other.stop();
    sub.stop();

    // The source stops publishing.
    check(sub.start(rc, err), "the subscriber starts again");
    src.stopPublishing();
    check(waitFor([&] { return contains(sub.stats().error, "stopped publishing"); }, 2),
          "the source stops: " + sub.stats().error);
    sub.stop();

    // A restart of the source publishes again, with a new stream ID.
    check(src.startPublishing({addr, 1}, err), "the source publishes again");
    const uint32_t id1 = src.publisher().status().streamId;
    sc.rate = 2e6;
    check(src.start(sc, err) && src.publisher().active() && src.publisher().status().streamId != id1,
          "after a restart at 2 MS/s it publishes again, with a new stream ID");
    check(sub.start(rc, err) && sub.stats().rate == 2e6, "the subscriber opens at 2 MS/s");
    sub.stop();

    // Multicast: both sides use the group. Skipped where the computer has no
    // multicast route (some CI containers).
    src.stopPublishing();
    StreamConfig mc = rc;
    mc.args = "239.10.0.1:" + std::to_string(15830 + getpid() % 100);
    if (src.startPublishing({mc.args, 1}, err) && sub.start(mc, err)) {
        check(sub.stats().rate == 2e6, "multicast " + mc.args + ": the subscriber receives the stream");
        check(waitFor([&] { return sub.stats().received > 200000; }, 2), "multicast: samples arrive");
        sub.stop();
    } else {
        printf("  [SKIP] multicast %s: %s\n", mc.args.c_str(), err.c_str());
    }
    src.stop();

    // Errors at open.
    rc.args = "192.0.2.1:5600";  // TEST-NET-1: not on this computer
    check(!sub.start(rc, err) && contains(err, "not an address of this computer"), "192.0.2.1: " + err);
    rc.args = "localhost:5600";
    check(!sub.start(rc, err) && contains(err, "not an IPv4"), "localhost: " + err);
    rc.args = addr;  // nobody publishes now
    const auto t0 = std::chrono::steady_clock::now();
    check(!sub.start(rc, err) && contains(err, "no stream"), "no source: " + err);
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(waited > 2.5 && waited < 4, "after 3 s (" + std::to_string(waited).substr(0, 4) + " s)");

    const std::vector<std::string> drivers = sdrDrivers();
    check(std::find(drivers.begin(), drivers.end(), "zmq") != drivers.end(), "zmq is in the driver list");
    bool listed = false;
    for (const DeviceInfo& d : enumerateDevices("zmq")) listed = listed || (d.driver == "zmq" && d.args == defaultStreamAddress());
    check(listed, "the device list has a ZMQ stream on the default address");
}
#endif
}  // namespace

int main() {
    checkHeader();
    checkInfo();
    checkRejects();
    checkAddress();
    checkDefaultAddress();
    checkTracker();
#ifdef WVT_HAVE_ZMQ
    checkSockets();
    checkPublisher();
    checkSubscriber();
#else
    printf("libzmq: not in this build (WVT_WITH_ZMQ), the socket tests are skipped\n");
    StreamPublisher pub;
    std::string err;
    check(!pub.start(std::make_shared<SampleRing>(4096, 4), {}, {"127.0.0.1:5600", 1}, err) &&
              err.find("without ZMQ") != std::string::npos,
          "the publisher reports that this build has no ZMQ");
#endif
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
