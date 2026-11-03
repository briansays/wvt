// Network stream receiver (issue #96): a ZMQ DISH socket on UDP receives the
// stream of another wvt (net/zmq_pub) and delivers it like a device. The
// address is where this wvt receives: an address of this computer (the
// source sends to it) or a multicast group (the same group as the source).
//
// The source sets the frequency, the rate, the format and the gain; the
// stream is one way, so this device cannot change them. It opens when the
// first data message arrives, because the live engine needs the rate and the
// format before it starts.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <zmq.h>

#include "common/log.h"
#include "common/ui_util.h"
#include "device/sdr_device.h"
#include "net/zmq_wire.h"

namespace {

constexpr int kRecvHwm = 20000;          // messages queued in libzmq: at most 160 MB
constexpr int kRecvBuffer = 8 << 20;     // the UDP socket buffer, if the system allows it
constexpr int kRecvTimeoutMs = 100;
constexpr double kOpenWaitSec = 3;       // for the first data message
constexpr double kQuietSec = 2;          // no message for this long: "waiting for the source"
constexpr auto kBadLogPeriod = std::chrono::seconds(5);

// libzmq binds a udp:// endpoint later, on its I/O thread: zmq_bind returns 0
// and a bind error is never reported. It also allows a second socket on the
// port. So bind a plain UDP socket (without SO_REUSEADDR) first, and close it:
// that gives the error now. 0 if the bind works, else the errno.
int probeBind(const std::string& ip, int port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;  // cannot test; libzmq will try
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    int e = 0;
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1 && bind(fd, (const sockaddr*)&a, sizeof a) != 0) e = errno;
    close(fd);
    return e;
}

