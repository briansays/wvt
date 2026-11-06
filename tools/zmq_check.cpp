// ZMQ streaming self-test (issue #96).
//  - The wire format (src/net/zmq_wire): the header, the info message, the
//    rejection of bad messages, the stream address, the default address and
//    the sample index tracker. These need no libzmq and run in every build.
//  - The message formats from files (src/net/zmq_format, issue #101): the
//    example files in config/, the built-in format as a file, the field
//    types, the sample conversions and the errors of bad files. No libzmq.
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
#include "net/zmq_format.h"
#include "net/zmq_pub.h"
#include "net/zmq_wire.h"

#include <sys/stat.h>
#include <unistd.h>

#include <complex>
#include <cstdlib>

#ifdef WVT_HAVE_ZMQ
#include <unistd.h>
#include <zmq.h>

#include <cstdlib>
#include <functional>

#include "common/json.h"
#include "device/live.h"
#include "device/sdr_device.h"

#include <mutex>
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

// ---- message formats (issue #101) -----------------------------------------

#ifndef WVT_CONFIG_EXAMPLES
#define WVT_CONFIG_EXAMPLES "config"
#endif

// Parses a format from text; err gets the error.
bool formatFrom(const std::string& text, StreamFormat& f, std::string& err, std::vector<std::string>* warn = nullptr) {
    std::vector<std::string> w;
    f = StreamFormat{};
    err.clear();
    const bool ok = parseStreamFormat(text, "test.ini", f, w, err);
    if (warn) *warn = std::move(w);
    return ok;
}

bool decodes(const StreamFormat& f, const std::vector<uint8_t>& msg, StreamMessage& m, std::vector<uint8_t>& scratch,
             std::string* whyOut = nullptr, FormatExtra* extraOut = nullptr) {
    FormatExtra extra;
    std::string why;
    const bool ok = decodeStreamMessage(f, msg.data(), msg.size(), m, extra, scratch, why);
    if (whyOut) *whyOut = why;
    if (extraOut) *extraOut = extra;
    return ok;
}

void putBE(std::vector<uint8_t>& v, size_t at, uint64_t x, int n) {
    for (int i = 0; i < n; ++i) v[at + (size_t)i] = (uint8_t)(x >> (8 * (n - 1 - i)));
}

int16_t i16At(const uint8_t* p, size_t i) {
    int16_t v = 0;
    memcpy(&v, p + 2 * i, 2);
    return v;
}

