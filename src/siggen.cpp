#include "siggen.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "dsp.h"
#include "json.h"
#include "recorder.h"
#include "ui_util.h"

namespace {
constexpr double kTwoPi = 2 * M_PI;
constexpr double kNever = 4e18;  // a sample position that never comes

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

std::string jsonString(const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += c;
        }
    }
    return o + "\"";
}

std::string lower(const char* s) {
    std::string o(s);
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

std::string baseName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

double playFileRate(const WaveformParams& p) { return p.playRate > 0 ? p.playRate : p.rate; }

// True if a and b schedule pulses (or playback repeats) the same way.
bool sameSchedule(const WaveformParams& a, const WaveformParams& b) {
    return a.mode == b.mode && a.rate == b.rate && a.pri == b.pri && a.priPattern == b.priPattern &&
           a.stagger == b.stagger && a.jitterPct == b.jitterPct && a.seed == b.seed && a.burstPulses == b.burstPulses &&
           a.burstGap == b.burstGap && a.playLoop == b.playLoop && a.playGap == b.playGap;
}

// True if a and b chain the same recordings (a change otherwise starts the chain again).
bool sameChain(const WaveformParams& a, const WaveformParams& b) {
    if (a.mode != b.mode || a.rate != b.rate || a.chain.size() != b.chain.size()) return false;
    for (size_t i = 0; i < a.chain.size(); ++i) {
        const ChainItem &x = a.chain[i], &y = b.chain[i];
        if (x.path != y.path || !(x.fp == y.fp) || x.fileRate != y.fileRate || x.start != y.start || x.len != y.len) return false;
    }
    return true;
}

// True if a and b sweep the same way (a change otherwise starts the period again).
bool sameSweep(const WaveformParams& a, const WaveformParams& b) {
    return a.mode == b.mode && a.rate == b.rate && a.fSweep == b.fSweep && a.fFrom == b.fFrom && a.fTo == b.fTo &&
           a.sweepPeriod == b.sweepPeriod && a.fStep == b.fStep && a.fDwell == b.fDwell && a.aSweep == b.aSweep &&
           a.aFrom == b.aFrom && a.aTo == b.aTo;
}

// Value of a sweep shape at sample k of a period of P samples. `step`: the
// stepped sweep's step (Hz) and dwell (samples).
double shapeAt(SweepShape s, double from, double to, int64_t k, int64_t P, double step, int64_t dwell) {
    const double u = (double)k / (double)P;
    switch (s) {
        case SweepShape::RampUp: return from + (to - from) * u;
        case SweepShape::RampDown: return to + (from - to) * u;
        case SweepShape::Triangle: return 2 * k < P ? from + (to - from) * 2 * u : to - (to - from) * (2 * u - 1);
        case SweepShape::Stepped: return from + (to >= from ? step : -step) * (double)(k / dwell);
        default: return from;
    }
}
}  // namespace

WaveformParams chainItemParams(const WaveformParams& p, size_t i) {
    WaveformParams q;
    q.mode = GenMode::Playback;
    q.rate = p.rate;
    q.fc = p.fc;
    if (i < p.chain.size()) {
        const ChainItem& c = p.chain[i];
        q.playPath = c.path;
        q.playFp = c.fp;
        q.playRate = c.fileRate;
        q.playStart = c.start;
        q.playLen = c.len;
        q.offset = c.offset;
        q.ampDb = c.ampDb;
    }
    return q;
}

ChainItem makeChainItem(const std::string& path, const WaveformParams& p, std::string* note) {
    const FileMetaGuess g = guessMeta(expandHome(path));
    ChainItem c;
    c.path = g.dataPath;
    if (g.haveFmt) {
        c.fp.fmt = g.fmt;
        c.fp.bigEndian = g.bigEndian;
    }
    c.fileRate = g.haveRate ? g.rate : 0;
    std::string n = g.source.empty() ? "no metadata found: set its format and rate" : "settings from " + g.source;
    if (g.haveFc) {
        c.offset = g.fc - p.fc;
        if (!(std::fabs(c.offset) < p.rate / 2)) {
            n += "; its frequency (" + fmtSI(g.fc, "Hz", 9) + ") is outside the band, so it plays at the center";
            c.offset = 0;
        }
    }
    if (note) *note = n;
    return c;
}

namespace {
std::string dirOf(const std::string& path) {
    const size_t k = path.find_last_of('/');
    return k == std::string::npos ? "." : path.substr(0, k);
}
}  // namespace

