// Analysis self-test (issue #111): each kind of the Analysis window on
// signals with known values, most of them from the signal generator. Exit
// status is nonzero on failure.
#include <unistd.h>

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "dsp/analysis.h"
#include "io/iq_file.h"
#include "siggen/siggen.h"

namespace {
int failures = 0;
std::string g_dir;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
bool near(double a, double b, double tol) { return std::isfinite(a) && std::abs(a - b) <= tol; }
std::string fmt(const char* f, double a, double b = 0) {
    char s[200];
    snprintf(s, sizeof s, f, a, b);
    return s;
}

using cvec = std::vector<std::complex<float>>;

// Writes x as a cf32 file and returns a source for it.
Source writeFile(const std::string& name, const cvec& x, double fs, double fc) {
    const std::string path = g_dir + "/" + name + ".cf32";
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(x.data(), sizeof(x[0]), x.size(), f);
    fclose(f);
    std::string err;
    Source s;
    s.file = IqFile::open(path, err);
    if (!s.file) {
        printf("cannot open %s: %s\n", path.c_str(), err.c_str());
        exit(1);
    }
    s.fp.fmt = SampleFormat::CF32;
    s.fs = fs;
    s.fc = fc;
    return s;
}

cvec generate(const WaveformParams& p, int64_t n) {
    cvec x((size_t)n);
    SigGen g(p);
    g.generate(x.data(), n);
    return x;
}

AnaReq request(AnaKind k, const Source& s, int64_t n) {
    AnaReq r;
    r.kind = k;
    r.reg.src = s;
    r.reg.s0 = 0;
    r.reg.s1 = n;
    return r;
}

std::shared_ptr<AnaRes> run(const AnaReq& r) {
    auto res = computeAnalysis(r, [] { return false; });
    if (!res) {
        printf("  analysis returned nothing\n");
        exit(1);
    }
    if (!res->err.empty()) printf("  (error: %s)\n", res->err.c_str());
    return res;
}

void testMeasure() {
    printf("measurements\n");
    const double fs = 1e6, fc = 100e6;
    WaveformParams p;
    p.mode = GenMode::CW;
    p.rate = fs;
    p.fc = fc;
    p.offset = 100e3;
    p.ampDb = -6;
    p.dcI = 0.05;
    p.iqGainDb = 0.5;
    p.iqPhaseDeg = 3;
    p.noise = true;
    p.snrDb = 40;
    const int64_t n = 1 << 20;
    const Source s = writeFile("cw", generate(p, n), fs, fc);
    auto r = run(request(AnaKind::Measure, s, n));
    const MeasureRes& m = r->measure;
    check(near(m.meanDb, -6.0, 0.3), fmt("mean power %.2f dBFS (about -6)", m.meanDb));
    check(near(m.carrierHz, fc + 100e3, 5), fmt("carrier %.1f Hz from 100.1 MHz", m.carrierHz - (fc + 100e3)));
    check(near(m.dcI, 0.05, 0.002) && near(m.dcQ, 0, 0.002), fmt("DC offset I %.4f, Q %.4f", m.dcI, m.dcQ));
    check(near(m.gainImbDb, -0.5, 0.05), fmt("I/Q gain imbalance %.3f dB (I over Q: -0.5)", m.gainImbDb));
    check(near(m.phaseImbDeg, 3, 0.2), fmt("I/Q phase imbalance %.3f degrees (3)", m.phaseImbDeg));
    check(m.paprDb < 3, fmt("PAPR of a tone %.2f dB (small)", m.paprDb));
    check(m.snrDb > 30, fmt("SNR in the occupied bandwidth %.1f dB", m.snrDb));

    // a flat band of 201 tones, 1 kHz apart: the occupied bandwidth is about 200 kHz
    cvec x((size_t)n);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> ph(0, 2 * M_PI);
    std::vector<double> phases(201);
    for (auto& v : phases) v = ph(rng);
    for (int64_t k = 0; k < n; ++k) {
        std::complex<double> v = 0;
        for (int t = 0; t < 201; ++t) v += std::polar(0.02, 2 * M_PI * (t - 100) * 1e3 * k / fs + phases[t]);
        x[k] = std::complex<float>(v);
    }
    const Source sb = writeFile("band", x, fs, fc);
    auto rb = run(request(AnaKind::Measure, sb, n));
    check(near(rb->measure.obwHz, 200e3, 6e3), fmt("99 %% occupied bandwidth %.0f Hz (about 200 kHz)", rb->measure.obwHz));
    check(near(rb->measure.centroidHz, fc, 500), fmt("centroid %.0f Hz from the center", rb->measure.centroidHz - fc));
    check(rb->measure.paprDb > 5, fmt("PAPR of 201 tones %.1f dB (large)", rb->measure.paprDb));
}

void testSpectrum() {
    printf("spectrum\n");
    const double fs = 1e6, fc = 0;
    const int64_t n = 1 << 21;
    cvec x((size_t)n);
    for (int64_t k = 0; k < n; ++k) {
        const double t = (double)k / fs;
        x[k] = std::complex<float>(std::polar(std::pow(10.0, -10 / 20.0), 2 * M_PI * 50e3 * t) +
                                   std::polar(std::pow(10.0, -40 / 20.0), 2 * M_PI * -200e3 * t));
    }
    const Source s = writeFile("two_tones", x, fs, fc);
    AnaReq q = request(AnaKind::Spectrum, s, n);
    q.spectrum.nfft = 65536;
    q.spectrum.band = true;
    q.spectrum.fLo = 40e3;
    q.spectrum.fHi = 60e3;
    auto r = run(q);
    const SpectrumRes& sp = r->spectrum;
    auto peakNear = [&](double f) {
        double best = -300;
        for (size_t i = 0; i < sp.f.size(); ++i)
            if (std::abs(sp.f[i] - f) < 50) best = std::max(best, sp.avgDb[i]);
        return best;
    };
    check(near(peakNear(50e3), -10, 1.0), fmt("tone at 50 kHz reads %.2f dBFS (-10)", peakNear(50e3)));
    check(near(peakNear(-200e3), -40, 1.0), fmt("tone at -200 kHz reads %.2f dBFS (-40)", peakNear(-200e3)));
    check(near(sp.bandDb, -10, 0.1), fmt("channel power 40 to 60 kHz %.3f dBFS (-10)", sp.bandDb));
    check(sp.acprLoDb < -60 && sp.acprHiDb < -60, fmt("ACPR %.1f / %.1f dBc (none)", sp.acprLoDb, sp.acprHiDb));
    check(near(sp.totalDb, 10 * std::log10(0.1 + 1e-4), 0.1), fmt("total power %.3f dBFS", sp.totalDb));
    check(sp.averages == (n - 65536) / 32768 + 1, fmt("averages %.0f", (double)sp.averages));
    check(near(sp.rbwHz, 1.9 * fs / 65536, 0.2 * fs / 65536), fmt("RBW %.2f Hz", sp.rbwHz));
}

void testHistogram() {
    printf("histograms and CCDF\n");
    const double fs = 1e6;
    const int64_t n = 1 << 20;
    cvec x((size_t)n);
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0, (float)std::sqrt(0.5));
    for (auto& v : x) v = {g(rng), g(rng)};
    const Source s = writeFile("noise", x, fs, 0);
    auto r = run(request(AnaKind::Histogram, s, n));
    const HistogramRes& h = r->hist;
    check(near(h.papr1e2, 10 * std::log10(std::log(100.0)), 0.2), fmt("Gaussian CCDF at 1 %%: %.2f dB (6.63)", h.papr1e2));
    check(near(h.papr1e3, 10 * std::log10(std::log(1000.0)), 0.25), fmt("at 0.1 %%: %.2f dB (8.39)", h.papr1e3));
    check(near(h.h[(int)HistQty::I].stdev, std::sqrt(0.5), 0.01), fmt("I standard deviation %.4f (0.707)", h.h[(int)HistQty::I].stdev));
    check(near(h.h[(int)HistQty::I].mean, 0, 0.005), "I mean is 0");
    double total = 0;
    for (double c : h.h[(int)HistQty::PM].counts) total += c;
    check(total == (double)n, "every sample is in the PM histogram");
}

