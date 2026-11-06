#include "dsp/analysis.h"

#include "common/ui_util.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>

namespace {

constexpr double kTiny = 1e-30;
inline double dB(double p) { return 10.0 * std::log10(std::max(p, kTiny)); }
inline double wrapPi(double a) {
    while (a > M_PI) a -= 2 * M_PI;
    while (a < -M_PI) a += 2 * M_PI;
    return a;
}

// The inverse FFT (scaled by 1/n) with the forward FFT: conj(fft(conj(x))) / n.
void inverseFft(const FFT& f, cf* d) {
    const int n = f.size();
    for (int i = 0; i < n; ++i) d[i] = std::conj(d[i]);
    f.forward(d);
    const float s = 1.f / (float)n;
    for (int i = 0; i < n; ++i) d[i] = std::conj(d[i]) * s;
}

// The value at fraction q (0 to 1) of v (a copy is sorted in part).
double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)std::llround(q * (double)(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}
template <class T>
double percentileOf(const std::vector<T>& x, double q, size_t maxN = 1'000'000) {
    std::vector<double> v;
    const size_t step = std::max<size_t>(1, x.size() / maxN);
    v.reserve(x.size() / step + 1);
    for (size_t i = 0; i < x.size(); i += step)
        if (std::isfinite((double)x[i])) v.push_back((double)x[i]);
    return percentile(std::move(v), q);
}

// Parabolic interpolation of the peak at k of y (dB or linear): the offset in bins, -0.5 to 0.5.
double parabolic(const double* y, int n, int k, double* peak = nullptr) {
    if (peak) *peak = y[k];
    if (k <= 0 || k >= n - 1) return 0;
    const double a = y[k - 1], b = y[k], c = y[k + 1];
    const double den = a - 2 * b + c;
    if (den >= 0) return 0;
    const double d = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    if (peak) *peak = b - 0.25 * (a - c) * d;
    return d;
}

// Reads all of [j, j + n): false when cancelled.
bool readAll(const ChannelReader& rd, int64_t j, int64_t n, std::vector<cf>& y, const CancelFn& cancelled,
             Progress* progress) {
    y.assign((size_t)n, cf(0, 0));
    if (progress) progress->reset(n);
    return rd.read(j, n, y.data(), cancelled, true, progress);
}

// Reads [j, j + n) in blocks of `block` outputs; each block also has the next
// `overlap` outputs (so that a window that starts in this block can end in
// the next). fn(block data, valid length, total length read, first index)
// returns false to stop. False when cancelled.
template <class Fn>
bool forBlocks(const ChannelReader& rd, int64_t j, int64_t n, int64_t block, int64_t overlap, const CancelFn& cancelled,
               Progress* progress, Fn&& fn) {
    if (progress) progress->reset(n);
    std::vector<cf> y;
    for (int64_t a = 0; a < n; a += block) {
        const int64_t len = std::min(block, n - a);
        const int64_t total = std::min(len + overlap, n - a);
        y.resize((size_t)total);
        if (!rd.read(j + a, total, y.data(), cancelled, true, nullptr)) return false;
        if (progress) progress->add(len);
        if (!fn(y.data(), len, total, j + a)) break;
        if (cancelled()) return false;
    }
    return !cancelled();
}

// Welch accumulator: sum, max and min of the bin powers.
struct WelchAcc {
    std::vector<double> sum, mx, mn;
    int64_t count = 0;
    void init(int n) {
        sum.assign(n, 0.0);
        mx.assign(n, 0.0);
        mn.assign(n, INFINITY);
        count = 0;
    }
    void merge(const WelchAcc& o) {
        for (size_t i = 0; i < sum.size(); ++i) {
            sum[i] += o.sum[i];
            mx[i] = std::max(mx[i], o.mx[i]);
            mn[i] = std::min(mn[i], o.mn[i]);
        }
        count += o.count;
    }
};

// nseg FFTs of x at starts 0, hop, 2 hop, ... (each needs nfft samples of x).
void welchSegments(const cf* x, int64_t nseg, int64_t hop, int nfft, const std::vector<float>& w, double norm,
                   WelchAcc& acc, const CancelFn* cancelled) {
    if (nseg <= 0) return;
    auto fft = getFFT(nfft);
    std::mutex m;
    parallelFor((int)nseg, [&](int b, int e) {
        WelchAcc local;
        local.init(nfft);
        std::vector<cf> buf(nfft);
        for (int s = b; s < e; ++s) {
            if (cancelled && (s & 15) == 0 && (*cancelled)()) break;
            const cf* p = x + (int64_t)s * hop;
            for (int i = 0; i < nfft; ++i) buf[i] = p[i] * w[i];
            fft->forward(buf.data());
            for (int i = 0; i < nfft; ++i) {
                const double pw = std::norm(buf[(i + nfft / 2) % nfft]) * norm;
                local.sum[i] += pw;
                local.mx[i] = std::max(local.mx[i], pw);
                local.mn[i] = std::min(local.mn[i], pw);
            }
            ++local.count;
        }
        std::lock_guard<std::mutex> lk(m);
        acc.merge(local);
    }, 4);
}

double windowNorm(const std::vector<float>& w) {
    double s = 0;
    for (float v : w) s += v;
    return 1.0 / std::max(kTiny, s * s);
}

// Welch over [j, j + n) of the reader, read in blocks.
bool welchStream(const ChannelReader& rd, int64_t j, int64_t n, int nfft, int window, double overlap,
                 const CancelFn& cancelled, Progress* progress, WelchAcc& acc) {
    acc.init(nfft);
    const std::vector<float> w = makeWindow((WindowType)window, nfft);
    const double norm = windowNorm(w);
    const int64_t hop = std::max<int64_t>(1, (int64_t)std::llround(nfft * (1.0 - std::clamp(overlap, 0.0, 0.95))));
    if (n < nfft) {
        std::vector<cf> y((size_t)nfft, cf(0, 0));
        if (!rd.read(j, n, y.data(), cancelled, true, progress)) return false;
        welchSegments(y.data(), 1, hop, nfft, w, norm, acc, &cancelled);
        return !cancelled();
    }
    const int64_t nseg = (n - nfft) / hop + 1;
    const int64_t segPerBlock = std::max<int64_t>(1, (1 << 22) / hop);
    const int64_t block = segPerBlock * hop;
    return forBlocks(rd, j, n, block, nfft, cancelled, progress, [&](const cf* y, int64_t, int64_t total, int64_t ja) {
        const int64_t first = (ja - j) / hop;  // block starts are multiples of hop
        const int64_t inBlock = std::min(segPerBlock, nseg - first);
        int64_t fit = 0;
        while (fit < inBlock && fit * hop + nfft <= total) ++fit;
        welchSegments(y, fit, hop, nfft, w, norm, acc, &cancelled);
        return first + inBlock < nseg;
    });
}

// The frequency of each output of the spectrum (fftshifted), absolute Hz.
std::vector<double> binFreqs(int nfft, double rate, double center) {
    std::vector<double> f(nfft);
    for (int i = 0; i < nfft; ++i) f[i] = center + (i - nfft / 2) * rate / nfft;
    return f;
}

// Instantaneous frequency arg(y[n] conj(y[n-1])) in cycles per sample (y[0] gets y[1]'s).
std::vector<float> instFreq(const std::vector<cf>& y) {
    std::vector<float> f(y.size(), 0.f);
    for (size_t k = 1; k < y.size(); ++k) f[k] = (float)(std::arg(y[k] * std::conj(y[k - 1])) / (2 * M_PI));
    if (f.size() > 1) f[0] = f[1];
    return f;
}

// The x^power spectrum of y: its peak (Hz from 0 of y), over the median (dB).
double powerNPeak(const std::vector<cf>& y, int power, double rate, std::vector<double>* fo, std::vector<double>* dbo,
                  double* promDb) {
    const int64_t n = (int64_t)y.size();
    const int nfft = std::clamp(nextPow2(n), 64, 1 << 18);
    std::vector<cf> z((size_t)std::max<int64_t>(n, nfft), cf(0, 0));
    for (int64_t k = 0; k < n; ++k) {
        cf v = y[k], p = v;
        for (int i = 1; i < power; ++i) p *= v;
        z[k] = p;
    }
    std::vector<double> avg, mx, mn;
    welchPsd(z.data(), n, nfft, (int)WindowType::Hann, 0.5, avg, mx, mn);
    std::vector<double> db(nfft);
    for (int i = 0; i < nfft; ++i) db[i] = dB(avg[i]);
    const int k = (int)(std::max_element(db.begin(), db.end()) - db.begin());
    double pk = 0;
    const double d = parabolic(db.data(), nfft, k, &pk);
    if (promDb) *promDb = pk - percentile(db, 0.5);
    if (fo) *fo = binFreqs(nfft, rate, 0);
    if (dbo) *dbo = std::move(db);
    return (k + d - nfft / 2) * rate / nfft;
}

// Max-pools x, y to at most m points (keeps the largest y of each group).
void poolMax(std::vector<double>& x, std::vector<double>& y, size_t m) {
    if (y.size() <= m) return;
    std::vector<double> xo(m), yo(m);
    for (size_t b = 0; b < m; ++b) {
        const size_t a = y.size() * b / m, e = std::max(a + 1, y.size() * (b + 1) / m);
        size_t best = a;
        for (size_t i = a; i < e; ++i)
            if (y[i] > y[best]) best = i;
        xo[b] = x[(a + e - 1) / 2];
        yo[b] = y[best];
    }
    x.swap(xo);
    y.swap(yo);
}

// Two-means on values: the threshold between the two clusters.
double twoMeansThreshold(const std::vector<float>& v) {
    double lo = percentileOf(v, 0.1), hi = percentileOf(v, 0.9);
    if (!(hi > lo)) return lo;
    const size_t step = std::max<size_t>(1, v.size() / 1'000'000);
    for (int it = 0; it < 20; ++it) {
        const double t = 0.5 * (lo + hi);
        double s0 = 0, s1 = 0;
        int64_t n0 = 0, n1 = 0;
        for (size_t i = 0; i < v.size(); i += step) {
            const double x = v[i];
            if (!std::isfinite(x)) continue;
            if (x > t) { s1 += x; ++n1; } else { s0 += x; ++n0; }
        }
        if (!n0 || !n1) break;
        const double nlo = s0 / (double)n0, nhi = s1 / (double)n1;
        if (std::abs(nlo - lo) + std::abs(nhi - hi) < 1e-9 * (std::abs(lo) + std::abs(hi) + 1e-12)) break;
        lo = nlo;
        hi = nhi;
    }
    return 0.5 * (lo + hi);
}

// Linear interpolation of v at fractional index x (clamped).
template <class T>
double lerpAt(const std::vector<T>& v, double x) {
    if (v.empty()) return NAN;
    if (x <= 0) return (double)v.front();
    const size_t i = (size_t)x;
    if (i + 1 >= v.size()) return (double)v.back();
    const double f = x - (double)i;
    return (double)v[i] * (1 - f) + (double)v[i + 1] * f;
}
cf lerpAt(const std::vector<cf>& v, double x) {
    if (v.empty()) return cf(0, 0);
    if (x <= 0) return v.front();
    const size_t i = (size_t)x;
    if (i + 1 >= v.size()) return v.back();
    const float f = (float)(x - (double)i);
    return v[i] * (1 - f) + v[i + 1] * f;
}

// --------------------------------------------------------------------------
// Measurements
// --------------------------------------------------------------------------
bool doMeasure(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
               Progress* progress) {
    MeasureRes& m = res.measure;
    const int nfft = std::clamp(nextPow2(r.measure.nfft), 64, 1 << 20);
    // The time-domain sums, in one pass.
    double sI = 0, sQ = 0, sII = 0, sQQ = 0, sIQ = 0, sP = 0, pMax = 0;
    if (!forBlocks(rd, j0, n, 1 << 22, 0, cancelled, nullptr, [&](const cf* y, int64_t len, int64_t, int64_t) {
            for (int64_t k = 0; k < len; ++k) {
                const double i = y[k].real(), q = y[k].imag();
                sI += i; sQ += q; sII += i * i; sQQ += q * q; sIQ += i * q;
                const double p = i * i + q * q;
                sP += p;
                pMax = std::max(pMax, p);
            }
            return true;
        }))
        return false;
    WelchAcc acc;
    if (!welchStream(rd, j0, n, nfft, r.measure.window, 0.5, cancelled, progress, acc)) return false;
    const double N = (double)n;
    const double mean = sP / N;
    m.meanDb = dB(mean);
    m.peakDb = dB(pMax);
    m.paprDb = m.peakDb - m.meanDb;
    m.dcI = sI / N;
    m.dcQ = sQ / N;
    m.dcDb = dB((m.dcI * m.dcI + m.dcQ * m.dcQ) / std::max(kTiny, mean));
    const double vI = sII / N - m.dcI * m.dcI, vQ = sQQ / N - m.dcQ * m.dcQ, cIQ = sIQ / N - m.dcI * m.dcQ;
    m.gainImbDb = (vI > 0 && vQ > 0) ? dB(vI / vQ) : 0;
    m.phaseImbDeg = (vI > 0 && vQ > 0) ? std::asin(std::clamp(cIQ / std::sqrt(vI * vQ), -1.0, 1.0)) * 180 / M_PI : 0;

    const double rate = rd.rate(), binHz = rate / nfft;
    std::vector<double> p(nfft);
    const double cnt = (double)std::max<int64_t>(1, acc.count);
    for (int i = 0; i < nfft; ++i) p[i] = acc.sum[i] / cnt;
    m.f = binFreqs(nfft, rate, rd.center());
    m.psdDb.resize(nfft);
    for (int i = 0; i < nfft; ++i) m.psdDb[i] = dB(p[i]);
    m.rbwHz = windowEnbw(r.measure.window, nfft) * binHz;
    const double noise = percentile(p, 0.2);
    m.noiseDb = dB(noise);
    const int kp = (int)(std::max_element(m.psdDb.begin(), m.psdDb.end()) - m.psdDb.begin());
    m.carrierHz = m.f[kp] + parabolic(m.psdDb.data(), nfft, kp) * binHz;
    double sw = 0, swf = 0;
    for (int i = 0; i < nfft; ++i) {
        const double e = std::max(0.0, p[i] - noise);
        sw += e;
        swf += e * m.f[i];
    }
    m.centroidHz = sw > 0 ? swf / sw : m.carrierHz;
    // 99 % occupied bandwidth
    const double total = std::accumulate(p.begin(), p.end(), 0.0);
    double cum = 0;
    int lo = 0, hi = nfft - 1;
    for (int i = 0; i < nfft; ++i) {
        cum += p[i];
        if (cum >= 0.005 * total) { lo = i; break; }
    }
    cum = 0;
    for (int i = nfft - 1; i >= 0; --i) {
        cum += p[i];
        if (cum >= 0.005 * total) { hi = i; break; }
    }
    m.obwLo = m.f[lo] - binHz / 2;
    m.obwHi = m.f[hi] + binHz / 2;
    m.obwHz = m.obwHi - m.obwLo;
    // -26 dB bandwidth
    const double t26 = m.psdDb[kp] - 26;
    int a = kp, b = kp;
    for (int i = 0; i < nfft; ++i)
        if (m.psdDb[i] >= t26) { a = i; break; }
    for (int i = nfft - 1; i >= 0; --i)
        if (m.psdDb[i] >= t26) { b = i; break; }
    m.bw26Lo = m.f[a] - binHz / 2;
    m.bw26Hi = m.f[b] + binHz / 2;
    m.bw26Hz = m.bw26Hi - m.bw26Lo;
    // SNR in the occupied bandwidth
    double inBand = 0;
    for (int i = lo; i <= hi; ++i) inBand += p[i];
    const double nb = noise * (hi - lo + 1);
    m.snrDb = inBand > nb ? dB((inBand - nb) / std::max(kTiny, nb)) : -INFINITY;
    return true;
}

// --------------------------------------------------------------------------
// Spectrum
// --------------------------------------------------------------------------
bool doSpectrum(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
                Progress* progress) {
    SpectrumRes& s = res.spectrum;
    const int nfft = std::clamp(nextPow2(r.spectrum.nfft), 64, 1 << 20);
    WelchAcc acc;
    if (!welchStream(rd, j0, n, nfft, r.spectrum.window, r.spectrum.overlap, cancelled, progress, acc)) return false;
    const double rate = rd.rate(), binHz = rate / nfft;
    const double enbw = windowEnbw(r.spectrum.window, nfft);
    s.rbwHz = enbw * binHz;
    s.averages = acc.count;
    s.f = binFreqs(nfft, rate, rd.center());
    s.avgDb.resize(nfft);
    s.maxDb.resize(nfft);
    s.minDb.resize(nfft);
    const double cnt = (double)std::max<int64_t>(1, acc.count);
    double total = 0;
    for (int i = 0; i < nfft; ++i) {
        const double p = acc.sum[i] / cnt;
        total += p;
        s.avgDb[i] = dB(p);
        s.maxDb[i] = dB(acc.mx[i]);
        s.minDb[i] = dB(std::isfinite(acc.mn[i]) ? acc.mn[i] : 0.0);
    }
    s.totalDb = dB(total / enbw);
    if (r.spectrum.band && r.spectrum.fHi > r.spectrum.fLo) {
        auto bandPower = [&](double a, double b) -> double {
            if (b < s.f.front() - binHz / 2 || a > s.f.back() + binHz / 2) return NAN;
            double sum = 0;
            for (int i = 0; i < nfft; ++i)
                if (s.f[i] >= a && s.f[i] < b) sum += acc.sum[i] / cnt;
            return sum / enbw;
        };
        const double w = r.spectrum.fHi - r.spectrum.fLo;
        const double pb = bandPower(r.spectrum.fLo, r.spectrum.fHi);
        s.bandDb = dB(pb);
        const double pl = bandPower(r.spectrum.fLo - w, r.spectrum.fLo);
        const double ph = bandPower(r.spectrum.fHi, r.spectrum.fHi + w);
        s.acprLoDb = std::isfinite(pl) ? dB(pl) - s.bandDb : NAN;
        s.acprHiDb = std::isfinite(ph) ? dB(ph) - s.bandDb : NAN;
    }
    return true;
}

// --------------------------------------------------------------------------
// Histograms and CCDF
// --------------------------------------------------------------------------
void fillHist(Histogram& h, const std::vector<float>& v, double lo, double hi, int bins) {
    h.lo = lo;
    h.hi = hi > lo ? hi : lo + 1;
    h.counts.assign(bins, 0.0);
    double s = 0, s2 = 0;
    int64_t n = 0;
    const double sc = bins / (h.hi - h.lo);
    for (float x : v) {
        if (!std::isfinite(x)) continue;
        s += x;
        s2 += (double)x * x;
        ++n;
        const int k = std::clamp((int)((x - h.lo) * sc), 0, bins - 1);
        h.counts[k] += 1;
    }
    if (n) {
        h.mean = s / (double)n;
        h.stdev = std::sqrt(std::max(0.0, s2 / (double)n - h.mean * h.mean));
    }
}

bool doHistogram(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
                 Progress* progress) {
    std::vector<cf> y;
    if (!readAll(rd, j0, n, y, cancelled, progress)) return false;
    HistogramRes& h = res.hist;
    const int bins = std::clamp(r.hist.bins, 10, 4096);
    std::vector<float> v(y.size());
    auto range = [&](double& lo, double& hi) {
        lo = percentileOf(v, 0.0005);
        hi = percentileOf(v, 0.9995);
        if (!(hi > lo)) { lo -= 0.5; hi += 0.5; }
        const double pad = 0.02 * (hi - lo);
        lo -= pad;
        hi += pad;
    };
    double lo, hi;
    for (size_t k = 0; k < y.size(); ++k) v[k] = y[k].real();
    double mI = 0;
    for (float x : v) mI = std::max(mI, (double)std::abs(x));
    for (size_t k = 0; k < y.size(); ++k) mI = std::max(mI, (double)std::abs(y[k].imag()));
    mI = std::max(mI, 1e-12) * 1.02;
    fillHist(h.h[(int)HistQty::I], v, -mI, mI, bins);
    for (size_t k = 0; k < y.size(); ++k) v[k] = y[k].imag();
    fillHist(h.h[(int)HistQty::Q], v, -mI, mI, bins);
    for (size_t k = 0; k < y.size(); ++k) v[k] = std::abs(y[k]);
    double mA = 0;
    for (float x : v) mA = std::max(mA, (double)x);
    fillHist(h.h[(int)HistQty::AM], v, 0, std::max(mA, 1e-12) * 1.02, bins);
    {
        std::vector<float> f = instFreq(y);
        for (auto& x : f) x = (float)(x * rd.rate());
        v.swap(f);
        range(lo, hi);
        fillHist(h.h[(int)HistQty::FM], v, lo, hi, bins);
    }
    v.resize(y.size());
    for (size_t k = 0; k < y.size(); ++k) v[k] = std::arg(y[k]);
    fillHist(h.h[(int)HistQty::PM], v, -M_PI, M_PI, bins);
    if (cancelled()) return false;
    // CCDF of the power over its mean
    double mean = 0;
    for (const cf& c : y) mean += std::norm(c);
    mean /= std::max<size_t>(1, y.size());
    const double step = h.ccdfStepDb;
    const int nb = (int)(40 / step);
    std::vector<double> c(nb, 0.0);
    int64_t cnt = 0;
    double peak = 0;
    for (const cf& z : y) {
        const double p = std::norm(z);
        ++cnt;
        if (mean <= 0 || p <= mean) continue;
        const double x = 10 * std::log10(p / mean);
        peak = std::max(peak, x);
        c[std::min(nb - 1, (int)(x / step))] += 1;
    }
    h.peakDb = peak;
    double above = 0;
    std::vector<double> prob(nb);
    for (int k = nb - 1; k >= 0; --k) {
        above += c[k];
        prob[k] = above / (double)std::max<int64_t>(1, cnt);
    }
    int last = 0;
    for (int k = 0; k < nb; ++k)
        if (prob[k] > 0) last = k;
    for (int k = 0; k <= std::min(nb - 1, last + 1); ++k) {
        h.ccdfDb.push_back(k * step);
        h.ccdfProb.push_back(prob[k]);
    }
    auto at = [&](double pr) -> double {
        for (size_t k = 0; k < h.ccdfProb.size(); ++k)
            if (h.ccdfProb[k] <= pr) return h.ccdfDb[k];
        return h.ccdfDb.empty() ? 0.0 : h.ccdfDb.back();
    };
    h.papr1e2 = at(1e-2);
    h.papr1e3 = at(1e-3);
    h.papr1e4 = at(1e-4);
    return true;
}

// --------------------------------------------------------------------------
// Constellation
// --------------------------------------------------------------------------
// Removes the carrier offset from y (output j0 + k at time timeOf(j0 + k)).
// Returns the offset (Hz). PowerN also removes the mean phase.
double removeCarrier(std::vector<cf>& y, int mode, double manualHz, int power, double rate, int64_t j0) {
    double fo = 0;
    switch ((CarrierFix)mode) {
        case CarrierFix::Manual: fo = manualHz; break;
        case CarrierFix::MeanFreq: {
            std::complex<double> s = 0;
            for (size_t k = 1; k < y.size(); ++k) s += std::complex<double>(y[k] * std::conj(y[k - 1]));
            fo = std::arg(s) / (2 * M_PI) * rate;
            break;
        }
        case CarrierFix::PowerN: fo = powerNPeak(y, power, rate, nullptr, nullptr, nullptr) / power; break;
        default: return 0;
    }
    const double frac = fo / rate;
    if (frac != 0) {
        parallelFor((int)((y.size() + 4095) / 4096), [&](int cb, int ce) {
            for (int c = cb; c < ce; ++c) {
                const size_t a = (size_t)c * 4096, b = std::min(y.size(), a + 4096);
                const double cyc = std::fmod(frac * (double)(j0 + (int64_t)a), 1.0);
                std::complex<double> rot = std::polar(1.0, -2 * M_PI * cyc);
                const std::complex<double> st = std::polar(1.0, -2 * M_PI * frac);
                for (size_t k = a; k < b; ++k) {
                    y[k] *= cf((float)rot.real(), (float)rot.imag());
                    rot *= st;
                }
            }
        });
    }
    if ((CarrierFix)mode == CarrierFix::PowerN) {
        std::complex<double> s = 0;
        for (const cf& v : y) {
            cf p = v;
            for (int i = 1; i < power; ++i) p *= v;
            s += std::complex<double>(p);
        }
        double ph = std::arg(s) / power;
        if (power == 4) ph -= M_PI / 4;  // QPSK on the diagonals
        const cf rot((float)std::cos(-ph), (float)std::sin(-ph));
        for (cf& v : y) v *= rot;
    }
    return fo;
}

bool doConstellation(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res,
                     const CancelFn& cancelled, Progress* progress) {
    std::vector<cf> y;
    if (!readAll(rd, j0, n, y, cancelled, progress)) return false;
    ConstellationRes& c = res.cons;
    const ConstellationParams& p = r.cons;
    const int power = p.power == 2 || p.power == 8 ? p.power : 4;
    c.removedHz = removeCarrier(y, p.carrier, p.manualHz, power, rd.rate(), j0);
    double ms = 0;
    for (const cf& v : y) ms += std::norm(v);
    ms /= std::max<size_t>(1, y.size());
    c.scale = p.normalize && ms > 0 ? 1.0 / std::sqrt(ms) : 1.0;
    // the points: every sample, or one per symbol
    std::vector<cf> pts;
    if (p.symbolRate > 0) {
        const double T = 1.0 / p.symbolRate, rate = rd.rate();
        const double tA = rd.timeOf((double)j0), tB = rd.timeOf((double)(j0 + n - 1));
        double t = p.symbolT0 + std::ceil((tA - p.symbolT0) / T) * T;
        for (; t <= tB && pts.size() < 4'000'000; t += T) pts.push_back(lerpAt(y, t * rate - (double)j0));
    } else {
        pts.swap(y);
    }
    for (cf& v : pts) v *= (float)c.scale;
    std::vector<float> mag(pts.size());
    for (size_t k = 0; k < pts.size(); ++k) mag[k] = std::abs(pts[k]);
    c.extent = std::max(1e-9, 1.25 * percentileOf(mag, 0.999));
    c.bins = std::clamp(p.bins, 16, 512);
    c.density.assign((size_t)c.bins * c.bins, 0.f);
    const double sc = c.bins / (2 * c.extent);
    double sm = 0, sm2 = 0;
    for (size_t k = 0; k < pts.size(); ++k) {
        const int x = (int)((pts[k].real() + c.extent) * sc), yy = (int)((c.extent - pts[k].imag()) * sc);
        if (x >= 0 && x < c.bins && yy >= 0 && yy < c.bins) c.density[(size_t)yy * c.bins + x] += 1;
        sm += mag[k];
        sm2 += (double)mag[k] * mag[k];
    }
    const double np = (double)std::max<size_t>(1, pts.size());
    c.rmsMag = std::sqrt(sm2 / np);
    c.magStdev = std::sqrt(std::max(0.0, sm2 / np - (sm / np) * (sm / np)));
    const size_t m = std::clamp(p.maxPoints, 100, 200000);
    const size_t step = std::max<size_t>(1, pts.size() / m);
    for (size_t k = 0; k < pts.size(); k += step) {
        c.xs.push_back(pts[k].real());
        c.ys.push_back(pts[k].imag());
    }
    if (p.symbolRate > 0) res.used = (int64_t)pts.size();
    return true;
}

// --------------------------------------------------------------------------
// Pulses
// --------------------------------------------------------------------------
bool doPulses(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
              Progress* progress) {
    PulseRes& pr = res.pulse;
    const PulseParams& p = r.pulse;
    const double rate = rd.rate();
    const int L = std::clamp(p.smooth, 1, 1 << 16);
    // The noise floor: the 20th percentile of the smoothed power in up to 64 blocks.
    {
        const int nb = (int)std::min<int64_t>(64, std::max<int64_t>(1, n / 16384));
        const int64_t bl = std::min<int64_t>(n, 16384);
        std::vector<double> pw;
        std::vector<cf> y((size_t)bl);
        for (int b = 0; b < nb; ++b) {
            const int64_t a = j0 + (n - bl) * b / std::max(1, nb - 1);
            if (!rd.read(a, bl, y.data(), cancelled)) return false;
            std::vector<float> q(y.size());
            for (size_t k = 0; k < y.size(); ++k) q[k] = std::norm(y[k]);
            movingAverage(q, L);
            for (float v : q) pw.push_back(v);
        }
        // The 20th percentile of noise power smoothed over L samples (a gamma
        // distribution with shape L) is under its mean: correct it with the
        // Wilson-Hilferty approximation of that percentile.
        const double k = L, z = -0.8416;
        const double c = std::pow(1 - 1 / (9 * k) + z * std::sqrt(1 / (9 * k)), 3);
        pr.noiseDb = dB(percentile(pw, 0.2) / std::max(0.05, c));
    }
    pr.thresholdDb = p.autoThreshold ? pr.noiseDb + p.thresholdDb : p.thresholdDb;
    const double thrHi = std::pow(10.0, pr.thresholdDb / 10);
    const double thrLo = thrHi * std::pow(10.0, -std::max(0.0, p.hysteresisDb) / 10);
    const int64_t minW = std::max<int64_t>(1, (int64_t)std::llround(p.minWidthS * rate));
    // Pass 1: the edges, with a causal boxcar (shifted by (L - 1) / 2).
    struct Edge {
        int64_t a, b;  // output indices: first above the threshold, first below
    };
    std::vector<Edge> edges;
    std::vector<double> ring(L, 0.0);
    double acc = 0;
    int64_t idx = 0;
    bool in = false, startedHigh = false;
    int64_t start = 0;
    const int64_t shift = (L - 1) / 2;
    const size_t maxP = (size_t)std::clamp(p.maxPulses, 1, 10'000'000);
    if (!forBlocks(rd, j0, n, 1 << 22, 0, cancelled, progress, [&](const cf* y, int64_t len, int64_t, int64_t ja) {
            for (int64_t k = 0; k < len; ++k, ++idx) {
                const double v = std::norm(y[k]);
                acc += v - ring[idx % L];
                ring[idx % L] = v;
                if (idx < L - 1) continue;
                const double pw = acc / L;
                const int64_t at = ja + k - shift;
                if (idx == L - 1 && pw > thrHi) startedHigh = true;
                if (!in && pw > thrHi) {
                    in = true;
                    start = at;
                } else if (in && pw < thrLo) {
                    in = false;
                    if (!startedHigh && at - start >= minW) edges.push_back({start, at});
                    startedHigh = false;
                    if (edges.size() >= maxP) {
                        pr.truncated = true;
                        return false;
                    }
                }
            }
            return true;
        }))
        return false;
    // Pass 2: measure each pulse
    pr.pulses.resize(edges.size());
    std::atomic<bool> ab{false};
    parallelFor((int)edges.size(), [&](int b, int e) {
        std::vector<cf> y;
        std::vector<float> env;
        for (int i = b; i < e; ++i) {
            if ((i & 63) == 0 && cancelled()) { ab = true; return; }
            const Edge& ed = edges[i];
            const int64_t w = ed.b - ed.a;
            const int64_t m = std::max<int64_t>(8, w / 2 + L);
            const int64_t a = ed.a - m, len = w + 2 * m;
            y.resize((size_t)len);
            rd.read(a, len, y.data(), cancelled, false);
            env.resize(y.size());
            for (size_t k = 0; k < y.size(); ++k) env[k] = std::norm(y[k]);
            movingAverage(env, L);
            for (float& v : env) v = std::sqrt(std::max(0.f, v));
            Pulse& pu = pr.pulses[i];
            const int64_t ia = m, ib = m + w;  // the pulse in env
            const int64_t ca = ia + w / 5, cb = std::max(ca + 1, ib - w / 5);
            std::vector<double> top;
            double pk = 0, sp = 0;
            for (int64_t k = ia; k < ib; ++k) {
                pk = std::max(pk, (double)std::norm(y[k]));
                sp += std::norm(y[k]);
            }
            for (int64_t k = ca; k < cb; ++k) top.push_back(env[k]);
            const double A = w >= 5 ? percentile(top, 0.5) : std::sqrt(pk);
            pu.peakDb = dB(pk);
            pu.meanDb = dB(sp / (double)std::max<int64_t>(1, w));
            const int64_t mid = (ia + ib) / 2;
            // the crossing of level x A before mid (rising) or after mid (falling), fractional index
            auto rising = [&](double lv) -> double {
                const double t = lv * A;
                for (int64_t k = mid; k > 0; --k)
                    if (env[k - 1] < t && env[k] >= t) return (double)(k - 1) + (t - env[k - 1]) / (env[k] - env[k - 1]);
                return NAN;
            };
            auto falling = [&](double lv) -> double {
                const double t = lv * A;
                for (int64_t k = mid; k + 1 < (int64_t)env.size(); ++k)
                    if (env[k] >= t && env[k + 1] < t) return (double)k + (env[k] - t) / (env[k] - env[k + 1]);
                return NAN;
            };
            double r50 = rising(0.5), f50 = falling(0.5);
            if (!std::isfinite(r50)) r50 = (double)ia;
            if (!std::isfinite(f50)) f50 = (double)ib;
            pu.toa = rd.timeOf((double)a + r50);
            pu.width = (f50 - r50) / rate;
            pu.rise = (rising(0.9) - rising(0.1)) / rate;
            pu.fall = (falling(0.1) - falling(0.9)) / rate;
            // frequency and chirp over the middle 80 %
            const int64_t fa = std::max<int64_t>(1, (int64_t)(r50 + 0.1 * (f50 - r50)));
            const int64_t fb = std::min<int64_t>((int64_t)y.size(), (int64_t)(f50 - 0.1 * (f50 - r50)) + 1);
            double sw = 0, swf = 0, sx = 0, sxx = 0, sxy = 0, sy = 0;
            int64_t cnt = 0;
            std::complex<double> ph = 0;
            for (int64_t k = fa; k < fb; ++k) {
                const cf d = y[k] * std::conj(y[k - 1]);
                ph += std::complex<double>(d);
                const double f = std::arg(d) / (2 * M_PI) * rate;
                const double wt = std::norm(y[k]);
                sw += wt;
                swf += wt * f;
                const double x = (double)(k - fa) / rate;
                sx += x; sxx += x * x; sxy += x * f; sy += f;
                ++cnt;
            }
            pu.freqHz = rd.center() + (sw > 0 ? swf / sw : std::arg(ph) / (2 * M_PI) * rate);
            const double den = (double)cnt * sxx - sx * sx;
            pu.chirpHzPerS = cnt > 2 && den > 0 ? ((double)cnt * sxy - sx * sy) / den : 0;
        }
    }, 16);
    if (ab) return false;
    std::sort(pr.pulses.begin(), pr.pulses.end(), [](const Pulse& x, const Pulse& z) { return x.toa < z.toa; });
    std::vector<double> pris, widths;
    for (size_t i = 0; i < pr.pulses.size(); ++i) {
        if (i) {
            pr.pulses[i].pri = pr.pulses[i].toa - pr.pulses[i - 1].toa;
            pris.push_back(pr.pulses[i].pri);
        }
        widths.push_back(pr.pulses[i].width);
    }
    if (!widths.empty()) pr.meanWidth = std::accumulate(widths.begin(), widths.end(), 0.0) / (double)widths.size();
    if (!pris.empty()) {
        pr.meanPri = std::accumulate(pris.begin(), pris.end(), 0.0) / (double)pris.size();
        pr.dutyCycle = pr.meanPri > 0 ? pr.meanWidth / pr.meanPri : 0;
        std::vector<double> s = pris;
        std::sort(s.begin(), s.end());
        double sum = s[0];
        int cnt = 1;
        for (size_t i = 1; i <= s.size(); ++i) {
            if (i < s.size() && s[i] <= (sum / cnt) * 1.01) {
                sum += s[i];
                ++cnt;
                continue;
            }
            pr.priGroups.push_back(sum / cnt);
            if (i < s.size()) {
                sum = s[i];
                cnt = 1;
            }
        }
    }
    // Overlay
    const int no = (int)std::min<size_t>(pr.pulses.size(), (size_t)std::clamp(p.overlayPulses, 0, 2000));
    if (no > 0) {
        const double W = percentile(widths, 0.5);
        const int64_t pre = (int64_t)std::ceil(std::max(0.25 * W * rate, 4.0) + L);
        const int64_t post = (int64_t)std::ceil(std::max(1.25 * W * rate, 8.0) + L);
        const int64_t K = pre + post;
        const int64_t outN = std::min<int64_t>(K, 800);
        for (int64_t i = 0; i < outN; ++i) pr.overlayT.push_back(((double)(i * K / outN) - (double)pre) / rate);
        pr.overlayDb.assign(no, std::vector<float>(outN, -200.f));
        parallelFor(no, [&](int b, int e) {
            std::vector<cf> y((size_t)K);
            for (int i = b; i < e; ++i) {
                const int64_t t = (int64_t)std::llround(pr.pulses[i].toa * rate);
                rd.read(t - pre, K, y.data(), cancelled, false);
                std::vector<float>& row = pr.overlayDb[i];
                for (int64_t k = 0; k < K; ++k) {
                    const int64_t o = k * outN / K;
                    row[o] = std::max(row[o], (float)dB(std::norm(y[k])));
                }
            }
        }, 8);
    }
    return !cancelled();
}

// --------------------------------------------------------------------------
// Correlation
// --------------------------------------------------------------------------
bool doAutocorr(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
                Progress* progress) {
    CorrelationRes& c = res.corr;
    n = std::min<int64_t>(n, 1 << 22);
    res.used = n;
    std::vector<cf> y;
    if (!readAll(rd, j0, n, y, cancelled, progress)) return false;
    std::complex<double> mean = 0;
    for (const cf& v : y) mean += std::complex<double>(v);
    mean /= (double)std::max<int64_t>(1, n);
    const int nf = nextPow2(2 * n);
    std::vector<cf> X((size_t)nf, cf(0, 0));
    for (int64_t k = 0; k < n; ++k) X[k] = y[k] - cf((float)mean.real(), (float)mean.imag());
    auto fft = getFFT(nf);
    fft->forward(X.data());
    for (cf& v : X) v = cf(std::norm(v), 0.f);
    inverseFft(*fft, X.data());
    if (cancelled()) return false;
    const double r0 = std::max(kTiny, (double)std::abs(X[0]));
    const int64_t maxLag = std::max<int64_t>(2, n / 2);
    std::vector<double> lag((size_t)maxLag), mag((size_t)maxLag);
    for (int64_t k = 0; k < maxLag; ++k) {
        lag[k] = (double)k / rd.rate();
        mag[k] = std::abs(X[k]) / r0;
    }
    // the main lobe ends where |R| first falls under 0.5
    int64_t lobe = 1;
    while (lobe < maxLag && mag[lobe] >= 0.5) ++lobe;
    std::vector<CorrPeak> pk;
    for (int64_t k = std::max<int64_t>(lobe, 1); k + 1 < maxLag; ++k)
        if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > 0.05) pk.push_back({lag[k], mag[k]});
    std::sort(pk.begin(), pk.end(), [](const CorrPeak& a, const CorrPeak& b) { return a.v > b.v; });
    // keep peaks at least one lobe apart
    const double sep = (double)lobe / rd.rate();
    for (const CorrPeak& p : pk) {
        bool near = false;
        for (const CorrPeak& q : c.peaks) near |= std::abs(q.x - p.x) < sep;
        if (!near) c.peaks.push_back(p);
        if (c.peaks.size() >= 10) break;
    }
    poolMax(lag, mag, 20000);
    c.x = std::move(lag);
    c.y = std::move(mag);
    return true;
}