bool saveChain(const std::string& path, const WaveformParams& p, std::string& err) {
    const std::string full = expandHome(path);
    const std::string dir = dirOf(full) + "/";
    std::string j = "{\n  \"wvt_chain\": 1,\n";
    j += "  \"sample_rate\": " + num(p.rate) + ",\n";
    j += "  \"center_freq\": " + num(p.fc) + ",\n";
    j += "  \"loops\": " + std::to_string(p.chainLoops) + ",\n";
    j += "  \"start_delay_s\": " + num(p.startDelay) + ",\n";
    j += "  \"items\": [";
    for (size_t i = 0; i < p.chain.size(); ++i) {
        const ChainItem& c = p.chain[i];
        std::string f = c.path;
        if (f.rfind(dir, 0) == 0) f = f.substr(dir.size());  // beside the chain file, or below it
        std::string fmt = formatName(c.fp.fmt);
        fmt = fmt.substr(0, fmt.find(' ')) + (c.fp.bigEndian ? "_be" : "_le");
        j += std::string(i ? "," : "") + "\n    {\"path\": " + jsonQuote(f) + ", \"datatype\": \"" + fmt + "\"";
        if (c.fp.swapIQ) j += ", \"swap_iq\": true";
        if (c.fp.headerBytes) j += ", \"header_bytes\": " + std::to_string(c.fp.headerBytes);
        j += ", \"file_rate\": " + num(c.fileRate) + ", \"start_s\": " + num(c.start) + ", \"length_s\": " + num(c.len) +
             ", \"offset_hz\": " + num(c.offset) + ", \"level_dbfs\": " + (c.keepLevel ? std::string("null") : num(c.ampDb)) +
             ", \"repeats\": " + std::to_string(c.repeats) + ", \"gap_after_s\": " + num(c.gapAfter) + "}";
    }
    j += "\n  ]\n}\n";
    FILE* f = fopen(full.c_str(), "wb");
    if (!f) {
        err = "can't write " + full + ": " + strerror(errno);
        return false;
    }
    const bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    if (fclose(f) != 0 || !ok) {
        err = "can't write " + full;
        return false;
    }
    return true;
}

bool loadChain(const std::string& path, WaveformParams& p, std::string& err) {
    const std::string full = expandHome(path);
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) {
        err = "can't open " + full + ": " + strerror(errno);
        return false;
    }
    std::string text;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    JsonValue v;
    if (!parseJson(text, v, err)) {
        err = full + ": " + err;
        return false;
    }
    const JsonValue* items = v.get("items");
    if (!v.get("wvt_chain") || !items || items->type != JsonValue::Type::Array) {
        err = full + " is not a wvt chain file";
        return false;
    }
    std::vector<ChainItem> chain;
    for (const JsonValue& it : items->arr) {
        ChainItem c;
        c.path = it.string("path");
        if (c.path.empty()) {
            err = full + ": an item has no path";
            return false;
        }
        if (c.path[0] != '/' && c.path[0] != '~') c.path = dirOf(full) + "/" + c.path;
        c.path = expandHome(c.path);
        bool be = false;
        if (!parseFormatName(it.string("datatype", "cf32_le"), c.fp.fmt, be)) {
            err = full + ": unknown datatype " + it.string("datatype");
            return false;
        }
        c.fp.bigEndian = be;
        c.fp.swapIQ = it.boolean("swap_iq");
        c.fp.headerBytes = (int64_t)it.number("header_bytes");
        c.fileRate = it.number("file_rate");
        c.start = it.number("start_s");
        c.len = it.number("length_s");
        c.offset = it.number("offset_hz");
        const JsonValue* lvl = it.get("level_dbfs");
        c.keepLevel = !lvl || lvl->type != JsonValue::Type::Number;
        if (!c.keepLevel) c.ampDb = lvl->num;
        c.repeats = (int)it.number("repeats", 1);
        c.gapAfter = it.number("gap_after_s");
        chain.push_back(std::move(c));
    }
    p.chain = std::move(chain);
    p.chainLoops = (int)v.number("loops");
    p.startDelay = v.number("start_delay_s");
    if (v.number("sample_rate") > 0) p.rate = v.number("sample_rate");
    if (v.get("center_freq")) p.fc = v.number("center_freq");
    p.mode = GenMode::Chain;
    return true;
}

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

