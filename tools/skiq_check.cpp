// Sidekiq self-test (issue #4). Every build checks the logic that needs no
// SDK: the gain in dB to a gain index, the lost-sample count from the RF
// timestamps, the sample conversion (I/Q order and 12-bit scaling), the
// sample rate rule and the RF port names. With -DWVT_SKIQ_SIM=ON it also checks the backend against
// the simulated card in tools/skiq_sim: the device list, open and its errors,
// streaming in real time, the tone at the right frequency and level on a
// 12-bit and a 16-bit card, a retune while streaming (no false gap from the
// timestamp slip), the overload note, the USB (polled) transport, an overrun
// with the exact count of lost samples, a host that stalls at 100 MS/s (its
// loss counted), a card removal, and the RF ports of every handle, fixed and
// TRX (issue #85). Transmit (issue #86): the gain to attenuation mapping, the
// sample conversion and the block size in every build; with the simulator the
// transmitted level and tone, retune, attenuation, ports, the rules for one
// card that receives and transmits (no false gap from a transmit retune),
// underruns, removal, and a quiet card after stop. Exit status is nonzero on failure.
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "device/sdr_device.h"
#include "device/sdr_tx.h"
#ifdef WVT_HAVE_SKIQ
#include "sidekiq_api.h"  // the simulator's, for skiq_sim_tx_state
#endif

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void testGain() {
    printf("gain in dB to a gain index\n");
    // AD9361 cards (mPCIe, M.2): index 0-76, 1 dB per step.
    check(skiqGainIndex(30, 0, 76, 1) == 30 && skiqGainIndex(30.4, 0, 76, 1) == 30, "M.2: 30 dB -> index 30");
    check(skiqGainIndex(-5, 0, 76, 1) == 0 && skiqGainIndex(100, 0, 76, 1) == 76, "M.2: clamped to 0-76");
    // ADRV9009 cards (X2, X4): 0.5 dB per step from the lowest index.
    check(skiqGainIndex(0, 195, 255, 0.5) == 195 && skiqGainIndex(30, 195, 255, 0.5) == 255, "X4: 0 dB -> 195, 30 dB -> 255");
    check(skiqGainIndex(10.2, 195, 255, 0.5) == 215 && skiqGainDb(215, 195, 0.5) == 10, "X4: 10.2 dB -> index 215 = 10 dB");
    check(skiqGainIndex(50, 195, 255, 0.5) == 255 && skiqGainIndex(-1, 195, 255, 0.5) == 195, "X4: clamped to 195-255");
    check(skiqGainIndex(NAN, 10, 20, 1) == 10 && skiqGainIndex(5, 10, 20, 0) == 10, "bad input: the lowest index");
}

void testTimestamps() {
    printf("lost samples from the RF timestamps\n");
    check(skiqLostSamples(1000, 1000) == 0, "no gap");
    check(skiqLostSamples(1000, 1000 + 1018) == 1018, "one lost block of 1018 samples");
    check(skiqLostSamples(UINT64_MAX - 99, 900) == 1000, "the 64-bit counter wraps");
    check(skiqLostSamples(5000, 4000) == -1000, "the timestamp went back: negative");
}

void testConvert() {
    printf("sample conversion\n");
    const int16_t in12[] = {2047, -2048, 1, -1};
    int16_t out[4] = {};
    skiqConvert(in12, out, 4, 12, false);
    check(out[0] == 32752 && out[1] == -32768 && out[2] == 16 && out[3] == -16, "12 bits: full scale to 16 bits (x16)");
    const int16_t qi[] = {100, -200, 7, 9};
    skiqConvert(qi, out, 4, 12, true);
    check(out[0] == -3200 && out[1] == 1600 && out[2] == 144 && out[3] == 112, "Q,I order swapped to I,Q");
    const int16_t in16[] = {32767, -32768, 5, -5};
    skiqConvert(in16, out, 4, 16, false);
    check(out[0] == 32767 && out[1] == -32768 && out[2] == 5 && out[3] == -5, "16 bits: unchanged");
    int16_t inplace[] = {1, 2, 3, 4};
    skiqConvert(inplace, inplace, 4, 12, true);
    check(inplace[0] == 32 && inplace[1] == 16 && inplace[2] == 64 && inplace[3] == 48, "in place");
}

void testRate() {
    printf("sample rate rule\n");
    std::string err, warn;
    check(checkSampleRate("skiq", 10e6, err, warn) && warn.empty(), "10 MS/s allowed");
    check(checkSampleRate("skiq", 245.76e6, err, warn) && !warn.empty(), "245.76 MS/s allowed, with a disk warning");
    check(!checkSampleRate("skiq", 300e6, err, warn), "300 MS/s refused: " + err);
}