bool doSearch(const AnaReq& r, const ChannelReader& rd, AnaRes& res, const CancelFn& cancelled, Progress* progress) {
    CorrelationRes& c = res.corr;
    const CorrelationParams& p = r.corr;
    const int64_t tA = rd.outIndex(p.tmplS0), tB = rd.outIndex(p.tmplS1);
    const int64_t T = tB - tA;
    if (T < 4) {
        c.err = "The template is too short. Set the time markers on the signal to find, or select a bookmark.";
        return true;
    }
    if (T > (1 << 17)) {
        c.err = "The template has more than 131072 samples. Select a shorter template, or decimate with the channel filter.";
        return true;
    }
    const int64_t sA = rd.outIndex(p.searchS0), sB = rd.outIndex(p.searchS1);
    const int64_t S = sB - sA;
    if (S < T) {
        c.err = "The search range is shorter than the template.";
        return true;
    }
    c.tmplLen = (double)T / rd.rate();
    std::vector<cf> t((size_t)T);
    if (!rd.read(tA, T, t.data(), cancelled)) return false;
    double et = 0;
    for (const cf& v : t) et += std::norm(v);
    if (et <= 0) {
        c.err = "The template has no signal (all zeros).";
        return true;
    }
    const int nf = std::clamp(nextPow2(4 * T), 1 << 14, 1 << 19);
    const int64_t L = nf - T + 1;  // valid lags in a block
    auto fft = getFFT(nf);
    std::vector<cf> H((size_t)nf, cf(0, 0));
    std::copy(t.begin(), t.end(), H.begin());
    fft->forward(H.data());
    const int64_t positions = S - T + 1;
    const int64_t nb = (positions + L - 1) / L;
    const int P = 4000;
    std::vector<float> env(P, 0.f);
    std::vector<std::pair<int64_t, float>> cand;
    std::mutex m;
    std::atomic<bool> ab{false};
    if (progress) progress->reset(nb);
    parallelFor((int)nb, [&](int b0, int b1) {
        std::vector<cf> x((size_t)nf);
        std::vector<double> cs((size_t)nf + 1);
        std::vector<float> le(P, 0.f);
        std::vector<std::pair<int64_t, float>> lc;
        for (int b = b0; b < b1; ++b) {
            if (cancelled()) { ab = true; break; }
            const int64_t j = sA + (int64_t)b * L;
            const int64_t len = std::min<int64_t>(nf, sB - j);
            std::fill(x.begin(), x.end(), cf(0, 0));
            rd.read(j, len, x.data(), cancelled, false);
            cs[0] = 0;
            for (int k = 0; k < nf; ++k) cs[k + 1] = cs[k] + std::norm(x[k]);
            fft->forward(x.data());
            for (int k = 0; k < nf; ++k) x[k] *= std::conj(H[k]);
            inverseFft(*fft, x.data());
            const int64_t valid = std::min<int64_t>(L, positions - (int64_t)b * L);
            bool run = false;
            int64_t best = 0;
            float bv = 0;
            for (int64_t k = 0; k < valid; ++k) {
                const double ex = cs[k + T] - cs[k];
                const float s = ex > 0 ? (float)(std::abs(x[k]) / std::sqrt(et * ex)) : 0.f;
                const int64_t pos = (int64_t)b * L + k;
                const int o = (int)std::min<int64_t>(P - 1, pos * P / positions);
                le[o] = std::max(le[o], s);
                if (s >= p.threshold) {
                    if (!run || s > bv) { best = j + k; bv = s; }
                    run = true;
                } else if (run) {
                    lc.push_back({best, bv});
                    run = false;
                }
            }
            if (run) lc.push_back({best, bv});
            if (progress) progress->add(1);
        }
        std::lock_guard<std::mutex> lk(m);
        for (int i = 0; i < P; ++i) env[i] = std::max(env[i], le[i]);
        cand.insert(cand.end(), lc.begin(), lc.end());
    }, 1);
    if (ab) return false;
    std::sort(cand.begin(), cand.end());
    std::vector<std::pair<int64_t, float>> hits;
    for (const auto& h : cand) {
        if (!hits.empty() && h.first - hits.back().first < T) {
            if (h.second > hits.back().second) hits.back() = h;
        } else {
            hits.push_back(h);
        }
    }
    const size_t mh = (size_t)std::clamp(p.maxHits, 1, 1'000'000);
    if (hits.size() > mh) {
        hits.resize(mh);
        c.truncated = true;
    }
    for (const auto& h : hits) c.peaks.push_back({rd.timeOf((double)h.first), h.second});
    c.x.resize(P);
    c.y.resize(P);
    for (int i = 0; i < P; ++i) {
        c.x[i] = rd.timeOf((double)sA + ((double)i + 0.5) * (double)positions / P);
        c.y[i] = env[i];
    }
    res.used = S;
    res.wanted = S;
    res.t0 = rd.timeOf((double)sA);
    return true;
}