// QPSK, rectangular symbols at 125 kBd, 8 samples a symbol, on a carrier offset of 7 kHz.
cvec qpsk(int64_t n, double fs, double off, double baud, std::vector<int>* sym = nullptr, uint32_t seed = 5) {
    std::mt19937 rng(seed);
    cvec x((size_t)n);
    const int64_t sps = (int64_t)std::llround(fs / baud);
    int q = 0;
    for (int64_t k = 0; k < n; ++k) {
        if (k % sps == 0) {
            q = (int)(rng() % 4);
            if (sym) sym->push_back(q);
        }
        const double a = M_PI / 4 + q * M_PI / 2;
        x[k] = std::complex<float>(std::polar(0.5, a + 2 * M_PI * off * k / fs));
    }
    return x;
}

void testConstellation() {
    printf("constellation\n");
    const double fs = 1e6, baud = 125e3;
    const int64_t n = 1 << 18;
    const Source s = writeFile("qpsk", qpsk(n, fs, 7e3, baud), fs, 0);
    AnaReq q = request(AnaKind::Constellation, s, n);
    q.cons.carrier = (int)CarrierFix::PowerN;
    q.cons.power = 4;
    q.cons.symbolRate = baud;
    q.cons.symbolT0 = 0.5 / baud;
    auto r = run(q);
    const ConstellationRes& c = r->cons;
    check(near(c.removedHz, 7e3, 5), fmt("removed offset %.2f Hz (7000)", c.removedHz));
    double err = 0;
    for (size_t i = 0; i < c.xs.size(); ++i) {
        const double ix = c.xs[i] >= 0 ? M_SQRT1_2 : -M_SQRT1_2, iy = c.ys[i] >= 0 ? M_SQRT1_2 : -M_SQRT1_2;
        err += std::hypot(c.xs[i] - ix, c.ys[i] - iy);
    }
    err /= std::max<size_t>(1, c.xs.size());
    check(err < 0.05, fmt("the points are on the 4 QPSK points (mean error %.4f)", err));
    check(near(c.rmsMag, 1, 0.01), fmt("normalized RMS %.4f", c.rmsMag));
    double dens = 0;
    for (float v : c.density) dens += v;
    check(dens > 0.99 * (double)r->used, "the density has all the points");
}

