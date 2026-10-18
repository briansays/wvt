#include "audio_dsp.h"

#include <algorithm>
#include <cmath>

const char* audioModeName(AudioMode m) {
    switch (m) {
        case AudioMode::AM: return "AM";
        case AudioMode::NFM: return "NFM";
        case AudioMode::WFM: return "WFM";
        case AudioMode::USB: return "USB";
        case AudioMode::LSB: return "LSB";
        case AudioMode::CW: return "CW";
        default: return "?";
    }
}

double audioModeDefaultBw(AudioMode m) {
    switch (m) {
        case AudioMode::AM: return 10e3;
        case AudioMode::NFM: return 12.5e3;
        case AudioMode::WFM: return 200e3;
        case AudioMode::USB:
        case AudioMode::LSB: return 2.8e3;
        case AudioMode::CW: return 500;
        default: return 10e3;
    }
}

const char* deemphasisName(Deemphasis d) {
    switch (d) {
        case Deemphasis::Off: return "Off";
        case Deemphasis::Us50: return "50 us";
        case Deemphasis::Us75: return "75 us";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// FIR decimator
// ---------------------------------------------------------------------------
template <class T>
void FirDecim<T>::design(const std::vector<float>& h, int decim) {
    n_ = std::max<int>(1, (int)h.size());
    d_ = std::max(1, decim);
    idx_.clear();
    coef_.clear();
    for (int i = 0; i < (int)h.size(); ++i)
        if (std::fabs(h[i]) > 1e-9f) {
            idx_.push_back(i);
            coef_.push_back(h[i]);
        }
    reset();
}

template <class T>
void FirDecim<T>::reset() {
    buf_.assign((size_t)n_ - 1, T{});
    skip_ = 0;
}

template <class T>
void FirDecim<T>::process(const T* in, size_t n, std::vector<T>& out) {
    out.clear();
    const size_t hist = (size_t)n_ - 1;
    buf_.insert(buf_.end(), in, in + n);
    size_t e = hist + skip_;  // newest input sample of the next output
    if (e < buf_.size()) out.reserve((buf_.size() - e) / d_ + 1);
    const size_t K = idx_.size();
    const int* idx = idx_.data();
    const float* c = coef_.data();
    for (; e < buf_.size(); e += d_) {
        const T* x = &buf_[e - hist];
        T acc{};
        for (size_t j = 0; j < K; ++j) acc += c[j] * x[idx[j]];
        out.push_back(acc);
    }
    skip_ = (int)(e - buf_.size());
    buf_.erase(buf_.begin(), buf_.end() - hist);
}

template class FirDecim<cf>;
template class FirDecim<float>;

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------
void Resampler::design(double ratio, double cutoff) {
    step_ = 1.0 / ratio;
    const double c = std::clamp(cutoff, 1e-4, 0.5);
    table_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
    for (int ph = 0; ph <= kPhases; ++ph) {
        float* h = &table_[(size_t)ph * kTaps];
        double sum = 0;
        for (int k = 0; k < kTaps; ++k) {
            // Distance from the output instant, which lies `frac` after tap kTaps/2-1.
            const double x = k - (kTaps / 2 - 1) - (double)ph / kPhases;
            const double s = x == 0 ? 2 * c : std::sin(2 * M_PI * c * x) / (M_PI * x);
            const double u = (x + kTaps / 2.0) / kTaps;
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * u) + 0.08 * std::cos(4 * M_PI * u);
            h[k] = (float)(s * w);
            sum += h[k];
        }
        if (sum != 0)
            for (int k = 0; k < kTaps; ++k) h[k] = (float)(h[k] / sum);
    }
    reset();
}

void Resampler::reset() {
    buf_.assign(kTaps / 2, 0.f);
    t_ = 0;
}

void Resampler::process(const float* in, size_t n, std::vector<float>& out) {
    buf_.insert(buf_.end(), in, in + n);
    for (;;) {
        const size_t i = (size_t)t_;
        if (i + kTaps > buf_.size()) break;
        const double fp = (t_ - (double)i) * kPhases;
        const int ph = std::min(kPhases - 1, (int)fp);
        const float mu = (float)(fp - ph);
        const float* h0 = &table_[(size_t)ph * kTaps];
        const float* h1 = h0 + kTaps;
        const float* x = &buf_[i];
        float a = 0, b = 0;
        for (int k = 0; k < kTaps; ++k) {
            a += h0[k] * x[k];
            b += h1[k] * x[k];
        }
        out.push_back(a + mu * (b - a));
        t_ += step_;
    }
    const size_t drop = std::min((size_t)t_, buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + drop);
    t_ -= (double)drop;
}

// ---------------------------------------------------------------------------
// Demodulator
// ---------------------------------------------------------------------------
namespace {
// Highest audio frequency each mode needs.
double audioCutoff(const AudioParams& p) {
    switch ((AudioMode)p.mode) {
        case AudioMode::AM: return p.bw / 2;
        case AudioMode::NFM: return std::min(p.bw / 2, 5e3);
        case AudioMode::WFM: return 15e3;
        case AudioMode::USB:
        case AudioMode::LSB: return p.bw;
        case AudioMode::CW: return 2000 + p.bw / 2;  // covers any BFO pitch
        default: return p.bw / 2;
    }
}

// Coefficient of a one-pole low-pass with time constant tau at rate fs.
float onePole(double tau, double fs) { return (float)(1.0 - std::exp(-1.0 / std::max(1e-9, tau * fs))); }

std::complex<double> rotation(double hz, double fs) { return std::polar(1.0, 2 * M_PI * hz / fs); }
}  // namespace

AudioDemod::AudioDemod(double inRate, double outRate) : inRate_(inRate), outRate_(outRate) {}

void AudioDemod::setParams(const AudioParams& p) {
    const bool rebuild = !built_ || p.mode != p_.mode || p.bw != p_.bw || p.deemph != p_.deemph;
    p_ = p;
    p_.bfo = std::clamp(p_.bfo, 100.0, 2000.0);
    gainLin_ = (float)std::pow(10.0, p_.gainDb / 20);
    sqOpenThr_ = (float)std::pow(10.0, p_.squelchDb / 10);
    sqCloseThr_ = sqOpenThr_ * 0.5f;  // 3 dB hysteresis
    if (rebuild) {
        build();
        return;
    }
    rotStep_ = rotation(-p_.offset, inRate_);
    bfoStep_ = rotation(shiftHz(), ifRate_);
}

double AudioDemod::shiftHz() const {
    switch ((AudioMode)p_.mode) {
        case AudioMode::USB: return p_.bw / 2;
        case AudioMode::LSB: return -p_.bw / 2;
        case AudioMode::CW: return p_.bfo;
        default: return 0;
    }
}

void AudioDemod::setRatioTrim(double trim) {
    trim_ = trim;
    rs_.setRatio(outRate_ / audRate_ * (1 + trim_));
}

void AudioDemod::build() {
    built_ = true;
    const double bw = std::clamp(p_.bw, 50.0, inRate_);
    const double acut = audioCutoff(p_);

    // Half-band stages down to an IF rate of at least twice the channel width,
    // and high enough for the audio.
    const double minIf = std::max(2 * bw, 2.2 * acut);
    halfbands_.clear();
    double r = inRate_;
    while (r / 2 >= minIf && halfbands_.size() < 20) {
        // A short filter is enough while the channel is narrow compared with
        // the stage: only the band near Fs/2 folds onto it.
        const double rel = bw / 2 / r;
        FirDecim<cf> f;
        f.design(designLowpass(rel < 0.05 ? 15 : 31, 0.25), 2);
        halfbands_.push_back(std::move(f));
        r /= 2;
    }
    ifRate_ = r;
    const int taps = std::clamp((int)std::ceil(44 * r / bw), 31, 511) | 1;
    chan_.design(designLowpass(taps, std::min(bw / 2 / r, 0.49)), 1);

    const double tau = (Deemphasis)p_.deemph == Deemphasis::Us50 ? 50e-6 : (Deemphasis)p_.deemph == Deemphasis::Us75 ? 75e-6 : 0;
    deA_ = tau > 0 ? onePole(tau, r) : 0.f;
    dcP_ = (float)std::max(0.9, 1 - 2 * M_PI * 20 / r);
    pwrA_ = onePole(0.02, r);
    sqA_ = onePole(0.005, r);
    carA_ = onePole(0.1, r);
    agcAtk_ = onePole(0.002, r);
    agcRel_ = onePole(0.3, r);
    fmScale_ = (float)(r / (2 * M_PI) / ((AudioMode)p_.mode == AudioMode::WFM ? 75e3 : std::max(500.0, 0.2 * bw)) * 0.5);

    // Audio: real half-band stages while the rate stays at or above the output
    // rate, then the resampler (which also sets the audio bandwidth).
    audioHalfbands_.clear();
    double a = r;
    while (a / 2 >= outRate_ && audioHalfbands_.size() < 20) {
        FirDecim<float> f;
        f.design(designLowpass(31, 0.25), 2);
        audioHalfbands_.push_back(std::move(f));
        a /= 2;
    }
    audRate_ = a;
    rs_.design(outRate_ / a * (1 + trim_), std::min({acut, 0.45 * outRate_, 0.45 * a}) / a);

    rotStep_ = rotation(-p_.offset, inRate_);
    bfoStep_ = rotation(shiftHz(), ifRate_);
    reset();
}

void AudioDemod::reset() {
    if (!built_) return;
    for (auto& f : halfbands_) f.reset();
    chan_.reset();
    for (auto& f : audioHalfbands_) f.reset();
    rs_.reset();
    rot_ = bfoRot_ = {1, 0};
    prev_ = {0, 0};
    carrier_ = agcEnv_ = deY_ = dcX_ = dcY_ = 0;
    pwr_ = 0;
    sqOpen_ = !p_.squelch;
    sqGain_ = sqOpen_ ? 1.f : 0.f;
}

void AudioDemod::process(const cf* in, size_t n, std::vector<float>& out) {
    if (!built_) build();
    if (n == 0) return;

    // 1. Shift the channel to 0 Hz.
    w0_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float rr = (float)rot_.real(), ri = (float)rot_.imag();
        const float xr = in[i].real(), xi = in[i].imag();
        w0_[i] = cf(xr * rr - xi * ri, xr * ri + xi * rr);
        rot_ *= rotStep_;
        if (++rotCount_ == 4096) {
            rot_ /= std::abs(rot_);
            rotCount_ = 0;
        }
    }

    // 2. Decimate and filter the channel.
    for (auto& f : halfbands_) {
        f.process(w0_.data(), w0_.size(), w1_);
        w0_.swap(w1_);
    }
    chan_.process(w0_.data(), w0_.size(), w1_);

    // 3. Demodulate at the IF rate.
    const AudioMode mode = (AudioMode)p_.mode;
    a0_.resize(w1_.size());
    for (size_t k = 0; k < w1_.size(); ++k) {
        cf y = w1_[k];
        const float p = std::norm(y);
        pwr_ += pwrA_ * (p - pwr_);
        float v = 0;
        switch (mode) {
            case AudioMode::AM: {
                const float env = std::sqrt(p);
                if (p_.agc) {
                    if (carrier_ <= 0) carrier_ = env;  // start at the first sample, not from zero
                    carrier_ += carA_ * (env - carrier_);
                    v = env / std::max(carrier_, 1e-9f) - 1.f;
                } else {
                    v = env;
                }
                break;
            }
            case AudioMode::NFM:
            case AudioMode::WFM: {
                const float dr = y.real() * prev_.real() + y.imag() * prev_.imag();
                const float di = y.imag() * prev_.real() - y.real() * prev_.imag();
                v = std::atan2(di, dr) * fmScale_;
                prev_ = y;
                break;
            }
            default: {  // USB, LSB, CW
                if (p_.agc) {
                    const float env = std::sqrt(p);
                    if (agcEnv_ <= 0) agcEnv_ = env;
                    agcEnv_ += (env > agcEnv_ ? agcAtk_ : agcRel_) * (env - agcEnv_);
                    y *= 0.3f / std::max(agcEnv_, 1e-4f);
                }
                const float br = (float)bfoRot_.real(), bi = (float)bfoRot_.imag();
                v = y.real() * br - y.imag() * bi;
                bfoRot_ *= bfoStep_;
                if ((k & 1023) == 0) bfoRot_ /= std::abs(bfoRot_);
                break;
            }
        }
        if (deA_ > 0) {
            deY_ += deA_ * (v - deY_);
            v = deY_;
        }
        // DC block (carrier offset in FM, carrier level in AM)
        const float dc = v - dcX_ + dcP_ * dcY_;
        dcX_ = v;
        dcY_ = dc;
        v = dc;
        if (p_.squelch) {
            if (sqOpen_ ? pwr_ < sqCloseThr_ : pwr_ > sqOpenThr_) sqOpen_ = !sqOpen_;
        } else {
            sqOpen_ = true;
        }
        sqGain_ += sqA_ * ((sqOpen_ ? 1.f : 0.f) - sqGain_);
        a0_[k] = v * sqGain_ * gainLin_;
    }

    // 4. Down to the audio rate.
    for (auto& f : audioHalfbands_) {
        f.process(a0_.data(), a0_.size(), a1_);
        a0_.swap(a1_);
    }
    rs_.process(a0_.data(), a0_.size(), out);
}