// --------------------------------------------------------------------------
// Symbols
// --------------------------------------------------------------------------
// The detector of symbol transitions: power, or the size of the change of
// the instantaneous frequency.
std::vector<float> symbolDetector(const std::vector<cf>& y, int method) {
    std::vector<float> d(y.size(), 0.f);
    if ((SymMethod)method == SymMethod::Envelope) {
        for (size_t k = 0; k < y.size(); ++k) d[k] = std::norm(y[k]);
    } else if ((SymMethod)method == SymMethod::EnvChange) {
        for (size_t k = 1; k < y.size(); ++k) d[k] = std::abs(std::abs(y[k]) - std::abs(y[k - 1]));
    } else {
        std::vector<float> f = instFreq(y);
        for (size_t k = 1; k < y.size(); ++k) d[k] = std::abs(f[k] - f[k - 1]);
    }
    return d;
}

struct RateEstimate {
    double rate = 0, t0 = 0, snrDb = 0;
};

// The symbol rate and the time of a symbol center, from the strongest line
// of the detector between lo and hi.
RateEstimate estimateRate(const std::vector<cf>& y, int method, double rate, double lo, double hi, double tFirst,
                          std::vector<double>* f, std::vector<double>* db) {
    RateEstimate e;
    const std::vector<float> d = symbolDetector(y, method);
    e.rate = strongestLine(d, rate, lo, hi, f, db, &e.snrDb);
    if (!(e.rate > 0)) return e;
    // the phase of the line, at the exact rate
    std::complex<double> X = 0;
    double mean = 0;
    for (float v : d) mean += v;
    mean /= std::max<size_t>(1, d.size());
    const double w = -2 * M_PI * e.rate / rate;
    std::complex<double> rot = 1, st = std::polar(1.0, w);
    for (size_t k = 0; k < d.size(); ++k) {
        X += (d[k] - mean) * rot;
        rot *= st;
        if ((k & 1023) == 1023) rot /= std::abs(rot);
    }
    const double T = 1.0 / e.rate;
    // the detector peaks at -arg(X) / (2 pi rate) after the first sample
    double tp = -std::arg(X) / (2 * M_PI * e.rate);
    // Transitions peak between symbols (and the detector is half a sample late); Envelope at the centers.
    if ((SymMethod)method != SymMethod::Envelope) tp += T / 2 - 0.5 / rate;
    tp = std::fmod(tp, T);
    if (tp < 0) tp += T;
    e.t0 = tFirst + tp;
    return e;
}