void testPortNames() {
    printf("RF port names\n");
    const std::vector<SkiqPort> ports = {{"J1", "A1", false}, {"J2", "A1", false}, {"J1", "A1", true}, {"J3", "A2", false}};
    check(skiqPortLabel(ports[3]) == "J3 (RX A2)" && skiqPortLabel(ports[2]) == "J1 (TRX A1)",
          "labels: J3 (RX A2), J1 (TRX A1)");
    check(skiqFindPort(ports, "J1 (TRX A1)") == 2 && skiqFindPort(ports, "j3 (rx a2)") == 3, "found by label, any case");
    check(skiqFindPort(ports, "J1") == 0 && skiqFindPort(ports, "j3") == 3, "a port name alone: the first with that name");
    check(skiqFindPort(ports, "J9") == -1 && skiqFindPort(ports, "J3 (RX B1)") == -1 && skiqFindPort(ports, "") == -1,
          "unknown: -1");
}

void testTxHelpers() {
    printf("transmit helpers (issue #86)\n");
    // M.2: attenuation 0-359 quarter dB. Gain 0 dB = the most attenuation.
    check(skiqTxAttenuation(0, 0, 359) == 359 && skiqTxAttenuation(89.75, 0, 359) == 0, "gain 0 -> 359, 89.75 dB -> 0");
    check(skiqTxAttenuation(10, 0, 359) == 319 && skiqTxGainDb(319, 359) == 10, "gain 10 dB -> attenuation 319 = 10 dB");
    check(skiqTxAttenuation(-5, 0, 359) == 359 && skiqTxAttenuation(200, 0, 359) == 0, "clamped to 0-359");
    check(skiqTxAttenuation(NAN, 0, 359) == 359 && skiqTxAttenuation(5, 20, 10) == 20, "bad input: the quiet end");
    const std::complex<float> in[] = {{1, 0}, {-1, 0.5f}, {2, -3}, {NAN, 0.25f}};
    int16_t out[8] = {};
    skiqTxConvert(in, out, 4, 12, false);
    check(out[0] == 2047 && out[1] == 0 && out[2] == -2047 && out[3] == 1024 && out[4] == 2047 && out[5] == -2047 &&
              out[6] == 0 && out[7] == 512,
          "12 bits: full scale 2047, clamped, NaN -> 0");
    skiqTxConvert(in, out, 2, 16, true);
    check(out[0] == 0 && out[1] == 32767 && out[2] == 16384 && out[3] == -32767, "16 bits, Q before I");
    check(skiqTxBlockSamples(1e6) == 4092 && skiqTxBlockSamples(0) == 252 && skiqTxBlockSamples(245.76e6) == 16380,
          "block: 4092 at 1 MS/s, 252 at least, 16380 at most");
    bool inc = true;
    for (double r : {233e3, 2e6, 10e6, 61.44e6, 1e9}) inc = inc && (skiqTxBlockSamples(r) + 4) % 256 == 0;
    check(inc, "block + header is a multiple of 256 words");
    check(skiqPortLabel({"J1", "A1", false, true}) == "J1 (TX A1)" && skiqPortLabel({"J1", "A1", true, true}) == "J1 (TRX A1)",
          "port labels: J1 (TX A1), J1 (TRX A1)");
}

#ifdef WVT_HAVE_SKIQ
void sleepS(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

void setEnv(const char* k, const char* v) {
    if (v) setenv(k, v, 1);
    else unsetenv(k);
}

StreamConfig simCfg() {
    StreamConfig c;
    c.driver = "skiq";
    c.fc = 100e6;
    c.rate = 2e6;
    c.gain = 30;
    return c;
}

// Streams for `secs` and collects the samples.
struct Capture {
    std::mutex m;
    std::vector<std::complex<float>> x;
    std::atomic<int64_t> lost{0};
    std::atomic<int> gaps{0}, unknownGaps{0};

    bool start(SdrDevice& d, std::string& err) {
        return d.start(
            [this](const uint8_t* p, int64_t n) {
                const auto* s = (const int16_t*)p;
                std::lock_guard<std::mutex> lk(m);
                for (int64_t i = 0; i < n; ++i) x.emplace_back(s[2 * i] / 32768.f, s[2 * i + 1] / 32768.f);
            },
            [this](int64_t n, const char*) {
                ++gaps;
                if (n > 0) lost += n;
                else ++unknownGaps;
            },
            err);
    }
    std::vector<std::complex<float>> take() {
        std::lock_guard<std::mutex> lk(m);
        return std::move(x);
    }
};

// The tone's offset from the center (mean phase step) and its level in dBFS,
// over the last n samples.
void tone(const std::vector<std::complex<float>>& x, size_t n, double rate, double& offset, double& dbfs) {
    offset = dbfs = NAN;
    if (x.size() < n + 1) return;
    std::complex<double> acc = 0;
    double p = 0;
    for (size_t i = x.size() - n; i < x.size(); ++i) {
        acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1]));
        p += std::norm(x[i]);
    }
    offset = std::arg(acc) * rate / (2 * M_PI);
    dbfs = 10 * std::log10(p / n);
}