void checkFormats() {
    printf("message formats (issue #101)\n");
    std::string err, why;
    std::vector<std::string> warn;
    StreamMessage m;
    std::vector<uint8_t> scratch;

    // The built-in format is always there.
    const auto none = scanStreamFormats("");
    check(none.size() == 1 && none[0].ok() && none[0].format.builtin && none[0].format.id == "wvt",
          "no config folder: only the built-in format");
    check(findStreamFormat(none, "WVT") == &none[0] && !findStreamFormat(none, "other"), "find by id, any case");

    // The examples in config/.
    const auto list = scanStreamFormats(WVT_CONFIG_EXAMPLES);
    bool allOk = list.size() == 5;
    std::string bad;
    for (const auto& e : list) {
        if (!e.ok() || !e.warnings.empty()) {
            allOk = false;
            bad += " " + e.format.id + ": " + e.error + (e.warnings.empty() ? "" : e.warnings[0]);
        }
    }
    check(allOk, "the 4 example files load without errors or warnings" + bad);
    const auto* gr = findStreamFormat(list, "gnuradio_sub_cf32");
    const auto* raw = findStreamFormat(list, "raw_pull_ci16");
    const auto* ex = findStreamFormat(list, "example_header");
    const auto* wf = findStreamFormat(list, "wvt_ci16");
    check(gr && raw && ex && wf && list[1].format.id == "example_header", "ids from the file names, sorted");
    if (!gr || !raw || !ex || !wf) return;
    check(gr->format.socket == FormatSocket::Sub && gr->format.tcp && !gr->format.bind && gr->format.frame == 0 &&
              gr->format.address == "127.0.0.1:5555" && gr->format.name == "GNU Radio PUB Sink (cf32)",
          "GNU Radio: sub, tcp, connect, the last frame, the address and the name");
    check(raw->format.socket == FormatSocket::Pull && raw->format.fixedRate == 1e6 && raw->format.fixedFc == 433.92e6,
          "raw PULL: pull, the rate and frequency from [stream]");

    // The built-in format and the same format as a file decode a ci16 message the same.
    {
        StreamHeader h = sampleHeader();
        h.count = 5;
        h.flags = 0;
        std::vector<uint8_t> msg = dataMessage(h);
        for (size_t i = 0; i < 10; ++i) {
            const auto v = (int16_t)(1000 * (int)i - 4000);
            memcpy(msg.data() + kStreamHeaderBytes + 2 * i, &v, 2);
        }
        StreamMessage a, b;
        std::vector<uint8_t> s1, s2;
        FormatExtra xa, xb;
        const bool okA = decodes(builtinStreamFormat(), msg, a, s1, nullptr, &xa);
        const bool okB = decodes(wf->format, msg, b, s2, &why, &xb);
        check(okA && okB && a.h.fc == b.h.fc && a.h.rate == b.h.rate && a.h.gain == b.h.gain && a.h.index == b.h.index &&
                  a.h.count == b.h.count && a.h.timeNs == b.h.timeNs && a.h.fmt == b.h.fmt && xa.hasIndex && xb.hasIndex &&
                  a.sampleBytes == b.sampleBytes && memcmp(a.samples, b.samples, a.sampleBytes) == 0,
              "zmq_wvt_ci16.ini decodes a wvt message like the built-in format " + why);
        check(b.samples == msg.data() + kStreamHeaderBytes, "little-endian ci16: the samples are not copied");
        h.fmt = SampleFormat::CU8;
        check(!decodes(wf->format, dataMessage(h), b, s2, &why) && why.find("match.format") != std::string::npos,
              "a cu8 wvt message does not match the ci16 file: " + why);
        const auto info = packStreamInfo(sampleHeader(), {"uhd", "B205mini"});
        check(!decodes(wf->format, info, b, s2, &why) && why.find("match.kind") != std::string::npos,
              "an info message does not match: " + why);
    }

    // The example header: big-endian fields, a scale, a counter, big-endian ci16.
    {
        std::vector<uint8_t> msg(32 + 3 * 4, 0);
        memcpy(msg.data(), "ABCD", 4);
        msg[4] = 1;
        putBE(msg, 8, 77, 4);
        double fc = 2.4e9;
        uint64_t bits = 0;
        memcpy(&bits, &fc, 8);
        putBE(msg, 12, bits, 8);
        putBE(msg, 20, 2400, 4);  // kS/s
        putBE(msg, 24, 1793750400000000000ULL, 8);
        const int16_t vals[6] = {1, -2, 300, -400, 32767, -32768};
        for (size_t i = 0; i < 6; ++i) putBE(msg, 32 + 2 * i, (uint16_t)vals[i], 2);
        FormatExtra x;
        const bool ok = decodes(ex->format, msg, m, scratch, &why, &x);
        bool same = ok && m.sampleBytes == 12;
        for (size_t i = 0; same && i < 6; ++i) same = i16At(m.samples, i) == vals[i];
        check(ok && m.h.fc == 2.4e9 && m.h.rate == 2.4e6 && m.h.gain == 0 && m.h.count == 3 && x.hasSeq && x.seq == 77 &&
                  !x.hasIndex && m.h.timeNs == 1793750400000000000ULL && m.h.fmt == SampleFormat::CI16,
              "example header: fc f64be, rate u32be * 1e3, seq, time " + why);
        check(same, "big-endian ci16 samples become little-endian ci16");
        msg[0] = 'X';
        check(!decodes(ex->format, msg, m, scratch, &why) && why.find("match.magic") != std::string::npos,
              "another magic: " + why);
        msg[0] = 'A';
        msg[4] = 2;
        check(!decodes(ex->format, msg, m, scratch, &why) && why.find("match.type") != std::string::npos,
              "another type: " + why);
        msg[4] = 1;
        msg.resize(31);
        check(!decodes(ex->format, msg, m, scratch, &why) && why.find("too short") != std::string::npos,
              "shorter than the header: " + why);
    }

    // Raw cf32: no header, the rate from the file.
    {
        std::vector<uint8_t> msg(8 * 4);
        const float v[8] = {0.5f, -0.5f, 0.25f, 1, 0, 0, -1, 0.125f};
        memcpy(msg.data(), v, sizeof v);
        const bool ok = decodes(gr->format, msg, m, scratch, &why);
        check(ok && m.h.count == 4 && m.h.rate == 2e6 && m.h.fc == 100e6 && m.samples == msg.data() &&
                  m.h.fmt == SampleFormat::CF32,
              "raw cf32: 4 samples in place, rate and fc from the file " + why);
        msg.resize(7);
        check(!decodes(gr->format, msg, m, scratch, &why) && why.find("whole samples") != std::string::npos,
              "7 bytes are not whole samples: " + why);
        msg.assign(gr->format.maxBytes + 8, 0);
        check(!decodes(gr->format, msg, m, scratch, &why) && why.find("too large") != std::string::npos,
              "larger than max_bytes: " + why);
    }

    // Conversions: Q/I order, ci8 and cf64 to cf32, a count field, signed matches.
    {
        StreamFormat f;
        check(formatFrom("[socket]\ntype = pull\n[samples]\nformat = cu8\norder = qi\n[stream]\nrate = 1e6\n", f, err),
              "cu8, order qi " + err);
        const std::vector<uint8_t> qi = {1, 2, 3, 4};
        check(decodes(f, qi, m, scratch) && m.h.fmt == SampleFormat::CU8 && m.sampleBytes == 4 && m.samples[0] == 2 &&
                  m.samples[1] == 1 && m.samples[2] == 4 && m.samples[3] == 3,
              "cu8 Q/I becomes I/Q");
        check(formatFrom("[socket]\ntype = pull\n[samples]\nformat = ci8\n[stream]\nrate = 1e6\n", f, err), "ci8 " + err);
        const std::vector<uint8_t> c8 = {64, (uint8_t)-64};
        std::complex<float> c{};
        const bool ok8 = decodes(f, c8, m, scratch);
        if (ok8) memcpy(&c, m.samples, sizeof c);
        check(ok8 && m.h.fmt == SampleFormat::CF32 && m.sampleBytes == 8 && c == std::complex<float>(0.5f, -0.5f),
              "ci8 becomes cf32 (64 -> 0.5)");
        check(formatFrom("[socket]\ntype = pull\n[samples]\nformat = cf64\nendian = big\n[stream]\nrate = 1e6\n", f, err),
              "cf64 big endian " + err);
        std::vector<uint8_t> c64(16);
        const double re = 0.75, im = -0.25;
        uint64_t b1 = 0, b2 = 0;
        memcpy(&b1, &re, 8);
        memcpy(&b2, &im, 8);
        putBE(c64, 0, b1, 8);
        putBE(c64, 8, b2, 8);
        const bool ok64 = decodes(f, c64, m, scratch);
        if (ok64) memcpy(&c, m.samples, sizeof c);
        check(ok64 && c == std::complex<float>(0.75f, -0.25f), "cf64 big endian becomes cf32");

        check(formatFrom("[socket]\ntype = sub\n[header]\nbytes = 4\ncount = u16le @ 0\nmatch.m = i16be @ 2 == -2\n"
                         "[samples]\nformat = ci16\n[stream]\nrate = 1e6\n",
                         f, err),
              "a count field and a signed match " + err);
        check(f.matches.size() == 1 && f.matches[0].bytes == std::string("\xff\xfe", 2), "i16be -2 is the bytes ff fe");
        std::vector<uint8_t> msg = {2, 0, 0xff, 0xfe, 1, 0, 2, 0, 3, 0, 4, 0, 9, 9, 9, 9};
        check(decodes(f, msg, m, scratch) && m.h.count == 2 && m.sampleBytes == 8, "count 2: the rest of the message is not used");
        msg[0] = 4;
        check(!decodes(f, msg, m, scratch, &why) && why.find("count") != std::string::npos, "count 4 does not fit: " + why);
    }

    // Files with errors and warnings.
    {
        const std::string ok = "[socket]\ntype = sub\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n";
        StreamFormat f;
        struct Bad {
            std::string text, want, what;
        };
        const Bad bads[] = {
            {"[samples]\nformat = cf32\n[stream]\nrate = 1e6\n", "type is missing", "no socket type"},
            {"[socket]\ntype = dish\ntransport = tcp\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n",
             "dish works only", "dish on tcp"},
            {"[socket]\ntype = pull\ntransport = udp\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n",
             "udp works only", "pull on udp"},
            {"[socket]\ntype = dish\nmode = connect\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n",
             "mode = bind", "dish that connects"},
            {"[socket]\ntype = sub\n[samples]\nformat = cf32\n", "rate is missing", "no rate"},
            {"[socket]\ntype = sub\n[stream]\nrate = 1e6\n", "format is missing", "no sample format"},
            {ok + "[samples]\nformat = ci12\n", "ci12", "an unknown sample format"},
            {ok + "[header]\nbytes = 8\nfc = f64le @ 4\n", "outside the header", "a field outside the header"},
            {ok + "[header]\nbytes = 4\nmatch.m = \"ABCDE\" @ 0\n", "outside the header", "a match outside the header"},
            {ok + "[header]\nbytes = 8\nfc = u24le @ 0\n", "unknown type", "an unknown field type"},
            {ok + "[header]\nbytes = 8\nindex = u64le @ 0 * 2\n", "scale is allowed only", "a scale on index"},
            {ok + "[header]\nbytes = 8\ncount = f32le @ 0\n", "integer type", "a float count"},
            {ok + "[header]\nbytes = 8\nmatch.m = u8 @ 0 == 256\n", "does not fit", "a match value too large"},
            {ok + "[header]\nbytes = 16\nindex = u64le @ 0\nseq = u32le @ 8\n", "not both", "index and seq"},
            {"[socket]\ntype = sub\n[samples]\nformat = cf32\n[stream]\nrate = 2.4M\n", "not a number", "2.4M"},
            {"type = sub\n", "before the first [section]", "a key before a section"},
            {"[socket]\ntype = router\n", "not dish, sub or pull", "an unknown socket type"},
            {ok + "[header]\nfc = f64le 0\n", "TYPE @ OFFSET", "a field without @"},
        };
        for (const Bad& b : bads)
            check(!formatFrom(b.text, f, err) && err.find(b.want) != std::string::npos, "error: " + b.what + ": " + err);
        check(formatFrom(ok + "[socket]\naddres = 1.2.3.4:5\n[extra]\nx = 1\n", f, err, &warn) && warn.size() == 2 &&
                  warn[0].find("did you mean 'address'") != std::string::npos && warn[1].find("[extra]") != std::string::npos,
              "warnings: a typo with a suggestion, an unknown section");
        check(formatFrom(ok, f, err) && f.tcp && !f.bind && f.name == "test.ini", "sub defaults: tcp, connect, the file name");
        check(formatFrom(ok + "[format]\nname = \"A # B\"   # comment\n", f, err) && f.name == "A # B",
              "a quoted value keeps its #; the comment after it is removed");
    }

    // A folder with a bad file and a file that takes the built-in name.
    {
        const char* tmp = getenv("TMPDIR");
        const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/wvt_zmq_formats_" + std::to_string(getpid());
        mkdir(dir.c_str(), 0755);
        auto write = [&](const char* name, const char* text) {
            if (FILE* fp = fopen((dir + "/" + name).c_str(), "w")) {
                fputs(text, fp);
                fclose(fp);
            }
        };
        write("zmq_bad.ini", "[socket]\ntype = dish\ntransport = tcp\n");
        write("zmq_wvt.ini", "[socket]\ntype = sub\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n");
        write("zmq_good.ini", "[socket]\ntype = sub\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n");
        write("other.ini", "not a format");
        const auto l = scanStreamFormats(dir);
        const auto* b = findStreamFormat(l, "bad");
        const auto* g = findStreamFormat(l, "good");
        check(l.size() == 4 && b && !b->ok() && b->error.find("zmq_bad.ini") != std::string::npos && g && g->ok(),
              "a bad file is listed with its error; other.ini is not a format file");
        check(l[3].format.id == "wvt" && !l[3].ok() && l[3].error.find("taken") != std::string::npos,
              "zmq_wvt.ini cannot take the built-in name: " + (l.size() > 3 ? l[3].error : std::string()));
        for (const char* n : {"zmq_bad.ini", "zmq_wvt.ini", "zmq_good.ini", "other.ini"}) unlink((dir + "/" + n).c_str());
        rmdir(dir.c_str());
    }

    // The format by id from the config folder.
    {
        StreamFormat f;
        setStreamFormatDir(WVT_CONFIG_EXAMPLES);
        check(resolveStreamFormat("", f, err) && f.builtin && resolveStreamFormat("WVT", f, err) && f.builtin,
              "\"\" and \"wvt\" are the built-in format");
        check(resolveStreamFormat("gnuradio_sub_cf32", f, err) && f.socket == FormatSocket::Sub, "a file by its id");
        check(streamFormatSummary(f) == "sub, tcp connect 127.0.0.1:5555, cf32 (complex float32)",
              "the summary of a file: " + streamFormatSummary(f));
        check(resolveStreamFormat("example_header", f, err) && streamFormatSummary(f).find("ci16 (complex int16) big endian") != std::string::npos,
              "the summary says big endian: " + streamFormatSummary(f));
        check(streamFormatSummary(builtinStreamFormat()) == "dish, udp bind, the wvt header",
              "the summary of the built-in format: " + streamFormatSummary(builtinStreamFormat()));
        check(isBuiltinFormatId("") && isBuiltinFormatId("wvt") && isBuiltinFormatId("WVT") && !isBuiltinFormatId("wvt_ci16"),
              "isBuiltinFormatId");
        check(!resolveStreamFormat("nope", f, err) && err.find("no ZMQ format 'nope'") != std::string::npos,
              "an unknown id: " + err);
        setStreamFormatDir("");
        check(!resolveStreamFormat("gnuradio_sub_cf32", f, err) && err.find("no config folder") != std::string::npos,
              "no config folder: only the built-in format: " + err);
    }

    // Where the subscriber receives.
    {
        FormatEndpoint ep;
        check(formatEndpoint(builtinStreamFormat(), "", ep, err) && ep.endpoint.rfind("udp://", 0) == 0 &&
                  ep.udp.port == kStreamDefaultPort,
              "built-in, no address: the default address on udp (" + ep.endpoint + ")");
        check(formatEndpoint(builtinStreamFormat(), "udp://239.1.2.3:7000", ep, err) && ep.udp.multicast &&
                  ep.endpoint == "udp://239.1.2.3:7000",
              "built-in: udp:// before a multicast group");
        check(!formatEndpoint(builtinStreamFormat(), "tcp://1.2.3.4:5", ep, err) && err.find("not tcp") != std::string::npos,
              "built-in: tcp:// is refused: " + err);
        StreamFormat sub;
        formatFrom("[socket]\ntype = sub\naddress = 10.0.0.5:5555\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n", sub,
                   err);
        check(formatEndpoint(sub, "", ep, err) && ep.endpoint == "tcp://10.0.0.5:5555" && ep.port == 5555,
              "sub: the address of the file");
        check(formatEndpoint(sub, "tcp://host.local:6000", ep, err) && ep.endpoint == "tcp://host.local:6000" &&
                  ep.text == "host.local:6000",
              "sub: a typed tcp:// address with a host name");
        check(!formatEndpoint(sub, "udp://1.2.3.4:5", ep, err) && err.find("not udp") != std::string::npos,
              "sub: udp:// is refused: " + err);
        check(!formatEndpoint(sub, "1.2.3.4", ep, err) && err.find("host:port") != std::string::npos, "no port: " + err);
        check(!formatEndpoint(sub, "1.2.3.4:70000", ep, err), "port 70000 is refused");
        check(!formatEndpoint(sub, "*:5555", ep, err) && err.find("mode = bind") != std::string::npos,
              "* with connect: " + err);
        sub.address.clear();
        check(!formatEndpoint(sub, "", ep, err) && err.find("no address") != std::string::npos, "sub without an address: " + err);
        StreamFormat pull;
        formatFrom("[socket]\ntype = pull\nmode = bind\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n", pull, err);
        check(formatEndpoint(pull, "*:5556", ep, err) && ep.endpoint == "tcp://*:5556", "pull that binds: *:port");
    }

    // The message counter.
    {
        MessageCounter c(8);
        using E = MessageCounter::Event;
        bool order = c.next(250).event == E::First;
        for (uint64_t s = 251; s < 256 + 3; ++s) order = order && c.next(s).event == E::InOrder;  // wraps at 256
        check(order, "8 bits: 250 to 255, then 0, 1, 2 in order");
        auto r = c.next(6);
        check(r.event == E::Lost && r.lost == 3, "3 to 5 missing: 3 messages lost");
        check(c.next(6).event == E::Late && c.next(5).event == E::Late, "a repeat and an older message: late");
        check(c.next(7).event == E::InOrder, "then in order again");
        MessageCounter w(64);
        w.next(100000);
        check(w.next(100000 - 5000).event == E::Restart, "64 bits: 5000 back is a restart");
        check(w.next(95001 + 2000000).event == E::Restart, "2 000 000 ahead is a restart");
        check(w.next(95001 + 2000001).event == E::InOrder, "then in order from there");
        check(fieldTypeBits(FieldType::U16) == 16 && fieldTypeBits(FieldType::I64) == 64, "field type bits");
    }
}

