#pragma once
// Publishes a live stream over ZMQ on UDP (issue #96): a RADIO socket sends
// the samples of a SampleRing in the wvt wire format (net/zmq_wire.h) to a
// unicast address (the subscriber's) or a multicast group.
//
// The publisher is a lossy reader of the ring, like the spectrum and the
// audio: it leaves the lossless gate to the recorder, so a recording does not
// change. If it falls behind (the ring overwrote samples that it did not
// send), it skips to new samples; the sample index in the messages tells the
// subscriber how many it lost.
//
// The class has the same layout in every build. Without ZMQ (WVT_HAVE_ZMQ),
// start() fails with "built without ZMQ".
#include <cstdint>
#include <memory>
#include <string>

#include "common/sample_ring.h"
#include "io/iq_file.h"

struct PublishConfig {
    std::string address;  // "a.b.c.d[:port]" or "iface;group[:port]" (parseStreamAddress)
    int ttl = 1;          // multicast hops; 1 = the local network only
};

struct PublishSource {
    SampleFormat fmt = SampleFormat::CI16;  // native format of the ring
    double rate = 0;
    double fc = 0;
    double gain = 0;
    bool agc = false;
    std::string driver, description;  // for the info message
};

class StreamPublisher {
public:
    struct Status {
        bool active = false;
        std::string address;     // as parsed, e.g. "127.0.0.1:5600"
        std::string error;       // why publishing stopped by itself
        uint32_t streamId = 0;
        int64_t messages = 0;    // data messages sent
        int64_t samples = 0;     // samples sent
        int64_t skipped = 0;     // samples not sent because the publisher fell behind
        double messagesPerSec = 0, bytesPerSec = 0;  // over the last second
    };

    StreamPublisher();
    ~StreamPublisher();
    StreamPublisher(const StreamPublisher&) = delete;
    StreamPublisher& operator=(const StreamPublisher&) = delete;

    // Starts with the newest sample of the ring. The sample index of the
    // stream counts from there.
    bool start(const std::shared_ptr<SampleRing>& ring, const PublishSource& src, const PublishConfig& cfg, std::string& err);
    // Sends what is in the ring now, then an end-of-stream message.
    void stop();
    bool active() const;
    const PublishConfig& config() const;

    // The center frequency is `fc` from ring position `pos` on. The message
    // that starts there has the retune flag.
    void noteRetune(int64_t pos, double fc);
    void setGain(double db, bool agc);
    Status status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
