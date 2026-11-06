#include "net/zmq_pub.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "common/log.h"
#include "net/zmq_format.h"
#include "net/zmq_wire.h"

#ifdef WVT_HAVE_ZMQ
#include <zmq.h>

namespace {
// Messages that the RADIO socket keeps while the network is slower than the
// stream: at most 80 MB. RADIO drops the messages above this without an
// error, and the subscriber sees the gap.
constexpr int kSendHwm = 10000;
constexpr int64_t kSendQueueBytes = 80 << 20;  // tcp formats: the HWM from the message size
constexpr int kSendBuffer = 4 << 20;   // the UDP socket buffer, if the system allows it
constexpr int kLingerMs = 200;         // to send the end-of-stream message at stop
constexpr auto kIdleWait = std::chrono::milliseconds(5);
constexpr auto kInfoPeriod = std::chrono::seconds(1);
constexpr auto kSkipLogPeriod = std::chrono::seconds(5);

const char* fmtText(SampleFormat f) {
    return f == SampleFormat::CU8 ? "cu8" : f == SampleFormat::CI16 ? "ci16" : f == SampleFormat::CF32 ? "cf32" : "?";
}

uint64_t wallNowNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace
#endif

struct StreamPublisher::Impl {
    PublishConfig cfg;
    PublishSource src;
    StreamFormat fmt;      // builtinStreamFormat(), or a format file
    std::string addrText;  // for messages
    std::shared_ptr<SampleRing> ring;
    std::thread th;
    std::atomic<bool> active{false}, quit{false};
    std::atomic<int64_t> stopAt{0};

    struct Retune {
        int64_t pos;
        double fc;
    };
    mutable std::mutex m;  // guards everything below
    std::condition_variable cv;
    std::deque<Retune> retunes;
    double gain = 0;
    bool agc = false;
    Status st;

#ifdef WVT_HAVE_ZMQ
    void* ctx = nullptr;
    void* sock = nullptr;

    void closeSocket() {
        if (sock) zmq_close(sock);
        if (ctx) zmq_ctx_term(ctx);
        sock = ctx = nullptr;
    }

    // Sends one frame (`more`: another frame of the message follows); false
    // on an error that stops publishing. `dropped` is set when the socket
    // dropped it (the queue is full, or PUSH has no peer).
    bool send(zmq_msg_t& msg, std::string& err, bool* dropped = nullptr, bool more = false) {
        if (fmt.socket == FormatSocket::Dish) zmq_msg_set_group(&msg, fmt.group.c_str());
        if (zmq_msg_send(&msg, sock, ZMQ_DONTWAIT | (more ? ZMQ_SNDMORE : 0)) >= 0) return true;
        const int e = zmq_errno();
        zmq_msg_close(&msg);
        if (e == EAGAIN || e == EINTR) {  // the subscriber sees the gap
            if (dropped) *dropped = true;
            return true;
        }
        err = std::string("send failed: ") + zmq_strerror(e);
        return false;
    }

    bool sendBytes(const uint8_t* p, size_t n, std::string& err, bool* dropped = nullptr, bool more = false) {
        zmq_msg_t msg;
        zmq_msg_init_size(&msg, n);
        if (n) memcpy(zmq_msg_data(&msg), p, n);
        return send(msg, err, dropped, more);
    }
    bool sendBytes(const std::vector<uint8_t>& bytes, std::string& err) { return sendBytes(bytes.data(), bytes.size(), err); }

    // One message of a format file: the topic frame (sub with a topic), then
    // the data. A topic that the queue takes is followed by its data frame.
    bool sendFormat(const std::vector<uint8_t>& data, std::string& err, bool& dropped) {
        const bool topic = fmt.socket == FormatSocket::Sub && !fmt.topic.empty();
        if (topic) {
            if (!sendBytes((const uint8_t*)fmt.topic.data(), fmt.topic.size(), err, &dropped, true)) return false;
            if (dropped) return true;
        }
        return sendBytes(data.data(), data.size(), err, &dropped);
    }