void testPulses() {
    printf("pulses\n");
    const double fs = 10e6;
    WaveformParams p;
    p.mode = GenMode::Pulsed;
    p.rate = fs;
    p.fc = 1e9;
    p.offset = 1e6;
    p.ampDb = -6;
    p.pri = 100e-6;
    p.pw = 10e-6;
    p.startDelay = 20e-6;
    p.noise = true;
    p.snrDb = 40;
    p.intra = IntraMod::LfmUp;
    p.chirpBw = 2e6;
    const int64_t n = (int64_t)(fs * 0.01);  // 10 ms: 100 pulses
    const Source s = writeFile("pulses", generate(p, n), fs, p.fc);
    auto r = run(request(AnaKind::Pulses, s, n));
    const PulseRes& pr = r->pulse;
    check(pr.pulses.size() == 100, fmt("%.0f pulses (100)", (double)pr.pulses.size()));
    if (!pr.pulses.empty()) {
        const Pulse& u = pr.pulses[1];
        check(near(u.toa, 120e-6, 0.2e-6), fmt("TOA of the second pulse %.3f us (120)", u.toa * 1e6));
        check(near(u.width, 10e-6, 0.2e-6), fmt("width %.3f us (10)", u.width * 1e6));
        check(near(u.pri, 100e-6, 0.05e-6), fmt("PRI %.4f us (100)", u.pri * 1e6));
        check(near(u.freqHz, p.fc + 1e6, 30e3), fmt("frequency %.0f Hz from fc + 1 MHz", u.freqHz - p.fc - 1e6));
        check(near(u.chirpHzPerS, 2e6 / 10e-6, 0.1 * 2e6 / 10e-6), fmt("chirp %.3g Hz/s (2e11)", u.chirpHzPerS));
        check(near(u.peakDb, -6, 0.5), fmt("peak %.2f dBFS (-6)", u.peakDb));
        check(u.rise < 0.3e-6, fmt("rise time of a rectangular pulse %.3f us", u.rise * 1e6));
    }
    check(pr.priGroups.size() == 1, fmt("PRI groups %.0f (1: fixed)", (double)pr.priGroups.size()));
    check(near(pr.dutyCycle, 0.1, 0.005), fmt("duty cycle %.4f (0.1)", pr.dutyCycle));
    check((int)pr.overlayDb.size() == std::min<int>(200, (int)pr.pulses.size()) && !pr.overlayT.empty(), "the overlay has the pulses");

    // stagger: 100, 130 us
    p.intra = IntraMod::None;
    p.priPattern = PriPattern::Stagger;
    p.stagger = {100e-6, 130e-6};
    const Source s2 = writeFile("stagger", generate(p, n), fs, p.fc);
    auto r2 = run(request(AnaKind::Pulses, s2, n));
    const auto& g = r2->pulse.priGroups;
    check(g.size() == 2 && near(g[0], 100e-6, 0.1e-6) && near(g[1], 130e-6, 0.1e-6),
          fmt("stagger: %.0f PRI groups (100 and 130 us)", (double)g.size()));

    // the same pulses through the channel filter at the pulse frequency
    AnaReq q = request(AnaKind::Pulses, s, n);
    q.reg.chanEnable = true;
    q.reg.chanOffset = 1e6;
    q.reg.chanBw = 3e6;
    auto r3 = run(q);
    check(r3->pulse.pulses.size() == 100, fmt("through the channel filter: %.0f pulses", (double)r3->pulse.pulses.size()));
    if (r3->pulse.pulses.size() > 1)
        check(near(r3->pulse.pulses[1].freqHz, p.fc + 1e6, 30e3), "through the channel filter: the absolute frequency");
}

