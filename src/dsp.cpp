#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

FFT::FFT(int n) : n_(n) {
    int log2n = 0;
    while ((1 << log2n) < n) ++log2n;
    tw_.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
        double a = -2.0 * M_PI * k / n;
        tw_[k] = cf((float)std::cos(a), (float)std::sin(a));
    }
    rev_.resize(n);
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < log2n; ++b)
            if (i & (1 << b)) r |= 1 << (log2n - 1 - b);
        rev_[i] = r;
    }
}

void FFT::forward(cf* d) const {
    const int n = n_;
    for (int i = 0; i < n; ++i)
        if (i < rev_[i]) std::swap(d[i], d[rev_[i]]);
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                const cf w = tw_[j * step];
                const cf a = d[i + j];
                const cf b = d[i + j + half];
                // manual complex multiply: avoids slow NaN-checking std::complex operator*
                const float br = b.real() * w.real() - b.imag() * w.imag();
                const float bi = b.real() * w.imag() + b.imag() * w.real();
                d[i + j] = cf(a.real() + br, a.imag() + bi);
                d[i + j + half] = cf(a.real() - br, a.imag() - bi);
            }
        }
    }
}

std::shared_ptr<const FFT> getFFT(int n) {
    static std::mutex m;
    static std::map<int, std::shared_ptr<const FFT>> cache;
    std::lock_guard<std::mutex> lk(m);
    auto& p = cache[n];
    if (!p) p = std::make_shared<FFT>(n);
    return p;
}

const char* windowName(WindowType w) {
    switch (w) {
        case WindowType::Rect: return "Rectangular";
        case WindowType::Hann: return "Hann";
        case WindowType::Hamming: return "Hamming";
        case WindowType::Blackman: return "Blackman";
        case WindowType::BlackmanHarris: return "Blackman-Harris";
        case WindowType::FlatTop: return "Flat top";
        default: return "?";
    }
}

std::vector<float> makeWindow(WindowType w, int n) {
    std::vector<float> v(n, 1.f);
    if (n <= 1) return v;
    for (int i = 0; i < n; ++i) {
        double x = 2.0 * M_PI * i / (n - 1);
        double r = 1.0;
        switch (w) {
            case WindowType::Rect: r = 1.0; break;
            case WindowType::Hann: r = 0.5 - 0.5 * std::cos(x); break;
            case WindowType::Hamming: r = 0.54 - 0.46 * std::cos(x); break;
            case WindowType::Blackman: r = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x); break;
            case WindowType::BlackmanHarris:
                r = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
                break;
            case WindowType::FlatTop:
                r = 0.21557895 - 0.41663158 * std::cos(x) + 0.277263158 * std::cos(2 * x) -
                    0.083578947 * std::cos(3 * x) + 0.006947368 * std::cos(4 * x);
                break;
            default: break;
        }
        v[i] = (float)r;
    }
    return v;
}

std::vector<float> designLowpass(int taps, double fc) {
    taps = std::max(1, taps | 1);
    std::vector<float> h(taps);
    const int m = taps / 2;
    double sum = 0;
    for (int i = 0; i < taps; ++i) {
        int k = i - m;
        double s = (k == 0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * k) / (M_PI * k);
        double x = taps > 1 ? 2.0 * M_PI * i / (taps - 1) : 0.0;
        double w = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x);
        if (taps == 1) w = 1.0;
        h[i] = (float)(s * w);
        sum += h[i];
    }
    if (sum != 0)
        for (auto& v : h) v = (float)(v / sum);
    return h;
}

void parallelFor(int n, const std::function<void(int, int)>& fn, int minChunk) {
    if (n <= 0) return;
    int hw = (int)std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    int nThreads = std::max(1, std::min(hw, n / std::max(1, minChunk)));
    if (nThreads <= 1) {
        fn(0, n);
        return;
    }
    std::vector<std::thread> th;
    th.reserve(nThreads - 1);
    for (int t = 1; t < nThreads; ++t) {
        int b = (int)((int64_t)n * t / nThreads), e = (int)((int64_t)n * (t + 1) / nThreads);
        th.emplace_back([&fn, b, e] { fn(b, e); });
    }
    fn(0, (int)((int64_t)n / nThreads));
    for (auto& t : th) t.join();
}

void movingAverage(std::vector<float>& v, int len) {
    if (len <= 1 || v.size() < 2) return;
    const int n = (int)v.size();
    const int h = len / 2;
    std::vector<double> cs(n + 1, 0.0);
    std::vector<int> cnt(n + 1, 0);
    for (int i = 0; i < n; ++i) {
        bool ok = std::isfinite(v[i]);
        cs[i + 1] = cs[i] + (ok ? v[i] : 0.0);
        cnt[i + 1] = cnt[i] + (ok ? 1 : 0);
    }
    for (int i = 0; i < n; ++i) {
        int a = std::max(0, i - h), b = std::min(n, i - h + len);
        int c = cnt[b] - cnt[a];
        if (!std::isfinite(v[i])) continue;  // keep gaps as gaps
        v[i] = c > 0 ? (float)((cs[b] - cs[a]) / c) : NAN;
    }
}