    void run(int64_t startPos, uint64_t startWallNs) {
        using clock = std::chrono::steady_clock;
        const int bps = ring->bytesPerSample();
        const bool builtin = fmt.builtin;
        const int64_t maxN = builtin ? (int64_t)streamMaxSamples(src.fmt) : (int64_t)publishMaxSamples(fmt);
        std::vector<uint8_t> raw, packed;  // format files: the samples of the ring, the message
        uint64_t seq = 0;                  // format files: the message counter
        int64_t bytesSent = 0;
        const int64_t cap = ring->capacity();
        StreamHeader h;
        h.fmt = src.fmt;
        h.rate = src.rate;
        h.fc = src.fc;
        {
            std::lock_guard<std::mutex> lk(m);
            h.streamId = st.streamId;
        }
        const StreamInfo info{src.driver, src.description};
        bool retuneFlag = false;
        int64_t pos = startPos;
        std::string err;
        auto nextInfo = clock::now();
        auto statsFrom = clock::now();
        int64_t statsMsgs = 0, statsBytes = 0;
        int64_t skippedLogged = 0;
        auto lastSkipLog = clock::now() - kSkipLogPeriod;

        auto skipTo = [&](int64_t to) {
            if (to <= pos) return;
            std::lock_guard<std::mutex> lk(m);
            st.skipped += to - pos;
            pos = to;
        };

        while (err.empty()) {
            const bool stopping = quit.load();
            const int64_t w = ring->written();
            const int64_t end = stopping ? std::min(w, stopAt.load()) : w;

            {
                std::lock_guard<std::mutex> lk(m);
                h.gain = gain;
                h.flags = agc ? kStreamFlagAgc : 0;
            }
            const auto now = clock::now();
            if (builtin && now >= nextInfo) {
                h.index = (uint64_t)(pos - startPos);
                if (!sendBytes(packStreamInfo(h, info), err)) break;
                nextInfo = now + kInfoPeriod;
            }
            if (const auto dt = std::chrono::duration<double>(now - statsFrom).count(); dt >= 1) {
                std::lock_guard<std::mutex> lk(m);
                st.messagesPerSec = (double)(st.messages - statsMsgs) / dt;
                st.bytesPerSec = (double)(bytesSent - statsBytes) / dt;
                statsMsgs = st.messages;
                statsBytes = bytesSent;
                statsFrom = now;
            }
            int64_t sk = 0;
            {
                std::lock_guard<std::mutex> lk(m);
                sk = st.skipped;
            }
            if (sk != skippedLogged && now - lastSkipLog >= kSkipLogPeriod) {
                LOG_WARN("stream", "publisher fell behind: %lld samples not sent since the last report",
                         (long long)(sk - skippedLogged));
                skippedLogged = sk;
                lastSkipLog = now;
            }

            // Far behind: the ring overwrites these samples soon. Keep a
            // quarter of the ring, so that the reads that follow succeed.
            if (w - pos > cap * 3 / 4) skipTo(w - cap / 4);

            if (pos < end) {
                int64_t n = std::min(maxN, end - pos);
                {
                    std::lock_guard<std::mutex> lk(m);
                    while (!retunes.empty() && retunes.front().pos <= pos) {
                        h.fc = retunes.front().fc;
                        retuneFlag = true;
                        retunes.pop_front();
                    }
                    // A message does not cross a retune.
                    if (!retunes.empty() && retunes.front().pos < pos + n) n = retunes.front().pos - pos;
                }
                h.kind = StreamKind::Data;
                h.count = (uint32_t)n;
                h.index = (uint64_t)(pos - startPos);
                h.timeNs = startWallNs + (uint64_t)((double)(pos - startPos) / src.rate * 1e9);
                if (retuneFlag) h.flags |= kStreamFlagRetune;
                bool dropped = false;
                size_t bytes = 0;
                if (builtin) {
                    zmq_msg_t msg;
                    bytes = kStreamHeaderBytes + (size_t)n * bps;
                    zmq_msg_init_size(&msg, bytes);
                    auto* p = (uint8_t*)zmq_msg_data(&msg);
                    if (!ring->read(pos, n, p + kStreamHeaderBytes)) {
                        zmq_msg_close(&msg);
                        skipTo(ring->written());  // overwritten during the copy
                        continue;
                    }
                    packStreamHeader(h, p);
                    if (!send(msg, err, &dropped)) break;
                } else {
                    raw.resize((size_t)n * bps);
                    if (!ring->read(pos, n, raw.data())) {
                        skipTo(ring->written());
                        continue;
                    }
                    std::string why;
                    if (!encodeStreamMessage(fmt, h, seq, src.fmt, raw.data(), (size_t)n, packed, why)) {
                        err = why;
                        break;
                    }
                    ++seq;  // also for a dropped message: the subscriber counts it as lost
                    bytes = packed.size();
                    if (!sendFormat(packed, err, dropped)) break;
                }
                retuneFlag = false;
                pos += n;
                std::lock_guard<std::mutex> lk(m);
                if (dropped) {
                    st.skipped += n;
                } else {
                    ++st.messages;
                    st.samples += n;
                    bytesSent += (int64_t)bytes;
                }
                continue;  // more samples may wait
            }
            if (stopping) break;
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, kIdleWait, [&] { return quit.load(); });
        }