void testOpen() {
    printf("device list and open\n");
    bool found = false;
    for (auto& d : enumerateDevices())
        found |= d.driver == "skiq" && d.args == "WVT01" && d.label == "Sidekiq M.2 serial WVT01 (PCIe)";
    check(found, "the simulated M.2 (serial WVT01, PCIe) is listed");

    StreamConfig c = simCfg();
    std::string err;
    auto d = openDevice(c, err);
    check(d != nullptr, "open the first card" + (err.empty() ? "" : ": " + err));
    if (!d) return;
    check(d->description() == "Sidekiq M.2 serial WVT01 (PCIe)", "description: " + d->description());
    check(d->format() == SampleFormat::CI16 && d->sampleRate() == 2e6 && d->centerFreq() == 100e6, "ci16, 2 MS/s, 100 MHz");
    const GainRange g = d->gainRange();
    check(g.min == 0 && g.max == 76 && g.step == 1 && g.agc, "gain 0-76 dB in 1 dB steps, with AGC");
    const DeviceCaps caps = d->caps();
    check(caps.product == "M.2" && !caps.usb && caps.rate.min == 233e3 && caps.rate.max == 61.44e6,
          "caps: M.2, PCIe, 233 kS/s to 61.44 MS/s");
    const std::vector<std::string> m2Ports = {"J1 (RX A1)", "J2 (RX A1)", "J1 (TRX A1)"};
    check(caps.antennas == m2Ports, "ports: J1 (RX A1), J2 (RX A1), J1 (TRX A1)");
    std::string e2;
    check(!openDevice(c, e2) && e2.find("open already") != std::string::npos, "a second open is refused: " + e2);
    found = false;
    for (auto& dev : enumerateDevices()) found |= dev.driver == "skiq" && dev.args == "WVT01";
    check(found, "the open card stays in the device list");
    d.reset();

    c.args = "WVT01";
    d = openDevice(c, err);
    check(d != nullptr, "open by serial number");
    d.reset();
    c.args = "#0";
    d = openDevice(c, err);
    check(d != nullptr, "open by card number #0");
    d.reset();
    c.args = "NOPE1";
    check(!openDevice(c, err) && err.find("serial number NOPE1") != std::string::npos, "unknown serial: " + err);
    c.args = "#5";
    check(!openDevice(c, err) && err.find("no Sidekiq card #5") != std::string::npos, "no card #5: " + err);
    c.args = "#x";
    check(!openDevice(c, err) && err.find("card number") != std::string::npos, "bad card number refused");
    c.args = "";

    c.rate = 100e6;
    check(!openDevice(c, err) && err.find("233 kS/s to 61.44 MS/s") != std::string::npos, "100 MS/s on an M.2: " + err);
    c.rate = 2e6;
    c.fc = 10e6;
    check(!openDevice(c, err) && err.find("cannot tune") != std::string::npos, "10 MHz on an M.2: " + err);
    c.fc = 100e6;
    c.antenna = "J9";
    check(!openDevice(c, err) && err.find("ports: J1 (RX A1), J2 (RX A1), J1 (TRX A1)") != std::string::npos,
          "port J9: " + err);
    c.antenna = "j1";
    d = openDevice(c, err);
    check(d != nullptr, "port j1 (any case)" + (d ? "" : ": " + err));
    d.reset();
    c.antenna = "";
    c.extRef = true;
    check(!openDevice(c, err) && err.find("ref_clock") != std::string::npos, "M.2 external reference: " + err);
    c.extRef = false;

    setEnv("WVT_SKIQ_SIM_BUSY", "4242");
    check(!openDevice(c, err) && err.find("in use by process 4242") != std::string::npos, "card in use: " + err);
    found = false;
    for (auto& dev : enumerateDevices()) found |= dev.label.find("in use by process 4242") != std::string::npos;
    check(found, "the device list names the process that holds the card");
    setEnv("WVT_SKIQ_SIM_BUSY", nullptr);
    setEnv("WVT_SKIQ_SIM_NONE", "1");
    check(!openDevice(c, err) && err.find("no Sidekiq card found") != std::string::npos, "no card: " + err);
    found = false;
    for (auto& dev : enumerateDevices()) found |= dev.driver == "skiq" && dev.label == "Sidekiq (none detected)";
    check(found, "no card: a \"none detected\" entry");
    setEnv("WVT_SKIQ_SIM_NONE", nullptr);
}

