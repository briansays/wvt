#include "siggen.h"

#include <algorithm>
#include <cstdio>

#include "ui_util.h"

namespace {
constexpr double kTwoPi = 2 * M_PI;

const char* barkerCode(int len) {
    switch (len) {
        case 2: return "+-";
        case 3: return "++-";
        case 4: return "++-+";
        case 5: return "+++-+";
        case 7: return "+++--+-";
        case 11: return "+++---+--+-";
        case 13: return "+++++--++-+-+";
        default: return nullptr;
    }
}

// Rising edge shape over x in [0, 1].
double edgeShape(EdgeShape e, double x) {
    x = std::clamp(x, 0.0, 1.0);
    switch (e) {
        case EdgeShape::Linear: return x;
        case EdgeShape::RaisedCos: return 0.5 - 0.5 * std::cos(M_PI * x);
        case EdgeShape::Gaussian: return std::exp(-4.6 * (1 - x) * (1 - x));  // 1% at the start of the edge
        default: return 1;
    }
}

uint64_t xorshift(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}
double uniform(uint64_t& s) { return (double)(xorshift(s) >> 11) * (1.0 / 9007199254740992.0); }

std::string num(double v) {
    char b[40];
    snprintf(b, sizeof b, "%.12g", v);
    return b;
}
}  // namespace

const char* edgeName(EdgeShape e) {
    switch (e) {
        case EdgeShape::Rect: return "Rectangular";
        case EdgeShape::Linear: return "Linear";
        case EdgeShape::RaisedCos: return "Raised cosine";
        case EdgeShape::Gaussian: return "Gaussian";
        default: return "?";
    }
}

const char* priPatternName(PriPattern p) {
    switch (p) {
        case PriPattern::Fixed: return "Fixed";
        case PriPattern::Stagger: return "Stagger";
        case PriPattern::Jitter: return "Jitter";
        default: return "?";
    }
}

const char* intraModName(IntraMod m) {
    switch (m) {
        case IntraMod::None: return "None";
        case IntraMod::LfmUp: return "LFM up-chirp";
        case IntraMod::LfmDown: return "LFM down-chirp";
        case IntraMod::LfmTriangle: return "LFM triangle";
        case IntraMod::Barker: return "Barker code";
        default: return "?";
    }
}

const std::vector<int>& barkerLengths() {
    static const std::vector<int> v{2, 3, 4, 5, 7, 11, 13};
    return v;
}

double WaveformParams::minPri() const {
    switch (priPattern) {
        case PriPattern::Stagger: return stagger.empty() ? 0 : *std::min_element(stagger.begin(), stagger.end());
        case PriPattern::Jitter: return pri * (1 - jitterPct / 100);
        default: return pri;
    }
}

