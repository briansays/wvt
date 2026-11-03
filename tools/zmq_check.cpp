// ZMQ streaming self-test (issue #96).
//  - The wire format (src/net/zmq_wire): the header, the info message, the
//    rejection of bad messages, the stream address, the default address and
//    the sample index tracker. These need no libzmq and run in every build.
//  - With WVT_HAVE_ZMQ: the libzmq that wvt links has the draft RADIO and
//    DISH sockets, and one message of the largest size that wvt sends goes
//    through udp:// on the loopback.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "net/zmq_wire.h"

#ifdef WVT_HAVE_ZMQ
#include <zmq.h>
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
#else
    printf("libzmq: not in this build (WVT_WITH_ZMQ), the socket test is skipped\n");
#endif
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
