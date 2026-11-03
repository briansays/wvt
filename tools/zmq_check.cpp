// ZMQ streaming self-test (issue #96). Stage 1 (the build): the libzmq that
// wvt links has the draft RADIO and DISH sockets, and one message of the
// largest size that wvt sends goes through udp:// on the loopback.
// Later stages add the wire format, the publisher and the subscriber.
#include <zmq.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

constexpr const char* kGroup = "wvt";
// Header (64 bytes) and 8000 bytes of samples: the largest message of the plan.
constexpr size_t kMessageBytes = 64 + 8000;
}  // namespace

int main() {
    int major = 0, minor = 0, patch = 0;
    zmq_version(&major, &minor, &patch);
    printf("libzmq %d.%d.%d\n", major, minor, patch);
    check(zmq_has("draft") != 0, "libzmq has the draft API");

    void* ctx = zmq_ctx_new();
    void* dish = zmq_socket(ctx, ZMQ_DISH);
    void* radio = zmq_socket(ctx, ZMQ_RADIO);
    check(dish && radio, "RADIO and DISH sockets");
    if (!dish || !radio) {
        printf("FAILED\n");
        return 1;
    }

    // A free port: the subscriber binds, so try a few.
    std::string endpoint;
    for (int port = 15600; port < 15700 && endpoint.empty(); ++port) {
        const std::string e = "udp://127.0.0.1:" + std::to_string(port);
        if (zmq_bind(dish, e.c_str()) == 0) endpoint = e;
    }
    check(!endpoint.empty(), "DISH binds on the loopback (" + endpoint + ")");
    check(zmq_join(dish, kGroup) == 0, "DISH joins the group");
    check(!endpoint.empty() && zmq_connect(radio, endpoint.c_str()) == 0, "RADIO connects to the DISH");

    const int timeoutMs = 100;
    zmq_setsockopt(dish, ZMQ_RCVTIMEO, &timeoutMs, sizeof timeoutMs);

    std::vector<unsigned char> sent(kMessageBytes);
    for (size_t i = 0; i < sent.size(); ++i) sent[i] = (unsigned char)(i * 7 + 3);

    // UDP has no handshake: send until the DISH receives one (the RADIO can
    // drop messages until its connection is up), for at most 3 s.
    bool got = false, same = false, group = false;
    size_t size = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!got && std::chrono::steady_clock::now() < end) {
        zmq_msg_t m;
        zmq_msg_init_size(&m, sent.size());
        memcpy(zmq_msg_data(&m), sent.data(), sent.size());
        zmq_msg_set_group(&m, kGroup);
        if (zmq_msg_send(&m, radio, 0) < 0) zmq_msg_close(&m);

        zmq_msg_t r;
        zmq_msg_init(&r);
        if (zmq_msg_recv(&r, dish, 0) >= 0) {
            got = true;
            size = zmq_msg_size(&r);
            same = size == sent.size() && memcmp(zmq_msg_data(&r), sent.data(), size) == 0;
            group = zmq_msg_group(&r) && strcmp(zmq_msg_group(&r), kGroup) == 0;
        }
        zmq_msg_close(&r);
    }
    check(got, "a message arrives through udp:// on the loopback");
    check(same, "it has the " + std::to_string(kMessageBytes) + " bytes that were sent (got " + std::to_string(size) + ")");
    check(group, "it has the group name");

    const int linger = 0;
    zmq_setsockopt(radio, ZMQ_LINGER, &linger, sizeof linger);
    zmq_setsockopt(dish, ZMQ_LINGER, &linger, sizeof linger);
    zmq_close(radio);
    zmq_close(dish);
    zmq_ctx_term(ctx);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