void testCorrelation() {
    printf("correlation\n");
    const double fs = 1e6;
    WaveformParams p;
    p.mode = GenMode::Pulsed;
    p.rate = fs;
    p.offset = 0;
    p.pri = 1e-3;
    p.pw = 130e-6;  // Barker 13 at 10 us a chip
    p.intra = IntraMod::Barker;
    p.barkerLen = 13;
    p.startDelay = 100e-6;
    p.noise = true;
    p.snrDb = 20;
    const int64_t n = (int64_t)(fs * 0.02);  // 20 pulses
    const Source s = writeFile("barker", generate(p, n), fs, 0);
    // autocorrelation: the PRI
    AnaReq a = request(AnaKind::Correlation, s, n);
    auto ra = run(a);
    bool foundPri = false;
    for (const CorrPeak& k : ra->corr.peaks) foundPri |= near(k.x, 1e-3, 2e-6);
    check(foundPri, "autocorrelation has a peak at the PRI (1 ms)");
    // search: the first pulse as the template
    AnaReq q = request(AnaKind::Correlation, s, n);
    q.corr.search = true;
    q.corr.tmplS0 = 100;
    q.corr.tmplS1 = 100 + 130;
    q.corr.searchS0 = 0;
    q.corr.searchS1 = n;
    q.corr.threshold = 0.7;
    auto rq = run(q);
    const auto& h = rq->corr.peaks;
    check(h.size() == 20, fmt("search finds %.0f pulses (20)", (double)h.size()));
    bool times = true;
    for (size_t i = 0; i < h.size(); ++i) times &= near(h[i].x, 100e-6 + (double)i * 1e-3, 1.5e-6);
    check(times, "each hit is at the pulse start");
    check(!h.empty() && h[0].v > 0.95, fmt("the template's own hit scores %.3f", h.empty() ? 0 : h[0].v));
    // a template that is too long
    q.corr.tmplS1 = q.corr.tmplS0 + 200000;
    auto rl = run(q);
    check(!rl->corr.err.empty(), "a template over 131072 samples is refused");
}

