#include "net/zmq_wire.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {

constexpr char kMagic[4] = {'W', 'V', 'T', 'Z'};

// The sample format codes on the wire. They do not depend on the order of
// SampleFormat, so that enum can change.
uint8_t formatCode(SampleFormat f) {
    switch (f) {
        case SampleFormat::CU8: return 1;
        case SampleFormat::CI16: return 2;
        case SampleFormat::CF32: return 3;
        default: return 0;
    }
}

bool formatFromCode(uint8_t c, SampleFormat& f) {
    switch (c) {
        case 1: f = SampleFormat::CU8; return true;
        case 2: f = SampleFormat::CI16; return true;
        case 3: f = SampleFormat::CF32; return true;
        default: return false;
    }
}

void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
void putF64(uint8_t* p, double d) {
    uint64_t v = 0;
    memcpy(&v, &d, sizeof v);
    put64(p, v);
}
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
double getF64(const uint8_t* p) {
    const uint64_t v = get64(p);
    double d = 0;
    memcpy(&d, &v, sizeof d);
    return d;
}

// At most `max` bytes, not in the middle of a UTF-8 character.
std::string cutText(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

// Received text goes to the window and the log: no control characters.
std::string cleanText(const uint8_t* p, size_t n) {
    std::string s((const char*)p, n);
    for (char& c : s)
        if ((unsigned char)c < 0x20 || c == 0x7f) c = '?';
    return s;
}

bool isDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// "a.b.c.d" -> the four numbers.
bool parseIpv4(const std::string& s, int (&o)[4]) {
    size_t i = 0;
    for (int k = 0; k < 4; ++k) {
        const size_t j = k < 3 ? s.find('.', i) : s.size();
        if (j == std::string::npos) return false;
        const std::string part = s.substr(i, j - i);
        if (!isDigits(part) || part.size() > 3) return false;
        o[k] = std::stoi(part);
        if (o[k] > 255) return false;
        i = j + 1;
    }
    return true;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

int streamBytesPerSample(SampleFormat f) {
    switch (f) {
        case SampleFormat::CU8: return 2;
        case SampleFormat::CI16: return 4;
        case SampleFormat::CF32: return 8;
        default: return 0;
    }
}

uint32_t streamMaxSamples(SampleFormat f) {
    const int bps = streamBytesPerSample(f);
    return bps ? (uint32_t)(kStreamMaxPayload / bps) : 0;
}

void packStreamHeader(const StreamHeader& h, uint8_t* out) {
    memset(out, 0, kStreamHeaderBytes);
    memcpy(out, kMagic, 4);
    out[4] = kStreamVersion;
    out[5] = (uint8_t)h.kind;
    out[6] = formatCode(h.fmt);
    out[7] = h.flags;
    put32(out + 8, h.streamId);
    put32(out + 12, h.count);
    put64(out + 16, h.index);
    putF64(out + 24, h.fc);
    putF64(out + 32, h.rate);
    putF64(out + 40, h.gain);
    put64(out + 48, h.timeNs);
}

std::vector<uint8_t> packStreamInfo(const StreamHeader& h, const StreamInfo& info) {
    StreamHeader ih = h;
    ih.kind = StreamKind::Info;
    ih.count = 0;
    const std::string driver = cutText(info.driver, kStreamMaxInfoText);
    const std::string desc = cutText(info.description, kStreamMaxInfoText);
    std::vector<uint8_t> out(kStreamHeaderBytes + 2 + driver.size() + 2 + desc.size());
    packStreamHeader(ih, out.data());
    uint8_t* p = out.data() + kStreamHeaderBytes;
    for (const std::string* s : {&driver, &desc}) {
        put16(p, (uint16_t)s->size());
        std::copy(s->begin(), s->end(), p + 2);  // a length, not a terminating 0
        p += 2 + s->size();
    }
    return out;
}

bool parseStreamMessage(const uint8_t* p, size_t n, StreamMessage& m, std::string& why) {
    m = StreamMessage{};
    if (n < kStreamHeaderBytes) {
        why = "too short (" + std::to_string(n) + " bytes)";
        return false;
    }
    if (memcmp(p, kMagic, 4) != 0) {
        why = "not a wvt stream message";
        return false;
    }
    if (p[4] != kStreamVersion) {
        why = "version " + std::to_string(p[4]) + " (this wvt reads version " + std::to_string(kStreamVersion) + ")";
        return false;
    }
    StreamHeader& h = m.h;
    if (p[5] != (uint8_t)StreamKind::Data && p[5] != (uint8_t)StreamKind::Info) {
        why = "unknown message kind " + std::to_string(p[5]);
        return false;
    }
    h.kind = (StreamKind)p[5];
    if (!formatFromCode(p[6], h.fmt)) {
        why = "unknown sample format " + std::to_string(p[6]);
        return false;
    }
    h.flags = p[7];
    h.streamId = get32(p + 8);
    h.count = get32(p + 12);
    h.index = get64(p + 16);
    h.fc = getF64(p + 24);
    h.rate = getF64(p + 32);
    h.gain = getF64(p + 40);
    h.timeNs = get64(p + 48);
    if (!std::isfinite(h.rate) || h.rate <= 0 || h.rate > 1e10) {
        why = "bad sample rate";
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

    const uint8_t* body = p + kStreamHeaderBytes;
    const size_t rest = n - kStreamHeaderBytes;
    if (h.kind == StreamKind::Data) {
        // count is 32 bits and a sample at most 8 bytes: no overflow in 64 bits.
        const uint64_t bytes = (uint64_t)h.count * (uint64_t)streamBytesPerSample(h.fmt);
        if (bytes != rest) {
            why = std::to_string(h.count) + " samples need " + std::to_string(bytes) + " bytes, the message has " +
                  std::to_string(rest);
            return false;
        }
        m.samples = rest ? body : nullptr;
        m.sampleBytes = rest;
        return true;
    }

    if (h.count != 0) {
        why = "an info message with samples";
        return false;
    }
    size_t at = 0;
    for (std::string* s : {&m.info.driver, &m.info.description}) {
        if (rest - at < 2) {
            why = "info message cut short";
            return false;
        }
        const size_t len = get16(body + at);
        at += 2;
        if (len > kStreamMaxInfoText) {
            why = "info text too long (" + std::to_string(len) + " bytes)";
            return false;
        }
        if (len > rest - at) {
            why = "info message cut short";
            return false;
        }
        *s = cleanText(body + at, len);
        at += len;
    }
    if (at != rest) {
        why = "extra bytes after the info message";
        return false;
    }
    return true;
}

std::string StreamAddress::text() const {
    return (iface.empty() ? "" : iface + ";") + ip + ":" + std::to_string(port);
}

std::string StreamAddress::endpoint() const { return "udp://" + text(); }

bool parseStreamAddress(const std::string& in, StreamAddress& a, std::string& err) {
    a = StreamAddress{};
    std::string s = trim(in);
    if (s.empty()) {
        err = "no address";
        return false;
    }
    const size_t semi = s.find(';');
    if (semi != std::string::npos) {
        a.iface = s.substr(0, semi);
        s = s.substr(semi + 1);
        if (a.iface.empty() || a.iface.size() > 63) {
            err = "bad interface name";
            return false;
        }
        for (char c : a.iface)
            if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') {
                err = "bad interface name '" + a.iface + "'";
                return false;
            }
    }
    std::string host = s;
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        const std::string port = s.substr(colon + 1);
        if (!isDigits(port) || port.size() > 5 || std::stoi(port) < 1 || std::stoi(port) > 65535) {
            err = "bad port '" + port + "' (1 to 65535)";
            return false;
        }
        a.port = std::stoi(port);
    }
    int o[4] = {};
    if (!parseIpv4(host, o)) {
        err = "'" + host + "' is not an IPv4 address (a.b.c.d)";
        return false;
    }
    if (o[0] == 0) {
        err = host + " is not an address to stream to";
        return false;
    }
    if (o[0] >= 240) {
        err = host + " is a reserved or broadcast address";
        return false;
    }
    a.multicast = o[0] >= 224;
    if (!a.iface.empty() && !a.multicast) {
        err = "an interface applies to a multicast group only (224.0.0.0 to 239.255.255.255)";
        return false;
    }
    a.ip = std::to_string(o[0]) + "." + std::to_string(o[1]) + "." + std::to_string(o[2]) + "." + std::to_string(o[3]);
    return true;
}

std::string pickStreamAddress(const std::vector<StreamIface>& ifaces) {
    // The lowest rank wins; the interface order breaks a tie.
    int best = -1;
    std::string ip;
    for (const auto& f : ifaces) {
        int o[4] = {};
        if (!f.up || f.loopback || f.pointToPoint || !parseIpv4(f.ip, o)) continue;
        if (o[0] == 0 || o[0] == 127 || o[0] >= 224) continue;
        const int rank = (f.running ? 0 : 2) + (o[0] == 169 && o[1] == 254 ? 1 : 0);
        if (best < 0 || rank < best) {
            best = rank;
            ip = f.ip;
        }
    }
    return (ip.empty() ? std::string("127.0.0.1") : ip) + ":" + std::to_string(kStreamDefaultPort);
}

std::vector<StreamIface> streamInterfaces() {
    std::vector<StreamIface> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (const ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN] = {};
        const auto* sin = (const sockaddr_in*)a->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) continue;
        StreamIface f;
        f.name = a->ifa_name ? a->ifa_name : "";
        f.ip = buf;
        f.up = (a->ifa_flags & IFF_UP) != 0;
        f.running = (a->ifa_flags & IFF_RUNNING) != 0;
        f.loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        f.pointToPoint = (a->ifa_flags & IFF_POINTOPOINT) != 0;
        out.push_back(std::move(f));
    }
    freeifaddrs(list);
    return out;
}

std::string defaultStreamAddress() { return pickStreamAddress(streamInterfaces()); }

StreamTracker::Result StreamTracker::next(const StreamHeader& h) {
    Result r;
    if (!started_ || h.streamId != id_) {
        r.event = started_ ? Event::NewStream : Event::First;
        started_ = true;
        id_ = h.streamId;
        expected_ = h.index + h.count;
        return r;
    }
    if (h.index == expected_) {
        r.event = Event::InOrder;
        expected_ += h.count;
    } else if (h.index > expected_) {
        r.event = Event::Lost;
        r.lost = h.index - expected_;
        expected_ = h.index + h.count;
    } else {
        // Before the expected index: a late or repeated message. A message
        // that overlaps the expected index is dropped too (a part of it is a
        // repeat); the next message then shows the rest as lost.
        r.event = Event::Late;
    }
    return r;
}