class ZmqDevice : public SdrDevice {
public:
    ~ZmqDevice() override {
        stop();
        if (sock_) {
            const int linger = 0;
            zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof linger);
            zmq_close(sock_);
        }
        if (ctx_) zmq_ctx_term(ctx_);
    }

    // Binds, joins the group and waits for the first data message.
    bool open(const StreamConfig& cfg, std::string& err) {
        const std::string text = cfg.args.empty() ? defaultStreamAddress() : cfg.args;
        if (!parseStreamAddress(text, addr_, err)) return false;
        ctx_ = zmq_ctx_new();
        sock_ = ctx_ ? zmq_socket(ctx_, ZMQ_DISH) : nullptr;
        if (!sock_) {
            err = std::string("cannot open a ZMQ DISH socket: ") + zmq_strerror(zmq_errno());
            return false;
        }
        const int hwm = kRecvHwm, rcvbuf = kRecvBuffer, timeout = kRecvTimeoutMs, loop = 1;
        zmq_setsockopt(sock_, ZMQ_RCVHWM, &hwm, sizeof hwm);
        zmq_setsockopt(sock_, ZMQ_RCVBUF, &rcvbuf, sizeof rcvbuf);
        zmq_setsockopt(sock_, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
        if (addr_.multicast) zmq_setsockopt(sock_, ZMQ_MULTICAST_LOOP, &loop, sizeof loop);  // a source on this computer
        // A multicast group allows more subscribers on the port, so only
        // unicast is tested.
        int e = addr_.multicast ? 0 : probeBind(addr_.ip, addr_.port);
        if (!e && zmq_bind(sock_, addr_.endpoint().c_str()) != 0) e = zmq_errno();
        if (e) {
            if (e == EADDRINUSE)
                err = "port " + std::to_string(addr_.port) + " is in use (another subscriber?)";
            else if (e == EADDRNOTAVAIL && !addr_.multicast)
                err = addr_.ip + " is not an address of this computer: type an address of this computer, and send to it "
                                 "from the source";
            else
                err = "cannot receive on " + addr_.text() + ": " + zmq_strerror(e);
            return false;
        }
        if (zmq_join(sock_, kStreamGroup) != 0) {
            err = std::string("cannot join the stream group: ") + zmq_strerror(zmq_errno());
            return false;
        }
        LOG_INFO("zmq", "receiving on %s%s, waiting for the source", addr_.text().c_str(),
                 addr_.multicast ? " (multicast)" : "");

        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(kOpenWaitSec);
        while (std::chrono::steady_clock::now() < end) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            if (zmq_msg_recv(&msg, sock_, 0) < 0) {
                zmq_msg_close(&msg);
                continue;
            }
            StreamMessage m;
            std::string why;
            const auto* p = (const uint8_t*)zmq_msg_data(&msg);
            const size_t n = zmq_msg_size(&msg);
            const bool ok = parseStreamMessage(p, n, m, why);
            if (ok && m.h.kind == StreamKind::Info) noteInfo(m);
            if (ok && m.h.kind == StreamKind::Data && !(m.h.flags & kStreamFlagEnd)) {
                // The stream starts with this message; start() delivers it.
                first_.assign(p, p + n);
                fmt_ = m.h.fmt;
                rate_ = m.h.rate;
                fc_ = m.h.fc;
                gain_ = m.h.gain;
                agc_ = (m.h.flags & kStreamFlagAgc) != 0;
                if (infoId_ != m.h.streamId) setInfo({}, 0);  // the info of an older stream
                zmq_msg_close(&msg);
                LOG_INFO("zmq", "stream %08x from the source: %s, %s, %s", m.h.streamId, streamFormatText(fmt_),
                         fmtSI(rate_, "S/s", 6).c_str(), fmtSI(fc_.load(), "Hz", 9).c_str());
                return true;
            }
            zmq_msg_close(&msg);
        }
        err = "no stream on " + addr_.text() + " in " + std::to_string((int)kOpenWaitSec) +
              " s (is the source publishing to this address?" +
              (addr_.multicast ? " Does the network pass multicast?)" : ")");
        return false;
    }

    bool start(DataFn data, GapFn gap, std::string&) override {
        stop();
        data_ = std::move(data);
        gap_ = std::move(gap);
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        quit_ = true;
        if (th_.joinable()) th_.join();
    }

    bool setCenterFreq(double, std::string& err) override {
        err = "the source sets the frequency (this wvt receives the stream on " + addr_.text() + ")";
        return false;
    }
    bool setGain(bool, double, std::string& err) override {
        err = "the source sets the gain (this wvt receives the stream on " + addr_.text() + ")";
        return false;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return fmt_; }
    GainRange gainRange() const override {
        // Display only: the gain of the source.
        const double g = gain_;
        return {g, g, 0.1, {}, false, "Gain", "dB"};
    }
    std::string description() const override {
        std::lock_guard<std::mutex> lk(m_);
        return "ZMQ " + addr_.text() + (info_.description.empty() ? "" : ": " + info_.description);
    }
    std::string notes() const override {
        const double quiet = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastMsg()).count();
        if (fatal().empty() && th_.joinable() && quiet >= kQuietSec)
            return "waiting for the source: no message for " + std::to_string((int)quiet) + " s";
        return {};
    }
    DeviceCaps caps() const override {
        DeviceCaps c;
        c.product = "ZMQ";
        return c;
    }
    std::string fatalError() const override { return fatal(); }
    void setRetuneMark(RetuneFn fn) override { retune_ = std::move(fn); }
    bool usesRetuneMark() const override { return true; }