void testSymbols() {
    printf("symbols\n");
    const double fs = 1e6, baud = 125e3;
    const int64_t n = 1 << 18;
    // 2-FSK, +-50 kHz, continuous phase, 8 samples a symbol
    cvec x((size_t)n);
    std::mt19937 rng(11);
    double ph = 0;
    int b = 0;
    for (int64_t k = 0; k < n; ++k) {
        if (k % 8 == 0) b = (int)(rng() & 1);
        ph += 2 * M_PI * (b ? 50e3 : -50e3) / fs;
        x[k] = std::complex<float>(std::polar(0.5, ph));
    }
    const Source s = writeFile("fsk", x, fs, 0);
    AnaReq q = request(AnaKind::Symbols, s, n);
    q.sym.method = (int)SymMethod::Transitions;
    q.sym.trace = (int)SymTrace::FM;
    auto r = run(q);
    const SymbolRes& sy = r->sym;
    check(near(sy.estRate, baud, 50), fmt("2-FSK symbol rate %.1f Bd (125000)", sy.estRate));
    double ph0 = std::fmod(sy.estT0 * baud, 1.0);
    if (ph0 < 0) ph0 += 1;
    // the symbols are samples 8 k to 8 k + 7: the centers are at 8 k + 3.5
    check(std::abs(ph0 - 3.5 / 8) < 0.04, fmt("symbol centers at %.3f of a symbol (0.4375)", ph0));
    double ok = 0;
    for (double v : sy.symV) ok += std::abs(std::abs(v) - 50e3) < 5e3;
    check(ok > 0.98 * (double)sy.symV.size(), fmt("FM at the symbol centers is +-50 kHz (%.1f %%)", 100 * ok / std::max<size_t>(1, sy.symV.size())));
    double eye = 0;
    for (float v : sy.eye) eye += v;
    check(sy.eyeW > 0 && eye > 0, "the eye has data");

    // QPSK on a 7 kHz offset: x^4 shows 4 x 7 kHz
    const Source sq = writeFile("qpsk2", qpsk(n, fs, 7e3, baud), fs, 0);
    AnaReq q2 = request(AnaKind::Symbols, sq, n);
    q2.sym.power = 4;
    auto r2 = run(q2);
    check(near(r2->sym.powCarrierHz, 7e3, 10), fmt("x^4 carrier %.1f Hz (7000)", r2->sym.powCarrierHz));
    check(r2->sym.powPeakDb > 20, fmt("the x^4 line is %.1f dB over the median", r2->sym.powPeakDb));
    check(near(r2->sym.estRate, baud, 50), fmt("QPSK symbol rate %.1f Bd", r2->sym.estRate));
}

