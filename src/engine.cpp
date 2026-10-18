#include "engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "gpu.h"

static constexpr float kEps = 1e-20f;

// ---------------------------------------------------------------------------
// Waterfall
// ---------------------------------------------------------------------------
std::shared_ptr<WaterfallRes> computeWaterfall(const WaterfallReq& r, const CancelFn& cancelled) {
    auto res = std::make_shared<WaterfallRes>();
    res->req = r;
    const Source& src = r.src;
    if (!src.file) return res;
    const int n = isPow2(r.nfft) ? r.nfft : 1024;
    const int64_t span = std::max<int64_t>(1, r.s1 - r.s0);

    const double hopUser = std::max(1.0, n * (1.0 - std::clamp(r.overlap, 0.0, 0.95)));
    int rows = (int)std::ceil(span / hopUser);
    rows = std::clamp(rows, std::min(r.minRows, r.maxRows), r.maxRows);
    rows = std::max(rows, 1);
    const double hop = (double)span / rows;
    const int cols = std::min(n, std::max(16, r.maxCols));
    const int binsPerCol = n / cols;
    // Each row averages enough FFTs, at most the nominal hop apart, to cover its whole
    // time slice. Spacing them wider would skip samples, and short pulses falling
    // between FFTs would strobe (show in some rows and not others). A budget caps
    // the work for very wide views; past it, coverage drops below 100%. The GPU
    // gets a larger budget, so it keeps full coverage further out.
    constexpr double kMaxInputSamplesCpu = 256e6, kMaxInputSamplesGpu = 2e9;
    const int minAvg = std::clamp(r.avgPerRow, 1, 64);
    const int needed = (int)std::min(1e9, std::ceil(hop / hopUser));
    auto avgFor = [&](double maxInput) {
        const int budget = (int)std::max(1.0, std::floor(maxInput / ((double)rows * n)));
        return std::max(minAvg, std::min(needed, budget));
    };
    bool gpu = gpuEnabled();
    int avg = avgFor(r.maxInput > 0 ? r.maxInput : gpu ? kMaxInputSamplesGpu : kMaxInputSamplesCpu);

    res->rows = rows;
    res->cols = cols;
    res->db.assign((size_t)rows * cols, -200.f);
    res->t0 = r.s0 / src.fs;
    res->t1 = r.s1 / src.fs;
    const double binHz = src.fs / n;
    res->f0 = src.fc + (-n / 2 - 0.5) * binHz;
    res->f1 = src.fc + (n / 2 - 0.5) * binHz;

    auto fft = getFFT(n);
    const auto win = makeWindow((WindowType)r.window, n);
    double wsum = 0;
    for (float w : win) wsum += w;
    const float norm = (float)(1.0 / (wsum * wsum));

    std::vector<float> linSum((size_t)rows * cols, 0.f);  // linear power per row for spectrum
    if (gpu) {
        GpuSpectrogramJob job;
        job.file = src.file.get();
        job.fp = src.fp;
        job.n = n;
        job.rows = rows;
        job.cols = cols;
        job.avg = avg;
        job.s0 = r.s0;
        job.hop = hop;
        job.win = win.data();
        job.scale = norm / avg;
        job.lin = linSum.data();
        job.db = res->db.data();
        const GpuResult g = gpuSpectrogram(job, cancelled);
        if (g == GpuResult::Cancelled) return nullptr;
        if (g == GpuResult::Failed) {
            gpu = false;
            avg = avgFor(r.maxInput > 0 ? r.maxInput : kMaxInputSamplesCpu);
        }
    }
    res->gpu = gpu;
    res->fftsPerRow = avg;
    res->coverage = std::min(1.0, (double)avg * n / hop);
    std::atomic<bool> aborted{false};

    if (!gpu) parallelFor(rows, [&](int rb, int re) {
        std::vector<cf> buf(n);
        std::vector<float> pw(n);
        for (int row = rb; row < re; ++row) {
            if (cancelled()) { aborted = true; return; }
            std::fill(pw.begin(), pw.end(), 0.f);
            for (int a = 0; a < avg; ++a) {
                if ((a & 63) == 63 && cancelled()) { aborted = true; return; }
                double center = r.s0 + row * hop + (a + 0.5) * hop / avg;
                int64_t start = (int64_t)std::llround(center) - n / 2;
                src.file->read(src.fp, start, n, buf.data());
                for (int i = 0; i < n; ++i) buf[i] *= win[i];
                fft->forward(buf.data());
                for (int i = 0; i < n; ++i) {
                    int k = (i + n / 2) & (n - 1);  // fftshift
                    pw[i] += std::norm(buf[k]);
                }
            }
            float* out = &res->db[(size_t)row * cols];
            float* lin = &linSum[(size_t)row * cols];
            const float s = norm / avg;
            for (int c = 0; c < cols; ++c) {
                float mx = 0.f;
                for (int b = 0; b < binsPerCol; ++b) mx = std::max(mx, pw[c * binsPerCol + b]);
                mx *= s;
                lin[c] = mx;
                out[c] = 10.f * std::log10(mx + kEps);
            }
        }
    }, 4);
    if (aborted || cancelled()) return nullptr;

    // Average / peak spectrum over the rows
    res->specF.resize(cols);
    res->specAvg.assign(cols, 0.0);
    res->specPeak.assign(cols, 0.0);
    const double colHz = (res->f1 - res->f0) / cols;
    for (int c = 0; c < cols; ++c) res->specF[c] = res->f0 + (c + 0.5) * colHz;
    for (int row = 0; row < rows; ++row)
        for (int c = 0; c < cols; ++c) {
            float v = linSum[(size_t)row * cols + c];
            res->specAvg[c] += v;
            res->specPeak[c] = std::max(res->specPeak[c], (double)v);
        }
    for (int c = 0; c < cols; ++c) {
        res->specAvg[c] = 10.0 * std::log10(res->specAvg[c] / rows + kEps);
        res->specPeak[c] = 10.0 * std::log10(res->specPeak[c] + kEps);
    }

    // Percentiles on a subsample for auto ranging
    std::vector<float> sample;
    size_t total = res->db.size(), stride = std::max<size_t>(1, total / 200000);
    sample.reserve(total / stride + 1);
    for (size_t i = 0; i < total; i += stride) sample.push_back(res->db[i]);
    if (!sample.empty()) {
        auto pct = [&](double p) {
            size_t k = std::min(sample.size() - 1, (size_t)(p * (sample.size() - 1)));
            std::nth_element(sample.begin(), sample.begin() + k, sample.end());
            return sample[k];
        };
        res->p5 = pct(0.05);
        res->p50 = pct(0.5);
        res->p999 = pct(0.999);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Demodulation
// ---------------------------------------------------------------------------
ChanDesign chanDesign(const DemodReq& r) {
    ChanDesign d;
    if (!r.chanEnable) return d;
    const double fs = r.src.fs;
    const double bw = std::clamp(r.chanBw, fs * 1e-5, fs);
    d.decim = r.decim > 0 ? r.decim : std::max(1, (int)std::floor(fs / (2.0 * bw)));
    d.taps = r.taps > 0 ? (r.taps | 1) : (std::clamp((int)std::lround(12.0 * fs / bw), 15, 8191) | 1);
    return d;
}

namespace {

struct RawChannels {
    std::vector<double> t;
    std::vector<float> i, q, am, fm, pm, pwr, amLin;
};

// Channelize + demodulate output samples j in [jA, jB) where output j sits at
// input sample index j*D. Everything is indexed by absolute sample number so
// results don't shift when the view pans.
bool processRange(const DemodReq& r, const ChanDesign& cd, const std::vector<float>& h, int64_t jA, int64_t jB,
                  RawChannels& out, const CancelFn& cancelled, bool threaded = true) {
    const int thr = threaded ? 1 : (1 << 30);  // parallelFor minChunk; huge = run inline
    const Source& src = r.src;
    const int D = cd.decim;
    const int64_t M = jB - jA + 1;  // +1 leading sample for the FM difference
    if (M < 2) return true;
    const int64_t j0 = jA - 1;
    std::vector<cf> y(M);

    if (!r.chanEnable) {
        src.file->read(src.fp, j0, M, y.data());
    } else {
        const int N = (int)h.size();
        const int half = N / 2;
        const int64_t inStart = j0 * D - half;
        const int64_t inLen = (M - 1) * D + N;
        std::vector<cf> x(inLen);
        src.file->read(src.fp, inStart, inLen, x.data());
        if (cancelled()) return false;
        // Frequency shift by -offset, phase referenced to absolute sample index.
        const double frac = r.chanOffset / src.fs;
        if (frac != 0.0) {
            parallelFor((int)((inLen + 4095) / 4096), [&](int cb, int ce) {
                for (int c = cb; c < ce; ++c) {
                    int64_t a = (int64_t)c * 4096, b = std::min<int64_t>(inLen, a + 4096);
                    double cyc = std::fmod(frac * (double)(inStart + a), 1.0);
                    std::complex<double> rot = std::polar(1.0, -2.0 * M_PI * cyc);
                    const std::complex<double> step = std::polar(1.0, -2.0 * M_PI * frac);
                    for (int64_t k = a; k < b; ++k) {
                        const float rr = (float)rot.real(), ri = (float)rot.imag();
                        const float xr = x[k].real(), xi = x[k].imag();
                        x[k] = cf(xr * rr - xi * ri, xr * ri + xi * rr);
                        rot *= step;
                    }
                }
            }, thr);
        }
        if (cancelled()) return false;
        // FIR low-pass + decimate (only the kept outputs are computed)
        std::atomic<bool> ab{false};
        parallelFor((int)M, [&](int b, int e) {
            for (int j = b; j < e; ++j) {
                if ((j & 8191) == 0 && cancelled()) { ab = true; return; }
                const cf* xp = &x[(size_t)j * D];
                float ar = 0, ai = 0;
                for (int m = 0; m < N; ++m) {
                    ar += h[m] * xp[m].real();
                    ai += h[m] * xp[m].imag();
                }
                y[j] = cf(ar, ai);
            }
        }, threaded ? 2048 : (1 << 30));
        if (ab) return false;
    }
    if (cancelled()) return false;

    const double fr = src.fs / D;
    const size_t K = (size_t)(M - 1);
    out.t.resize(K);
    out.i.resize(K);
    out.q.resize(K);
    out.am.resize(K);
    out.amLin.resize(K);
    out.fm.resize(K);
    out.pm.resize(K);
    out.pwr.resize(K);
    const float fmScale = (float)(fr / (2.0 * M_PI));
    const float fmOffset = (r.chanEnable && !r.fmRelative) ? (float)r.chanOffset : 0.f;
    std::vector<float> pmRaw(K);
    for (size_t k = 0; k < K; ++k) {
        const cf a = y[k + 1], p = y[k];
        out.t[k] = (double)((jA + (int64_t)k) * D) / src.fs;
        out.i[k] = a.real();
        out.q[k] = a.imag();
        const float pw = a.real() * a.real() + a.imag() * a.imag();
        out.pwr[k] = pw;
        out.amLin[k] = std::sqrt(pw);
        // a * conj(p)
        const float dr = a.real() * p.real() + a.imag() * p.imag();
        const float di = a.imag() * p.real() - a.real() * p.imag();
        out.fm[k] = std::atan2(di, dr) * fmScale + fmOffset;
        pmRaw[k] = std::atan2(a.imag(), a.real());
    }

    // AM
    std::vector<float> env = out.amLin;
    movingAverage(env, r.amSmooth);
    out.amLin = env;
    switch ((AmMode)r.amMode) {
        case AmMode::Linear: out.am = env; break;
        case AmMode::Decibel:
            for (size_t k = 0; k < K; ++k) out.am[k] = 20.f * std::log10(env[k] + 1e-10f);
            break;
        case AmMode::Normalized: {
            double s = 0;
            for (float v : env) s += v;
            float mean = (float)(s / std::max<size_t>(1, K));
            for (size_t k = 0; k < K; ++k) out.am[k] = mean > 0 ? env[k] / mean - 1.f : 0.f;
            break;
        }
    }

    // PM
    if ((PmMode)r.pmMode != PmMode::Wrapped) {
        double acc = 0;
        float prev = pmRaw.empty() ? 0.f : pmRaw[0];
        for (size_t k = 0; k < K; ++k) {
            float d = pmRaw[k] - prev;
            if (d > (float)M_PI) d -= 2.f * (float)M_PI;
            else if (d < -(float)M_PI) d += 2.f * (float)M_PI;
            acc += d;
            prev = pmRaw[k];
            out.pm[k] = (float)acc + (K ? pmRaw[0] : 0.f);
        }
        if ((PmMode)r.pmMode == PmMode::Detrended && K > 1) {
            // least-squares line removal (removes residual carrier offset)
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t k = 0; k < K; ++k) {
                double xk = (double)k;
                sx += xk; sy += out.pm[k]; sxx += xk * xk; sxy += xk * out.pm[k];
            }
            double den = K * sxx - sx * sx;
            double b = den != 0 ? (K * sxy - sx * sy) / den : 0;
            double a = (sy - b * sx) / K;
            for (size_t k = 0; k < K; ++k) out.pm[k] = (float)(out.pm[k] - (a + b * k));
        }
    } else {
        out.pm = pmRaw;
    }
    movingAverage(out.pm, r.pmSmooth);
    if (r.pmDegrees)
        for (auto& v : out.pm) v *= (float)(180.0 / M_PI);

    // FM
    movingAverage(out.fm, r.fmSmooth);

    // Squelch
    if (r.squelch) {
        std::vector<float> p = out.pwr;
        movingAverage(p, std::max(r.fmSmooth, 8));
        const float thr = (float)std::pow(10.0, r.squelchDb / 10.0);
        for (size_t k = 0; k < K; ++k)
            if (p[k] < thr) out.fm[k] = out.pm[k] = NAN;
    }
    for (auto& v : out.pwr) v = 10.f * std::log10(v + kEps);
    return true;
}

// Reduces a series to <= 2*P points by emitting each bucket's min and max in
// time order, so the visual envelope is exact.
void reduceInto(const std::vector<double>& t, const std::vector<float>& y, int P, Series& s) {
    const size_t M = y.size();
    if (M <= (size_t)(2 * P)) {
        s.t.insert(s.t.end(), t.begin(), t.end());
        s.y.insert(s.y.end(), y.begin(), y.end());
        return;
    }
    for (int b = 0; b < P; ++b) {
        size_t a = M * b / P, e = M * (b + 1) / P;
        size_t imin = SIZE_MAX, imax = SIZE_MAX;
        for (size_t k = a; k < e; ++k) {
            if (!std::isfinite(y[k])) continue;
            if (imin == SIZE_MAX || y[k] < y[imin]) imin = k;
            if (imax == SIZE_MAX || y[k] > y[imax]) imax = k;
        }
        if (imin == SIZE_MAX) {
            s.t.push_back(t[a]);
            s.y.push_back(NAN);
            continue;
        }
        size_t f = std::min(imin, imax), g = std::max(imin, imax);
        s.t.push_back(t[f]);
        s.y.push_back(y[f]);
        if (g != f) {
            s.t.push_back(t[g]);
            s.y.push_back(y[g]);
        }
    }
}

struct StatAcc {
    double sum = 0, sum2 = 0, mn = std::numeric_limits<double>::infinity(), mx = -std::numeric_limits<double>::infinity();
    int64_t n = 0;
    void add(const std::vector<float>& v) {
        for (float f : v) {
            if (!std::isfinite(f)) continue;
            sum += f; sum2 += (double)f * f; mn = std::min(mn, (double)f); mx = std::max(mx, (double)f); ++n;
        }
    }
    void merge(const StatAcc& o) {
        sum += o.sum; sum2 += o.sum2; mn = std::min(mn, o.mn); mx = std::max(mx, o.mx); n += o.n;
    }
    Stats get() const {
        Stats s;
        s.n = n;
        if (!n) return s;
        s.mean = sum / n;
        s.rms = std::sqrt(std::max(0.0, sum2 / n - s.mean * s.mean));
        s.min = mn;
        s.max = mx;
        return s;
    }
};

}  // namespace

std::shared_ptr<DemodRes> computeDemod(const DemodReq& r, const CancelFn& cancelled) {
    auto res = std::make_shared<DemodRes>();
    res->req = r;
    if (!r.src.file || r.s1 <= r.s0) return res;
    const ChanDesign cd = chanDesign(r);
    const int D = cd.decim;
    res->decim = D;
    res->taps = cd.taps;
    res->outRate = r.src.fs / D;
    std::vector<float> h;
    if (r.chanEnable) {
        const double bw = std::clamp(r.chanBw, r.src.fs * 1e-5, r.src.fs);
        h = designLowpass(cd.taps, 0.5 * bw / r.src.fs);
    }

    const int64_t jA = (r.s0 + D - 1) / D;
    const int64_t jB = std::max(jA + 1, (r.s1 + D - 1) / D);
    const int64_t nOut = jB - jA;
    res->nOut = nOut;
    const int P = std::max(100, r.displayPoints);
    StatAcc aI, aAm, aFm, aPm, aPw, aEnv;

    auto add = [&](RawChannels& c) {
        reduceInto(c.t, c.i, P, res->i);
        reduceInto(c.t, c.q, P, res->q);
        reduceInto(c.t, c.am, P, res->am);
        reduceInto(c.t, c.fm, P, res->fm);
        reduceInto(c.t, c.pm, P, res->pm);
        reduceInto(c.t, c.pwr, P, res->pwr);
        aI.add(c.i); aAm.add(c.am); aFm.add(c.fm); aPm.add(c.pm); aPw.add(c.pwr); aEnv.add(c.amLin);
    };

    // Dense processing holds the whole input span in memory; cap it (~256 MB).
    const int64_t kMaxDenseInput = 32'000'000;
    if (nOut <= r.maxDense && nOut * D <= kMaxDenseInput) {
        RawChannels c;
        if (!processRange(r, cd, h, jA, jB, c, cancelled)) return nullptr;
        add(c);
    } else {
        // Sparse: demodulate a short contiguous block at each display bucket,
        // buckets in parallel (each block is processed single-threaded).
        res->sparse = true;
        const int64_t block = std::clamp<int64_t>(r.maxDense / P, 64, 8192);
        struct Bucket {
            Series i, q, am, fm, pm, pwr;
            StatAcc aI, aAm, aFm, aPm, aPw, aEnv;
        };
        std::vector<Bucket> bk(P);
        std::atomic<bool> ab{false};
        parallelFor(P, [&](int b0, int b1) {
            for (int b = b0; b < b1; ++b) {
                if (cancelled()) { ab = true; return; }
                int64_t a = jA + nOut * b / P;
                RawChannels c;
                if (!processRange(r, cd, h, a, std::min(jB, a + block), c, cancelled, false)) { ab = true; return; }
                Bucket& o = bk[b];
                // collapse each block to its min/max pair
                reduceInto(c.t, c.i, 1, o.i);
                reduceInto(c.t, c.q, 1, o.q);
                reduceInto(c.t, c.am, 1, o.am);
                reduceInto(c.t, c.fm, 1, o.fm);
                reduceInto(c.t, c.pm, 1, o.pm);
                reduceInto(c.t, c.pwr, 1, o.pwr);
                o.aI.add(c.i); o.aAm.add(c.am); o.aFm.add(c.fm); o.aPm.add(c.pm); o.aPw.add(c.pwr); o.aEnv.add(c.amLin);
            }
        }, 8);
        if (ab) return nullptr;
        auto app = [](Series& d, const Series& s) {
            d.t.insert(d.t.end(), s.t.begin(), s.t.end());
            d.y.insert(d.y.end(), s.y.begin(), s.y.end());
        };
        for (auto& o : bk) {
            app(res->i, o.i); app(res->q, o.q); app(res->am, o.am); app(res->fm, o.fm); app(res->pm, o.pm); app(res->pwr, o.pwr);
            aI.merge(o.aI); aAm.merge(o.aAm); aFm.merge(o.aFm); aPm.merge(o.aPm); aPw.merge(o.aPw); aEnv.merge(o.aEnv);
        }
    }
    res->sI = aI.get();
    res->sAm = aAm.get();
    res->sFm = aFm.get();
    res->sPm = aPm.get();
    res->sPwr = aPw.get();
    Stats env = aEnv.get();
    res->amIndex = (env.max + env.min) > 0 ? (env.max - env.min) / (env.max + env.min) : 0;
    return res;
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------
std::shared_ptr<OverviewRes> computeOverview(const OverviewReq& r, const CancelFn& cancelled) {
    auto res = std::make_shared<OverviewRes>();
    if (!r.src.file) return res;
    const int64_t N = r.src.file->numSamples(r.src.fp);
    if (N <= 0) return res;
    const int P = (int)std::min<int64_t>(r.points, N);
    res->t.resize(P);
    res->mean.resize(P);
    res->peak.resize(P);
    std::atomic<bool> ab{false};
    parallelFor(P, [&](int b0, int b1) {
        std::vector<cf> buf;
        for (int b = b0; b < b1; ++b) {
            if ((b & 31) == 0 && cancelled()) { ab = true; return; }
            int64_t a = N * b / P, e = N * (b + 1) / P;
            // Read at most 4 short probes spread over the bucket.
            const int64_t probe = 2048;
            double sum = 0, pk = 0;
            int64_t cnt = 0;
            int nProbes = (e - a) > 4 * probe ? 4 : 1;
            for (int pI = 0; pI < nProbes; ++pI) {
                int64_t ps = a + (e - a) * pI / nProbes;
                int64_t len = std::min(probe, e - ps);
                if (len <= 0) continue;
                buf.resize(len);
                r.src.file->read(r.src.fp, ps, len, buf.data());
                for (const cf& v : buf) {
                    double p = std::norm(v);
                    sum += p;
                    pk = std::max(pk, p);
                }
                cnt += len;
            }
            res->t[b] = ((a + e) * 0.5) / r.src.fs;
            res->mean[b] = 10.0 * std::log10(sum / std::max<int64_t>(1, cnt) + kEps);
            res->peak[b] = 10.0 * std::log10(pk + kEps);
        }
    }, 16);
    if (ab) return nullptr;
    return res;
}