        if (err.empty() && builtin) {
            h.kind = StreamKind::Data;
            h.count = 0;
            h.index = (uint64_t)(pos - startPos);
            h.flags |= kStreamFlagEnd;
            std::vector<uint8_t> endMsg(kStreamHeaderBytes);
            packStreamHeader(h, endMsg.data());
            sendBytes(endMsg, err);
        }
        std::lock_guard<std::mutex> lk(m);
        if (!err.empty()) {
            st.error = err;
            LOG_ERROR("stream", "publishing to %s stopped: %s", addrText.c_str(), err.c_str());
        }
        active = false;
    }
#endif
};

StreamPublisher::StreamPublisher() : d_(std::make_unique<Impl>()) {}

StreamPublisher::~StreamPublisher() { stop(); }

bool StreamPublisher::active() const { return d_->active.load(); }

const PublishConfig& StreamPublisher::config() const { return d_->cfg; }

void StreamPublisher::noteRetune(int64_t pos, double fc) {
    if (!d_->active) return;
    std::lock_guard<std::mutex> lk(d_->m);
    d_->retunes.push_back({pos, fc});
}

void StreamPublisher::setGain(double db, bool agc) {
    std::lock_guard<std::mutex> lk(d_->m);
    d_->gain = db;
    d_->agc = agc;
}

StreamPublisher::Status StreamPublisher::status() const {
    std::lock_guard<std::mutex> lk(d_->m);
    Status s = d_->st;
    s.active = d_->active.load();
    return s;
}

#ifdef WVT_HAVE_ZMQ