void testStream() {
    printf("streaming (M.2, 12 bits)\n");
    StreamConfig c = simCfg();
    std::string err;
    auto d = openDevice(c, err);
    if (!d) {
        check(false, "open: " + err);
        return;
    }
    Capture cap;
    const auto t0 = std::chrono::steady_clock::now();
    check(cap.start(*d, err), "start streaming");
    sleepS(0.5);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto x = cap.take();
    char b[128];
    snprintf(b, sizeof b, "real-time rate: %.2f MS/s in %.2f s (2 expected)", x.size() / secs / 1e6, secs);
    check(x.size() / secs > 1.6e6 && x.size() / secs < 2.2e6, b);
    double off, db;
    tone(x, 8192, 2e6, off, db);
    snprintf(b, sizeof b, "tone at %+.1f kHz (100.2 MHz tuned to 100 MHz: +200 kHz)", off / 1e3);
    check(std::fabs(off - 200e3) < 1e3, b);
    snprintf(b, sizeof b, "tone at %.1f dBFS at 30 dB gain (-30 expected: 12 bits scaled to 16)", db);
    check(std::fabs(db + 30) < 1, b);

    // The simulated AD9361 slips its RF timestamp on a retune, as the real one does.
    check(d->setCenterFreq(100.1e6, err) && d->centerFreq() == 100.1e6, "retune to 100.1 MHz while streaming");
    sleepS(0.3);
    x = cap.take();
    tone(x, 8192, 2e6, off, db);
    snprintf(b, sizeof b, "tone moved to %+.1f kHz (+100 kHz)", off / 1e3);
    check(std::fabs(off - 100e3) < 1e3, b);
    check(d->notes().find("overload") == std::string::npos, "no overload at 30 dB");
    check(d->setGain(false, 62, err), "gain 62 dB while streaming");
    sleepS(0.3);
    check(d->notes().find("RF overload") != std::string::npos, "overload note at 62 dB: " + d->notes());
    check(d->setGain(false, 30, err), "back to 30 dB");
    sleepS(1.2);
    check(d->notes().find("overload") == std::string::npos, "the overload note clears after a second");
    check(d->setGain(true, 0, err), "automatic gain");
    sleepS(0.2);
    cap.take();
    sleepS(0.2);
    tone(cap.take(), 8192, 2e6, off, db);
    snprintf(b, sizeof b, "AGC: tone at %.1f dBFS (-20 expected)", db);
    check(std::fabs(db + 20) < 1, b);
    check(cap.gaps == 0 && d->fatalError().empty(), "no gaps (the timestamp slip of the retune is not one), no fatal error");
    d->stop();
    check(cap.start(*d, err), "start again after stop");
    sleepS(0.2);
    check(!cap.take().empty(), "samples after the restart");
    d->stop();
}

void testX4() {
    printf("Sidekiq X4 (16 bits)\n");
    setEnv("WVT_SKIQ_SIM_PART", "x4");
    StreamConfig c = simCfg();
    c.agc = true;
    c.extRef = true;
    std::string err;
    auto d = openDevice(c, err);
    check(d != nullptr, "open with the external reference" + (d ? "" : ": " + err));
    if (d) {
        const GainRange g = d->gainRange();
        check(g.min == 0 && g.max == 30 && g.step == 0.5 && !g.agc, "gain 0-30 dB in 0.5 dB steps, no AGC");
        check(d->notes().find("no automatic gain") != std::string::npos, "AGC asked: a note, manual gain: " + d->notes());
        std::string e2;
        check(!d->setGain(true, 0, e2), "setGain(auto) refused: " + e2);
        check(d->setGain(false, 30, err), "gain 30 dB");
        Capture cap;
        check(cap.start(*d, err), "start streaming");
        sleepS(0.3);
        double off, db;
        tone(cap.take(), 8192, 2e6, off, db);
        char b[96];
        snprintf(b, sizeof b, "tone at %.1f dBFS at 30 dB gain (-30 expected)", db);
        check(std::fabs(db + 30) < 1, b);
        check(d->setCenterFreq(100.1e6, err), "retune while streaming");
        sleepS(0.2);
        check(cap.gaps == 0, "no gaps");
        d->stop();
    }
    d.reset();

    // A host that stops reading for longer than the DMA buffer (64 MiB, 0.17 s
    // at 100 MS/s) loses samples, and wvt must count them. A slow or sanitizer
    // build may not keep up with 100 MS/s even without the stall, so only the
    // loss that the stall must cause is checked.
    c = simCfg();
    c.rate = 100e6;
    d = openDevice(c, err);
    if (d) {
        std::atomic<int64_t> lost{0};
        std::atomic<int> gaps{0};
        std::atomic<bool> stall{false};
        d->start(
            [&](const uint8_t*, int64_t) {
                if (stall.exchange(false)) sleepS(0.5);
            },
            [&](int64_t n, const char*) {
                ++gaps;
                lost += std::max<int64_t>(n, 0);
            },
            err);
        sleepS(0.5);
        const int gaps0 = gaps;
        const int64_t lost0 = lost;
        printf("  [info] 100 MS/s before the stall: %d gap(s), %.1f MS lost\n", gaps0, lost0 / 1e6);
        stall = true;
        sleepS(1.0);
        d->stop();
        char b[128];
        // At least the stall less what the buffer holds: 50 - 16.7 MS.
        snprintf(b, sizeof b, "a 0.5 s stall at 100 MS/s: %d gap(s), %.1f MS lost (33 MS or more)", gaps - gaps0,
                 (lost - lost0) / 1e6);
        check(gaps > gaps0 && lost - lost0 >= 33e6, b);
    }
    setEnv("WVT_SKIQ_SIM_PART", nullptr);
}