double WaveformParams::meanPri() const {
    if (priPattern == PriPattern::Stagger && !stagger.empty()) {
        double s = 0;
        for (double v : stagger) s += v;
        return s / (double)stagger.size();
    }
    return pri;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
ParamCheck checkParams(const WaveformParams& p) {
    ParamCheck c;
    auto err = [&](const std::string& s) { c.errors.push_back(s); };
    auto warn = [&](const std::string& s) { c.warnings.push_back(s); };
    if (!(p.rate > 0)) {
        err("Sample rate must be positive");
        return c;
    }
    const double nyq = p.rate / 2;
    if (!(p.ampDb <= 0)) err("Amplitude must be 0 dBFS or less");
    if (!(std::fabs(p.offset) < nyq)) err("Offset must be inside the band (|offset| < " + fmtSI(nyq, "Hz") + ")");
    if (p.mode == GenMode::CW) {
        if (p.driftHzPerSec != 0)
            warn("The tone drifts by " + fmtSI(std::fabs(p.driftHzPerSec), "Hz/s") + " and wraps around at the band edge");
    } else {
        const double pwS = p.pw * p.rate;
        if (!(pwS >= 1)) err("Pulse width must be at least one sample (" + fmtSI(1 / p.rate, "s") + ")");
        if (p.priPattern == PriPattern::Stagger) {
            if (p.stagger.empty()) err("Stagger needs at least one PRI");
            for (double v : p.stagger)
                if (!(v > 0)) err("Stagger PRIs must be positive");
        }
        if (p.priPattern == PriPattern::Jitter && !(p.jitterPct >= 0 && p.jitterPct < 100))
            err("Jitter must be 0-99 %");
        const double minPri = p.minPri();
        if (!((minPri - p.pw) * p.rate >= 1))
            err("PRI must be longer than the pulse width" +
                std::string(p.priPattern == PriPattern::Fixed ? "" : " (shortest PRI " + fmtSI(minPri, "s") + ")"));
        if (p.edge != EdgeShape::Rect) {
            if (p.rise < 0 || p.fall < 0) err("Rise and fall times can't be negative");
            else if (p.rise + p.fall > p.pw * (1 + 1e-9)) err("Rise + fall must fit inside the pulse width");
            else if ((p.rise > 0 && p.rise * p.rate < 2) || (p.fall > 0 && p.fall * p.rate < 2))
                warn("Edges shorter than 2 samples are effectively rectangular at this rate");
        }
        if (p.burstPulses < 0) err("Pulses per burst can't be negative");
        if (p.burstGap < 0) err("Burst gap can't be negative");
        if (p.startDelay < 0) err("Start delay can't be negative");
        if (p.intra == IntraMod::LfmUp || p.intra == IntraMod::LfmDown || p.intra == IntraMod::LfmTriangle) {
            if (!(p.chirpBw > 0)) err("Chirp bandwidth must be positive");
            else if (std::fabs(p.offset) + p.chirpBw / 2 > nyq)
                warn("The chirp runs past the band edge and will alias");
        }
        if (p.intra == IntraMod::Barker) {
            if (!barkerCode(p.barkerLen)) err("Barker codes exist only for lengths 2, 3, 4, 5, 7, 11 and 13");
            else if (pwS / p.barkerLen < 1) err("Barker chips would be shorter than one sample: widen the pulse or use a shorter code");
        }
    }
    if (p.noise && !std::isfinite(p.snrDb)) err("SNR must be a number");
    if (std::fabs(p.dcI) > 1 || std::fabs(p.dcQ) > 1) err("DC offset must be within full scale (-1 to 1)");
    if (std::fabs(p.iqPhaseDeg) >= 45) err("IQ phase error must be within +-45 degrees");
    if (p.linewidth < 0) err("Linewidth can't be negative");

    if (c.ok()) {
        const double amp = std::pow(10.0, p.ampDb / 20);
        double peak = amp * std::max(1.0, std::pow(10.0, p.iqGainDb / 20)) + std::max(std::fabs(p.dcI), std::fabs(p.dcQ));
        if (p.noise) peak += 3 * amp * std::sqrt(std::pow(10.0, -p.snrDb / 10) / 2);
        if (peak > 1) warn("Peaks exceed full scale: integer file formats and the USRP will clip");
    }
    return c;
}

std::string describeParams(const WaveformParams& p) {
    char amp[32];
    snprintf(amp, sizeof amp, "%.1f dBFS", p.ampDb);
    std::string s;
    if (p.mode == GenMode::CW) {
        s = "CW, offset " + fmtSI(p.offset, "Hz");
        if (p.driftHzPerSec != 0) s += ", drift " + fmtSI(p.driftHzPerSec, "Hz/s");
    } else {
        s = "pulsed, PRI " + fmtSI(p.pri, "s");
        if (p.priPattern == PriPattern::Stagger) {
            s = "pulsed, PRI stagger";
            for (size_t i = 0; i < p.stagger.size(); ++i) s += (i ? "/" : " ") + fmtSI(p.stagger[i], "s");
        } else if (p.priPattern == PriPattern::Jitter) {
            char j[32];
            snprintf(j, sizeof j, " +-%.3g%%", p.jitterPct);
            s += j;
        }
        s += ", PW " + fmtSI(p.pw, "s");
        char duty[32];
        snprintf(duty, sizeof duty, ", duty %.3g%%", 100 * p.pw / p.meanPri());
        s += duty;
        if (p.burstPulses > 0) s += ", bursts of " + std::to_string(p.burstPulses);
        if (p.intra == IntraMod::Barker) s += ", Barker-" + std::to_string(p.barkerLen);
        else if (p.intra != IntraMod::None) s += ", " + std::string(intraModName(p.intra)) + " " + fmtSI(p.chirpBw, "Hz");
        s += ", offset " + fmtSI(p.offset, "Hz");
    }
    s += ", " + std::string(amp);
    if (p.noise) s += ", SNR " + num(p.snrDb) + " dB";
    return s;
}

std::string paramsJson(const WaveformParams& p) {
    std::string j = "{";
    auto add = [&](const char* k, const std::string& v) { j += std::string(j.size() > 1 ? ", " : "") + "\"" + k + "\": " + v; };
    auto str = [](const char* s) { return std::string("\"") + s + "\""; };
    add("mode", str(p.mode == GenMode::CW ? "cw" : "pulsed"));
    add("offset_hz", num(p.offset));
    add("amplitude_dbfs", num(p.ampDb));
    add("phase_deg", num(p.phaseDeg));
    if (p.mode == GenMode::CW) {
        add("drift_hz_per_s", num(p.driftHzPerSec));
    } else {
        add("pri_s", num(p.pri));
        add("pulse_width_s", num(p.pw));
        add("edge", str(edgeName(p.edge)));
        if (p.edge != EdgeShape::Rect) {
            add("rise_s", num(p.rise));
            add("fall_s", num(p.fall));
        }
        add("pri_pattern", str(priPatternName(p.priPattern)));
        if (p.priPattern == PriPattern::Stagger) {
            std::string a = "[";
            for (size_t i = 0; i < p.stagger.size(); ++i) a += (i ? ", " : "") + num(p.stagger[i]);
            add("stagger_s", a + "]");
        }
        if (p.priPattern == PriPattern::Jitter) {
            add("jitter_pct", num(p.jitterPct));
            add("seed", std::to_string(p.seed));
        }
        add("burst_pulses", std::to_string(p.burstPulses));
        add("burst_gap_s", num(p.burstGap));
        add("start_delay_s", num(p.startDelay));
        add("intrapulse", str(intraModName(p.intra)));
        if (p.intra == IntraMod::Barker) add("barker_length", std::to_string(p.barkerLen));
        else if (p.intra != IntraMod::None) add("chirp_bw_hz", num(p.chirpBw));
    }
    if (p.noise) add("snr_db", num(p.snrDb));
    if (p.dcI != 0 || p.dcQ != 0) add("dc_offset", "[" + num(p.dcI) + ", " + num(p.dcQ) + "]");
    if (p.iqGainDb != 0) add("iq_gain_db", num(p.iqGainDb));
    if (p.iqPhaseDeg != 0) add("iq_phase_deg", num(p.iqPhaseDeg));
    if (p.linewidth != 0) add("linewidth_hz", num(p.linewidth));
    return j + "}";
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------
SigGen::SigGen(const WaveformParams& p, double tuneFc) : tuneFc_(tuneFc) {
    ph_ = p.phaseDeg * M_PI / 180;
    jitterRng_ = 0x9e3779b97f4a7c15ull ^ ((uint64_t)p.seed * 0xbf58476d1ce4e5b9ull);
    p_ = p;
    apply(p);
}

void SigGen::setTuning(double fc) { tuneFc_ = fc; }

void SigGen::setParams(const WaveformParams& p) {
    if (p == (hasPending_ ? pending_ : p_)) return;
    pending_ = p;
    hasPending_ = true;
}

void SigGen::apply(const WaveformParams& p) {
    const WaveformParams old = p_;
    p_ = p;
    amp_ = std::pow(10.0, p.ampDb / 20);
    ph_ += (p.phaseDeg - old.phaseDeg) * M_PI / 180;
    if (p.seed != old.seed) jitterRng_ = 0x9e3779b97f4a7c15ull ^ ((uint64_t)p.seed * 0xbf58476d1ce4e5b9ull);
    if (p.mode != GenMode::Pulsed) {
        tmpl_.clear();
        curStart_ = -1;
        return;
    }
    buildTemplate();
    if (lastStart_ < 0) {
        nextStart_ = std::max((double)n_, p.startDelay * p.rate);
    } else {
        if (p.burstPulses > 0 && burstCount_ >= p.burstPulses) burstCount_ = 0;
        scheduleAfter(lastStart_);
        nextStart_ = std::max(nextStart_, (double)n_);
    }
}

void SigGen::buildTemplate() {
    const double rate = p_.rate;
    const int64_t L = std::max<int64_t>(1, std::llround(p_.pw * rate));
    tmpl_.assign((size_t)L, {});
    const bool shaped = p_.edge != EdgeShape::Rect;
    const double rN = shaped ? p_.rise * rate : 0, fN = shaped ? p_.fall * rate : 0;
    const double B = p_.chirpBw;
    const char* code = p_.intra == IntraMod::Barker ? barkerCode(p_.barkerLen) : nullptr;
    double ph = 0;
    for (int64_t k = 0; k < L; ++k) {
        const double x = k + 0.5;  // sample center
        double env = 1;
        if (rN > 0 && x < rN) env = edgeShape(p_.edge, x / rN);
        if (fN > 0 && x > L - fN) env = std::min(env, edgeShape(p_.edge, (L - x) / fN));
        const double u = x / L;  // position in the pulse, 0..1
        double f = 0, sign = 1;
        switch (p_.intra) {
            case IntraMod::LfmUp: f = -B / 2 + B * u; break;
            case IntraMod::LfmDown: f = B / 2 - B * u; break;
            case IntraMod::LfmTriangle: f = u < 0.5 ? -B / 2 + 2 * B * u : B / 2 - 2 * B * (u - 0.5); break;
            case IntraMod::Barker:
                if (code) sign = code[std::min<int64_t>(p_.barkerLen - 1, k * p_.barkerLen / L)] == '+' ? 1 : -1;
                break;
            default: break;
        }
        ph += kTwoPi * f / rate;
        // (std::polar needs a non-negative magnitude, so the code sign multiplies.)
        tmpl_[(size_t)k] = (float)(env * sign) * std::polar(1.f, (float)ph);
    }
}

double SigGen::nextPri() {
    switch (p_.priPattern) {
        case PriPattern::Stagger:
            if (p_.stagger.empty()) return p_.pri;
            return p_.stagger[staggerIdx_++ % p_.stagger.size()];
        case PriPattern::Jitter: return p_.pri * (1 + p_.jitterPct / 100 * (2 * uniform(jitterRng_) - 1));
        default: return p_.pri;
    }
}

void SigGen::scheduleAfter(int64_t start) {
    // burstCount_ == 0 right after a pulse means that pulse ended a burst.
    const bool burstEnd = p_.burstPulses > 0 && burstCount_ == 0;
    nextStart_ = (double)start + (nextPri() + (burstEnd ? p_.burstGap : 0)) * p_.rate;
    // Never overlap the pulse that is playing (guards rounding at 1-sample gaps).
    nextStart_ = std::max(nextStart_, (double)(start + (int64_t)tmpl_.size() + 1));
}

double SigGen::gauss() {
    if (haveSpare_) {
        haveSpare_ = false;
        return spare_;
    }
    double u1 = std::max(uniform(rng_), 1e-300), u2 = uniform(rng_);
    double r = std::sqrt(-2 * std::log(u1));
    spare_ = r * std::sin(kTwoPi * u2);
    haveSpare_ = true;
    return r * std::cos(kTwoPi * u2);
}

void SigGen::carrier(std::complex<float>* out, const std::complex<float>* mod, int64_t m) {
    const double rate = p_.rate;
    const double f0 = p_.offset + (std::isnan(tuneFc_) ? 0 : p_.fc - tuneFc_);
    // A receiver tuned away from the signal doesn't see it (rather than an alias).
    const double amp = std::fabs(f0) < rate / 2 ? amp_ : 0;
    const double drift = p_.mode == GenMode::CW ? p_.driftHzPerSec : 0;
    const double sigma = p_.linewidth > 0 ? std::sqrt(kTwoPi * p_.linewidth / rate) : 0;
    for (int64_t k = 0; k < m;) {
        const int64_t b = std::min<int64_t>(256, m - k);
        const double inc = kTwoPi * (f0 + drift * (double)(n_ + k) / rate) / rate;
        if (sigma == 0) {
            // Rotator, re-anchored to the exact phase every block.
            std::complex<double> z = std::polar(amp, ph_);
            const std::complex<double> w = std::polar(1.0, inc);
            for (int64_t j = 0; j < b; ++j) {
                const std::complex<float> v(z);
                out[k + j] = mod ? v * mod[k + j] : v;
                z *= w;
            }
            ph_ += inc * (double)b;
        } else {
            for (int64_t j = 0; j < b; ++j) {
                const std::complex<float> v = std::polar((float)amp, (float)ph_);
                out[k + j] = mod ? v * mod[k + j] : v;
                ph_ += inc + sigma * gauss();
            }
        }
        ph_ = std::remainder(ph_, kTwoPi);
        k += b;
    }
}

void SigGen::advanceCarrier(int64_t m) {
    const double rate = p_.rate;
    const double f0 = p_.offset + (std::isnan(tuneFc_) ? 0 : p_.fc - tuneFc_);
    ph_ += kTwoPi * f0 / rate * (double)m;
    if (p_.linewidth > 0) ph_ += std::sqrt(kTwoPi * p_.linewidth / rate * (double)m) * gauss();
    ph_ = std::remainder(ph_, kTwoPi);
}

void SigGen::impair(std::complex<float>* out, int64_t m) {
    const bool iq = p_.iqGainDb != 0 || p_.iqPhaseDeg != 0;
    const bool dc = p_.dcI != 0 || p_.dcQ != 0;
    if (!p_.noise && !iq && !dc) return;
    const float g = (float)std::pow(10.0, p_.iqGainDb / 20);
    const float c = (float)std::cos(p_.iqPhaseDeg * M_PI / 180), s = (float)std::sin(p_.iqPhaseDeg * M_PI / 180);
    const double sigma = p_.noise ? amp_ * std::sqrt(std::pow(10.0, -p_.snrDb / 10) / 2) : 0;
    const float di = (float)p_.dcI, dq = (float)p_.dcQ;
    for (int64_t k = 0; k < m; ++k) {
        float re = out[k].real(), im = out[k].imag();
        if (iq) im = g * (c * im + s * re);
        if (sigma > 0) {
            re += (float)(sigma * gauss());
            im += (float)(sigma * gauss());
        }
        out[k] = {re + di, im + dq};
    }
}

void SigGen::generate(std::complex<float>* out, int64_t n) {
    pulses_.clear();
    if (hasPending_ && (p_.mode == GenMode::CW || curStart_ < 0)) {
        hasPending_ = false;
        apply(pending_);
    }
    int64_t i = 0;
    while (i < n) {
        if (p_.mode == GenMode::CW) {
            carrier(out + i, nullptr, n - i);
            n_ += n - i;
            i = n;
            break;
        }
        const int64_t L = (int64_t)tmpl_.size();
        if (curStart_ >= 0) {
            const int64_t off = n_ - curStart_;
            const int64_t m = std::min(n - i, L - off);
            carrier(out + i, tmpl_.data() + off, m);
            i += m;
            n_ += m;
            if (off + m >= L) {
                curStart_ = -1;
                if (hasPending_) {
                    hasPending_ = false;
                    apply(pending_);
                }
            }
            continue;
        }
        const int64_t s = std::max(n_, (int64_t)std::llround(nextStart_));
        const int64_t m = std::min(n - i, s - n_);
        if (m > 0) {
            std::fill(out + i, out + i + m, std::complex<float>(0.f, 0.f));
            advanceCarrier(m);
            i += m;
            n_ += m;
            continue;
        }
        curStart_ = lastStart_ = n_;
        pulses_.push_back({n_, L});
        ++burstCount_;
        if (p_.burstPulses > 0 && burstCount_ >= p_.burstPulses) burstCount_ = 0;
        scheduleAfter(n_);
    }
    impair(out, n);
}

// ---------------------------------------------------------------------------
// Bus
// ---------------------------------------------------------------------------
void SigGenBus::publish(const WaveformParams& p) {
    std::lock_guard<std::mutex> lk(m_);
    if (p == p_) return;
    p_ = p;
    ver_.fetch_add(1, std::memory_order_release);
}

WaveformParams SigGenBus::get(uint64_t* version) const {
    std::lock_guard<std::mutex> lk(m_);
    if (version) *version = ver_.load(std::memory_order_acquire);
    return p_;
}

SigGenBus& sigGenBus() {
    static SigGenBus bus;
    return bus;
}

BusSigGen::BusSigGen(double rate, double tuneFc) : rate_(rate), gen_(fetch(), tuneFc) {}

WaveformParams BusSigGen::fetch() {
    WaveformParams p = sigGenBus().get(&ver_);
    p.rate = rate_;
    return p;
}

void BusSigGen::generate(std::complex<float>* out, int64_t n) {
    if (sigGenBus().version() != ver_) gen_.setParams(fetch());
    gen_.generate(out, n);
}
