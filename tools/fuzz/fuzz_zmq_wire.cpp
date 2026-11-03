// libFuzzer target: the wvt network stream (issue #96). A subscriber parses
// every UDP datagram that reaches its port, from any sender, so the message
// parser must reject all bad input. The same input is also given to the
// address parser (the Address field, --publish, --device zmq:) and, as a
// header, to the sample index tracker.
#include <cstdint>
#include <string>

#include "net/zmq_wire.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    StreamMessage m;
    std::string why;
    if (parseStreamMessage(data, size, m, why)) {
        // A message that parses has its samples inside the input.
        if (m.sampleBytes && (m.samples < data || m.samples + m.sampleBytes > data + size)) __builtin_trap();
        if (m.sampleBytes != (size_t)m.h.count * (size_t)streamBytesPerSample(m.h.fmt) && m.h.kind == StreamKind::Data)
            __builtin_trap();
        StreamTracker t;
        t.next(m.h);
        t.next(m.h);
        // Packing what was read gives a message that parses the same way.
        if (m.h.kind == StreamKind::Info) {
            const auto again = packStreamInfo(m.h, m.info);
            StreamMessage m2;
            if (!parseStreamMessage(again.data(), again.size(), m2, why) || m2.info.description != m.info.description)
                __builtin_trap();
        }
    }
    StreamAddress a;
    if (parseStreamAddress(std::string((const char*)data, size), a, why)) {
        StreamAddress b;
        if (!parseStreamAddress(a.text(), b, why) || b.text() != a.text()) __builtin_trap();
    }
    return 0;
}
