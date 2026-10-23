// Sidekiq self-test (issue #4). Every build checks the logic that needs no
// SDK: the gain in dB to a gain index, the lost-sample count from the RF
// timestamps, the sample conversion (I/Q order and 12-bit scaling) and the
// sample rate rule. With -DWVT_SKIQ_SIM=ON it also checks the backend against
// the simulated card in tools/skiq_sim: the device list, open and its errors,
// streaming in real time, the tone at the right frequency and level on a
// 12-bit and a 16-bit card, a retune while streaming (no false gap from the
// timestamp slip), the overload note, the USB (polled) transport, an overrun
// with the exact count of lost samples, a host that stalls at 100 MS/s (its
// loss counted), and a card removal. Exit status is nonzero on failure.
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
    check(caps.product == "M.2" && !caps.usb && caps.antennas.size() == 2 && caps.antennas[1] == "J2" &&
              caps.rate.min == 233e3 && caps.rate.max == 61.44e6,
          "caps: M.2, PCIe, ports J1 and J2, 233 kS/s to 61.44 MS/s");
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
    check(!openDevice(c, err) && err.find("ports: J1, J2") != std::string::npos, "port J9: " + err);
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
    // at 100 MS/s) loses samples, and wvt must count them.
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
        check(gaps == 0, "100 MS/s: no gaps while the host keeps up");
        stall = true;
        sleepS(1.0);
        d->stop();
        char b[128];
        // At least the stall less the buffer (33 MS), at most the stall and a delivery (52 MS).
        snprintf(b, sizeof b, "a 0.5 s stall at 100 MS/s: %d gap(s), %.1f MS lost (33-52 MS)", gaps.load(), lost / 1e6);
        check(gaps >= 1 && lost >= 33e6 && lost <= 52e6, b);
    }
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
        sleepS(0.6);
        check(d->fatalError().find("card removed") != std::string::npos, "removal: " + d->fatalError());
        d->stop();
    }
    d.reset();
    setEnv("WVT_SKIQ_SIM_UNPLUG", nullptr);
}
#endif
}  // namespace

int main() {
    testGain();
    testTimestamps();
    testConvert();
    testRate();
#ifdef WVT_HAVE_SKIQ
    testOpen();
    testStream();
    testX4();
    testTransportAndFaults();
#else
    printf("(the backend checks need -DWVT_SKIQ_SIM=ON)\n");
#endif
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
