// Time segment self-test (issue #16): segmentRange() rules for -1, clamping
// and bad values, and computeOverview() on a sample range (it must read only
// inside the range and report file time). Exit status is nonzero on failure.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/engine.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void testRange() {
    printf("segmentRange\n");
    const double fs = 1e6;
    const int64_t n = 100'000'000;  // 100 s
    auto r = segmentRange(-1, -1, fs, n);
    check(r.err.empty() && r.warn.empty() && r.s0 == 0 && r.s1 == n && r.whole(n), "-1 to -1 is the whole file");
    r = segmentRange(-1, 20, fs, n);
    check(r.err.empty() && r.s0 == 0 && r.s1 == 20'000'000, "-1 to 20: the first 20 s");
    r = segmentRange(80, -1, fs, n);
    check(r.err.empty() && r.s0 == 80'000'000 && r.s1 == n, "80 to -1: 80 s to the end");
    r = segmentRange(40, 50, fs, n);
    check(r.err.empty() && r.s0 == 40'000'000 && r.s1 == 50'000'000 && !r.whole(n), "40 to 50");
    r = segmentRange(-5, -0.5, fs, n);
    check(r.err.empty() && r.whole(n), "any negative value means -1");
    r = segmentRange(50, 40, fs, n);
    check(!r.err.empty() && r.whole(n), "end before start: error, whole file (" + r.err + ")");
    r = segmentRange(40, 40, fs, n);
    check(!r.err.empty(), "end equal to start: error");
    r = segmentRange(50, 40, fs, 0);
    check(!r.err.empty(), "end before start is an error with no file too");
    r = segmentRange(150, -1, fs, n);
    check(!r.err.empty() && r.whole(n), "start past the end of the file: error (" + r.err + ")");
    r = segmentRange(90, 200, fs, n);
    check(r.err.empty() && !r.warn.empty() && r.s0 == 90'000'000 && r.s1 == n, "end past the end: clamped with a warning");
    r = segmentRange(10, 10.0000001, fs, n);
    check(!r.err.empty(), "shorter than one sample: error");
    r = segmentRange(40, 50, fs, 0);
    check(r.err.empty() && r.s0 == 0 && r.s1 == 0, "no file: empty range, no error");
    // The values are seconds: a new rate selects new samples.
    r = segmentRange(40, 50, 2e6, 2 * n);
    check(r.s0 == 80'000'000 && r.s1 == 100'000'000, "seconds map through the sample rate");
}

// A 1 s cf32 file at 100 kS/s: amplitude 1 in the first half, 0.01 in the second.
void testOverview(const std::string& dir) {
    printf("overview range\n");
    const double fs = 100e3;
    const int64_t n = 100'000;
    std::vector<std::complex<float>> x(n);
    for (int64_t i = 0; i < n; ++i) {
        const float a = i < n / 2 ? 1.f : 0.01f;
        const double ph = 2 * M_PI * 1e3 * i / fs;
        x[i] = {a * (float)std::cos(ph), a * (float)std::sin(ph)};
    }
    std::vector<uint8_t> raw(n * 8);
    encodeSamples(SampleFormat::CF32, x.data(), n, raw.data());
    const std::string path = dir + "/two_level.cf32";
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(raw.data(), 1, raw.size(), f);
    fclose(f);
    std::string err;
    auto file = IqFile::open(path, err);
    check(file != nullptr, "test file opens " + err);
    if (!file) return;
    CancelFn never = [] { return false; };

    OverviewReq whole;
    whole.src = Source{file, FormatParams{}, fs, 0};
    whole.points = 100;
    auto w = computeOverview(whole, never);
    check(w && w->t.size() == 100 && w->t.front() < 0.01 && w->t.back() > 0.99, "whole file: 100 points over 0..1 s");

    OverviewReq seg = whole;
    seg.s0 = n / 2 + 1000;
    seg.s1 = n - 1000;
    auto s = computeOverview(seg, never);
    bool inside = s && !s->t.empty(), quiet = inside;
    if (s)
        for (size_t i = 0; i < s->t.size(); ++i) {
            if (s->t[i] < 0.51 || s->t[i] > 0.99) inside = false;
            if (s->peak[i] > -39) quiet = false;  // the loud first half is not read
        }
    check(s && s->t.size() == 100, "segment: 100 points");
    check(inside, "segment: times are file time, inside 0.51..0.99 s");
    check(quiet, "segment: reads only the quiet half (peak -40 dBFS)");

    OverviewReq past = whole;
    past.s0 = n + 10;
    auto p = computeOverview(past, never);
    check(p && p->t.empty(), "range past the end of the file: no points");
    unlink(path.c_str());
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/wvt_segment_check_XXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    testRange();
    testOverview(tmpl);
    rmdir(tmpl);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