void testCyclo() {
    printf("cyclostationary\n");
    const double fs = 1e6, baud = 125e3;
    const int64_t n = 1 << 17;
    // BPSK with rectangular symbols has cyclic features at multiples of the symbol rate
    std::mt19937 rng(9);
    cvec x((size_t)n);
    int b = 0;
    std::normal_distribution<float> g(0, 0.05f);
    for (int64_t k = 0; k < n; ++k) {
        if (k % 8 == 0) b = (int)(rng() & 1);
        x[k] = {(b ? 0.5f : -0.5f) + g(rng), g(rng)};
    }
    const Source s = writeFile("bpsk", x, fs, 0);
    AnaReq q = request(AnaKind::Cyclo, s, n);
    q.cyclo.np = 64;
    q.cyclo.maxP = 512;
    auto r = run(q);
    const CycloRes& c = r->cyclo;
    check(c.na > 0 && c.nf > 0 && c.db.size() == (size_t)c.na * c.nf, "the SCD image has data");
    bool found = false;
    for (size_t i = 0; i < std::min<size_t>(3, c.peaks.size()); ++i) found |= near(c.peaks[i].x, baud, 2 * fs / (c.l * c.p));
    check(found, fmt("a strong cyclic feature at the symbol rate (strongest at %.0f Hz)", c.peaks.empty() ? 0 : c.peaks[0].x));
    AnaReq qs = request(AnaKind::Cyclo, s, 100);
    auto rs = run(qs);
    check(!rs->err.empty(), "a region that is too short is refused");
}