// The publisher's side of the formats (phase 6): encode, then decode.
void checkEncode() {
    printf("publishing with message formats (issue #101)\n");
    std::string err, why;
    std::vector<std::string> warn;
    setStreamFormatDir(WVT_CONFIG_EXAMPLES);

    // 8 ci16 samples of the ring: (k * 1000, -k * 1000).
    constexpr size_t n = 8;
    int16_t ring[2 * n];
    for (size_t k = 0; k < n; ++k) {
        ring[2 * k] = (int16_t)(1000 * k);
        ring[2 * k + 1] = (int16_t)(-1000 * (int)k);
    }
    StreamHeader h;
    h.fc = 433.92e6;
    h.rate = 1e6;
    h.gain = 12.5;
    h.index = 123456789012ULL;
    h.timeNs = 1700000000123456789ULL;
    std::vector<uint8_t> out, scratch;
    StreamMessage m;
    FormatExtra extra;

    // Each example file: what the encoder writes, the decoder reads back.
    for (const char* id : {"gnuradio_sub_cf32", "raw_pull_ci16", "example_header", "wvt_ci16"}) {
        StreamFormat f;
        check(resolveStreamFormat(id, f, err) && checkPublishFormat(f, err), std::string(id) + ": can publish " + err);
        h.rate = f.rate.set ? 1e6 : f.fixedRate;
        h.fc = f.fc.set ? 433.92e6 : f.fixedFc;
        const bool enc = encodeStreamMessage(f, h, 77, SampleFormat::CI16, (const uint8_t*)ring, n, out, why);
        const bool dec = enc && decodeStreamMessage(f, out.data(), out.size(), m, extra, scratch, why);
        bool same = dec && m.h.count == n;
        for (size_t k = 0; same && k < n; ++k) {
            float re = 0, im = 0;
            if (m.h.fmt == SampleFormat::CI16) {
                re = i16At(m.samples, 2 * k) / 32768.f;
                im = i16At(m.samples, 2 * k + 1) / 32768.f;
            } else {
                memcpy(&re, m.samples + 8 * k, 4);
                memcpy(&im, m.samples + 8 * k + 4, 4);
            }
            same = std::fabs(re - ring[2 * k] / 32768.f) < 1e-6f && std::fabs(im - ring[2 * k + 1] / 32768.f) < 1e-6f;
        }
        check(same && m.h.rate == h.rate && m.h.fc == h.fc, std::string(id) + ": the samples, the rate and fc come back " + why);
        if (f.seq.set) check(extra.hasSeq && extra.seq == 77, std::string(id) + ": the message counter");
        if (f.index.set) check(extra.hasIndex && m.h.index == h.index, std::string(id) + ": the sample index");
        if (f.timeNs.set) check(m.h.timeNs == h.timeNs, std::string(id) + ": the time, exact to the ns");
        if (f.gain.set) check(m.h.gain == h.gain, std::string(id) + ": the gain");
    }
    // The wvt file writes the built-in wire format: the built-in decoder reads it.
    {
        StreamFormat f;
        resolveStreamFormat("wvt_ci16", f, err);
        h.rate = 1e6;
        encodeStreamMessage(f, h, 0, SampleFormat::CI16, (const uint8_t*)ring, n, out, why);
        check(parseStreamMessage(out.data(), out.size(), m, why) && m.h.count == n && m.h.index == h.index &&
                  m.h.fmt == SampleFormat::CI16 && i16At(m.samples, 2) == 1000,
              "zmq_wvt_ci16.ini writes messages that the built-in format reads " + why);
    }

    // Every sample format, byte order and I/Q order, from each ring format.
    {
        const char* fmts[] = {"cu8", "ci8", "ci16", "ci32", "cf32", "cf64"};
        int bad = 0, total = 0;
        for (const char* sf : fmts)
            for (const char* endian : {"little", "big"})
                for (const char* order : {"iq", "qi"}) {
                    StreamFormat f;
                    const std::string text = std::string("[socket]\ntype = pull\n[header]\nbytes = 4\nseq = u32be @ 0\n[samples]\nformat = ") +
                                             sf + "\nendian = " + endian + "\norder = " + order + "\n[stream]\nrate = 1e6\n";
                    if (!formatFrom(text, f, err)) {
                        ++bad;
                        continue;
                    }
                    // From cf32 samples of the ring: k / 16 and -k / 32, exact in every format but cu8.
                    std::complex<float> c[n];
                    for (size_t k = 0; k < n; ++k) c[k] = {(float)k / 16.f, -(float)k / 32.f};
                    ++total;
                    if (!encodeStreamMessage(f, h, 1, SampleFormat::CF32, (const uint8_t*)c, n, out, why) ||
                        out.size() != 4 + n * (size_t)formatBytesPerSample(f.sampleFmt) ||
                        !decodeStreamMessage(f, out.data(), out.size(), m, extra, scratch, why)) {
                        ++bad;
                        continue;
                    }
                    FormatParams fp;
                    fp.fmt = m.h.fmt;
                    std::complex<float> back[n];
                    decodeSamples(fp, m.samples, n, back);
                    // cu8 has no 0: 127.5 is 128, +0.0039 in I and Q.
                    const float tol = f.sampleFmt == SampleFormat::CU8 ? 0.006f : f.sampleFmt == SampleFormat::CI8 ? 0.004f : 1e-6f;
                    for (size_t k = 0; k < n; ++k)
                        if (std::abs(back[k] - c[k]) > tol) {
                            ++bad;
                            printf("  layout %s %s %s: sample %zu is (%g, %g), not (%g, %g)\n", sf, endian, order, k,
                                   back[k].real(), back[k].imag(), c[k].real(), c[k].imag());
                            break;
                        }
                }
        check(bad == 0 && total == 24, "24 sample layouts (6 formats x byte order x I/Q order): encode, then decode (" +
                                           std::to_string(bad) + " bad)");
    }

    // Values that do not fit, and the limits of a message.
    {
        StreamFormat f;
        const std::string base = "[socket]\ntype = sub\n[samples]\nformat = ci16\n[stream]\nrate = 1e6\n";
        check(formatFrom(base + "[header]\nbytes = 2\nfc = u16le @ 0\n", f, err), "a u16 fc field");
        h.fc = 2.4e9;
        check(!encodeStreamMessage(f, h, 0, SampleFormat::CI16, (const uint8_t*)ring, 1, out, why) &&
                  why.find("fc 2400000000 does not fit in u16le") != std::string::npos,
              "fc 2.4e9 in u16: " + why);
        check(formatFrom(base + "[header]\nbytes = 2\nfc = u16le @ 0 * 1e6\n", f, err) &&
                  encodeStreamMessage(f, h, 0, SampleFormat::CI16, (const uint8_t*)ring, 1, out, why) && out[0] == 0x60 &&
                  out[1] == 0x09,
              "with the scale 1e6: 2400 in the field");
        check(formatFrom(base + "[header]\nbytes = 1\ncount = u8 @ 0\n", f, err) && publishMaxSamples(f) == 255,
              "a u8 count: at most 255 samples in a message");
        check(formatFrom(base, f, err) && publishMaxSamples(f) == (256u << 10) / 4, "tcp: 256 KiB of samples");
        check(formatFrom("[socket]\ntype = dish\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n", f, err) &&
                  publishMaxSamples(f) == 8064 / 8,
              "dish: a datagram of 8064 bytes");
        check(formatFrom(base + "[socket]\ntopic = iq\nframes = 1\n", f, err) && !checkPublishFormat(f, err) &&
                  err.find("frames = last or 2") != std::string::npos,
              "a topic needs frames = last or 2: " + err);
        check(formatFrom(base + "[socket]\ntopic = iq\nframes = 2\n", f, err) && checkPublishFormat(f, err), "a topic with frames = 2");
        check(formatFrom(base + "[socket]\nframes = 3\n", f, err) && !checkPublishFormat(f, err), "frames = 3 without a topic: " + err);
        check(formatFrom(base + "[socket]\nmax_bytes = 64\n[header]\nbytes = 63\n", f, err) && !checkPublishFormat(f, err) &&
                  err.find("no sample fits") != std::string::npos,
              "a header that leaves no room: " + err);
    }

    // Where the publisher sends: the other mode.
    {
        StreamFormat f;
        FormatEndpoint ep;
        resolveStreamFormat("gnuradio_sub_cf32", f, err);
        check(publishEndpoint(f, "*:5555", ep, err) && ep.endpoint == "tcp://*:5555",
              "the subscriber connects, so the publisher binds: * is allowed " + err);
        check(!formatEndpoint(f, "*:5555", ep, err), "the subscriber cannot connect to *");
        f.bind = true;
        check(!publishEndpoint(f, "*:5555", ep, err) && publishEndpoint(f, "", ep, err) && ep.text == "127.0.0.1:5555",
              "a subscriber that binds: the publisher connects, to the file's address");
    }

    // The loop check: a publisher that sends to this wvt's own subscriber.
    {
        StreamFormat gr, pull, wvt, wvtFile;
        resolveStreamFormat("gnuradio_sub_cf32", gr, err);
        resolveStreamFormat("raw_pull_ci16", pull, err);
        resolveStreamFormat("wvt", wvt, err);
        resolveStreamFormat("wvt_ci16", wvtFile, err);
        check(publishReachesSubscriber(wvt, "127.0.0.1:5600", wvt, "127.0.0.1:5600"), "built-in to built-in, same address: a loop");
        check(!publishReachesSubscriber(wvt, "127.0.0.1:5600", wvt, "127.0.0.1:5601"), "another port: no loop");
        check(publishReachesSubscriber(wvtFile, "127.0.0.1:5600", wvt, "127.0.0.1:5600"),
              "the built-in format to the wvt file (a dish in group wvt): a loop");
        check(publishReachesSubscriber(gr, "127.0.0.1:5555", gr, "*:5555"), "PUB binds on * where SUB connects: a loop");
        check(publishReachesSubscriber(gr, "localhost:5555", gr, "127.0.0.1:5555"), "localhost and 127.0.0.1: a loop");
        check(!publishReachesSubscriber(gr, "127.0.0.1:5555", pull, "127.0.0.1:5555"), "PUSH does not reach SUB");
        check(!publishReachesSubscriber(gr, "10.0.0.2:5555", gr, "127.0.0.1:5555"), "another host: no loop");
        check(!publishReachesSubscriber(gr, "127.0.0.1:5555", wvt, "127.0.0.1:5555"), "udp does not reach tcp");
    }
    h = StreamHeader{};
    setStreamFormatDir("");
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

// ---- the subscriber with format files (issue #101) -------------------------

// Sends messages from its own thread on a socket that binds on the loopback.
// make(seq) gives the frames of message `seq`; no frames: skip that seq.
class Sender {
public:
    using Make = std::function<std::vector<std::vector<uint8_t>>(uint64_t seq)>;
    explicit Sender(int type) : ctx_(zmq_ctx_new()) {
        sock_ = zmq_socket(ctx_, type);
        if (sock_ && zmq_bind(sock_, "tcp://127.0.0.1:*") == 0) {
            char buf[256] = {};
            size_t len = sizeof buf;
            zmq_getsockopt(sock_, ZMQ_LAST_ENDPOINT, buf, &len);
            endpoint_ = buf;
        }
    }
    ~Sender() {
        stop();
        const int linger = 0;
        if (sock_) zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof linger);
        if (sock_) zmq_close(sock_);
        zmq_ctx_term(ctx_);
    }
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    // "127.0.0.1:port", for the Address field.
    std::string address() const { return endpoint_.size() > 6 ? endpoint_.substr(6) : std::string(); }
    void run(Make make, int periodMs = 1) {
        th_ = std::thread([this, make = std::move(make), periodMs] {
            for (uint64_t seq = 0; !stop_; ++seq) {
                const auto frames = make(seq);
                for (size_t i = 0; i < frames.size(); ++i) {
                    const int flags = ZMQ_DONTWAIT | (i + 1 < frames.size() ? ZMQ_SNDMORE : 0);
                    zmq_send(sock_, frames[i].data(), frames[i].size(), flags);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
            }
        });
    }
    void stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }

private:
    void* ctx_;
    void* sock_ = nullptr;
    std::string endpoint_;
    std::thread th_;
    std::atomic<bool> stop_{false};
};