const char* sweepShapeName(SweepShape s) {
    switch (s) {
        case SweepShape::Off: return "Off";
        case SweepShape::RampUp: return "Ramp up";
        case SweepShape::RampDown: return "Ramp down";
        case SweepShape::Triangle: return "Triangle";
        case SweepShape::Stepped: return "Stepped";
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

int WaveformParams::sweepSteps() const {
    if (!(fStep > 0)) return 1;
    return (int)std::min(1e7, std::floor(std::fabs(fTo - fFrom) / fStep + 1e-9) + 1);
}

int64_t WaveformParams::sweepSamples() const {
    if (fSweep == SweepShape::Stepped) return (int64_t)sweepSteps() * std::max<int64_t>(1, std::llround(fDwell * rate));
    return std::max<int64_t>(2, std::llround(sweepPeriod * rate));
}

double WaveformParams::sweepOffsetAt(int64_t k) const {
    if (fSweep == SweepShape::Off) return offset;
    return shapeAt(fSweep, fFrom, fTo, k, sweepSamples(), fStep, std::max<int64_t>(1, std::llround(fDwell * rate)));
}

double WaveformParams::sweepLevelAt(int64_t k) const {
    if (aSweep == SweepShape::Off || aSweep == SweepShape::Stepped) return ampDb;
    return shapeAt(aSweep, aFrom, aTo, k, sweepSamples(), 0, 1);
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
    const bool fSweep = p.mode == GenMode::CW && p.fSweep != SweepShape::Off;
    const bool aSweep = p.mode == GenMode::CW && p.aSweep != SweepShape::Off;
    if (!(p.ampDb <= 0)) err("Amplitude must be 0 dBFS or less");
    if (!fSweep && p.mode != GenMode::Chain && !(std::fabs(p.offset) < nyq)) err("Offset must be inside the band (|offset| < " + fmtSI(nyq, "Hz") + ")");
    if (p.mode == GenMode::CW) {
        if (fSweep) {
            if (p.driftHzPerSec != 0) warn("Drift is ignored while the frequency sweeps");
            if (!(std::fabs(p.fFrom) < nyq && std::fabs(p.fTo) < nyq))
                err("The sweep must stay inside the band (|from| and |to| < " + fmtSI(nyq, "Hz") + ")");
            if (p.fFrom == p.fTo) err("The sweep needs different From and To frequencies");
            if (p.fSweep == SweepShape::Stepped) {
                if (!(p.fStep > 0)) err("The frequency step must be positive");
                else if (p.sweepSteps() > 1000000) err("More than 1000000 steps: use a larger step");
                if (!(p.fDwell * p.rate >= 1)) err("Dwell must be at least one sample (" + fmtSI(1 / p.rate, "s") + ")");
            }
        } else if (p.driftHzPerSec != 0) {
            warn("The tone drifts by " + fmtSI(std::fabs(p.driftHzPerSec), "Hz/s") + " and wraps around at the band edge");
        }
        if (aSweep) {
            if (p.aSweep == SweepShape::Stepped) err("The level can't be stepped");
            if (!(p.aFrom <= 0 && p.aTo <= 0)) err("Sweep levels must be 0 dBFS or less");
            if (p.aFrom == p.aTo) err("The level sweep needs different From and To levels");
        }
        if ((fSweep || aSweep) && p.fSweep != SweepShape::Stepped && !(p.sweepPeriod * p.rate >= 2))
            err("The sweep period must be at least 2 samples (" + fmtSI(2 / p.rate, "s") + ")");
    } else if (p.mode == GenMode::Chain) {
        if (p.chain.empty()) err("Add at least one recording to the chain");
        for (size_t i = 0; i < p.chain.size(); ++i) {
            const ChainItem& c = p.chain[i];
            const std::string n = "Item " + std::to_string(i + 1) + ": ";
            const double fr = c.fileRate > 0 ? c.fileRate : p.rate;
            if (c.path.empty()) err(n + "choose a recording");
            if (c.fileRate < 0) err(n + "the recording's sample rate can't be negative");
            else if (fr / p.rate > 64 || p.rate / fr > 64) err(n + "the recording's rate and the output rate differ by more than 64x");
            if (c.start < 0 || c.len < 0) err(n + "the segment start and length can't be negative");
            if (!(std::fabs(c.offset) < nyq)) err(n + "the offset must be inside the band (|offset| < " + fmtSI(nyq, "Hz") + ")");
            if (!c.keepLevel && !(c.ampDb <= 0)) err(n + "the level must be 0 dBFS or less");
            if (c.repeats < 1) err(n + "repeats must be at least 1");
            if (c.gapAfter < 0) err(n + "the gap can't be negative");
        }
        if (p.chainLoops < 0) err("The loop count can't be negative");
        if (p.startDelay < 0) err("Start delay can't be negative");
    } else if (p.mode == GenMode::Playback) {
        const double fr = playFileRate(p);
        if (p.playPath.empty()) err("Choose a recording to play");
        if (p.playRate < 0) err("The recording's sample rate can't be negative");
        else if (fr / p.rate > 64 || p.rate / fr > 64) err("The recording's rate and the output rate differ by more than 64x");
        else if (fr > p.rate * (1 + 1e-9))
            warn("The recording (" + fmtSI(fr, "S/s") + ") is resampled down to " + fmtSI(p.rate, "S/s") +
                 ": anything outside +-" + fmtSI(nyq, "Hz") + " of its center is filtered out");
        if (p.playStart < 0 || p.playLen < 0) err("The segment start and length can't be negative");
        if (p.playGap < 0) err("The repeat gap can't be negative");
        if (p.startDelay < 0) err("Start delay can't be negative");
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

    if (c.ok() && p.mode != GenMode::Chain) {  // (a chain's levels are known only once it's loaded)
        const double amp = std::pow(10.0, (aSweep ? std::max(p.aFrom, p.aTo) : p.ampDb) / 20);
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
    if (p.mode == GenMode::CW && p.fSweep == SweepShape::Stepped) {
        s = "CW stepped " + fmtSI(p.fFrom, "Hz") + " to " + fmtSI(p.fTo, "Hz") + " by " + fmtSI(p.fStep, "Hz") + ", " +
            fmtSI(p.fDwell, "s") + " dwell (" + std::to_string(p.sweepSteps()) + " steps)";
    } else if (p.mode == GenMode::CW && p.fSweep != SweepShape::Off) {
        s = "CW sweep " + fmtSI(p.fFrom, "Hz") + " to " + fmtSI(p.fTo, "Hz") + ", " + lower(sweepShapeName(p.fSweep)) + " " +
            fmtSI(p.sweepPeriod, "s");
    } else if (p.mode == GenMode::CW) {
        s = "CW, offset " + fmtSI(p.offset, "Hz");
        if (p.driftHzPerSec != 0) s += ", drift " + fmtSI(p.driftHzPerSec, "Hz/s");
    } else if (p.mode == GenMode::Chain) {
        s = "chain of " + std::to_string(p.chain.size()) + (p.chain.size() == 1 ? " recording" : " recordings");
        s += p.chainLoops > 0 ? ", " + std::to_string(p.chainLoops) + (p.chainLoops == 1 ? " pass" : " passes") : ", looped";
        if (p.noise) s += ", SNR " + num(p.snrDb) + " dB";
        return s;
    } else if (p.mode == GenMode::Playback) {
        s = "playback of " + baseName(p.playPath);
        if (p.playStart > 0 || p.playLen > 0)
            s += " from " + fmtSI(p.playStart, "s") + (p.playLen > 0 ? " for " + fmtSI(p.playLen, "s") : " to the end");
        const double fr = playFileRate(p);
        if (std::fabs(fr - p.rate) > 1e-9 * p.rate) s += ", resampled from " + fmtSI(fr, "S/s");
        if (!p.playLoop) s += ", once";
        else if (p.playGap > 0) s += ", repeated after " + fmtSI(p.playGap, "s") + " gaps";
        else s += ", looped";
        s += ", offset " + fmtSI(p.offset, "Hz");
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
    if (p.mode == GenMode::CW && p.aSweep != SweepShape::Off) {
        snprintf(amp, sizeof amp, "%.1f to %.1f dBFS", p.aFrom, p.aTo);
        s += ", level " + std::string(amp) + " " + lower(sweepShapeName(p.aSweep));
        if (p.fSweep == SweepShape::Off) s += " " + fmtSI(p.sweepPeriod, "s");
    } else {
        s += ", " + std::string(amp);
    }
    if (p.noise) s += ", SNR " + num(p.snrDb) + " dB";
    return s;
}

std::string paramsJson(const WaveformParams& p) {
    std::string j = "{";
    auto add = [&](const char* k, const std::string& v) { j += std::string(j.size() > 1 ? ", " : "") + "\"" + k + "\": " + v; };
    auto str = [](const char* s) { return std::string("\"") + s + "\""; };
    add("mode", str(p.mode == GenMode::CW       ? "cw"
                    : p.mode == GenMode::Pulsed ? "pulsed"
                    : p.mode == GenMode::Chain  ? "chain"
                                                : "playback"));
    if (p.mode != GenMode::Chain) {  // (each chain item has its own)
        add("offset_hz", num(p.offset));
        add("amplitude_dbfs", num(p.ampDb));
    }
    add("phase_deg", num(p.phaseDeg));
    if (p.mode == GenMode::CW) {
        add("drift_hz_per_s", num(p.driftHzPerSec));
        if (p.sweeping()) {
            auto name = [&](SweepShape v) { return str(lower(sweepShapeName(v)).c_str()); };
            std::string w = "{\"period_s\": " + num((double)p.sweepSamples() / p.rate);
            if (p.fSweep != SweepShape::Off) {
                w += ", \"frequency\": " + name(p.fSweep) + ", \"from_hz\": " + num(p.fFrom) + ", \"to_hz\": " + num(p.fTo);
                if (p.fSweep == SweepShape::Stepped)
                    w += ", \"step_hz\": " + num(p.fStep) + ", \"dwell_s\": " + num(p.fDwell);
            }
            if (p.aSweep != SweepShape::Off)
                w += ", \"level\": " + name(p.aSweep) + ", \"from_dbfs\": " + num(p.aFrom) + ", \"to_dbfs\": " + num(p.aTo);
            add("sweep", w + "}");
        }
    } else if (p.mode == GenMode::Chain) {
        std::string a = "[";
        for (size_t i = 0; i < p.chain.size(); ++i) {
            const ChainItem& c = p.chain[i];
            std::string fmt = formatName(c.fp.fmt);
            fmt = fmt.substr(0, fmt.find(' ')) + (c.fp.bigEndian ? "_be" : "");
            a += std::string(i ? ", " : "") + "{\"source_file\": " + jsonString(c.path) + ", \"source_datatype\": \"" + fmt +
                 "\", \"source_rate_hz\": " + num(c.fileRate > 0 ? c.fileRate : p.rate) + ", \"segment_start_s\": " +
                 num(c.start) + ", \"segment_length_s\": " + num(c.len) + ", \"offset_hz\": " + num(c.offset) +
                 ", \"level_dbfs\": " + (c.keepLevel ? std::string("\"as recorded\"") : num(c.ampDb)) +
                 ", \"repeats\": " + std::to_string(c.repeats) + ", \"gap_after_s\": " + num(c.gapAfter) + "}";
        }
        add("chain", a + "]");
        add("loops", std::to_string(p.chainLoops));
        add("start_delay_s", num(p.startDelay));
    } else if (p.mode == GenMode::Playback) {
        add("source_file", jsonString(p.playPath));
        std::string fmt = formatName(p.playFp.fmt);
        fmt = fmt.substr(0, fmt.find(' ')) + (p.playFp.bigEndian ? "_be" : "");
        add("source_datatype", str(fmt.c_str()));
        add("source_rate_hz", num(playFileRate(p)));
        add("segment_start_s", num(p.playStart));
        add("segment_length_s", num(p.playLen));
        add("loop", p.playLoop ? "true" : "false");
        add("repeat_gap_s", num(p.playGap));
        add("start_delay_s", num(p.startDelay));
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
    amp_ = noiseAmp_ = std::pow(10.0, p.ampDb / 20);
    offset_ = p.offset;
    ph_ += (p.phaseDeg - old.phaseDeg) * M_PI / 180;
    if (p.seed != old.seed) jitterRng_ = 0x9e3779b97f4a7c15ull ^ ((uint64_t)p.seed * 0xbf58476d1ce4e5b9ull);
    if (p.mode == GenMode::Chain) {
        applyChain(old, p);
        return;
    }
    waves_.clear();
    curItem_ = -1;
    chainEnd_ = false;
    if (p.mode == GenMode::CW) {
        // A sweep setting change starts the period again (the phase stays continuous).
        if (!sameSweep(old, p)) sweepN_ = 0;
        if (p.aSweep != SweepShape::Off) noiseAmp_ = std::pow(10.0, std::max(p.aFrom, p.aTo) / 20);  // noise follows the peak
        tmpl_.reset();
        wave_.reset();
        err_.clear();
        curStart_ = -1;
        return;
    }
    const auto prev = tmpl_;
    if (p.mode == GenMode::Pulsed) {
        wave_.reset();
        err_.clear();
        buildTemplate();
    } else {
        auto w = loadPlayback(p, err_);
        if (w) err_.clear();
        if (w != wave_) {
            wave_ = w;
            tmpl_ = w ? std::shared_ptr<const std::vector<std::complex<float>>>(w, &w->s) : nullptr;
        }
        if (w) amp_ /= w->peak;  // the recording's peak plays at ampDb
    }
    if (tmpl_ != prev) {
        curStart_ = -1;
        // A new recording starts over (after the start delay, if the stream is that young).
        if (p.mode == GenMode::Playback) lastStart_ = -1;
    }
    if (lastStart_ < 0) {
        nextStart_ = std::max((double)n_, p.startDelay * p.rate);
    } else {
        // Reschedule only if the timing changed: scheduling draws the next stagger
        // or jitter PRI, so redoing it for an unrelated change would skip one.
        if (!sameSchedule(old, p)) {
            if (p.burstPulses > 0 && burstCount_ >= p.burstPulses) burstCount_ = 0;
            scheduleAfter(lastStartF_);
        }
        nextStart_ = std::max(nextStart_, (double)n_);
    }
}

void SigGen::applyChain(const WaveformParams& old, const WaveformParams& p) {
    // The items normally come from the cache: the window loads them before it publishes.
    std::vector<std::shared_ptr<const PlaybackWave>> w(p.chain.size());
    err_.clear();
    if (p.chain.empty()) err_ = "the chain is empty";
    for (size_t i = 0; i < p.chain.size(); ++i) {
        std::string e;
        w[i] = loadPlayback(chainItemParams(p, i), e);
        if (!w[i]) {
            err_ = "item " + std::to_string(i + 1) + ": " + e;
            w.clear();
            break;
        }
    }
    waves_ = std::move(w);
    wave_.reset();
    // The SNR refers to the loudest item.
    noiseAmp_ = 0;
    for (size_t i = 0; i < waves_.size(); ++i)
        noiseAmp_ = std::max(noiseAmp_, p.chain[i].keepLevel ? waves_[i]->peak : std::pow(10.0, p.chain[i].ampDb / 20));
    if (!(noiseAmp_ > 0)) noiseAmp_ = 1;

    if (old.mode != GenMode::Chain || !sameChain(old, p) || waves_.empty() || lastStart_ < 0) {
        // Different recordings (or nothing played yet): start from the first item.
        const bool wasChain = old.mode == GenMode::Chain && lastStart_ >= 0;
        chainIdx_ = 0;
        chainRep_ = chainPass_ = chainDone_ = 0;
        chainEnd_ = false;
        curStart_ = -1;
        curItem_ = -1;
        tmpl_.reset();
        if (!wasChain) {
            lastStart_ = -1;
            nextStart_ = std::max((double)n_, p.startDelay * p.rate);
        } else if (nextStart_ >= kNever) {
            nextStart_ = (double)n_;  // a finished chain plays again
        }
        nextStart_ = std::max(nextStart_, (double)n_);
        return;
    }
    // Same recordings. A larger loop count revives a chain that was ending.
    if (chainEnd_ && (p.chainLoops == 0 || chainDone_ < p.chainLoops)) {
        chainEnd_ = false;
        nextStart_ = std::max(chainResume_, (double)n_);
    }
    if (chainIdx_ >= p.chain.size()) chainIdx_ = 0;
    if (curItem_ >= 0) setItemLevel();  // offset and level apply at once
}

// The carrier offset and gain of the item that is playing.
void SigGen::setItemLevel() {
    const ChainItem& c = p_.chain[(size_t)curItem_];
    offset_ = c.offset;
    amp_ = c.keepLevel ? 1.0 : std::pow(10.0, c.ampDb / 20) / waves_[(size_t)curItem_]->peak;
}

void SigGen::startChainItem() {
    curItem_ = (int)chainIdx_;
    const auto& w = waves_[chainIdx_];
    tmpl_ = std::shared_ptr<const std::vector<std::complex<float>>>(w, &w->s);
    if (chainIdx_ == 0 && chainRep_ == 0) ++chainPass_;
    setItemLevel();
}

bool SigGen::canApplyNow() const {
    if (curStart_ < 0) return true;
    if (p_.mode == GenMode::Pulsed) return false;  // after the pulse
    // A chain with other recordings starts again after the item that plays.
    if (p_.mode == GenMode::Chain && pending_.mode == GenMode::Chain) return sameChain(p_, pending_);
    return true;
}

void SigGen::buildTemplate() {
    const double rate = p_.rate;
    const int64_t L = std::max<int64_t>(1, std::llround(p_.pw * rate));
    auto t = std::make_shared<std::vector<std::complex<float>>>((size_t)L);
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
        (*t)[(size_t)k] = (float)(env * sign) * std::polar(1.f, (float)ph);
    }
    tmpl_ = std::move(t);
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

// start: where the last pulse was due, unrounded, so fractional PRIs don't accumulate rounding.
void SigGen::scheduleAfter(double start) {
    if (p_.mode == GenMode::Chain) {
        const ChainItem& c = p_.chain[(size_t)curItem_];
        const double next = start + (double)tmplLen() + c.gapAfter * p_.rate;
        if (++chainRep_ >= std::max(1, c.repeats)) {
            chainRep_ = 0;
            if (++chainIdx_ >= p_.chain.size()) {
                chainIdx_ = 0;
                ++chainDone_;
            }
        }
        if (chainIdx_ == 0 && chainRep_ == 0 && p_.chainLoops > 0 && chainDone_ >= p_.chainLoops) {
            // The last pass: the output ends after this item's gap.
            chainEnd_ = true;
            chainEndAt_ = std::llround(next);
            chainResume_ = next;
            nextStart_ = kNever;
        } else {
            nextStart_ = next;
        }
        return;
    }
    if (p_.mode == GenMode::Playback) {
        nextStart_ = p_.playLoop ? start + (double)tmplLen() + p_.playGap * p_.rate : kNever;
        return;
    }
    // burstCount_ == 0 right after a pulse means that pulse ended a burst.
    const bool burstEnd = p_.burstPulses > 0 && burstCount_ == 0;
    nextStart_ = start + (nextPri() + (burstEnd ? p_.burstGap : 0)) * p_.rate;
    // Never overlap the pulse that is playing (guards rounding at 1-sample gaps).
    nextStart_ = std::max(nextStart_, start + (double)(tmplLen() + 1));
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
    const double f0 = offset_ + (std::isnan(tuneFc_) ? 0 : p_.fc - tuneFc_);
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

// The CW sweep: within a piece where the frequency and the level (in dB) are
// linear in time, a rotator whose step itself rotates gives the exact
// quadratic phase. The pieces end at the period, a triangle's turn and each step.
void SigGen::sweep(std::complex<float>* out, int64_t m) {
    const double rate = p_.rate;
    const double base = std::isnan(tuneFc_) ? 0 : p_.fc - tuneFc_;
    const double sigma = p_.linewidth > 0 ? std::sqrt(kTwoPi * p_.linewidth / rate) : 0;
    const bool fs = p_.fSweep != SweepShape::Off, as = p_.aSweep != SweepShape::Off;
    const int64_t P = p_.sweepSamples();
    const int64_t mid = (P + 1) / 2;
    const bool tri = p_.fSweep == SweepShape::Triangle || p_.aSweep == SweepShape::Triangle;
    const int64_t dwell = p_.fSweep == SweepShape::Stepped ? std::max<int64_t>(1, std::llround(p_.fDwell * rate)) : 0;
    for (int64_t k = 0; k < m;) {
        if (sweepN_ >= P) sweepN_ = 0;
        if (sweepN_ == 0) pulses_.push_back({n_ + k, P});
        int64_t end = P;
        if (tri && sweepN_ < mid) end = mid;
        if (dwell) end = std::min(end, (sweepN_ / dwell + 1) * dwell);
        const int64_t b = std::min({(int64_t)256, m - k, end - sweepN_});
        // Frequency (Hz) and level (dB) at the first sample, and their change per sample.
        double f0, df, a0, da;
        if (fs) {
            f0 = p_.sweepOffsetAt(sweepN_);
            df = b > 1 ? p_.sweepOffsetAt(sweepN_ + 1) - f0 : 0;
        } else {
            f0 = p_.offset + p_.driftHzPerSec * (double)(n_ + k) / rate;
            df = p_.driftHzPerSec / rate;
        }
        f0 += base;
        a0 = as ? p_.sweepLevelAt(sweepN_) : p_.ampDb;
        da = as && b > 1 ? p_.sweepLevelAt(sweepN_ + 1) - a0 : 0;
        // A receiver tuned away from the signal doesn't see it (rather than an alias).
        const bool inBand = std::fabs(f0) < rate / 2 && std::fabs(f0 + df * (double)(b - 1)) < rate / 2;
        const double amp = inBand ? std::pow(10.0, a0 / 20) : 0;
        const double r = std::pow(10.0, da / 20);
        const double inc = kTwoPi * f0 / rate, dinc = kTwoPi * df / rate;
        if (sigma == 0) {
            std::complex<double> z = std::polar(amp, ph_);
            std::complex<double> w = std::polar(r, inc);
            const std::complex<double> dw = std::polar(1.0, dinc);
            for (int64_t j = 0; j < b; ++j) {
                out[k + j] = std::complex<float>(z);
                z *= w;
                w *= dw;
            }
            ph_ += inc * (double)b + dinc * (double)b * (double)(b - 1) / 2;
        } else {
            double a = amp;
            for (int64_t j = 0; j < b; ++j) {
                out[k + j] = std::polar((float)a, (float)ph_);
                ph_ += inc + dinc * (double)j + sigma * gauss();
                a *= r;
            }
        }
        ph_ = std::remainder(ph_, kTwoPi);
        sweepN_ += b;
        k += b;
    }
}

void SigGen::seekSweep(int64_t k) {
    const int64_t P = p_.sweepSamples();
    sweepN_ = ((k % P) + P) % P;
}

void SigGen::advanceCarrier(int64_t m) {
    const double rate = p_.rate;
    const double f0 = offset_ + (std::isnan(tuneFc_) ? 0 : p_.fc - tuneFc_);
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
    const double sigma = p_.noise ? noiseAmp_ * std::sqrt(std::pow(10.0, -p_.snrDb / 10) / 2) : 0;
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
    // Pulsed changes wait for the pulse to end, and so does a chain of other
    // recordings; playback changes (a recording can be long) and CW changes apply now.
    if (hasPending_ && canApplyNow()) {
        hasPending_ = false;
        apply(pending_);
    }
    int64_t i = 0;
    while (i < n) {
        if (p_.mode == GenMode::CW) {
            if (p_.sweeping()) sweep(out + i, n - i);
            else carrier(out + i, nullptr, n - i);
            n_ += n - i;
            i = n;
            break;
        }
        const int64_t L = tmplLen();
        if (p_.mode == GenMode::Chain ? waves_.empty() : L == 0) {  // playback without a recording: silence
            std::fill(out + i, out + n, std::complex<float>(0.f, 0.f));
            advanceCarrier(n - i);
            n_ += n - i;
            break;
        }
        if (curStart_ >= 0) {
            const int64_t off = n_ - curStart_;
            const int64_t m = std::min(n - i, L - off);
            carrier(out + i, tmpl_->data() + off, m);
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
        // On time, keep the exact due position. Late (after a change), restart the grid here.
        lastStartF_ = std::llround(nextStart_) == n_ ? nextStart_ : (double)n_;
        curStart_ = lastStart_ = n_;
        if (p_.mode == GenMode::Chain) {
            const int rep = chainRep_;
            startChainItem();
            pulses_.push_back({n_, tmplLen(), curItem_, rep});
        } else {
            pulses_.push_back({n_, L});
        }
        ++burstCount_;
        if (p_.burstPulses > 0 && burstCount_ >= p_.burstPulses) burstCount_ = 0;
        scheduleAfter(lastStartF_);
    }
    impair(out, n);
}

// ---------------------------------------------------------------------------
// Playback recordings
// ---------------------------------------------------------------------------
namespace {
struct WaveKey {
    std::string path;
    FormatParams fp;
    double fileRate = 0, rate = 0, start = 0, len = 0;
    bool operator==(const WaveKey&) const = default;
};
WaveKey waveKey(const WaveformParams& p) { return {p.playPath, p.playFp, playFileRate(p), p.rate, p.playStart, p.playLen}; }

// The cache: every loaded waveform, up to a budget. The least recently used
// ones that nothing else holds are dropped first (issue #27: a chain keeps
// several recordings loaded).
struct CacheEntry {
    WaveKey key;
    std::shared_ptr<const PlaybackWave> w;
    uint64_t used = 0;
};
std::mutex gWaveM;  // guards the cache
std::mutex gLoadM;  // one load at a time, so concurrent users wait for it instead of repeating it
std::vector<CacheEntry> gCache;
uint64_t gUseClock = 0;
int64_t gBudget = kPlayCacheBudget;

int64_t cacheSamplesLocked() {
    int64_t n = 0;
    for (const auto& e : gCache) n += (int64_t)e.w->s.size();
    return n;
}

double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; ++k) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
        if (t < 1e-14 * s) break;
    }
    return s;
}

// Arbitrary-ratio resampler: Kaiser-windowed sinc (about 80 dB stopband),
// with a table of kernel phases interpolated linearly. ratio = in rate / out rate.
bool resample(const std::vector<cf>& x, double ratio, std::vector<cf>& y, const std::function<bool()>& cancelled) {
    const int64_t N = (int64_t)x.size();
    const int64_t M = (int64_t)std::floor((double)N / ratio);
    const double stretch = std::max(1.0, ratio);  // downsampling widens the kernel
    const double cutoff = 0.45 / stretch;         // cycles per input sample
    const int W = (int)std::ceil(32 * stretch);    // taps on each side
    const int T = 2 * W, P = 256;
    const double beta = 8, i0b = besselI0(beta);
    std::vector<float> tab((size_t)(P + 1) * T);
    for (int ph = 0; ph <= P; ++ph)
        for (int j = 0; j < T; ++j) {
            const double u = (double)ph / P + (W - 1) - j;  // distance from the output instant, input samples
            const double r = u / W;
            const double w = std::fabs(r) < 1 ? besselI0(beta * std::sqrt(1 - r * r)) / i0b : 0;
            const double a = 2 * cutoff * u;
            const double sinc = a == 0 ? 1 : std::sin(M_PI * a) / (M_PI * a);
            tab[(size_t)ph * T + j] = (float)(2 * cutoff * sinc * w);
        }
    y.assign((size_t)M, {});
    constexpr int64_t kBlock = 4096;
    std::atomic<bool> stop{false};
    parallelFor((int)((M + kBlock - 1) / kBlock), [&](int b, int e) {
        for (int blk = b; blk < e && !stop; ++blk) {
            if (cancelled && cancelled()) stop = true;
            const int64_t m1 = std::min(M, (int64_t)(blk + 1) * kBlock);
            for (int64_t m = (int64_t)blk * kBlock; m < m1; ++m) {
                const double t = (double)m * ratio;
                const int64_t i0 = (int64_t)std::floor(t);
                const double fp = (t - (double)i0) * P;
                const int ph = std::min(P - 1, (int)fp);
                const float a = (float)(fp - ph);
                const float* h0 = &tab[(size_t)ph * T];
                const float* h1 = h0 + T;
                const int64_t k0 = i0 - (W - 1);
                const int j0 = (int)std::max<int64_t>(0, -k0), j1 = (int)std::min<int64_t>(T, N - k0);
                cf acc = 0;
                for (int j = j0; j < j1; ++j) acc += x[(size_t)(k0 + j)] * (h0[j] + a * (h1[j] - h0[j]));
                y[(size_t)m] = acc;
            }
        }
    });
    return !stop;
}
}  // namespace

std::shared_ptr<const PlaybackWave> cachedPlayback(const WaveformParams& p) {
    const WaveKey k = waveKey(p);
    std::lock_guard<std::mutex> lk(gWaveM);
    for (auto& e : gCache)
        if (e.key == k) {
            e.used = ++gUseClock;
            return e.w;
        }
    return nullptr;
}

void forgetPlayback() {
    std::lock_guard<std::mutex> lk(gWaveM);
    gCache.clear();
}

int64_t playbackCacheSamples() {
    std::lock_guard<std::mutex> lk(gWaveM);
    return cacheSamplesLocked();
}

void setPlaybackCacheBudget(int64_t samples) {
    std::lock_guard<std::mutex> lk(gWaveM);
    gBudget = samples > 0 ? samples : kPlayCacheBudget;
}

std::shared_ptr<const PlaybackWave> loadPlayback(const WaveformParams& p, std::string& err,
                                                 const std::function<bool()>& cancelled) {
    if (auto w = cachedPlayback(p)) return w;
    std::lock_guard<std::mutex> load(gLoadM);
    if (auto w = cachedPlayback(p)) return w;
    if (p.playPath.empty()) {
        err = "no recording chosen";
        return nullptr;
    }
    const double fr = playFileRate(p);
    if (!(fr > 0) || !(p.rate > 0)) {
        err = "sample rates must be positive";
        return nullptr;
    }
    auto f = IqFile::open(p.playPath, err);
    if (!f) return nullptr;
    auto w = std::make_shared<PlaybackWave>();
    w->fileSamples = f->numSamples(p.playFp);
    w->fileRate = fr;
    const int64_t s0 = std::llround(p.playStart * fr);
    if (s0 >= w->fileSamples) {
        err = "the segment starts after the end of the recording (" + fmtSI((double)w->fileSamples / fr, "s") + " long)";
        return nullptr;
    }
    const int64_t n = p.playLen > 0 ? std::min<int64_t>(w->fileSamples - s0, std::llround(p.playLen * fr)) : w->fileSamples - s0;
    const double ratio = fr / p.rate;
    if (n < 1) {
        err = "the segment is empty";
        return nullptr;
    }
    if (n > kMaxPlaySamples || (double)n / ratio > (double)kMaxPlaySamples) {
        err = "the segment is too long to play (" + fmtSI((double)n / fr, "s") + "; up to " +
              fmtSI((double)kMaxPlaySamples, "S") + " at either rate): choose a shorter segment";
        return nullptr;
    }
    std::vector<cf> x((size_t)n);
    for (int64_t done = 0; done < n;) {
        if (cancelled && cancelled()) {
            err = "cancelled";
            return nullptr;
        }
        const int64_t m = std::min<int64_t>(1 << 20, n - done);
        f->read(p.playFp, s0 + done, m, x.data() + done);
        done += m;
    }
    // The level reference is the recording itself, so resampling doesn't change the gain.
    for (auto& v : x) {
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = 0;  // garbage in a float file
        w->peak = std::max(w->peak, (double)std::abs(v));
    }
    if (std::fabs(ratio - 1) < 1e-9) {
        w->s = std::move(x);
    } else if (!resample(x, ratio, w->s, cancelled)) {
        err = "cancelled";
        return nullptr;
    }
    for (const auto& v : w->s) w->outPeak = std::max(w->outPeak, (double)std::abs(v));
    if (w->s.empty() || !(w->peak > 0)) {
        err = "the segment is silent (all zeros)";
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(gWaveM);
    gCache.push_back({waveKey(p), w, ++gUseClock});
    // Over the budget: drop the least recently used waveforms nobody holds (not this one).
    for (int64_t total = cacheSamplesLocked(); total > gBudget;) {
        size_t victim = gCache.size();
        for (size_t i = 0; i + 1 < gCache.size(); ++i)
            if (gCache[i].w.use_count() == 1 && (victim == gCache.size() || gCache[i].used < gCache[victim].used)) victim = i;
        if (victim == gCache.size()) break;
        total -= (int64_t)gCache[victim].w->s.size();
        gCache.erase(gCache.begin() + (std::ptrdiff_t)victim);
    }
    return w;
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
    passes_.store(gen_.chainPasses(), std::memory_order_relaxed);
    finished_.store(gen_.finished(), std::memory_order_relaxed);
    sigGenBus().reportChain(gen_.chainPasses(), gen_.finished());
}