std::vector<float> traceOf(const std::vector<cf>& y, int trace, double rate) {
    std::vector<float> v(y.size());
    switch ((SymTrace)trace) {
        case SymTrace::I: for (size_t k = 0; k < y.size(); ++k) v[k] = y[k].real(); break;
        case SymTrace::Q: for (size_t k = 0; k < y.size(); ++k) v[k] = y[k].imag(); break;
        case SymTrace::AM: for (size_t k = 0; k < y.size(); ++k) v[k] = std::abs(y[k]); break;
        case SymTrace::FM: {
            v = instFreq(y);
            for (float& x : v) x = (float)(x * rate);
            break;
        }
        default: for (size_t k = 0; k < y.size(); ++k) v[k] = std::arg(y[k]); break;
    }
    return v;
}

bool doSymbols(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
               Progress* progress) {
    std::vector<cf> y;
    if (!readAll(rd, j0, n, y, cancelled, progress)) return false;
    SymbolRes& s = res.sym;
    const SymbolParams& p = r.sym;
    const double rate = rd.rate();
    const double tFirst = rd.timeOf((double)j0);
    const double lo = p.minRate > 0 ? p.minRate : std::max(rate / 2000.0, 4.0 * rate / (double)n);
    const double hi = p.maxRate > 0 ? std::min(p.maxRate, rate / 2) : rate * 0.45;
    const RateEstimate e = estimateRate(y, p.method, rate, lo, hi, tFirst, &s.lineF, &s.lineDb);
    s.estRate = e.rate;
    s.estT0 = e.t0;
    s.lineSnrDb = e.snrDb;
    poolMax(s.lineF, s.lineDb, 8192);
    s.rate = p.rate > 0 ? p.rate : s.estRate;
    s.t0 = p.haveT0 ? p.t0 : s.estT0;
    if (cancelled()) return false;
    const int power = p.power == 2 || p.power == 8 ? p.power : 4;
    s.powPeakHz = powerNPeak(y, power, rate, &s.powF, &s.powDb, &s.powPeakDb);
    s.powCarrierHz = s.powPeakHz / power;
    poolMax(s.powF, s.powDb, 8192);
    if (cancelled()) return false;
    const std::vector<float> tr = traceOf(y, p.trace, rate);
    s.traceY = tr;
    s.traceT.resize(tr.size());
    for (size_t k = 0; k < tr.size(); ++k) s.traceT[k] = (float)((double)k / rate);
    if (s.rate > 0) {
        const double T = 1.0 / s.rate;
        const double tA = tFirst, tB = tFirst + (double)(n - 1) / rate;
        double t = s.t0 + std::ceil((tA - s.t0) / T) * T;
        for (; t <= tB && s.symT.size() < 1'000'000; t += T) {
            s.symT.push_back(t);
            s.symV.push_back(lerpAt(tr, (t - tFirst) * rate));
        }
        // eye diagram over the first 2 M samples
        const int S = std::clamp(p.eyeSymbols, 1, 8);
        s.eyeW = s.eyeH = std::clamp(p.eyeBins, 32, 512);
        s.eyeLo = percentileOf(tr, 0.001);
        s.eyeHi = percentileOf(tr, 0.999);
        const double pad = 0.05 * std::max(1e-12, s.eyeHi - s.eyeLo);
        s.eyeLo -= pad;
        s.eyeHi += pad;
        s.eye.assign((size_t)s.eyeW * s.eyeH, 0.f);
        const double span = S * T, sps = rate * T;
        const int sub = std::clamp((int)std::ceil(s.eyeW / (S * std::max(1.0, sps))), 1, 64);
        const size_t m = std::min<size_t>(tr.size(), 2'000'000);
        const double ys = s.eyeH / (s.eyeHi - s.eyeLo);
        for (size_t k = 0; k + 1 < m; ++k) {
            for (int q = 0; q < sub; ++q) {
                const double fr = (double)q / sub;
                const double t = tFirst + ((double)k + fr) / rate;
                double u = std::fmod(t - s.t0 + span / 2, span);
                if (u < 0) u += span;
                const double v = tr[k] * (1 - fr) + tr[k + 1] * fr;
                const int x = std::min(s.eyeW - 1, (int)(u / span * s.eyeW));
                const int yy = (int)((s.eyeHi - v) * ys);
                if (yy >= 0 && yy < s.eyeH) s.eye[(size_t)yy * s.eyeW + x] += 1;
            }
        }
    }
    return !cancelled();
}

