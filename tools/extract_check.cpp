// extract_check - region export self-test (issue #18): byte-exact copies,
// the band filter (level, band edges, rejection, rate), chunk joins, SigMF
// metadata and names, the checks, ci16 clipping and cancel.
//
//   extract_check [scratch_dir]     exit status 0 = all checks passed
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "extract.h"
#include "iq_file.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string fmt(const char* f, double a, double b = 0) {
    char buf[160];
    snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

struct Tone {
    double f, amp;
};

// Sum of complex tones at offsets f (Hz) from the center, sample rate fs.
std::vector<cf> tones(int64_t n, double fs, const std::vector<Tone>& ts) {
    std::vector<cf> x(n);
    for (int64_t i = 0; i < n; ++i) {
        std::complex<double> v = 0;
        for (const auto& t : ts) v += t.amp * std::polar(1.0, 2 * M_PI * t.f * (double)i / fs);
        x[i] = cf((float)v.real(), (float)v.imag());
    }
    return x;
}

bool writeFile(const std::string& path, const std::vector<cf>& x, SampleFormat f, int header = 0, bool bigEndian = false) {
    const int bps = formatBytesPerSample(f), half = bps / 2;
    std::vector<uint8_t> raw(x.size() * bps);
    encodeSamples(f, x.data(), (int64_t)x.size(), raw.data());
    if (bigEndian)
        for (size_t v = 0; v < x.size() * 2; ++v) std::reverse(&raw[v * half], &raw[v * half] + half);
    FILE* o = fopen(path.c_str(), "wb");
    if (!o) return false;
    for (int k = 0; k < header; ++k) fputc(0xa5, o);
    const bool ok = fwrite(raw.data(), 1, raw.size(), o) == raw.size();
    return fclose(o) == 0 && ok;
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

// Runs an export to the end. Returns the status (error set on a start failure).
ExtractJob::Status runJob(const ExtractConfig& c) {
    ExtractJob job;
    std::string err;
    if (!job.start(c, err)) {
        ExtractJob::Status st;
        st.error = "start: " + err;
        return st;
    }
    while (job.status().active) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return job.status();
}

// Reads an exported cf32 / ci16 file back as complex samples.
std::vector<cf> readBack(const std::string& path, SampleFormat f) {
    std::string err;
    auto file = IqFile::open(path, err);
    if (!file) return {};
    FormatParams fp;
    fp.fmt = f;
    std::vector<cf> y(file->numSamples(fp));
    file->read(fp, 0, (int64_t)y.size(), y.data());
    return y;
}

// Amplitude of the tone at f (Hz) in y at rate fs, Blackman-Harris weighted
// (about 90 dB of rejection for other tones far enough away).
double amplitudeAt(const std::vector<cf>& y, size_t from, size_t to, double f, double fs) {
    const auto w = makeWindow(WindowType::BlackmanHarris, (int)(to - from));
    std::complex<double> acc = 0;
    double ws = 0;
    for (size_t k = from; k < to; ++k) {
        const double wk = w[k - from];
        acc += wk * std::complex<double>(y[k].real(), y[k].imag()) * std::polar(1.0, -2 * M_PI * f * (double)k / fs);
        ws += wk;
    }
    return std::abs(acc) / ws;
}

ExtractConfig baseConfig(std::shared_ptr<IqFile> file, SampleFormat f, double fs, double fc, const std::string& out) {
    ExtractConfig c;
    c.src.file = file;
    c.src.fp.fmt = f;
    c.src.fs = fs;
    c.src.fc = fc;
    c.s0 = 0;
    c.s1 = file->numSamples(c.src.fp);
    c.path = out;
    return c;
}

void testCopy(const std::string& dir) {
    printf("full band: the bytes are copied\n");
    const double fs = 2e6, fc = 433.92e6;
    const auto x = tones(300'000, fs, {{123e3, 0.4}, {-401e3, 0.2}});
    for (SampleFormat f : {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CU8}) {
        const std::string src = dir + "/copy_src.bin", out = dir + "/copy_out";
        writeFile(src, x, f, 7);
        std::string err;
        auto file = IqFile::open(src, err);
        ExtractConfig c = baseConfig(file, f, fs, fc, out);
        c.src.fp.headerBytes = 7;
        c.s1 = file->numSamples(c.src.fp);
        c.s0 = 1000;
        c.s1 = 251'000;
        auto st = runJob(c);
        const int bps = formatBytesPerSample(f);
        const std::string got = readAll(out + ".sigmf-data"), all = readAll(src);
        const bool same = st.done && got == all.substr(7 + 1000 * bps, 250'000 * bps);
        check(same, std::string(formatName(f)) + ": the output is the source bytes of the range" + (st.error.empty() ? "" : " (" + st.error + ")"));
        const FileMetaGuess g = guessMeta(out + ".sigmf-meta");
        check(g.haveFmt && g.fmt == f && g.rate == fs && g.fc == fc, std::string(formatName(f)) + ": the metadata has the same format, rate and frequency");
        const std::string meta = readAll(out + ".sigmf-meta");
        check(meta.find("\"wvt:extract\"") != std::string::npos && meta.find("\"sample_start\": 1000") != std::string::npos &&
                  meta.find("core:datetime") == std::string::npos,
              "the metadata names the source range and has no made-up start time");
        file.reset();
        unlink(src.c_str());
        unlink((out + ".sigmf-data").c_str());
        unlink((out + ".sigmf-meta").c_str());
    }

    // Big-endian with swapped I/Q comes out little-endian in the standard order.
    const std::string src = dir + "/be_src.cs16", out = dir + "/be_out";
    writeFile(src, x, SampleFormat::CI16, 0, true);
    std::string err;
    auto file = IqFile::open(src, err);
    ExtractConfig c = baseConfig(file, SampleFormat::CI16, fs, fc, out);
    c.src.fp.bigEndian = true;
    c.src.fp.swapIQ = true;
    auto st = runJob(c);
    std::vector<cf> want(x.size());
    file->read(c.src.fp, 0, (int64_t)want.size(), want.data());
    const auto got = readBack(out + ".sigmf-data", SampleFormat::CI16);
    check(st.done && got == want, "big-endian, swapped I/Q: the same samples, little-endian in the standard order");
    file.reset();
    unlink(src.c_str());
    unlink((out + ".sigmf-data").c_str());
    unlink((out + ".sigmf-meta").c_str());
}

void testBand(const std::string& dir) {
    printf("band: mix, filter, decimate\n");
    const double fs = 1e6, fc = 100e6;
    // Band 150..250 kHz above fc: tones at the low edge, inside and at the high
    // edge, and one outside that folds to -25 kHz after decimation by 8.
    const std::vector<Tone> ts = {{150e3, 0.1}, {230e3, 0.3}, {250e3, 0.1}, {300e3, 0.3}};
    const auto x = tones(2'000'000, fs, ts);
    const std::string src = dir + "/band_src.cf32", out = dir + "/band_out";
    writeFile(src, x, SampleFormat::CF32);
    std::string err;
    auto file = IqFile::open(src, err);
    ExtractConfig c = baseConfig(file, SampleFormat::CF32, fs, fc, out);
    c.fullBand = false;
    c.fLo = fc + 150e3;
    c.fHi = fc + 250e3;
    const ExtractPlan p = planExtract(c);
    check(p.err.empty() && p.decim == 8 && p.rate == 125e3 && p.center == fc + 200e3 && p.samples == 250'000,
          "plan: decimation 8, 125 kS/s at fc + 200 kHz, 250000 samples (taps " + std::to_string(p.taps) + ")");
    auto st = runJob(c);
    check(st.done, "the band export runs" + (st.error.empty() ? "" : " (" + st.error + ")"));
    const auto y = readBack(out + ".sigmf-data", SampleFormat::CF32);
    check(y.size() == 250'000, "the output has 250000 samples");
    if (y.size() == 250'000) {
        const size_t a = 2000, b = y.size() - 2000;  // away from the zero-padded ends
        const double r = p.rate;
        const double lo = 20 * std::log10(amplitudeAt(y, a, b, -50e3, r) / 0.1);
        const double mid = 20 * std::log10(amplitudeAt(y, a, b, 30e3, r) / 0.3);
        const double hi = 20 * std::log10(amplitudeAt(y, a, b, 50e3, r) / 0.1);
        const double out300 = 20 * std::log10(amplitudeAt(y, a, b, -25e3, r) / 0.3);
        check(std::fabs(mid) < 0.1, fmt("inside the band: %+.3f dB", mid));
        check(std::fabs(lo) < 0.1 && std::fabs(hi) < 0.1, fmt("band edges: low %+.3f dB, high %+.3f dB", lo, hi));
        check(out300 < -70, fmt("tone outside the band: %.1f dB", out300));
    }
    const FileMetaGuess g = guessMeta(out + ".sigmf-meta");
    check(g.haveFmt && g.fmt == SampleFormat::CF32 && g.rate == 125e3 && g.fc == fc + 200e3,
          "metadata: cf32, 125 kS/s, fc + 200 kHz");

    // Chunk joins: the export (chunks of 4 M input samples) against one ChanJob.
    {
        const auto x2 = tones(9'000'000, fs, {{230e3, 0.3}, {-100e3, 0.2}});
        const std::string src2 = dir + "/long_src.cf32", out2 = dir + "/long_out";
        writeFile(src2, x2, SampleFormat::CF32);
        auto f2 = IqFile::open(src2, err);
        ExtractConfig c2 = baseConfig(f2, SampleFormat::CF32, fs, fc, out2);
        c2.fullBand = false;
        c2.fLo = fc + 150e3;
        c2.fHi = fc + 250e3;
        c2.s0 = 12345;
        const ExtractPlan p2 = planExtract(c2);
        auto st2 = runJob(c2);
        const auto y2 = readBack(out2 + ".sigmf-data", SampleFormat::CF32);
        const auto h = designLowpass(p2.taps, p2.cutoff);
        ChanJob j;
        j.file = f2.get();
        j.fp.fmt = SampleFormat::CF32;
        j.inStart = c2.s0 - (int)h.size() / 2;
        j.outLen = p2.samples;
        j.decim = p2.decim;
        j.frac = p2.offset / fs;
        j.h = h.data();
        j.taps = (int)h.size();
        std::vector<cf> ref(j.outLen);
        channelizeCpu(j, ref.data(), [] { return false; });
        double diff = 0, peak = 0;
        bool sameSize = y2.size() == ref.size();
        for (size_t k = 0; sameSize && k < ref.size(); ++k) {
            diff = std::max(diff, (double)std::abs(y2[k] - ref[k]));
            peak = std::max(peak, (double)std::abs(ref[k]));
        }
        check(st2.done && sameSize && diff <= 1e-5 * peak,
              fmt("%.0f outputs over 3 chunks match one pass (max error %.2g of the peak)", (double)ref.size(), peak > 0 ? diff / peak : 0));
        f2.reset();
        unlink(src2.c_str());
        unlink((out2 + ".sigmf-data").c_str());
        unlink((out2 + ".sigmf-meta").c_str());
    }

    // ci16: scaled by 32768, and loud values are clipped and counted.
    {
        ExtractConfig c3 = c;
        c3.ci16 = true;
        c3.path = dir + "/band_ci16";
        auto st3 = runJob(c3);
        const auto y3 = readBack(c3.path + ".sigmf-data", SampleFormat::CI16);
        const double mid = y3.size() > 4000 ? 20 * std::log10(amplitudeAt(y3, 2000, y3.size() - 2000, 30e3, 125e3) / 0.3) : 99;
        check(st3.done && st3.clipped == 0 && std::fabs(mid) < 0.1, fmt("ci16: inside the band %+.3f dB, nothing clipped", mid));
        const auto loud = tones(400'000, fs, {{200e3, 1.5}});
        const std::string src4 = dir + "/loud.cf32";
        writeFile(src4, loud, SampleFormat::CF32);
        auto f4 = IqFile::open(src4, err);
        ExtractConfig c4 = baseConfig(f4, SampleFormat::CF32, fs, fc, dir + "/loud_out");
        c4.fullBand = false;
        c4.fLo = fc + 150e3;
        c4.fHi = fc + 250e3;
        c4.ci16 = true;
        auto st4 = runJob(c4);
        check(st4.done && st4.clipped > 0, "ci16: a tone above full scale is clipped and counted (" + std::to_string(st4.clipped) + ")");
        f4.reset();
        unlink(src4.c_str());
        for (const std::string& b : {c3.path, dir + "/loud_out"}) {
            unlink((b + ".sigmf-data").c_str());
            unlink((b + ".sigmf-meta").c_str());
        }
    }
    file.reset();
    unlink(src.c_str());
    unlink((out + ".sigmf-data").c_str());
    unlink((out + ".sigmf-meta").c_str());
}

void testChecks(const std::string& dir) {
    printf("checks\n");
    const double fs = 1e6, fc = 100e6;
    const std::string src = dir + "/chk_src.cf32";
    writeFile(src, tones(100'000, fs, {{1e3, 0.5}}), SampleFormat::CF32);
    std::string err;
    auto file = IqFile::open(src, err);
    const ExtractConfig base = baseConfig(file, SampleFormat::CF32, fs, fc, dir + "/chk_out");
    auto planErr = [&](auto change) {
        ExtractConfig c = base;
        change(c);
        return planExtract(c).err;
    };
    auto expectErr = [&](const char* what, auto change) {
        const std::string e = planErr(change);
        check(!e.empty(), std::string(what) + ": " + (e.empty() ? "no error" : e));
    };
    check(planErr([](ExtractConfig&) {}).empty(), "a whole-file copy is fine");
    expectErr("end before start", [](ExtractConfig& c) { c.s1 = c.s0; });
    expectErr("past the end of the file", [](ExtractConfig& c) { c.s1 += 1; });
    expectErr("outside the loaded segment", [](ExtractConfig& c) { c.limit0 = 1000; });
    expectErr("band outside the capture", [&](ExtractConfig& c) { c.fullBand = false; c.fLo = fc + 400e3; c.fHi = fc + 600e3; });
    expectErr("band wider than 80 %", [&](ExtractConfig& c) { c.fullBand = false; c.fLo = fc - 450e3; c.fHi = fc + 450e3; });
    expectErr("band narrower than fs / 100 000", [&](ExtractConfig& c) { c.fullBand = false; c.fLo = fc; c.fHi = fc + 5; });
    expectErr("no output name", [](ExtractConfig& c) { c.path = ""; });
    expectErr("output = the source file", [&](ExtractConfig& c) { c.sigmf = false; c.path = src; });

    // An existing output is kept unless overwrite is on.
    ExtractConfig c = base;
    auto st = runJob(c);
    check(st.done, "first export");
    check(planExtract(c).exists, "the plan sees the existing output");
    auto st2 = runJob(c);
    check(!st2.error.empty(), "a second export without overwrite fails: " + st2.error);
    c.overwrite = true;
    auto st3 = runJob(c);
    check(st3.done, "with overwrite it replaces the files");
    unlink((dir + "/chk_out.sigmf-data").c_str());
    unlink((dir + "/chk_out.sigmf-meta").c_str());

    // Raw output with the default name opens again with the right settings.
    ExtractConfig r = base;
    r.fullBand = false;
    r.fLo = fc - 100e3;
    r.fHi = fc + 100e3;
    r.sigmf = false;
    const ExtractPlan p = planExtract(r);
    r.path = defaultExtractPath(dir + "/wvt_20261020_120000_100.000MHz_1MSps.cf32", 0.25, p.center, p.rate);
    auto st4 = runJob(r);
    const FileMetaGuess g = guessMeta(st4.dataPath);
    check(st4.done && g.haveFmt && g.fmt == SampleFormat::CF32 && g.haveRate && g.rate == p.rate && g.haveFc && g.fc == p.center,
          "raw output, default name: " + st4.dataPath.substr(st4.dataPath.find_last_of('/') + 1) + " opens as cf32 at " +
              fmt("%.6g S/s, %.9g Hz", g.rate, g.fc));
    unlink(st4.dataPath.c_str());

    // The start time: source core:datetime + t0.
    {
        const std::string sbase = dir + "/timed";
        writeFile(sbase + ".sigmf-data", tones(100'000, fs, {{1e3, 0.5}}), SampleFormat::CF32);
        FILE* m = fopen((sbase + ".sigmf-meta").c_str(), "w");
        fprintf(m, "{\"global\": {\"core:datatype\": \"cf32_le\", \"core:sample_rate\": 1000000},"
                   " \"captures\": [{\"core:sample_start\": 0, \"core:frequency\": 100000000,"
                   " \"core:datetime\": \"2026-10-20T12:00:00.000Z\"}], \"annotations\": []}\n");
        fclose(m);
        const FileMetaGuess sg = guessMeta(sbase + ".sigmf-meta");
        auto tf = IqFile::open(sg.dataPath, err);
        ExtractConfig t = baseConfig(tf, SampleFormat::CF32, fs, fc, dir + "/timed_out");
        t.sourceStart = sg.startTime;
        t.s0 = 25'000;  // 25 ms
        auto st5 = runJob(t);
        const std::string meta = readAll(dir + "/timed_out.sigmf-meta");
        check(st5.done && meta.find("\"core:datetime\": \"2026-10-20T12:00:00.025Z\"") != std::string::npos,
              "core:datetime is the source start + 25 ms");
        tf.reset();
        for (const std::string& b : {sbase, dir + "/timed_out"}) {
            unlink((b + ".sigmf-data").c_str());
            unlink((b + ".sigmf-meta").c_str());
        }
    }
    file.reset();
    unlink(src.c_str());
}

void testCancel(const std::string& dir) {
    printf("cancel\n");
    const double fs = 1e6;
    const std::string src = dir + "/cancel_src.cf32";
    writeFile(src, tones(8'000'000, fs, {{1e3, 0.5}}), SampleFormat::CF32);
    std::string err;
    auto file = IqFile::open(src, err);
    ExtractConfig c = baseConfig(file, SampleFormat::CF32, fs, 0, dir + "/cancel_out");
    c.fullBand = false;
    c.fLo = -2e3;  // narrow band: many taps, so the job is still running when cancelled
    c.fHi = 2e3;
    ExtractJob job;
    const bool started = job.start(c, err);
    // Cancel once the job has written part of the output (a fixed wait is too
    // long on a fast computer: the job can finish first).
    for (int i = 0; i < 2000 && started; ++i) {
        const auto s = job.status();
        if (s.progress > 0 || s.done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    job.cancel();
    const auto st = job.status();
    check(started && st.cancelled && !st.done && !exists(dir + "/cancel_out.sigmf-data") && !exists(dir + "/cancel_out.sigmf-meta"),
          "cancel stops the job and deletes the partial files");
    file.reset();
    unlink(src.c_str());
}
}  // namespace

int main(int argc, char** argv) {
    std::string dir;
    std::string tmp;
    if (argc > 1) {
        dir = argv[1];
    } else {
        char tmpl[] = "/tmp/wvt_extract_check_XXXXXX";
        if (!mkdtemp(tmpl)) {
            perror("mkdtemp");
            return 1;
        }
        dir = tmp = tmpl;
    }
    testCopy(dir);
    testBand(dir);
    testChecks(dir);
    testCancel(dir);
    if (!tmp.empty()) rmdir(tmp.c_str());
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