// What a started device delivered.
struct Received {
    std::mutex m;
    std::vector<uint8_t> bytes;
    std::vector<int64_t> gaps;
    std::vector<std::string> gapWhy;
};

void startInto(SdrDevice& dev, Received& rx, int bytesPerSample) {
    std::string err;
    dev.start(
        [&rx, bytesPerSample](const uint8_t* p, int64_t n) {
            std::lock_guard<std::mutex> lk(rx.m);
            rx.bytes.insert(rx.bytes.end(), p, p + n * bytesPerSample);
        },
        [&rx](int64_t lost, const char* why) {
            std::lock_guard<std::mutex> lk(rx.m);
            rx.gaps.push_back(lost);
            rx.gapWhy.emplace_back(why);
        },
        err);
}

bool waitFor(const std::function<bool()>& ok, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ok();
}

size_t receivedBytes(Received& rx) {
    std::lock_guard<std::mutex> lk(rx.m);
    return rx.bytes.size();
}

void checkFormatDevice() {
    printf("subscriber with format files (issue #101)\n");
    setStreamFormatDir(WVT_CONFIG_EXAMPLES);
    std::string err;
    StreamConfig cfg;
    cfg.driver = "zmq";

    // 1. GNU Radio PUB Sink: raw cf32 on PUB / SUB.
    {
        constexpr int n = 64;
        Sender tx(ZMQ_PUB);
        tx.run([](uint64_t) {
            std::vector<float> v(2 * n);
            for (int k = 0; k < n; ++k) {
                v[2 * (size_t)k] = 0.01f * (float)k;
                v[2 * (size_t)k + 1] = -0.01f * (float)k;
            }
            std::vector<uint8_t> b(v.size() * sizeof(float));
            memcpy(b.data(), v.data(), b.size());
            return std::vector<std::vector<uint8_t>>{b};
        });
        cfg.format = "gnuradio_sub_cf32";
        cfg.args = tx.address();
        auto dev = openDevice(cfg, err);
        check(dev && dev->sampleRate() == 2e6 && dev->centerFreq() == 100e6 && dev->format() == SampleFormat::CF32,
              "GNU Radio cf32 on SUB: opens with the rate and frequency of the file " + err);
        if (dev) {
            check(dev->description().find("GNU Radio PUB Sink") != std::string::npos, "the description: " + dev->description());
            Received rx;
            startInto(*dev, rx, 8);
            const bool got = waitFor([&] { return receivedBytes(rx) >= 10 * n * 8; }, 3);
            dev->stop();
            bool same = got;
            for (int k = 0; same && k < n; ++k) {
                float iq[2];
                memcpy(iq, rx.bytes.data() + 8 * (size_t)k, 8);
                same = iq[0] == 0.01f * (float)k && iq[1] == -0.01f * (float)k;
            }
            check(got && same, "the samples arrive as sent (" + std::to_string(rx.bytes.size() / 8) + " samples)");
            check(rx.gaps.empty(), "no format field for loss: no gaps");
        }
    }

    // 2. Raw ci16 on PUSH / PULL.
    {
        constexpr int n = 32;
        Sender tx(ZMQ_PUSH);
        tx.run([](uint64_t) {
            std::vector<int16_t> v(2 * n);
            for (int k = 0; k < n; ++k) {
                v[2 * (size_t)k] = (int16_t)k;
                v[2 * (size_t)k + 1] = (int16_t)-k;
            }
            std::vector<uint8_t> b(v.size() * 2);
            memcpy(b.data(), v.data(), b.size());
            return std::vector<std::vector<uint8_t>>{b};
        });
        cfg.format = "raw_pull_ci16";
        cfg.args = tx.address();
        auto dev = openDevice(cfg, err);
        check(dev && dev->sampleRate() == 1e6 && dev->format() == SampleFormat::CI16, "raw ci16 on PULL: opens " + err);
        if (dev) {
            Received rx;
            startInto(*dev, rx, 4);
            const bool got = waitFor([&] { return receivedBytes(rx) >= 10 * n * 4; }, 3);
            dev->stop();
            bool same = got;
            for (size_t k = 0; same && k < 3 * (size_t)n; ++k)
                same = i16At(rx.bytes.data(), 2 * k) == (int16_t)(k % n) && i16At(rx.bytes.data(), 2 * k + 1) == -(int16_t)(k % n);
            check(got && same, "PUSH / PULL loses nothing: the samples in order");
        }
    }

    // 3. The example header: two frames (a topic, then the message), a
    //    message counter with gaps, big-endian ci16.
    {
        Sender tx(ZMQ_PUB);
        tx.run([](uint64_t seq) {
            if (seq % 10 == 5) return std::vector<std::vector<uint8_t>>{};  // lost: the counter skips one
            std::vector<uint8_t> msg(32 + 3 * 4, 0);
            memcpy(msg.data(), "ABCD", 4);
            msg[4] = 1;
            putBE(msg, 8, seq, 4);
            const double fc = 2.4e9;
            uint64_t bits = 0;
            memcpy(&bits, &fc, 8);
            putBE(msg, 12, bits, 8);
            putBE(msg, 20, 500, 4);  // 500 kS/s
            for (size_t i = 0; i < 6; ++i) putBE(msg, 32 + 2 * i, (uint16_t)(int16_t)(i + 1), 2);
            return std::vector<std::vector<uint8_t>>{{'i', 'q'}, msg};
        });
        cfg.format = "example_header";
        cfg.args = tx.address();
        auto dev = openDevice(cfg, err);
        check(dev && dev->sampleRate() == 5e5 && dev->centerFreq() == 2.4e9, "example header, two frames: opens " + err);
        if (dev) {
            Received rx;
            startInto(*dev, rx, 4);
            const bool got = waitFor(
                [&] {
                    std::lock_guard<std::mutex> lk(rx.m);
                    return rx.gaps.size() >= 3;
                },
                3);
            dev->stop();
            bool gapsOk = got;
            for (size_t i = 0; i < rx.gaps.size(); ++i) gapsOk = gapsOk && rx.gaps[i] == 3 && rx.gapWhy[i] == "network loss";
            check(gapsOk, "each skipped counter value is a gap of 3 samples (" + std::to_string(rx.gaps.size()) + " gaps)");
            check(rx.bytes.size() >= 12 && i16At(rx.bytes.data(), 0) == 1 && i16At(rx.bytes.data(), 5) == 6,
                  "big-endian samples arrive little-endian");
        }
    }

    // 4. Messages that are not of the format; no source; errors before the socket.
    {
        Sender tx(ZMQ_PUB);
        tx.run([](uint64_t) { return std::vector<std::vector<uint8_t>>{{'h', 'e', 'l', 'l', 'o'}}; });
        cfg.format = "example_header";
        cfg.args = tx.address();
        check(!openDevice(cfg, err) && err.find("none is of the format") != std::string::npos, "other messages: " + err);
    }
    {
        std::string free;
        {
            Sender probe(ZMQ_PUSH);  // a port that was free a moment ago
            free = probe.address();
        }
        cfg.format = "raw_pull_ci16";
        cfg.args = free;
        check(!openDevice(cfg, err) && err.find("no message from tcp://" + free) != std::string::npos, "no source: " + err);
    }
    cfg.format = "nope";
    cfg.args.clear();
    check(!openDevice(cfg, err) && err.find("no ZMQ format 'nope'") != std::string::npos, "an unknown format: " + err);
    cfg.format = "gnuradio_sub_cf32";
    cfg.args = "localhost";
    check(!openDevice(cfg, err) && err.find("host:port") != std::string::npos, "an address without a port: " + err);

    // 5. A sample index in the header: exact loss, then a new rate.
    {
        const char* tmp = getenv("TMPDIR");
        const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/wvt_zmq_dev_" + std::to_string(getpid());
        mkdir(dir.c_str(), 0755);
        if (FILE* fp = fopen((dir + "/zmq_idx.ini").c_str(), "w")) {
            fputs("[socket]\ntype = sub\n[header]\nbytes = 16\nindex = u64le @ 0\nrate = f64le @ 8\n"
                  "[samples]\nformat = ci16\n",
                  fp);
            fclose(fp);
        }
        setStreamFormatDir(dir);
        std::atomic<bool> newRate{false};
        Sender tx(ZMQ_PUB);
        tx.run([&newRate](uint64_t seq) {
            if (seq % 7 == 3) return std::vector<std::vector<uint8_t>>{};  // lost: 4 samples
            std::vector<uint8_t> msg(16 + 4 * 4, 0);
            const uint64_t index = seq * 4;
            const double rate = newRate ? 2e6 : 1e6;
            memcpy(msg.data(), &index, 8);
            memcpy(msg.data() + 8, &rate, 8);
            return std::vector<std::vector<uint8_t>>{msg};
        });
        cfg.format = "idx";
        cfg.args = tx.address();
        err.clear();
        auto dev = openDevice(cfg, err);
        check(dev && dev->sampleRate() == 1e6, "a format with a sample index: opens " + err);
        if (dev) {
            Received rx;
            startInto(*dev, rx, 4);
            waitFor(
                [&] {
                    std::lock_guard<std::mutex> lk(rx.m);
                    return rx.gaps.size() >= 3;
                },
                3);
            bool gapsOk = true;
            {
                std::lock_guard<std::mutex> lk(rx.m);
                gapsOk = rx.gaps.size() >= 3;
                for (int64_t g : rx.gaps) gapsOk = gapsOk && g == 4;
            }
            check(gapsOk, "the sample index gives the exact loss: 4 samples per lost message");
            newRate = true;
            check(waitFor([&] { return dev->fatalError().find("another rate") != std::string::npos; }, 3),
                  "a new rate in the header stops the device: " + dev->fatalError());
            dev->stop();
        }
        unlink((dir + "/zmq_idx.ini").c_str());
        rmdir(dir.c_str());
    }
    setStreamFormatDir("");
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

// A tcp port on the loopback that is free now.
int freeTcpPort() {
    void* ctx = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_PUB);
    int port = 0;
    if (zmq_bind(sock, "tcp://127.0.0.1:*") == 0) {
        char buf[256] = {};
        size_t len = sizeof buf;
        zmq_getsockopt(sock, ZMQ_LAST_ENDPOINT, buf, &len);
        const char* colon = strrchr(buf, ':');
        port = colon ? atoi(colon + 1) : 0;  // NOLINT(cert-err34-c): a port from libzmq
    }
    const int linger = 0;
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof linger);
    zmq_close(sock);
    zmq_ctx_term(ctx);
    return port;
}