void testDemod() {
    printf("digital demodulation\n");
    const double fs = 1e6, baud = 125e3;
    const int64_t n = 1 << 17;
    // 2-FSK with known bits
    std::mt19937 rng(21);
    std::vector<int> bits;
    cvec x((size_t)n);
    double ph = 0;
    int b = 0;
    for (int64_t k = 0; k < n; ++k) {
        if (k % 8 == 0) {
            b = (int)(rng() & 1);
            bits.push_back(b);
        }
        ph += 2 * M_PI * (b ? 40e3 : -40e3) / fs;
        x[k] = std::complex<float>(std::polar(0.5, ph));
    }
    const Source s = writeFile("fsk_bits", x, fs, 0);
    AnaReq q = request(AnaKind::Demod, s, n);
    q.digital.mode = (int)DemodMode::FSK;
    auto r = run(q);
    const DigitalRes& d = r->digital;
    check(near(d.rate, baud, 50), fmt("2-FSK rate %.1f Bd", d.rate));
    // align: the best match of the decoded bits in the sent bits (offset 0 to 3)
    auto match = [](const std::vector<uint8_t>& got, const std::vector<int>& sent, int bps) {
        double best = 0;
        for (int off = -4 * bps; off <= 4 * bps; ++off) {
            int64_t same = 0, cnt = 0;
            for (size_t i = 0; i < got.size(); ++i) {
                const int64_t j = (int64_t)i + off;
                if (j < 0 || j >= (int64_t)sent.size()) continue;
                same += got[i] == sent[(size_t)j];
                ++cnt;
            }
            if (cnt > 100) best = std::max(best, (double)same / (double)cnt);
        }
        return best;
    };
    const double m = match(d.bits, bits, 1);
    check(m > 0.999, fmt("2-FSK bits match %.4f", m));

    // QPSK on a carrier offset: the differential bits do not depend on the phase ambiguity
    std::vector<int> syms;
    const Source sq = writeFile("qpsk_bits", qpsk(n, fs, 3e3, baud, &syms, 33), fs, 0);
    AnaReq q2 = request(AnaKind::Demod, sq, n);
    q2.digital.mode = (int)DemodMode::QPSK;
    q2.digital.differential = true;
    auto r2 = run(q2);
    std::vector<int> dbits;
    static const int gray[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    for (size_t k = 1; k < syms.size(); ++k) {
        const int dq = (syms[k] - syms[k - 1] + 4) % 4;
        dbits.push_back(gray[dq][0]);
        dbits.push_back(gray[dq][1]);
    }
    const double m2 = match(r2->digital.bits, dbits, 2);
    check(near(r2->digital.carrierHz, 3e3, 10), fmt("QPSK carrier %.1f Hz (3000)", r2->digital.carrierHz));
    check(m2 > 0.999, fmt("differential QPSK bits match %.4f", m2));

    // OOK
    std::vector<int> ob;
    cvec xo((size_t)n);
    std::normal_distribution<float> g(0, 0.01f);
    for (int64_t k = 0; k < n; ++k) {
        if (k % 10 == 0) ob.push_back((int)(rng() & 1));
        xo[k] = {(ob.back() ? 0.5f : 0.f) + g(rng), g(rng)};
    }
    const Source so = writeFile("ook", xo, fs, 0);
    AnaReq q3 = request(AnaKind::Demod, so, n);
    q3.digital.mode = (int)DemodMode::OOK;
    auto r3 = run(q3);
    check(near(r3->digital.rate, 100e3, 50), fmt("OOK rate %.1f Bd (100000)", r3->digital.rate));
    check(match(r3->digital.bits, ob, 1) > 0.999, fmt("OOK bits match %.4f", match(r3->digital.bits, ob, 1)));
}

void testCursors() {
    printf("symbol cursors\n");
    // 1 kBd, centers at 0.25 ms + k ms, a plot of 0 to 20 ms
    SymbolCursors c = symbolCursors(1000, 0.25e-3, 0, 20e-3);
    // centers at 1.25, 2.25 ... 19.25 ms (0.25 ms is in the left 2 %)
    check(c.xs.size() == 19 && near(c.s0, 1.25e-3, 1e-12), fmt("19 cursors, S0 at %.4f ms (1.25)", c.s0 * 1e3));
    check(c.n == 14 && near(c.handle, c.s0 + 14e-3, 1e-12), fmt("the handle is %.0f symbols after S0 (14)", c.n));
    check(near(rateFromHandle(c.s0, c.s0 + 14e-3 * 1.01, c.n), 1000 / 1.01, 1e-6), "dragging the handle 1 % out lowers the rate 1 %");
    check(rateFromHandle(c.s0, c.s0 - 1e-3, c.n) == 0, "a handle before S0 gives no rate");
    check(symbolCursors(1e6, 0, 0, 1).xs.empty(), "too many cursors in view: none");
    check(symbolCursors(0, 0, 0, 1).xs.empty() && symbolCursors(1000, NAN, 0, 1).xs.empty(), "no rate or no time: none");
    // the cursors follow t0 at any offset, also far into the file
    c = symbolCursors(9600, 3600.0 + 0.3 / 9600, 3600.0, 3600.01);
    bool on = !c.xs.empty();
    for (double x : c.xs) on &= std::abs(std::remainder((x - 3600.0) * 9600 - 0.3, 1.0)) < 1e-6;
    check(on, "an hour into the file, each cursor is on a symbol center");
}

void testReport() {
    printf("report\n");
    const double fs = 1e6;
    const int64_t n = 1 << 16;
    cvec x((size_t)n, {0.5f, 0.f});
    const Source s = writeFile("dc", x, fs, 0);
    for (int k = 0; k < kAnaKinds; ++k) {
        AnaKind kind;
        check(anaKindFromName(anaKindName((AnaKind)k), kind) && kind == (AnaKind)k,
              std::string("name ") + anaKindName((AnaKind)k));
        auto r = run(request((AnaKind)k, s, n));
        const std::string t = analysisReport(*r);
        check(t.rfind(std::string("# wvt analysis: ") + anaKindName((AnaKind)k), 0) == 0, "report starts with its header");
    }
    AnaKind kind;
    check(!anaKindFromName("nothing", kind), "an unknown name is refused");
    // a cancelled job returns nothing
    AnaReq q = request(AnaKind::Spectrum, s, n);
    check(computeAnalysis(q, [] { return true; }) == nullptr, "a cancelled analysis returns nothing");
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/wvt_analysis_XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) {
        perror("mkdtemp");
        return 1;
    }
    g_dir = d;
    testMeasure();
    testSpectrum();
    testHistogram();
    testConstellation();
    testPulses();
    testCorrelation();
    testSymbols();
    testCyclo();
    testDemod();
    testCursors();
    testReport();
    std::string cmd = "rm -rf '" + g_dir + "'";
    if (system(cmd.c_str()) != 0) printf("could not remove %s\n", g_dir.c_str());
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