// Opens with `port`, streams, and checks that the tone has the simulator's
// offset for that port: 10 kHz per port number from J2 (+200 kHz).
void checkPort(StreamConfig c, const std::string& port, double offsetHz) {
    c.antenna = port;
    std::string err;
    auto d = openDevice(c, err);
    if (!d) {
        check(false, "open with port " + port + ": " + err);
        return;
    }
    Capture cap;
    const bool ok = cap.start(*d, err);
    sleepS(0.3);
    d->stop();
    double off, db;
    tone(cap.take(), 8192, c.rate, off, db);
    char b[160];
    snprintf(b, sizeof b, "port %s: streams, tone at %+.1f kHz (%+.0f expected)%s", port.empty() ? "(default)" : port.c_str(), off / 1e3,
             offsetHz / 1e3, ok ? "" : (" - " + err).c_str());
    check(ok && std::fabs(off - offsetHz) < 1e3, b);
}

void testPorts() {
    printf("RF ports (issue #85)\n");
    StreamConfig c = simCfg();
    // M.2: handle A1, the fixed ports J1 and J2 (default) and the TRX port J1.
    checkPort(c, "", 200e3);
    checkPort(c, "J1 (RX A1)", 190e3);
    checkPort(c, "J1 (TRX A1)", 190e3);
    checkPort(c, "J2", 200e3);  // a port name alone, as in sessions before issue #85
    checkPort(c, "j1", 190e3);  // the first J1: the fixed one

    // X4: the handles A1, A2, B1 and B2, listed per handle only.
    setEnv("WVT_SKIQ_SIM_PART", "x4");
    std::string err;
    auto d = openDevice(c, err);
    const std::vector<std::string> x4Ports = {"J1 (RX A1)", "J2 (RX A2)", "J3 (RX B1)", "J4 (RX B2)"};
    check(d && d->caps().antennas == x4Ports, "X4 ports: J1 (RX A1), J2 (RX A2), J3 (RX B1), J4 (RX B2)");
    d.reset();
    checkPort(c, "", 190e3);
    checkPort(c, "J2 (RX A2)", 200e3);  // A2 needs the dual channel mode
    checkPort(c, "J3 (RX B1)", 210e3);
    checkPort(c, "j4 (rx b2)", 220e3);
    c.antenna = "J5";
    check(!openDevice(c, err) && err.find("(ports: J1 (RX A1), J2 (RX A2), J3 (RX B1), J4 (RX B2))") != std::string::npos,
          "X4 port J5: " + err);
    setEnv("WVT_SKIQ_SIM_PART", nullptr);
}