// The sample index of each received sample, from the test pattern.
std::vector<int64_t> patternIndexes(const Received& rx, SampleFormat fmt) {
    std::vector<int64_t> out;
    const size_t bps = fmt == SampleFormat::CI16 ? 4 : 8;
    for (size_t at = 0; at + bps <= rx.bytes.size(); at += bps) {
        int64_t i = 0, q = 0;
        if (fmt == SampleFormat::CI16) {
            i = i16At(rx.bytes.data() + at, 0);
            q = i16At(rx.bytes.data() + at, 1);
        } else {
            float v[2];
            memcpy(v, rx.bytes.data() + at, 8);
            i = std::lround(v[0] * 32768.f);
            q = std::lround(v[1] * 32768.f);
        }
        out.push_back(i | (q << 15));
    }
    return out;
}

// wvt publishes the pattern with a format file; the zmq device receives it
// with the same file. `pubAddr` and `subAddr` are the Address fields of the
// two sides.
void publishAndReceive(const std::string& id, const std::string& pubAddr, const std::string& subAddr, double rate,
                       double fc, bool lossless) {
    std::string err;
    auto ring = std::make_shared<SampleRing>(1 << 18, 4);
    PublishSource src;
    src.fmt = SampleFormat::CI16;
    src.rate = rate;
    src.fc = 100e6;
    src.gain = 20;
    StreamPublisher pub;
    if (!pub.start(ring, src, {pubAddr, 1, id}, err)) {
        check(false, id + ": the publisher starts: " + err);
        return;
    }
    // The device waits for the first message, so the ring fills before it opens.
    std::atomic<bool> quit{false};
    std::thread feed([&] {
        std::vector<int16_t> buf;
        for (int64_t at = 0; !quit; at += 1000) {
            fillPattern(buf, at, 1000);
            ring->push((const uint8_t*)buf.data(), 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    StreamConfig cfg;
    cfg.driver = "zmq";
    cfg.format = id;
    cfg.args = subAddr;
    auto dev = openDevice(cfg, err);
    check(dev && dev->sampleRate() == rate && dev->centerFreq() == fc, id + ": the device opens with the rate and fc " + err);
    Received rx;
    if (dev) {
        const SampleFormat fmt = dev->format();
        startInto(*dev, rx, fmt == SampleFormat::CI16 ? 4 : 8);
        waitFor([&] { return receivedBytes(rx) >= 40000 * 8; }, 3);
        dev->stop();
        std::lock_guard<std::mutex> lk(rx.m);
        const std::vector<int64_t> idx = patternIndexes(rx, fmt);
        int64_t lost = 0;
        bool order = idx.size() >= 20000;
        for (size_t k = 1; order && k < idx.size(); ++k) {
            order = idx[k] > idx[k - 1];
            lost += idx[k] - idx[k - 1] - 1;
        }
        int64_t gaps = 0;
        for (int64_t g : rx.gaps) gaps += g;
        check(order && (lossless ? lost == 0 : lost == gaps),
              id + ": " + std::to_string(idx.size()) + " samples arrive in order, " + std::to_string(lost) +
                  " lost, gaps report " + std::to_string(gaps));
    }
    quit = true;
    feed.join();
    const StreamPublisher::Status st = pub.status();
    pub.stop();
    check(st.active && st.error.empty() && st.messages > 0, id + ": the publisher sent " + std::to_string(st.messages) +
                                                             " messages without an error " + st.error);
}

void checkFormatPublisher() {
    printf("publisher with format files (issue #101)\n");
    setStreamFormatDir(WVT_CONFIG_EXAMPLES);
    std::string err;

    // PUB binds where the SUB connects; samples to cf32; the rate of the file.
    std::string a = "127.0.0.1:" + std::to_string(freeTcpPort());
    publishAndReceive("gnuradio_sub_cf32", a, a, 2e6, 100e6, true);  // both from [stream]
    // PUSH binds where the PULL connects.
    a = "127.0.0.1:" + std::to_string(freeTcpPort());
    publishAndReceive("raw_pull_ci16", a, a, 1e6, 433.92e6, true);
    // The header: big-endian samples, the rate in kS/s, a message counter.
    a = "127.0.0.1:" + std::to_string(freeTcpPort());
    publishAndReceive("example_header", "*:" + a.substr(a.rfind(':') + 1), a, 1e6, 100e6, true);
    // RADIO to DISH on udp, with the sample index: loss is exact.
    publishAndReceive("wvt_ci16", "127.0.0.1:15930", "127.0.0.1:15930", 1e6, 100e6, false);

    // A topic: two frames, the topic and the data.
    {
        const char* tmp = getenv("TMPDIR");
        const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/wvt_zmq_pub_" + std::to_string(getpid());
        mkdir(dir.c_str(), 0755);
        if (FILE* fp = fopen((dir + "/zmq_topic.ini").c_str(), "w")) {
            fputs("[socket]\ntype = sub\ntopic = iq\n[header]\nbytes = 8\nindex = u64le @ 0\n"
                  "[samples]\nformat = ci16\n[stream]\nrate = 1e6\nfc = 100e6\n",
                  fp);
            fclose(fp);
        }
        setStreamFormatDir(dir);
        a = "127.0.0.1:" + std::to_string(freeTcpPort());
        publishAndReceive("topic", a, a, 1e6, 100e6, true);
        unlink((dir + "/zmq_topic.ini").c_str());
        rmdir(dir.c_str());
        setStreamFormatDir(WVT_CONFIG_EXAMPLES);
    }

    // Errors at the start.
    {
        auto ring = std::make_shared<SampleRing>(4096, 4);
        PublishSource src;
        src.fmt = SampleFormat::CI16;
        src.rate = 1e6;
        src.fc = 100e6;
        StreamPublisher pub;
        check(!pub.start(ring, src, {"127.0.0.1:5555", 1, "nope"}, err) && err.find("no ZMQ format 'nope'") != std::string::npos,
              "an unknown format: " + err);
        check(!pub.start(ring, src, {"", 1, "raw_pull_ci16"}, err) || (pub.stop(), true), "the file's address when the field is empty");
        const int port = freeTcpPort();
        StreamPublisher first;
        err.clear();
        const std::string addr = "127.0.0.1:" + std::to_string(port);
        check(first.start(ring, src, {addr, 1, "gnuradio_sub_cf32"}, err), "a PUB binds on " + addr + " " + err);
        check(!pub.start(ring, src, {addr, 1, "gnuradio_sub_cf32"}, err) && err.find("is in use") != std::string::npos,
              "a second PUB on the same port: " + err);
        first.stop();
    }
    setStreamFormatDir("");
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
    check(pub.start(ring, src, {rx.address, 1, ""}, err), "the publisher starts (" + (err.empty() ? rx.address : err) + ")");
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
    check(pub.start(small, src, {rx.address, 1, ""}, err), "the publisher starts again on a small ring");
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
        if (!src.startPublishing({addr, 1, ""}, err)) break;
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
    check(st.notes.empty(), "no note right after the start: '" + st.notes + "'");
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
    check(st.received > 300000 && st.received <= sent && st.received >= sent - 100000,
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
    check(other.start(ring, os, {addr, 1, ""}, err), "a second source at 2 MS/s sends to the same address");
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
    check(src.startPublishing({addr, 1, ""}, err), "the source publishes again");
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
    if (src.startPublishing({mc.args, 1, ""}, err) && sub.start(mc, err)) {
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
    checkFormats();
    checkEncode();
#ifdef WVT_HAVE_ZMQ
    checkSockets();
    checkPublisher();
    checkSubscriber();
    checkFormatDevice();
    checkFormatPublisher();
#else
    printf("libzmq: not in this build (WVT_WITH_ZMQ), the socket tests are skipped\n");
    StreamPublisher pub;
    std::string err;
    check(!pub.start(std::make_shared<SampleRing>(4096, 4), {}, {"127.0.0.1:5600", 1, ""}, err) &&
              err.find("without ZMQ") != std::string::npos,
          "the publisher reports that this build has no ZMQ");
#endif
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