// --------------------------------------------------------------------------
// Cyclostationary analysis: the FFT accumulation method (FAM)
// --------------------------------------------------------------------------
bool doCyclo(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
             Progress* progress) {
    CycloRes& c = res.cyclo;
    const int np = std::clamp(nextPow2(r.cyclo.np), 16, 256);
    const int L = np / 4;
    int P = 16;
    while (P * 2 <= std::clamp(nextPow2(r.cyclo.maxP), 16, 8192) && (int64_t)(P * 2 - 1) * L + np <= n) P *= 2;
    if ((int64_t)(P - 1) * L + np > n) {
        res.err = "The region is too short for this analysis. Select a longer region.";
        return true;
    }
    const int64_t N = (int64_t)(P - 1) * L + np;
    res.used = N;
    c.np = np;
    c.p = P;
    c.l = L;
    c.samples = N;
    std::vector<cf> y;
    if (!readAll(rd, j0, N, y, cancelled, progress)) return false;
    const double rate = rd.rate();
    // the channelizer: X[p][k], fftshifted, with the phase correction
    const std::vector<float> a = makeWindow(WindowType::Hamming, np);
    std::vector<cf> X((size_t)P * np);
    auto f1 = getFFT(np);
    parallelFor(P, [&](int b, int e) {
        std::vector<cf> buf(np);
        for (int p = b; p < e; ++p) {
            for (int i = 0; i < np; ++i) buf[i] = y[(size_t)p * L + i] * a[i];
            f1->forward(buf.data());
            for (int k = 0; k < np; ++k) {
                const int kk = k - np / 2;
                const double ph = -2 * M_PI * (double)kk * (double)p * L / np;
                X[(size_t)p * np + k] = buf[(k + np / 2) % np] * cf((float)std::cos(ph), (float)std::sin(ph));
            }
        }
    });
    if (cancelled()) return false;
    // alpha grid [-rate, rate) in steps of da; f grid in half bins
    const double da = rate / ((double)L * P);
    const int naFull = 2 * L * P;
    const int nf = 2 * np;
    std::vector<float> grid((size_t)naFull * nf, 0.f);
    auto f2 = getFFT(P);
    std::atomic<bool> ab{false};
    std::mutex m;
    parallelFor(np, [&](int b, int e) {
        std::vector<cf> buf(P);
        std::vector<std::pair<size_t, float>> upd;
        for (int k1 = b; k1 < e; ++k1) {
            if (cancelled()) { ab = true; return; }
            for (int k2 = 0; k2 < np; ++k2) {
                for (int p = 0; p < P; ++p) buf[p] = X[(size_t)p * np + k1] * std::conj(X[(size_t)p * np + k2]);
                f2->forward(buf.data());
                const double a0 = (double)(k1 - k2) * rate / np;
                const int fi = k1 + k2;  // f0 = (f1 + f2) / 2 in half bins, 0 .. 2 np - 2
                for (int q = -P / 4; q < P / 4; ++q) {
                    const float v = std::abs(buf[(q + P) % P]);
                    const double al = a0 + q * da;
                    const int ai = (int)std::llround((al + rate) / da);
                    if (ai < 0 || ai >= naFull) continue;
                    upd.push_back({(size_t)fi * naFull + ai, v});
                }
            }
            std::lock_guard<std::mutex> lk(m);
            for (const auto& u : upd) grid[u.first] = std::max(grid[u.first], u.second);
            upd.clear();
        }
    });
    if (ab) return false;
    // the reference: the largest value at alpha = 0
    const int a0i = (int)std::llround(rate / da);
    float ref = 0;
    for (int fi = 0; fi < nf; ++fi) ref = std::max(ref, grid[(size_t)fi * naFull + a0i]);
    ref = std::max(ref, 1e-30f);
    c.alpha.resize(naFull);
    c.profileDb.resize(naFull);
    for (int ai = 0; ai < naFull; ++ai) {
        float mx = 0;
        for (int fi = 0; fi < nf; ++fi) mx = std::max(mx, grid[(size_t)fi * naFull + ai]);
        c.alpha[ai] = -rate + ai * da;
        c.profileDb[ai] = dB(mx / ref);
    }
    // the strongest alphas above rate / np (away from the alpha = 0 leakage)
    std::vector<CorrPeak> pk;
    for (int ai = 1; ai + 1 < naFull; ++ai) {
        if (c.alpha[ai] <= rate / np) continue;
        if (c.profileDb[ai] > c.profileDb[ai - 1] && c.profileDb[ai] >= c.profileDb[ai + 1])
            pk.push_back({c.alpha[ai], c.profileDb[ai]});
    }
    std::sort(pk.begin(), pk.end(), [](const CorrPeak& x, const CorrPeak& z) { return x.v > z.v; });
    for (const CorrPeak& q : pk) {
        bool near = false;
        for (const CorrPeak& o : c.peaks) near |= std::abs(o.x - q.x) < 4 * da;
        if (!near) c.peaks.push_back(q);
        if (c.peaks.size() >= 10) break;
    }
    // the image: alpha max-pooled to at most 1024 columns; row 0 = the largest f
    c.na = std::min(naFull, 1024);
    c.nf = nf;
    c.aLo = -rate;
    c.aHi = rate;
    c.fLo = -rate / 2;
    c.fHi = rate / 2;
    c.db.assign((size_t)c.na * nf, -100.f);
    for (int fi = 0; fi < nf; ++fi) {
        for (int ci = 0; ci < c.na; ++ci) {
            const int aa = (int)((int64_t)naFull * ci / c.na), ae = std::max(aa + 1, (int)((int64_t)naFull * (ci + 1) / c.na));
            float mx = 0;
            for (int ai = aa; ai < ae; ++ai) mx = std::max(mx, grid[(size_t)fi * naFull + ai]);
            c.db[(size_t)(nf - 1 - fi) * c.na + ci] = (float)dB(mx / ref);
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// Digital demodulation
// --------------------------------------------------------------------------
bool doDigital(const AnaReq& r, const ChannelReader& rd, int64_t j0, int64_t n, AnaRes& res, const CancelFn& cancelled,
               Progress* progress) {
    std::vector<cf> y;
    if (!readAll(rd, j0, n, y, cancelled, progress)) return false;
    DigitalRes& d = res.digital;
    const DigitalParams& p = r.digital;
    const double rate = rd.rate();
    const double tFirst = rd.timeOf((double)j0);
    const DemodMode mode = (DemodMode)std::clamp(p.mode, 0, (int)DemodMode::Count - 1);
    const bool psk = mode == DemodMode::BPSK || mode == DemodMode::QPSK;
    const int power = mode == DemodMode::QPSK ? 4 : 2;
    if (psk) d.carrierHz = removeCarrier(y, (int)CarrierFix::PowerN, 0, power, rate, j0);
    // the symbol rate and a symbol center
    const double lo = std::max(rate / 2000.0, 4.0 * rate / (double)n), hi = rate * 0.45;
    RateEstimate est;
    // OOK: the envelope changes; FSK: the frequency changes; PSK: the best of all.
    const int mA = mode == DemodMode::OOK ? (int)SymMethod::EnvChange : mode == DemodMode::FSK ? (int)SymMethod::Transitions : 0;
    const int mB = psk ? (int)SymMethod::Count : mA + 1;
    for (int m = mA; m < mB; ++m) {
        RateEstimate e = estimateRate(y, m, rate, lo, hi, tFirst, nullptr, nullptr);
        if (p.rate > 0) {  // the phase at the given rate: estimate again in a narrow range
            e = estimateRate(y, m, rate, p.rate * 0.999, p.rate * 1.001, tFirst, nullptr, nullptr);
            e.rate = p.rate;
        }
        if (e.rate > 0 && (est.rate <= 0 || e.snrDb > est.snrDb)) est = e;
    }
    if (!(est.rate > 0)) {
        res.err = "No symbol rate found. Type the symbol rate.";
        return true;
    }
    d.rate = est.rate;
    const double sps = rate / d.rate;
    if (sps < 2) {
        res.err = "Fewer than 2 samples in a symbol. Make the channel filter wider, or select a lower symbol rate.";
        return true;
    }
    // the soft values: real for OOK and FSK, complex for PSK
    std::vector<float> sr;
    if (mode == DemodMode::OOK) {
        sr.resize(y.size());
        for (size_t k = 0; k < y.size(); ++k) sr[k] = std::abs(y[k]);
    } else if (mode == DemodMode::FSK) {
        sr = instFreq(y);
        for (float& v : sr) v = (float)(v * rate);
        movingAverage(sr, std::max(1, (int)(sps / 2)));
    }
    if (!psk) {
        d.threshold = p.autoThreshold ? twoMeansThreshold(sr) : p.threshold;
        for (float& v : sr) v -= (float)d.threshold;
    }
    double pw = 0;
    if (psk) {
        for (const cf& v : y) pw += std::norm(v);
    } else {
        for (float v : sr) pw += (double)v * v;
    }
    pw = std::max(kTiny, pw / (double)std::max<size_t>(1, y.size()));
    // Gardner timing recovery, from the estimated first symbol center
    double tau = (est.t0 - tFirst) * rate;
    const double g = std::clamp(p.loopGain, 0.0, 0.5);
    const size_t maxS = (size_t)std::clamp(p.maxSymbols, 1, 100'000'000);
    double prevR = 0;
    cf prevC(0, 0);
    bool first = true;
    std::vector<cf> syms;
    while (tau < (double)y.size() - 1) {
        if (d.symT.size() >= maxS) {
            d.truncated = true;
            break;
        }
        double e = 0;
        if (psk) {
            const cf cur = lerpAt(y, tau), mid = lerpAt(y, tau - sps / 2);
            if (!first) e = std::real((cur - prevC) * std::conj(mid)) / pw;
            prevC = cur;
            syms.push_back(cur);
        } else {
            const double cur = lerpAt(sr, tau), mid = lerpAt(sr, tau - sps / 2);
            if (!first) e = (cur - prevR) * mid / pw;
            prevR = cur;
            d.soft.push_back((float)cur);
        }
        d.symT.push_back(tFirst + tau / rate);
        first = false;
        tau += sps - g * std::clamp(e, -1.0, 1.0) * sps / 2;
    }
    if (psk) {
        // the residual phase in blocks of 256 symbols (x^power), unwrapped
        const size_t B = 256;
        double prevPh = 0;
        for (size_t b = 0; b < syms.size(); b += B) {
            const size_t e = std::min(syms.size(), b + B);
            std::complex<double> s = 0;
            for (size_t k = b; k < e; ++k) {
                cf v = syms[k], q = v;
                for (int i = 1; i < power; ++i) q *= v;
                s += std::complex<double>(q);
            }
            double ph = std::arg(s) / power;
            const double amb = 2 * M_PI / power;
            if (b) ph = prevPh + wrapPi((ph - prevPh) * power) / power;
            (void)amb;
            prevPh = ph;
            const double rot = -ph + (power == 4 ? M_PI / 4 : 0);
            const cf rc((float)std::cos(rot), (float)std::sin(rot));
            for (size_t k = b; k < e; ++k) syms[k] *= rc;
        }
        for (const cf& v : syms) {
            d.soft.push_back(v.real());
            d.softQ.push_back(v.imag());
        }
        if (mode == DemodMode::BPSK) {
            d.bitsPerSymbol = 1;
            for (size_t k = 0; k < syms.size(); ++k) {
                if (p.differential) {
                    if (k) d.bits.push_back(std::real(syms[k] * std::conj(syms[k - 1])) < 0);
                } else {
                    d.bits.push_back(syms[k].real() < 0);
                }
            }
        } else {
            d.bitsPerSymbol = 2;
            auto quad = [](cf v) {  // 0: +I+Q, 1: -I+Q, 2: -I-Q, 3: +I-Q
                if (v.real() >= 0) return v.imag() >= 0 ? 0 : 3;
                return v.imag() >= 0 ? 1 : 2;
            };
            static const uint8_t gray[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
            for (size_t k = 0; k < syms.size(); ++k) {
                if (p.differential) {
                    if (!k) continue;
                    const int dq = (quad(syms[k]) - quad(syms[k - 1]) + 4) % 4;
                    d.bits.push_back(gray[dq][0]);
                    d.bits.push_back(gray[dq][1]);
                } else {
                    d.bits.push_back(syms[k].real() < 0);
                    d.bits.push_back(syms[k].imag() < 0);
                }
            }
        }
    } else {
        for (float v : d.soft) d.bits.push_back(v > 0);
    }
    return !cancelled();
}

}  // namespace

// --------------------------------------------------------------------------
// Public helpers
// --------------------------------------------------------------------------
int nextPow2(int64_t n) {
    int64_t p = 1;
    while (p < n && p < (1LL << 30)) p <<= 1;
    return (int)p;
}

double windowEnbw(int window, int n) {
    const std::vector<float> w = makeWindow((WindowType)window, n);
    double s = 0, s2 = 0;
    for (float v : w) {
        s += v;
        s2 += (double)v * v;
    }
    return s > 0 ? n * s2 / (s * s) : 1.0;
}

int64_t welchPsd(const cf* x, int64_t n, int nfft, int window, double overlap, std::vector<double>& avg,
                 std::vector<double>& mx, std::vector<double>& mn) {
    const std::vector<float> w = makeWindow((WindowType)window, nfft);
    const double norm = windowNorm(w);
    const int64_t hop = std::max<int64_t>(1, (int64_t)std::llround(nfft * (1.0 - std::clamp(overlap, 0.0, 0.95))));
    WelchAcc acc;
    acc.init(nfft);
    if (n < nfft) {
        std::vector<cf> y((size_t)nfft, cf(0, 0));
        std::copy(x, x + std::max<int64_t>(0, n), y.begin());
        welchSegments(y.data(), 1, hop, nfft, w, norm, acc, nullptr);
    } else {
        welchSegments(x, (n - nfft) / hop + 1, hop, nfft, w, norm, acc, nullptr);
    }
    const double c = (double)std::max<int64_t>(1, acc.count);
    avg.resize(nfft);
    for (int i = 0; i < nfft; ++i) avg[i] = acc.sum[i] / c;
    mx = acc.mx;
    mn = acc.mn;
    return n < nfft ? 0 : acc.count;
}

double strongestLine(const std::vector<float>& v, double fs, double lo, double hi, std::vector<double>* fo,
                     std::vector<double>* dbo, double* snrDb, std::complex<double>* phasor) {
    const int64_t n = (int64_t)v.size();
    if (n < 8) return 0;
    const int nf = std::clamp(nextPow2(n), 64, 1 << 22);
    double mean = 0;
    for (float x : v) mean += x;
    mean /= (double)n;
    std::vector<cf> X((size_t)nf, cf(0, 0));
    const std::vector<float> w = makeWindow(WindowType::Hann, (int)std::min<int64_t>(n, nf));
    for (int64_t k = 0; k < std::min<int64_t>(n, nf); ++k) X[k] = cf((float)((v[k] - mean) * w[k]), 0.f);
    auto fft = getFFT(nf);
    fft->forward(X.data());
    const int half = nf / 2;
    std::vector<double> db(half);
    for (int k = 0; k < half; ++k) db[k] = dB(std::norm(X[k]));
    const double bin = fs / nf;
    const int a = std::clamp((int)std::ceil(lo / bin), 1, half - 2), b = std::clamp((int)std::floor(hi / bin), a + 1, half - 2);
    int best = a;
    for (int k = a; k <= b; ++k)
        if (db[k] > db[best]) best = k;
    // Sharp symbol edges give harmonics as strong as the fundamental: take the
    // lowest sub-multiple whose line is within 6 dB of the strongest.
    {
        const double med = percentile(std::vector<double>(db.begin() + a, db.begin() + b + 1), 0.5);
        const double need = db[best] - std::min(6.0, 0.5 * (db[best] - med));
        int pick = best;
        for (int m = 2; m <= 16; ++m) {
            const double c = (double)best / m;
            if (c < a) break;
            int kb = (int)std::lround(c);
            for (int k = std::max(a, (int)c - 2); k <= std::min(b, (int)c + 2); ++k)
                if (db[k] > db[kb]) kb = k;
            if (db[kb] >= need) pick = kb;
        }
        best = pick;
    }
    double pk = 0;
    const double off = parabolic(db.data(), half, best, &pk);
    if (snrDb) {
        std::vector<double> range(db.begin() + a, db.begin() + b + 1);
        *snrDb = pk - percentile(range, 0.5);
    }
    if (phasor) *phasor = std::complex<double>(X[best]);
    if (fo) {
        fo->resize(half);
        for (int k = 0; k < half; ++k) (*fo)[k] = k * bin;
    }
    if (dbo) *dbo = std::move(db);
    return (best + off) * bin;
}

const char* anaKindName(AnaKind k) {
    static const char* n[] = {"measure", "spectrum", "histogram", "constellation", "pulses",
                              "correlation", "symbols", "cyclo", "demod"};
    return (int)k >= 0 && (int)k < kAnaKinds ? n[(int)k] : "";
}
const char* anaKindLabel(AnaKind k) {
    static const char* n[] = {"Measurements", "Spectrum", "Histograms", "Constellation", "Pulses",
                              "Correlation", "Symbols", "Cyclostationary", "Demodulation"};
    return (int)k >= 0 && (int)k < kAnaKinds ? n[(int)k] : "";
}
bool anaKindFromName(const std::string& s, AnaKind& out) {
    for (int i = 0; i < kAnaKinds; ++i)
        if (s == anaKindName((AnaKind)i)) {
            out = (AnaKind)i;
            return true;
        }
    return false;
}
const char* histQtyName(HistQty q) {
    static const char* n[] = {"I", "Q", "AM", "FM", "PM"};
    return (int)q >= 0 && (int)q < (int)HistQty::Count ? n[(int)q] : "";
}
const char* carrierFixName(CarrierFix c) {
    static const char* n[] = {"None", "Manual", "Mean frequency", "x^N peak"};
    return (int)c >= 0 && (int)c < (int)CarrierFix::Count ? n[(int)c] : "";
}
const char* symMethodName(SymMethod m) {
    static const char* n[] = {"Envelope", "Envelope changes", "Frequency changes"};
    return (int)m >= 0 && (int)m < (int)SymMethod::Count ? n[(int)m] : "";
}
const char* symTraceName(SymTrace t) {
    static const char* n[] = {"I", "Q", "AM", "FM", "PM"};
    return (int)t >= 0 && (int)t < (int)SymTrace::Count ? n[(int)t] : "";
}
const char* demodModeName(DemodMode m) {
    static const char* n[] = {"OOK", "2-FSK", "BPSK", "QPSK"};
    return (int)m >= 0 && (int)m < (int)DemodMode::Count ? n[(int)m] : "";
}

// --------------------------------------------------------------------------
// Options (--analyze-opt)
// --------------------------------------------------------------------------
const char* analysisOptionHelp() {
    return "nfft=N  window=rect|hann|hamming|blackman|bh|flattop  overlap=PCT  band=LO:HI (Hz)\n"
           "max-samples=N  bins=N  carrier=none|mean|power|HZ  power=2|4|8  threshold=X\n"
           "hysteresis=DB  min-width=S  smooth=N  absolute=0|1  template=T0:T1  search=T0:T1\n"
           "method=envelope|envchange|frequency  rate=BD  t0=S  trace=i|q|am|fm|pm  np=N  blocks=N\n"
           "mode=ook|fsk|bpsk|qpsk  differential=0|1  loop=G";
}

bool anaSetOption(AnaReq& r, const std::string& key, const std::string& value, std::string& err) {
    double v = 0;
    const bool num = parseSI(value.c_str(), v) && std::isfinite(v);
    auto needNum = [&](double lo, double hi) {
        if (!num || v < lo || v > hi) {
            char b[160];
            snprintf(b, sizeof b, "%s wants a number from %g to %g (got '%s')", key.c_str(), lo, hi, value.c_str());
            err = b;
            return false;
        }
        return true;
    };
    auto range = [&](double& a, double& b) {
        const size_t c = value.find(':');
        if (c == std::string::npos || !parseSI(value.substr(0, c).c_str(), a) || !parseSI(value.substr(c + 1).c_str(), b) ||
            !(b > a)) {
            err = key + " wants LO:HI with HI after LO (got '" + value + "')";
            return false;
        }
        return true;
    };
    auto pick = [&](std::initializer_list<const char*> names, int& out) {
        int i = 0;
        for (const char* n : names) {
            if (value == n) {
                out = i;
                return true;
            }
            ++i;
        }
        err = key + ": unknown value '" + value + "'";
        return false;
    };
    const double fs = r.reg.src.fs;
    if (key == "nfft") {
        if (!needNum(64, 1 << 20)) return false;
        r.measure.nfft = r.spectrum.nfft = nextPow2((int64_t)v);
    } else if (key == "window") {
        int w;
        if (!pick({"rect", "hann", "hamming", "blackman", "bh", "flattop"}, w)) return false;
        r.measure.window = r.spectrum.window = w;
    } else if (key == "overlap") {
        if (!needNum(0, 95)) return false;
        r.spectrum.overlap = v / 100;
    } else if (key == "band") {
        double a, b;
        if (!range(a, b)) return false;
        r.spectrum.band = true;
        r.spectrum.fLo = a;
        r.spectrum.fHi = b;
    } else if (key == "max-samples") {
        if (!needNum(1e4, 256e6)) return false;
        r.reg.maxSamples = (int64_t)v;
    } else if (key == "bins") {
        if (!needNum(10, 4096)) return false;
        r.hist.bins = (int)v;
        r.cons.bins = std::min(512, (int)v);
    } else if (key == "carrier") {
        if (value == "none") r.cons.carrier = (int)CarrierFix::None;
        else if (value == "mean") r.cons.carrier = (int)CarrierFix::MeanFreq;
        else if (value == "power") r.cons.carrier = (int)CarrierFix::PowerN;
        else if (num) {
            r.cons.carrier = (int)CarrierFix::Manual;
            r.cons.manualHz = v;
        } else {
            err = "carrier wants none, mean, power or a frequency (got '" + value + "')";
            return false;
        }
    } else if (key == "power") {
        if (!num || (v != 2 && v != 4 && v != 8)) {
            err = "power wants 2, 4 or 8";
            return false;
        }
        r.cons.power = r.sym.power = (int)v;
    } else if (key == "threshold") {
        if (!needNum(-300, 1e12)) return false;
        r.pulse.thresholdDb = v;
        r.corr.threshold = std::clamp(v, 0.05, 1.0);
        r.digital.autoThreshold = false;
        r.digital.threshold = v;
    } else if (key == "absolute") {
        r.pulse.autoThreshold = value == "0";
    } else if (key == "hysteresis") {
        if (!needNum(0, 60)) return false;
        r.pulse.hysteresisDb = v;
    } else if (key == "min-width") {
        if (!needNum(0, 1e6)) return false;
        r.pulse.minWidthS = v;
    } else if (key == "smooth") {
        if (!needNum(1, 65536)) return false;
        r.pulse.smooth = (int)v;
    } else if (key == "template") {
        double a, b;
        if (!range(a, b)) return false;
        r.corr.search = true;
        r.corr.tmplS0 = (int64_t)std::llround(a * fs);
        r.corr.tmplS1 = (int64_t)std::llround(b * fs);
    } else if (key == "search") {
        double a, b;
        if (!range(a, b)) return false;
        r.corr.searchS0 = (int64_t)std::llround(a * fs);
        r.corr.searchS1 = (int64_t)std::llround(b * fs);
    } else if (key == "method") {
        if (!pick({"envelope", "envchange", "frequency"}, r.sym.method)) return false;
    } else if (key == "rate") {
        if (!needNum(0, 1e12)) return false;
        r.sym.rate = r.digital.rate = v;
    } else if (key == "t0") {
        if (!needNum(-1e12, 1e12)) return false;
        r.sym.t0 = v;
        r.sym.haveT0 = true;
    } else if (key == "trace") {
        if (!pick({"i", "q", "am", "fm", "pm"}, r.sym.trace)) return false;
    } else if (key == "np") {
        if (!needNum(16, 256)) return false;
        r.cyclo.np = nextPow2((int64_t)v);
    } else if (key == "blocks") {
        if (!needNum(16, 8192)) return false;
        r.cyclo.maxP = nextPow2((int64_t)v);
    } else if (key == "mode") {
        if (!pick({"ook", "fsk", "bpsk", "qpsk"}, r.digital.mode)) return false;
    } else if (key == "differential") {
        r.digital.differential = value != "0";
    } else if (key == "loop") {
        if (!needNum(0, 0.5)) return false;
        r.digital.loopGain = v;
    } else {
        err = "unknown --analyze-opt key '" + key + "'";
        return false;
    }
    return true;
}

bool anaSetOptions(AnaReq& r, const std::string& list, std::string& err) {
    size_t a = 0;
    while (a < list.size()) {
        size_t e = list.find(',', a);
        if (e == std::string::npos) e = list.size();
        const std::string kv = list.substr(a, e - a);
        a = e + 1;
        if (kv.empty()) continue;
        const size_t q = kv.find('=');
        if (q == std::string::npos) {
            err = "--analyze-opt wants KEY=VALUE (got '" + kv + "')";
            return false;
        }
        if (!anaSetOption(r, kv.substr(0, q), kv.substr(q + 1), err)) return false;
    }
    return true;
}

AnaReq anaOnlyKind(const AnaReq& r) {
    AnaReq o;
    o.kind = r.kind;
    o.reg = r.reg;
    switch (r.kind) {
        case AnaKind::Measure: o.measure = r.measure; break;
        case AnaKind::Spectrum: o.spectrum = r.spectrum; break;
        case AnaKind::Histogram: o.hist = r.hist; break;
        case AnaKind::Constellation: o.cons = r.cons; break;
        case AnaKind::Pulses: o.pulse = r.pulse; break;
        case AnaKind::Correlation: o.corr = r.corr; break;
        case AnaKind::Symbols: o.sym = r.sym; break;
        case AnaKind::Cyclo: o.cyclo = r.cyclo; break;
        case AnaKind::Demod: o.digital = r.digital; break;
        default: break;
    }
    return o;
}

std::shared_ptr<AnaRes> computeAnalysis(const AnaReq& r, const CancelFn& cancelled, Progress* progress) {
    const auto start = std::chrono::steady_clock::now();
    auto res = std::make_shared<AnaRes>();
    res->req = r;
    const AnaRegion& g = r.reg;
    if (!g.src.file) {
        res->err = "No file is open.";
        return res;
    }
    const ChannelReader rd(g.src, g.chanEnable, g.chanOffset, g.chanBw, g.taps, g.decim);
    res->rate = rd.rate();
    res->center = rd.center();
    const int64_t j0 = rd.outIndex(g.s0), j1 = rd.outIndex(g.s1);
    res->wanted = std::max<int64_t>(0, j1 - j0);
    res->t0 = rd.timeOf((double)j0);
    const bool search = r.kind == AnaKind::Correlation && r.corr.search;
    if (res->wanted < 16 && !search) {
        res->err = "The region is too short. Select a longer region.";
        return res;
    }
    const bool streams = r.kind == AnaKind::Measure || r.kind == AnaKind::Spectrum || r.kind == AnaKind::Pulses;
    const int64_t n = streams ? res->wanted : std::min(res->wanted, std::max<int64_t>(16, g.maxSamples));
    res->used = n;
    bool ok = true;
    switch (r.kind) {
        case AnaKind::Measure: ok = doMeasure(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Spectrum: ok = doSpectrum(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Histogram: ok = doHistogram(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Constellation: ok = doConstellation(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Pulses: ok = doPulses(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Correlation:
            res->corr.search = search;
            ok = search ? doSearch(r, rd, *res, cancelled, progress) : doAutocorr(r, rd, j0, n, *res, cancelled, progress);
            break;
        case AnaKind::Symbols: ok = doSymbols(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Cyclo: ok = doCyclo(r, rd, j0, n, *res, cancelled, progress); break;
        case AnaKind::Demod: ok = doDigital(r, rd, j0, n, *res, cancelled, progress); break;
        default: res->err = "Unknown analysis."; break;
    }
    if (!ok) return nullptr;
    res->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return res;
}

// --------------------------------------------------------------------------
// The text report
// --------------------------------------------------------------------------
namespace {
struct Out {
    std::string s;
    void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        s += b;
        s += '\n';
    }
};
}  // namespace

std::string analysisReport(const AnaRes& r) {
    Out o;
    o.line("# wvt analysis: %s", anaKindName(r.req.kind));
    if (!r.err.empty()) {
        o.line("# error: %s", r.err.c_str());
        return o.s;
    }
    o.line("# rate_sps=%.10g center_hz=%.10g start_s=%.10g samples=%lld of %lld", r.rate, r.center, r.t0,
           (long long)r.used, (long long)r.wanted);
    switch (r.req.kind) {
        case AnaKind::Measure: {
            const MeasureRes& m = r.measure;
            o.line("quantity,value,unit");
            o.line("mean_power,%.3f,dBFS", m.meanDb);
            o.line("peak_power,%.3f,dBFS", m.peakDb);
            o.line("papr,%.3f,dB", m.paprDb);
            o.line("dc_i,%.6g,FS", m.dcI);
            o.line("dc_q,%.6g,FS", m.dcQ);
            o.line("dc_power,%.3f,dBc", m.dcDb);
            o.line("iq_gain_imbalance,%.4f,dB", m.gainImbDb);
            o.line("iq_phase_imbalance,%.4f,deg", m.phaseImbDeg);
            o.line("carrier,%.6f,Hz", m.carrierHz);
            o.line("centroid,%.6f,Hz", m.centroidHz);
            o.line("obw99,%.6f,Hz", m.obwHz);
            o.line("obw99_low,%.6f,Hz", m.obwLo);
            o.line("obw99_high,%.6f,Hz", m.obwHi);
            o.line("bw26,%.6f,Hz", m.bw26Hz);
            o.line("noise_floor_bin,%.3f,dBFS", m.noiseDb);
            o.line("snr_obw,%.3f,dB", m.snrDb);
            o.line("rbw,%.6f,Hz", m.rbwHz);
            break;
        }
        case AnaKind::Spectrum: {
            const SpectrumRes& s = r.spectrum;
            o.line("# rbw_hz=%.6g averages=%lld total_dbfs=%.3f", s.rbwHz, (long long)s.averages, s.totalDb);
            if (r.req.spectrum.band)
                o.line("# band_dbfs=%.3f acpr_low_dbc=%.3f acpr_high_dbc=%.3f", s.bandDb, s.acprLoDb, s.acprHiDb);
            o.line("freq_hz,avg_dbfs,max_dbfs,min_dbfs");
            for (size_t i = 0; i < s.f.size(); ++i)
                o.line("%.6f,%.3f,%.3f,%.3f", s.f[i], s.avgDb[i], s.maxDb[i], s.minDb[i]);
            break;
        }
        case AnaKind::Histogram: {
            const HistogramRes& h = r.hist;
            o.line("# papr_1e-2_db=%.3f papr_1e-3_db=%.3f papr_1e-4_db=%.3f peak_db=%.3f", h.papr1e2, h.papr1e3,
                   h.papr1e4, h.peakDb);
            o.line("quantity,bin_center,count");
            for (int q = 0; q < (int)HistQty::Count; ++q) {
                const Histogram& g = h.h[q];
                const double w = (g.hi - g.lo) / std::max<size_t>(1, g.counts.size());
                for (size_t i = 0; i < g.counts.size(); ++i)
                    o.line("%s,%.6g,%.0f", histQtyName((HistQty)q), g.lo + (i + 0.5) * w, g.counts[i]);
            }
            for (size_t i = 0; i < h.ccdfDb.size(); ++i) o.line("ccdf,%.3f,%.6g", h.ccdfDb[i], h.ccdfProb[i]);
            break;
        }
        case AnaKind::Constellation: {
            const ConstellationRes& c = r.cons;
            o.line("# removed_offset_hz=%.6f scale=%.6g rms=%.6g mag_stdev=%.6g", c.removedHz, c.scale, c.rmsMag,
                   c.magStdev);
            o.line("i,q");
            for (size_t i = 0; i < c.xs.size(); ++i) o.line("%.6g,%.6g", c.xs[i], c.ys[i]);
            break;
        }
        case AnaKind::Pulses: {
            const PulseRes& p = r.pulse;
            o.line("# pulses=%zu%s noise_dbfs=%.2f threshold_dbfs=%.2f mean_width_s=%.9g mean_pri_s=%.9g duty=%.6g",
                   p.pulses.size(), p.truncated ? " (truncated)" : "", p.noiseDb, p.thresholdDb, p.meanWidth, p.meanPri,
                   p.dutyCycle);
            std::string g;
            for (double v : p.priGroups) {
                char b[40];
                snprintf(b, sizeof b, "%s%.9g", g.empty() ? "" : " ", v);
                g += b;
            }
            o.line("# pri_groups_s=%s", g.c_str());
            o.line("toa_s,width_s,pri_s,peak_dbfs,mean_dbfs,freq_hz,chirp_hz_per_s,rise_s,fall_s");
            for (const Pulse& u : p.pulses)
                o.line("%.9f,%.9g,%.9g,%.3f,%.3f,%.3f,%.6g,%.6g,%.6g", u.toa, u.width, u.pri, u.peakDb, u.meanDb, u.freqHz,
                       u.chirpHzPerS, u.rise, u.fall);
            break;
        }
        case AnaKind::Correlation: {
            const CorrelationRes& c = r.corr;
            if (!c.err.empty()) {
                o.line("# error: %s", c.err.c_str());
                break;
            }
            if (c.search) {
                o.line("# template_s=%.9g hits=%zu%s", c.tmplLen, c.peaks.size(), c.truncated ? " (truncated)" : "");
                o.line("time_s,score");
            } else {
                o.line("lag_s,correlation");
            }
            for (const CorrPeak& p : c.peaks) o.line("%.9f,%.4f", p.x, p.v);
            break;
        }
        case AnaKind::Symbols: {
            const SymbolRes& s = r.sym;
            o.line("# estimated_rate_hz=%.6f estimated_t0_s=%.9f line_snr_db=%.2f", s.estRate, s.estT0, s.lineSnrDb);
            o.line("# rate_hz=%.6f t0_s=%.9f xN_peak_hz=%.6f xN_carrier_hz=%.6f xN_peak_db=%.2f", s.rate, s.t0,
                   s.powPeakHz, s.powCarrierHz, s.powPeakDb);
            o.line("time_s,value");
            for (size_t i = 0; i < s.symT.size(); ++i) o.line("%.9f,%.6g", s.symT[i], s.symV[i]);
            break;
        }
        case AnaKind::Cyclo: {
            const CycloRes& c = r.cyclo;
            o.line("# np=%d p=%d l=%d samples=%lld", c.np, c.p, c.l, (long long)c.samples);
            o.line("alpha_hz,db");
            for (const CorrPeak& p : c.peaks) o.line("%.6f,%.3f", p.x, p.v);
            break;
        }
        case AnaKind::Demod: {
            const DigitalRes& d = r.digital;
            o.line("# mode=%s rate_hz=%.6f carrier_hz=%.6f threshold=%.6g symbols=%zu bits=%zu%s",
                   demodModeName((DemodMode)r.req.digital.mode), d.rate, d.carrierHz, d.threshold, d.symT.size(),
                   d.bits.size(), d.truncated ? " (truncated)" : "");
            std::string bits;
            bits.reserve(d.bits.size());
            for (uint8_t b : d.bits) bits += (char)('0' + b);
            for (size_t i = 0; i < bits.size(); i += 64) o.s += bits.substr(i, 64) + "\n";
            break;
        }
        default: break;
    }
    return o.s;
}
