// Network stream receiver (issue #96): a ZMQ socket receives a stream of
// samples and delivers it like a device. The message format (issue #101) is
// the built-in wvt stream (a DISH on UDP that receives another wvt's
// net/zmq_pub), or a zmq_*.ini file of the config folder (net/zmq_format):
// a DISH, SUB or PULL socket, and the header layout of another tool.
//
// The source sets the frequency, the rate, the format and the gain; the
// stream is one way, so this device cannot change them. It opens when the
// first data message arrives, because the live engine needs the rate and the
// format before it starts.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
#include "net/zmq_format.h"
#include "net/zmq_wire.h"

namespace {

constexpr int kRecvHwm = 20000;            // messages queued in libzmq, at most
constexpr size_t kRecvMemory = 160 << 20;  // ... and at most about this many bytes of them
constexpr int kRecvBuffer = 8 << 20;       // the socket buffer, if the system allows it
constexpr int kRecvTimeoutMs = 100;
constexpr double kOpenWaitSec = 3;  // for the first data message
constexpr double kQuietSec = 2;     // no message for this long: "waiting for the source"
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

int socketType(FormatSocket s) {
    switch (s) {
        case FormatSocket::Sub: return ZMQ_SUB;
        case FormatSocket::Pull: return ZMQ_PULL;
        case FormatSocket::Dish: break;
    }
    return ZMQ_DISH;
}

const char* socketName(FormatSocket s) {
    switch (s) {
        case FormatSocket::Sub: return "SUB";
        case FormatSocket::Pull: return "PULL";
        case FormatSocket::Dish: break;
    }
    return "DISH";
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

    // Makes the socket and waits for the first data message.
    bool open(const StreamConfig& cfg, std::string& err) {
        if (!resolveStreamFormat(cfg.format, fmt_, err)) return false;
        if (!formatEndpoint(fmt_, cfg.args, ep_, err)) return false;
        const bool dish = fmt_.socket == FormatSocket::Dish;
        ctx_ = zmq_ctx_new();
        sock_ = ctx_ ? zmq_socket(ctx_, socketType(fmt_.socket)) : nullptr;
        if (!sock_) {
            err = std::string("cannot open a ZMQ ") + socketName(fmt_.socket) + " socket: " + zmq_strerror(zmq_errno());
            return false;
        }
        const int hwm = (int)std::clamp<size_t>(kRecvMemory / std::max<size_t>(fmt_.maxBytes, 1), 16, kRecvHwm);
        const int rcvbuf = kRecvBuffer, timeout = kRecvTimeoutMs, loop = 1;
        zmq_setsockopt(sock_, ZMQ_RCVHWM, &hwm, sizeof hwm);
        zmq_setsockopt(sock_, ZMQ_RCVBUF, &rcvbuf, sizeof rcvbuf);
        zmq_setsockopt(sock_, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
        if (!fmt_.builtin) {
            // libzmq drops a larger message (and, on tcp, the connection) before wvt sees it.
            const auto max = (int64_t)fmt_.maxBytes;
            zmq_setsockopt(sock_, ZMQ_MAXMSGSIZE, &max, sizeof max);
        }
        if (dish && ep_.udp.multicast) zmq_setsockopt(sock_, ZMQ_MULTICAST_LOOP, &loop, sizeof loop);  // a source on this computer
        if (fmt_.socket == FormatSocket::Sub)
            zmq_setsockopt(sock_, ZMQ_SUBSCRIBE, fmt_.topic.data(), fmt_.topic.size());

        int e = 0;
        // A multicast group allows more subscribers on the port, so only unicast is tested.
        if (dish && !ep_.udp.multicast) e = probeBind(ep_.udp.ip, ep_.udp.port);
        if (!e && (fmt_.bind ? zmq_bind(sock_, ep_.endpoint.c_str()) : zmq_connect(sock_, ep_.endpoint.c_str())) != 0)
            e = zmq_errno();
        if (e) {
            if (e == EADDRINUSE)
                err = "port " + std::to_string(ep_.port) + " is in use (another subscriber?)";
            else if (e == EADDRNOTAVAIL && fmt_.bind && !(dish && ep_.udp.multicast))
                err = ep_.text + " is not an address of this computer: type an address of this computer, and send to it "
                                 "from the source";
            else
                err = std::string("cannot ") + (fmt_.bind ? "receive on " : "connect to ") + ep_.endpoint + ": " +
                      zmq_strerror(e);
            return false;
        }
        if (dish && zmq_join(sock_, fmt_.group.c_str()) != 0) {
            err = std::string("cannot join the stream group: ") + zmq_strerror(zmq_errno());
            return false;
        }
        LOG_INFO("zmq", "%s %s%s with %s, format %s, waiting for the source", fmt_.bind ? "receiving on" : "connected to",
                 ep_.endpoint.c_str(), dish && ep_.udp.multicast ? " (multicast)" : "", socketName(fmt_.socket),
                 fmt_.id.c_str());

        int64_t bad = 0;
        std::string lastWhy;
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(kOpenWaitSec);
        while (std::chrono::steady_clock::now() < end) {
            std::string why;
            const int r = receive(rx_, why);
            if (r == 0) continue;
            StreamMessage m;
            FormatExtra extra;
            if (r < 0 || !decodeStreamMessage(fmt_, rx_.data(), rx_.size(), m, extra, scratch_, why)) {
                ++bad;
                lastWhy = why;
                continue;
            }
            if (m.h.kind == StreamKind::Info) noteInfo(m);
            if (m.h.kind == StreamKind::Data && !(m.h.flags & kStreamFlagEnd)) {
                // The stream starts with this message; start() delivers it.
                first_ = rx_;
                setLastMsg();  // notes() can run before run() starts
                fmt0_ = m.h.fmt;
                rate_ = m.h.rate;
                fc_ = m.h.fc;
                gain_ = m.h.gain;
                agc_ = (m.h.flags & kStreamFlagAgc) != 0;
                if (infoId_ != m.h.streamId) setInfo({}, 0);  // the info of an older stream
                LOG_INFO("zmq", "stream %08x from the source: %s, %s, %s", m.h.streamId, streamFormatText(fmt0_),
                         fmtSI(rate_, "S/s", 6).c_str(), fmtSI(fc_.load(), "Hz", 9).c_str());
                return true;
            }
        }
        if (bad) {
            err = std::to_string(bad) + " messages arrived on " + ep_.text + ", but none is of the format " + fmt_.name +
                  " (last: " + lastWhy + ")";
        } else if (fmt_.builtin) {
            err = "no stream on " + ep_.text + " in " + std::to_string((int)kOpenWaitSec) +
                  " s (is the source publishing to this address?" +
                  (ep_.udp.multicast ? " Does the network pass multicast?)" : ")");
        } else {
            err = "no message " + std::string(fmt_.bind ? "on " : "from ") + ep_.endpoint + " in " +
                  std::to_string((int)kOpenWaitSec) + " s (format " + fmt_.name + "; " +
                  (fmt_.bind ? "is the source sending to this address?)"
                             : "is the source sending on this address? libzmq connects in the background, so a wrong "
                               "address shows only as no message)");
        }
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
        err = "the source sets the frequency (this wvt receives the stream on " + ep_.text + ")";
        return false;
    }
    bool setGain(bool, double, std::string& err) override {
        err = "the source sets the gain (this wvt receives the stream on " + ep_.text + ")";
        return false;
    }

    double centerFreq() const override { return fc_; }
    double sampleRate() const override { return rate_; }
    SampleFormat format() const override { return fmt0_; }
    GainRange gainRange() const override {
        // Display only: the gain of the source.
        const double g = gain_;
        return {g, g, 0.1, {}, false, "Gain", "dB"};
    }
    std::string description() const override {
        if (!fmt_.builtin) return "ZMQ " + ep_.text + " (" + fmt_.name + ")";
        std::lock_guard<std::mutex> lk(m_);
        return "ZMQ " + ep_.text + (info_.description.empty() ? "" : ": " + info_.description);
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
    // One message: all its frames are read, and the frame of the format is
    // copied to `out`. 1: a message; 0: none (timeout); -1: the message has
    // no such frame (why set).
    int receive(std::vector<uint8_t>& out, std::string& why) {
        int frames = 0;
        bool got = false;
        for (;;) {
            zmq_msg_t part;
            zmq_msg_init(&part);
            if (zmq_msg_recv(&part, sock_, 0) < 0) {
                zmq_msg_close(&part);
                if (frames == 0) return 0;
                break;  // the rest of a message is always there; not expected
            }
            ++frames;
            if (fmt_.frame == 0 || frames == fmt_.frame) {
                const auto* p = (const uint8_t*)zmq_msg_data(&part);
                out.assign(p, p + zmq_msg_size(&part));
                got = true;
            }
            const bool more = zmq_msg_more(&part) != 0;
            zmq_msg_close(&part);
            if (!more) break;
        }
        if (!got) {
            why = "the message has " + std::to_string(frames) + " frames; the format reads frame " + std::to_string(fmt_.frame);
            return -1;
        }
        return 1;
    }

    void run() {
        StreamTracker tracker;
        MessageCounter counter(streamSeqBits(fmt_));
        int64_t bad = 0, badLogged = 0, late = 0;
        auto lastBadLog = std::chrono::steady_clock::now() - kBadLogPeriod;
        bool firstDone = false;
        setLastMsg();
        while (!quit_) {
            std::string why;
            const std::vector<uint8_t>* msg = &rx_;
            if (!firstDone) {
                msg = &first_;
                firstDone = true;
            } else {
                const int r = receive(rx_, why);
                if (r == 0) continue;  // timeout: check quit_
                if (r < 0) msg = nullptr;
            }
            setLastMsg();
            StreamMessage m;
            FormatExtra extra;
            if (!msg || !decodeStreamMessage(fmt_, msg->data(), msg->size(), m, extra, scratch_, why)) {
                ++bad;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastBadLog >= kBadLogPeriod) {
                    LOG_WARN("zmq", "%lld messages on %s that are not of the format %s were ignored (last: %s)",
                             (long long)(bad - badLogged), ep_.text.c_str(), fmt_.name.c_str(), why.c_str());
                    badLogged = bad;
                    lastBadLog = now;
                }
                continue;
            }
            if (fmt_.builtin ? handleWvt(m, tracker, late) : handleFile(m, extra, tracker, counter, late)) break;
        }
        if (late) LOG_DEBUG("zmq", "%lld late or repeated messages dropped", (long long)late);
    }

    // One message of the built-in format. Returns true when the stream ends.
    bool handleWvt(const StreamMessage& m, StreamTracker& tracker, int64_t& late) {
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
                if (h.rate != rate_ || h.fmt != fmt0_) return changed(h);
                LOG_INFO("zmq", "the source started again (stream %08x)", h.streamId);
                if (gap_) gap_(-1, "the source started again");
                break;
            case StreamTracker::Event::Lost:
                if (gap_) gap_((int64_t)r.lost, "network loss");
                break;
            default: break;
        }
        deliver(m);
        return false;
    }

    // One message of a format file. Loss comes from the sample index or the
    // message counter, if the format has one. Returns true when the stream ends.
    bool handleFile(const StreamMessage& m, const FormatExtra& extra, StreamTracker& tracker, MessageCounter& counter,
                    int64_t& late) {
        const StreamHeader& h = m.h;
        if (h.rate != rate_) return changed(h);
        auto restarted = [&] {
            LOG_INFO("zmq", "the source started again (the %s went back)", extra.hasIndex ? "sample index" : "message counter");
            if (gap_) gap_(-1, "the source started again");
        };
        if (extra.hasIndex) {
            const StreamTracker::Result r = tracker.next(h);
            if (r.event == StreamTracker::Event::Late) {
                // Far back (more than 1 s of samples): the source started again.
                if ((double)(tracker.expected() - h.index) <= rate_) {
                    ++late;
                    return false;
                }
                tracker.reset();
                tracker.next(h);
                restarted();
            } else if (r.event == StreamTracker::Event::Lost && gap_) {
                gap_((int64_t)r.lost, "network loss");
            }
        } else if (extra.hasSeq) {
            const MessageCounter::Result r = counter.next(extra.seq);
            switch (r.event) {
                case MessageCounter::Event::Late: ++late; return false;
                case MessageCounter::Event::Restart: restarted(); break;
                case MessageCounter::Event::Lost:
                    // The counter counts messages: the samples lost are an estimate.
                    if (gap_) gap_((int64_t)(r.lost * h.count), "network loss");
                    break;
                default: break;
            }
        }
        deliver(m);
        return false;
    }

    // The rate or the format changed: the live engine cannot follow.
    bool changed(const StreamHeader& h) {
        setFatal(std::string("the source started again with another ") +
                 (h.rate != rate_ ? "rate (" + fmtSI(h.rate, "S/s", 6) + ")" : "sample format") + ": click Start again");
        return true;
    }

    void deliver(const StreamMessage& m) {
        const StreamHeader& h = m.h;
        if (h.fc != fc_ || (h.flags & kStreamFlagRetune)) {
            // Before the samples, so the retune is at the right sample.
            if (h.fc != fc_) LOG_INFO("zmq", "the source tuned to %s", fmtSI(h.fc, "Hz", 9).c_str());
            fc_ = h.fc;
            if (retune_) retune_(h.fc);
        }
        gain_ = h.gain;
        agc_ = (h.flags & kStreamFlagAgc) != 0;
        if (h.count && data_) data_(m.samples, h.count);
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

    StreamFormat fmt_;
    FormatEndpoint ep_;
    void* ctx_ = nullptr;
    void* sock_ = nullptr;
    std::vector<uint8_t> first_, rx_, scratch_;
    SampleFormat fmt0_ = SampleFormat::CI16;  // of the samples that the device delivers
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