void testTransportAndFaults() {
    printf("USB transport, overrun, removal\n");
    setEnv("WVT_SKIQ_SIM_USB", "1");
    StreamConfig c = simCfg();
    std::string err;
    auto d = openDevice(c, err);
    check(d && d->description().find("(USB)") != std::string::npos && d->caps().usb, "USB in the description and caps");
    if (d) {
        Capture cap;
        const auto t0 = std::chrono::steady_clock::now();
        check(cap.start(*d, err), "start streaming (polled receive)");
        sleepS(0.4);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const size_t n = cap.take().size();
        char b[96];
        snprintf(b, sizeof b, "polled: %.2f MS/s (2 expected)", n / secs / 1e6);
        check(n / secs > 1.6e6 && n / secs < 2.2e6, b);
        check(d->setGain(false, 62, err), "gain 62 dB");
        sleepS(0.8);
        check(d->notes().find("RF overload") != std::string::npos, "overload from the polled state (no flag in USB blocks)");
        d->stop();
    }
    d.reset();
    setEnv("WVT_SKIQ_SIM_USB", nullptr);

    setEnv("WVT_SKIQ_SIM_OVERRUN", "1");
    d = openDevice(c, err);
    if (d) {
        Capture cap;
        cap.start(*d, err);
        sleepS(0.5);
        d->stop();
        char b[96];
        snprintf(b, sizeof b, "overrun: one gap of %lld samples (5 blocks = 5090)", (long long)cap.lost.load());
        check(cap.gaps == 1 && cap.lost == 5090 && cap.unknownGaps == 0, b);
    }
    d.reset();
    setEnv("WVT_SKIQ_SIM_OVERRUN", nullptr);

    setEnv("WVT_SKIQ_SIM_UNPLUG", "0.2");
    d = openDevice(c, err);
    if (d) {
        Capture cap;
        cap.start(*d, err);
        // The card fails at 0.2 s and the error comes after 10 failed reads 10 ms
        // apart; a loaded CI runner can take much longer, so wait up to 5 s.
        for (int i = 0; i < 100 && d->fatalError().empty(); ++i) sleepS(0.05);
        check(d->fatalError().find("card removed") != std::string::npos, "removal: " + d->fatalError());
        d->stop();
    }
    d.reset();
    setEnv("WVT_SKIQ_SIM_UNPLUG", nullptr);
}

// ---- transmit (issue #86) ----

TxConfig txCfg() {
    TxConfig c;
    c.driver = "skiq";
    c.fc = 100e6;
    c.rate = 2e6;
    c.gain = 30;
    return c;
}

skiq_sim_tx_state_t simTx() {
    skiq_sim_tx_state_t s{};
    skiq_sim_tx_state(&s);
    return s;
}

// A tone at `offset` Hz with amplitude `amp`; `stall` once makes the host late.
struct ToneFill {
    double rate, offset;
    std::atomic<float> amp{0.5f};
    std::atomic<double> stall{0};
    double ph = 0;
    SdrTx::FillFn fn() {
        return [this](std::complex<float>* out, int64_t n) {
            if (const double s = stall.exchange(0); s > 0) sleepS(s);
            const double w = 2 * M_PI * offset / rate;
            for (int64_t i = 0; i < n; ++i) {
                out[i] = std::polar(amp.load(), (float)ph);
                ph = std::fmod(ph + w, 2 * M_PI);
            }
        };
    }
};