private:
    void run() {
        StreamTracker tracker;
        int64_t bad = 0, badLogged = 0, late = 0;
        auto lastBadLog = std::chrono::steady_clock::now() - kBadLogPeriod;
        std::string lastWhy;
        bool firstDone = false;
        setLastMsg();
        while (!quit_) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (!firstDone) {
                p = first_.data();
                n = first_.size();
                firstDone = true;
            } else if (zmq_msg_recv(&msg, sock_, 0) >= 0) {
                p = (const uint8_t*)zmq_msg_data(&msg);
                n = zmq_msg_size(&msg);
            } else {
                zmq_msg_close(&msg);
                continue;  // timeout: check quit_
            }
            setLastMsg();
            StreamMessage m;
            std::string why;
            if (!parseStreamMessage(p, n, m, why)) {
                ++bad;
                lastWhy = why;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastBadLog >= kBadLogPeriod) {
                    LOG_WARN("zmq", "%lld messages on %s that are not a wvt stream were ignored (last: %s)",
                             (long long)(bad - badLogged), addr_.text().c_str(), lastWhy.c_str());
                    badLogged = bad;
                    lastBadLog = now;
                }
                zmq_msg_close(&msg);
                continue;
            }
            const bool stop = handle(m, tracker, late);
            zmq_msg_close(&msg);
            if (stop) break;
        }
        if (late) LOG_DEBUG("zmq", "%lld late or repeated messages dropped", (long long)late);
    }

    // One parsed message. Returns true when the stream ends.
    bool handle(const StreamMessage& m, StreamTracker& tracker, int64_t& late) {
        const StreamHeader& h = m.h;
        if (h.kind == StreamKind::Info) {
            if (!tracker.started() || h.streamId == tracker.streamId()) noteInfo(m);
            return false;
        }
        if (h.flags & kStreamFlagEnd) {
            if (tracker.started() && h.streamId != tracker.streamId()) return false;  // an old stream
            setFatal("the source stopped publishing");
            return true;
        }
        const StreamTracker::Result r = tracker.next(h);
        switch (r.event) {
            case StreamTracker::Event::Late: ++late; return false;
            case StreamTracker::Event::NewStream:
                if (h.rate != rate_ || h.fmt != fmt_) {
                    setFatal(std::string("the source started again with another ") +
                             (h.rate != rate_ ? "rate (" + fmtSI(h.rate, "S/s", 6) + ")" : "sample format") +
                             ": click Start again");
                    return true;
                }
                LOG_INFO("zmq", "the source started again (stream %08x)", h.streamId);
                if (gap_) gap_(-1, "the source started again");
                break;
            case StreamTracker::Event::Lost:
                if (gap_) gap_((int64_t)r.lost, "network loss");
                break;
            default: break;
        }
        if (h.fc != fc_ || (h.flags & kStreamFlagRetune)) {
            // Before the samples, so the retune is at the right sample.
            if (h.fc != fc_) LOG_INFO("zmq", "the source tuned to %s", fmtSI(h.fc, "Hz", 9).c_str());
            fc_ = h.fc;
            if (retune_) retune_(h.fc);
        }
        gain_ = h.gain;
        agc_ = (h.flags & kStreamFlagAgc) != 0;
        if (h.count && data_) data_(m.samples, h.count);
        return false;
    }

    static const char* streamFormatText(SampleFormat f) {
        return f == SampleFormat::CU8 ? "cu8" : f == SampleFormat::CI16 ? "ci16" : "cf32";
    }

    void noteInfo(const StreamMessage& m) {
        std::lock_guard<std::mutex> lk(m_);
        if (m.info.description != info_.description && !m.info.description.empty())
            LOG_INFO("zmq", "the source is %s (%s)", m.info.description.c_str(), m.info.driver.c_str());
        info_ = m.info;
        infoId_ = m.h.streamId;
    }
    void setInfo(const StreamInfo& i, uint32_t id) {
        std::lock_guard<std::mutex> lk(m_);
        info_ = i;
        infoId_ = id;
    }
    void setFatal(const std::string& e) {
        LOG_INFO("zmq", "%s", e.c_str());
        std::lock_guard<std::mutex> lk(m_);
        fatal_ = e;
    }
    std::string fatal() const {
        std::lock_guard<std::mutex> lk(m_);
        return fatal_;
    }
    void setLastMsg() { lastMsgNs_ = std::chrono::steady_clock::now().time_since_epoch().count(); }
    std::chrono::steady_clock::time_point lastMsg() const {
        return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(lastMsgNs_.load()));
    }

    StreamAddress addr_;
    void* ctx_ = nullptr;
    void* sock_ = nullptr;
    std::vector<uint8_t> first_;
    SampleFormat fmt_ = SampleFormat::CI16;
    double rate_ = 0;
    std::atomic<double> fc_{0}, gain_{0};
    std::atomic<bool> agc_{false};
    DataFn data_;
    GapFn gap_;
    RetuneFn retune_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::atomic<int64_t> lastMsgNs_{0};
    mutable std::mutex m_;  // guards info_, infoId_, fatal_
    StreamInfo info_;
    uint32_t infoId_ = 0;
    std::string fatal_;
};

}  // namespace

std::unique_ptr<SdrDevice> openZmqDevice(const StreamConfig& cfg, std::string& err) {
    auto dev = std::make_unique<ZmqDevice>();
    if (!dev->open(cfg, err)) return nullptr;
    return dev;
}

void enumerateZmq(std::vector<DeviceInfo>& out) {
    const std::string a = defaultStreamAddress();
    out.push_back({"zmq", a, "ZMQ stream (" + a + ")", "zmq", ""});
}
