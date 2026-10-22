// BB60 backend self-test (issue #2), against the simulated device in
// tools/bb_sim (build with -DWVT_BB_SIM=ON). It checks the device list, open
// and its errors, the rate and bandwidth rules, streaming in real time, the
// test tone at the right frequency, the ADC overload note, an unplug, and a
// SigMF recording across two retunes: each capture segment must start on the
// first sample of its new tuning. Exit status is nonzero on failure.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dsp/dsp.h"
#include "io/iq_file.h"
#include "common/json.h"
#include "device/live.h"
#include "device/sdr_device.h"

namespace {
int failures = 0;
constexpr double kTone = 100.2e6;  // the simulator's tone

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void sleepS(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

// The strongest frequency (offset from the center) in n samples.
double peakOffset(const std::vector<std::complex<float>>& x, size_t from, int n, double rate) {
    auto fft = getFFT(n);
    std::vector<cf> b(x.begin() + from, x.begin() + from + n);
    fft->forward(b.data());
    int best = 0;
    for (int i = 1; i < n; ++i)
        if (std::norm(b[i]) > std::norm(b[best])) best = i;
    if (best >= n / 2) best -= n;
    return best * rate / n;
}

std::vector<std::complex<float>> toComplex(const std::vector<int16_t>& v) {
    std::vector<std::complex<float>> x(v.size() / 2);
    for (size_t i = 0; i < x.size(); ++i) x[i] = {v[2 * i] / 32768.f, v[2 * i + 1] / 32768.f};
    return x;
}

void testRules() {
    printf("rates and bandwidths\n");
    const auto r = supportedRates("bb");
#ifdef __APPLE__
    check(r.size() == 8 && r.back() == 312.5e3, "macOS: 40 MS/s to 312.5 kS/s (decimation up to 128)");
#else
    check(r.size() == 14 && std::fabs(r.back() - 4882.8125) < 1e-6, "40 MS/s to 4.883 kS/s (decimation up to 8192)");
#endif
    check(r.front() == 40e6, "highest rate 40 MS/s");
    check(snapSampleRate("bb", 9e6) == 10e6 && snapSampleRate("bb", 15e6) == 20e6, "9 MS/s -> 10, 15 MS/s -> 20");
    check(snapSampleRate("uhd", 9e6) == 9e6, "continuous drivers are not snapped");
    check(bbMaxBandwidth(1) == 27e6 && bbMaxBandwidth(2) == 17.8e6 && bbMaxBandwidth(4) == 8e6 &&
              bbMaxBandwidth(256) == 140e3 && bbMaxBandwidth(8192) == 4e3,
          "bandwidth table (27 MHz, 17.8 MHz, 8 MHz, ... 140 kHz, ... 4 kHz)");
    std::string err, warn;
    check(checkSampleRate("bb", 20e6, err, warn) && !warn.empty(), "20 MS/s allowed, with a CPU warning");
    check(!checkSampleRate("bb", 80e6, err, warn), "80 MS/s refused: " + err);
}

void testOpen() {
    printf("device list and open\n");
    auto list = enumerateDevices();
    bool found = false;
    for (auto& d : list) found |= d.driver == "bb" && d.args == "12345678" && d.label.find("BB60D") != std::string::npos;
    check(found, "the simulated BB60D (serial 12345678) is listed");

    StreamConfig c;
    c.driver = "bb";
    c.fc = 100e6;
    c.rate = 9e6;  // not a BB60 rate
    c.gain = -20;
    c.bandwidth = 50e6;
    std::string err;
    auto d = openDevice(c, err);
    check(d != nullptr, "open the first BB60" + (err.empty() ? "" : ": " + err));
    if (!d) return;
    check(d->sampleRate() == 10e6, "9 MS/s snapped to 10 MS/s");
    check(d->notes().find("bandwidth limited to 8 MHz") != std::string::npos, "bandwidth limited: " + d->notes());
    check(d->description() == "Signal Hound BB60D serial 12345678", "description: " + d->description());
    const GainRange g = d->gainRange();
    check(std::string(g.label) == "Ref level" && std::string(g.unit) == "dBm" && !g.agc && g.max == 20,
          "the gain control is a reference level in dBm, -130 to 20, no AGC");
    check(d->usesRetuneMark(), "the BB60 marks its own retunes");
    std::string e2;
    check(!openDevice(c, e2), "a second open fails while the first is open");
    d.reset();
    c.args = "999";
    check(!openDevice(c, err), "an unknown serial fails: " + err.substr(0, 60));
    c.args = "abc";
    check(!openDevice(c, err) && err.find("serial number") != std::string::npos, "a non-number serial is refused");
    c.args = "";
    c.fc = 7e9;
    check(!openDevice(c, err) && err.find("6.4 GHz") != std::string::npos, "7 GHz is refused: " + err);
    setenv("WVT_BB_SIM_NONE", "1", 1);
    c.fc = 100e6;
    check(!openDevice(c, err) && err.find("cannot open") != std::string::npos, "no device: a clear error");
    unsetenv("WVT_BB_SIM_NONE");
}

void testStream() {
    printf("streaming\n");
    StreamConfig c;
    c.driver = "bb";
    c.fc = 100e6;
    c.rate = 2.5e6;
    c.gain = -20;
    std::string err;
    auto d = openDevice(c, err);
    if (!d) {
        check(false, "open: " + err);
        return;
    }
    std::mutex m;
    std::vector<int16_t> got;
    std::atomic<int> gaps{0};
    const auto t0 = std::chrono::steady_clock::now();
    check(d->start(
              [&](const uint8_t* p, int64_t n) {
                  std::lock_guard<std::mutex> lk(m);
                  got.insert(got.end(), (const int16_t*)p, (const int16_t*)p + 2 * n);
              },
              [&](int64_t, const char*) { ++gaps; }, err),
          "start streaming");
    sleepS(0.5);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<int16_t> copy;
    {
        std::lock_guard<std::mutex> lk(m);
        copy = std::move(got);
    }
    const double rate = copy.size() / 2 / secs;
    char b[96];
    snprintf(b, sizeof b, "real-time rate: %.2f MS/s in %.2f s (2.5 expected)", rate / 1e6, secs);
    check(rate > 2.0e6 && rate < 2.7e6, b);
    auto x = toComplex(copy);
    if (x.size() >= 8192) {
        const double off = peakOffset(x, x.size() - 8192, 8192, 2.5e6);
        snprintf(b, sizeof b, "tone at %+.1f kHz (100.2 MHz tuned to 100 MHz: +200 kHz)", off / 1e3);
        check(std::fabs(off - 200e3) < 1e3, b);
    }
    check(d->notes().find("ADC overload") == std::string::npos, "no overload at -20 dBm (the tone is -30 dBm)");
    check(d->setGain(false, -40, err) && d->sampleRate() == 2.5e6, "reference level -40 dBm while streaming");
    sleepS(0.3);
    check(d->notes().find("ADC overload") != std::string::npos, "overload note at -40 dBm: " + d->notes());
    check(d->setGain(false, -20, err), "back to -20 dBm");
    sleepS(1.2);
    check(d->notes().find("ADC overload") == std::string::npos, "the overload note clears after a second");
    check(gaps == 0 && d->fatalError().empty(), "no gaps, no fatal error");
    d->stop();

    // Unplug: the simulator disconnects after 0.2 s of streaming.
    setenv("WVT_BB_SIM_UNPLUG", "0.2", 1);
    check(d->start([](const uint8_t*, int64_t) {}, [](int64_t, const char*) {}, err), "start again");
    sleepS(0.6);
    check(d->fatalError().find("unplugged") != std::string::npos, "unplug: " + d->fatalError());
    d->stop();
    unsetenv("WVT_BB_SIM_UNPLUG");
}

// A SigMF recording across two retunes: the capture segments must start where
// the samples of the new tuning start.
void testRecordRetune() {
    printf("recording across retunes\n");
    char tmpl[] = "/tmp/wvt_bb_check_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        check(false, "temp folder");
        return;
    }
    LiveEngine live([] {});
    StreamConfig c;
    c.driver = "bb";
    c.fc = 100e6;
    c.rate = 1.25e6;
    c.gain = -20;
    std::string err;
    if (!live.start(c, err)) {
        check(false, "live start: " + err);
        return;
    }
    RecordConfig rc;
    rc.exactPath = std::string(dir) + "/rec";
    check(live.startRecording(rc, err), "start recording" + (err.empty() ? "" : ": " + err));
    sleepS(0.4);
    check(live.setCenterFreq(100.1e6, err), "retune to 100.1 MHz");
    sleepS(0.4);
    check(live.setCenterFreq(99.95e6, err), "retune to 99.95 MHz");
    sleepS(0.4);
    live.stopRecording();
    live.stop();

    std::ifstream f(rc.exactPath + ".sigmf-meta");
    std::stringstream ss;
    ss << f.rdbuf();
    JsonValue meta;
    if (!parseJson(ss.str(), meta, err)) {
        check(false, "metadata: " + err);
        return;
    }
    const JsonValue* caps = meta.get("captures");
    const size_t n = caps ? caps->arr.size() : 0;
    check(n == 3, "three capture segments (" + std::to_string(n) + ")");
    if (n != 3) return;
    auto file = IqFile::open(rc.exactPath + ".sigmf-data", err);
    if (!file) {
        check(false, "data: " + err);
        return;
    }
    FormatParams fp;
    fp.fmt = SampleFormat::CI16;
    const int64_t total = file->numSamples(fp);
    std::vector<std::complex<float>> x(total);
    file->read(fp, 0, total, x.data());
    const double expect[3] = {kTone - 100e6, kTone - 100.1e6, kTone - 99.95e6};
    for (size_t i = 0; i < 3; ++i) {
        const int64_t start = (int64_t)caps->arr[i].number("core:sample_start");
        const double fc = caps->arr[i].number("core:frequency");
        const int N = 1024;  // 0.8 ms: the first samples after the boundary
        char b[160];
        if (start + N > total) {
            check(false, "segment " + std::to_string(i) + " is too short");
            continue;
        }
        const double off = peakOffset(x, (size_t)start, N, 1.25e6);
        snprintf(b, sizeof b, "segment %zu at sample %lld, %.2f MHz: the first %d samples have the tone at %+.1f kHz (%+.1f expected)",
                 i, (long long)start, fc / 1e6, N, off / 1e3, expect[i] / 1e3);
        check(std::fabs(off - expect[i]) < 2.5e3 && std::fabs(fc - (i == 0 ? 100e6 : i == 1 ? 100.1e6 : 99.95e6)) < 1, b);
        if (i > 0 && start >= N) {
            // ... and the samples just before the boundary still have the old tuning.
            const double before = peakOffset(x, (size_t)(start - N), N, 1.25e6);
            snprintf(b, sizeof b, "  the %d samples before it have the previous tone (%+.1f kHz)", N, before / 1e3);
            check(std::fabs(before - expect[i - 1]) < 2.5e3, b);
        }
    }
    const std::string cmd = std::string("rm -rf '") + dir + "'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", dir);
}
}  // namespace

int main() {
    testRules();
    testOpen();
    testStream();
    testRecordRetune();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