void testTx() {
    printf("transmit (M.2, 12 bits)\n");
    std::string err;
    auto t = openTx(txCfg(), err);
    if (!t) {
        check(false, "open the transmitter: " + err);
        return;
    }
    const GainRange g = t->gainRange();
    check(g.min == 0 && g.max == 89.75 && g.step == 0.25, "gain 0-89.75 dB in 0.25 dB steps (the attenuation range)");
    check(t->description() == "Sidekiq M.2 serial WVT01 (PCIe)" && t->antenna() == "J1 (TX A1)",
          "description and default port: " + t->description() + ", " + t->antenna());
    check(t->sampleRate() == 2e6 && t->centerFreq() == 100e6 && t->gain() == 30, "2 MS/s, 100 MHz, gain 30 dB");
    check(!simTx().streaming && simTx().atten == 359, "quiet before start (the most attenuation)");

    ToneFill tone{2e6, 100e3};
    const auto t0 = std::chrono::steady_clock::now();
    check(t->start(tone.fn(), err), "start" + (err.empty() ? "" : ": " + err));
    sleepS(0.4);
    auto s = simTx();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char b[160];
    check(s.streaming && s.atten == 239 && s.lo == 100000000, "streaming at 100 MHz, attenuation 239 (59.75 dB) for 30 dB gain");
    snprintf(b, sizeof b, "tone at %+.1f kHz (+100 expected: I/Q order kept)", s.offsetHz / 1e3);
    check(std::fabs(s.offsetHz - 100e3) < 1e3, b);
    snprintf(b, sizeof b, "level %.2f dBFS at the DAC for amplitude 0.5 (-6.02 expected), %lld values out of range",
             s.levelDbfs, (long long)s.outOfRange);
    check(std::fabs(s.levelDbfs + 6.02) < 0.1 && s.outOfRange == 0, b);
    // The FPGA holds 100 ms ahead of real time.
    const double rate = s.samples / (secs + 0.1);
    snprintf(b, sizeof b, "real-time: %.2f MS/s (2 expected), %llu blocks of %llu", rate / 1e6, (unsigned long long)s.blocks,
             (unsigned long long)(s.blocks ? s.samples / s.blocks : 0));
    check(rate > 1.6e6 && rate < 2.4e6 && s.samples % skiqTxBlockSamples(2e6) == 0, b);
    check(t->stats().sent > 0 && t->stats().underflows == 0, "sent counted, no underruns");

    check(t->setGain(10, err) && simTx().atten == 319 && t->gain() == 10, "gain 10 dB: attenuation 319");
    check(t->setCenterFreq(200e6, err) && simTx().lo == 200000000 && t->centerFreq() == 200e6, "retune to 200 MHz");
    std::string e2;
    check(!t->setCenterFreq(10e6, e2) && simTx().lo == 200000000 && e2.find("cannot transmit at") != std::string::npos,
          "10 MHz refused, still at 200 MHz: " + e2);
    tone.amp = 1.0f;
    sleepS(0.25);
    s = simTx();
    snprintf(b, sizeof b, "amplitude 1: full scale (%.2f dBFS)", s.levelDbfs);
    check(std::fabs(s.levelDbfs) < 0.1, b);
    tone.amp = 1.5f;  // I and Q clamp on their own
    sleepS(0.25);
    check(simTx().outOfRange == 0, "amplitude 1.5: clamped, no values out of range");

    tone.stall = 0.3;  // longer than the FPGA's 100 ms
    sleepS(1.0);
    check(t->stats().underflows >= 1, "a 0.3 s stall: underrun counted (" + std::to_string(t->stats().underflows) + ")");
    t->stop();
    s = simTx();
    check(!s.streaming && s.atten == 359, "stop: not streaming, the most attenuation");
    check(t->start(tone.fn(), err) && simTx().streaming && simTx().atten == 319, "start again: attenuation back to 319");
    t->stop();
    t.reset();

    printf("transmit errors and ports\n");
    TxConfig c = txCfg();
    c.rate = 100e6;
    check(!openTx(c, err) && err.find("transmit rate must be 233 kS/s to 61.44 MS/s") != std::string::npos,
          "100 MS/s on an M.2: " + err);
    c = txCfg();
    c.antenna = "J9";
    check(!openTx(c, err) && err.find("(ports: J1 (TX A1), J1 (TRX A1))") != std::string::npos, "port J9: " + err);
    c = txCfg();
    c.ref = "gpsdo";
    check(!openTx(c, err) && err.find("internal or external") != std::string::npos, "reference gpsdo: " + err);
    c.ref = "external";
    check(!openTx(c, err) && err.find("ref_clock") != std::string::npos, "M.2 external reference: " + err);
    c = txCfg();
    c.antenna = "j1 (trx a1)";
    t = openTx(c, err);
    check(t && t->antenna() == "J1 (TRX A1)", "TRX port alone" + (t ? "" : ": " + err));
    if (t) {
        ToneFill f{2e6, 0};
        check(t->start(f.fn(), err) && simTx().streaming && simTx().trxTransmit, "transmits on the TRX port");
        t->stop();
    }
    t.reset();
    // The TRX port is back to receive: the receiver can use it.
    {
        StreamConfig rc = simCfg();
        rc.antenna = "J1 (TRX A1)";
        auto d = openDevice(rc, err);
        Capture cap;
        check(d && cap.start(*d, err), "then receive on the TRX port" + (d ? "" : ": " + err));
        if (d) d->stop();
    }

    setEnv("WVT_SKIQ_SIM_PART", "x4");
    c = txCfg();
    t = openTx(c, err);
    check(t && t->antenna() == "J5 (TX A1)" && t->gainRange().max == 41.75, "X4: default port J5 (TX A1), gain 0-41.75 dB");
    if (t) {
        ToneFill f{2e6, -250e3};
        t->start(f.fn(), err);
        sleepS(0.2);
        s = simTx();
        snprintf(b, sizeof b, "X4: 16 bits, %.2f dBFS, tone %+.1f kHz (-6.02, -250 expected)", s.levelDbfs, s.offsetHz / 1e3);
        check(std::fabs(s.levelDbfs + 6.02) < 0.1 && std::fabs(s.offsetHz + 250e3) < 1e3 && s.outOfRange == 0, b);
        t->stop();
    }
    t.reset();
    for (const char* port : {"J6 (TX A2)", "J7 (TX B1)", "j8"}) {
        c.antenna = port;
        t = openTx(c, err);
        ToneFill f{2e6, 0};
        const bool ok = t && t->start(f.fn(), err);
        const skiq_tx_hdl_t want = std::string(port) == "J6 (TX A2)" ? skiq_tx_hdl_A2
                                   : std::string(port) == "J7 (TX B1)" ? skiq_tx_hdl_B1
                                                                       : skiq_tx_hdl_B2;
        check(ok && simTx().hdl == want, std::string("X4 port ") + port + (ok ? "" : ": " + err));
        if (t) t->stop();
        t.reset();
    }
    setEnv("WVT_SKIQ_SIM_PART", nullptr);

    setEnv("WVT_SKIQ_SIM_UNPLUG", "0.2");
    t = openTx(txCfg(), err);
    if (t) {
        ToneFill f{2e6, 0};
        t->start(f.fn(), err);
        for (int i = 0; i < 100 && t->fatalError().empty(); ++i) sleepS(0.05);
        check(t->fatalError().find("card removed") != std::string::npos, "removal: " + t->fatalError());
        t->stop();
    }
    t.reset();
    setEnv("WVT_SKIQ_SIM_UNPLUG", nullptr);
}