bool StreamPublisher::start(const std::shared_ptr<SampleRing>& ring, const PublishSource& src, const PublishConfig& cfg,
                            std::string& err) {
    stop();
    Impl& d = *d_;
    StreamFormat fmt;
    if (!resolveStreamFormat(cfg.format, fmt, err) || !checkPublishFormat(fmt, err)) return false;
    // The built-in format sends to an address as before (zmq.address is its
    // default); a format file has its own default address.
    FormatEndpoint ep;
    if (!publishEndpoint(fmt, cfg.address, ep, err)) return false;
    if (!ring || streamBytesPerSample(src.fmt) == 0 || streamBytesPerSample(src.fmt) != ring->bytesPerSample()) {
        err = "this sample format cannot be published";
        return false;
    }
    if (!(src.rate > 0)) {
        err = "no sample rate";
        return false;
    }
    if (!fmt.builtin) {
        // A value that does not fit (fc in a u16 field) shows now, not after the first message.
        StreamHeader h;
        h.rate = src.rate;
        h.fc = src.fc;
        h.gain = src.gain;
        std::vector<uint8_t> zeros((size_t)ring->bytesPerSample()), out;
        if (!encodeStreamMessage(fmt, h, 0, src.fmt, zeros.data(), 1, out, err)) {
            err = "the format '" + fmt.name + "': " + err;
            return false;
        }
    }
    const bool dish = fmt.socket == FormatSocket::Dish;
    const int type = dish ? ZMQ_RADIO : fmt.socket == FormatSocket::Sub ? ZMQ_PUB : ZMQ_PUSH;
    const char* typeName = dish ? "RADIO" : fmt.socket == FormatSocket::Sub ? "PUB" : "PUSH";
    const bool bind = !dish && !fmt.bind;  // the subscriber connects: the publisher binds
    d.ctx = zmq_ctx_new();
    d.sock = d.ctx ? zmq_socket(d.ctx, type) : nullptr;
    if (!d.sock) {
        err = std::string("cannot open a ZMQ ") + typeName + " socket: " + zmq_strerror(zmq_errno());
        d.closeSocket();
        return false;
    }
    const int64_t msgBytes = (int64_t)fmt.headerBytes + (int64_t)publishMaxSamples(fmt) * formatBytesPerSample(fmt.sampleFmt);
    const int hwm = fmt.builtin || dish ? kSendHwm : (int)std::clamp<int64_t>(kSendQueueBytes / std::max<int64_t>(msgBytes, 1), 16, kSendHwm);
    const int sndbuf = kSendBuffer, linger = kLingerMs, hops = std::clamp(cfg.ttl, 1, 255);
    zmq_setsockopt(d.sock, ZMQ_SNDHWM, &hwm, sizeof hwm);
    if (dish) zmq_setsockopt(d.sock, ZMQ_SNDBUF, &sndbuf, sizeof sndbuf);
    zmq_setsockopt(d.sock, ZMQ_LINGER, &linger, sizeof linger);
    if (dish && ep.udp.multicast) zmq_setsockopt(d.sock, ZMQ_MULTICAST_HOPS, &hops, sizeof hops);
    if ((bind ? zmq_bind(d.sock, ep.endpoint.c_str()) : zmq_connect(d.sock, ep.endpoint.c_str())) != 0) {
        const int e = zmq_errno();
        if (bind && e == EADDRINUSE) err = "port " + std::to_string(ep.port) + " is in use (another publisher?)";
        else err = std::string("cannot ") + (bind ? "listen on " : "send to ") + ep.text + ": " + zmq_strerror(e);
        d.closeSocket();
        return false;
    }
    if (!fmt.builtin && !fmt.rate.set && std::fabs(fmt.fixedRate - src.rate) > 1e-6 * src.rate)
        LOG_WARN("stream", "format %s: the subscriber takes the rate %.6g S/s from [stream] of the file; the device "
                           "runs at %.6g S/s", fmt.id.c_str(), fmt.fixedRate, src.rate);

    std::random_device rd;
    uint32_t id = 0;
    while (id == 0) id = rd();
    d.cfg = cfg;
    d.src = src;
    d.fmt = fmt;
    d.addrText = ep.text;
    d.ring = ring;
    {
        std::lock_guard<std::mutex> lk(d.m);
        d.st = Status{};
        d.st.address = ep.text;
        d.st.format = fmt.builtin ? "wvt" : fmt.name;
        d.st.streamId = id;
        d.retunes.clear();
        d.gain = src.gain;
        d.agc = src.agc;
    }
    d.quit = false;
    d.active = true;
    const int64_t startPos = d.ring->written();
    const uint64_t startWall = wallNowNs();
    d.th = std::thread([&d, startPos, startWall] { d.run(startPos, startWall); });
    LOG_INFO("stream", "publishing to %s%s with %s (%s %s), format %s: stream %08x, %s, %.6g S/s, %.9g Hz", ep.text.c_str(),
             dish && ep.udp.multicast ? " (multicast)" : "", typeName, bind ? "bind" : "connect", ep.endpoint.c_str(),
             fmt.id.c_str(), id, fmtText(src.fmt), src.rate, src.fc);
    return true;
}

void StreamPublisher::stop() {
    Impl& d = *d_;
    if (!d.th.joinable()) return;
    d.stopAt = d.ring->written();
    d.quit = true;
    d.cv.notify_all();
    d.th.join();
    d.closeSocket();
    const Status s = status();
    LOG_INFO("stream", "stopped publishing to %s: %lld samples in %lld messages, %lld samples skipped",
             s.address.c_str(), (long long)s.samples, (long long)s.messages, (long long)s.skipped);
    std::lock_guard<std::mutex> lk(d.m);
    d.retunes.clear();
}

#else  // no ZMQ in this build

bool StreamPublisher::start(const std::shared_ptr<SampleRing>&, const PublishSource&, const PublishConfig&, std::string& err) {
    err = "this wvt was built without ZMQ (WVT_WITH_ZMQ)";
    return false;
}

void StreamPublisher::stop() {}

#endif