// One card, receive and transmit at once.
void testDuplex() {
    printf("receive and transmit on one card (issue #86)\n");
    std::string err;
    auto d = openDevice(simCfg(), err);
    if (!d) {
        check(false, "open the receiver: " + err);
        return;
    }
    Capture cap;
    check(cap.start(*d, err), "receive at 2 MS/s");
    TxConfig c = txCfg();
    c.rate = 4e6;
    check(!openTx(c, err) && err.find("receiving at 2 MS/s") != std::string::npos, "transmit at 4 MS/s refused: " + err);
    c = txCfg();
    c.antenna = "J1 (TRX A1)";
    check(!openTx(c, err) && err.find("cannot transmit while the live view receives") != std::string::npos,
          "TRX port refused: " + err);
    c = txCfg();
    c.args = "NOPE1";
    check(!openTx(c, err) && err.find("open already") != std::string::npos, "another card refused: " + err);
    c.args = "WVT01";
    auto t = openTx(c, err);
    check(t != nullptr, "transmit on the same card (by serial)" + (t ? "" : ": " + err));
    if (!t) return;
    ToneFill f{2e6, 50e3};
    check(t->start(f.fn(), err), "start transmitting while receiving");
    // Each transmit retune slips the AD9361's receive timestamps.
    for (double fc : {101e6, 102e6, 103e6}) {
        t->setCenterFreq(fc, err);
        sleepS(0.15);
    }
    sleepS(0.6);
    check(simTx().lo == 103000000 && simTx().streaming, "the transmitter retuned 3 times");
    check(cap.gaps == 0, "no false gaps in the receiver (" + std::to_string(cap.gaps.load()) + ")");
    check(d->fatalError().empty() && !cap.take().empty(), "the receiver still streams");
    const auto devs = enumerateDevices("skiq");
    check(devs.size() == 1 && devs[0].label == "Sidekiq M.2 serial WVT01 (PCIe)", "the device list shows the open card");
    // The receiver goes; the transmitter keeps the card.
    d->stop();
    d.reset();
    check(simTx().streaming, "the transmitter keeps the card when the receiver closes");
    StreamConfig rc = simCfg();
    rc.rate = 4e6;
    check(!openDevice(rc, err) && err.find("transmitting at 2 MS/s") != std::string::npos, "receive at 4 MS/s refused: " + err);
    rc = simCfg();
    rc.antenna = "J1 (TRX A1)";
    check(!openDevice(rc, err) && err.find("cannot receive while the Sidekiq transmits") != std::string::npos,
          "receive on the TRX port refused: " + err);
    d = openDevice(simCfg(), err);
    Capture cap2;
    check(d && cap2.start(*d, err), "receive again while transmitting" + (d ? "" : ": " + err));
    if (d) {
        sleepS(0.3);
        check(!cap2.take().empty() && cap2.gaps == 0, "it streams, no gaps");
        d->stop();
    }
    d.reset();
    t->stop();
    t.reset();
    // Both gone: the library has exited, so the card opens fresh.
    d = openDevice(simCfg(), err);
    check(d != nullptr, "the card opens again after both close" + (d ? "" : ": " + err));
}
#endif
}  // namespace

int main() {
    testGain();
    testTimestamps();
    testConvert();
    testRate();
    testPortNames();
    testTxHelpers();
#ifdef WVT_HAVE_SKIQ
    testOpen();
    testStream();
    testX4();
    testPorts();
    testTransportAndFaults();
    testTx();
    testDuplex();
#else
    printf("(the backend checks need -DWVT_SKIQ_SIM=ON)\n");
#endif
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
