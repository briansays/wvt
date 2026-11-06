// The Analysis window (issue #111, "Signal analysis" in docs/MANUAL.md): one
// tab for each kind in src/dsp/analysis.h. File mode only. Every tab uses the
// same input: a region of the file (the view, the time markers, a bookmark or
// the segment) through the channel filter of the viewer (or the full band).
// The open tab's calculation runs on its own worker; each tab keeps its last
// result, so a tab shows again at once.
#include <sys/stat.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "app/platform_x11.h"
#include "common/log.h"
#include "io/recorder.h"

namespace {
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColA(0.6f, 0.85f, 1.f, 1.f);
const ImVec4 kColB(0.9f, 0.4f, 0.3f, 0.8f);
const ImVec4 kColC(0.5f, 0.9f, 0.5f, 0.8f);
const ImVec4 kColCursor(1.f, 0.35f, 0.35f, 0.7f);
const char* kRegionNames[] = {"View", "Time markers", "Bookmark", "Whole segment"};
const int kNffts[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576};

float emW(float chars) { return ImGui::GetFontSize() * chars * 0.55f + ImGui::GetStyle().FramePadding.x * 2; }

bool nfftCombo(const char* id, int& n) {
    char cur[32];
    snprintf(cur, sizeof cur, "%d", n);
    ImGui::SetNextItemWidth(emW(9));
    bool ch = false;
    if (ImGui::BeginCombo(id, cur)) {
        for (int v : kNffts) {
            char b[32];
            snprintf(b, sizeof b, "%d", v);
            if (ImGui::Selectable(b, v == n)) {
                n = v;
                ch = true;
            }
        }
        ImGui::EndCombo();
    }
    return ch;
}

bool windowCombo(const char* id, int& w) {
    ImGui::SetNextItemWidth(emW(15));
    bool ch = false;
    if (ImGui::BeginCombo(id, windowName((WindowType)w))) {
        for (int i = 0; i < (int)WindowType::Count; ++i)
            if (ImGui::Selectable(windowName((WindowType)i), i == w)) {
                w = i;
                ch = true;
            }
        ImGui::EndCombo();
    }
    return ch;
}

template <class E>
bool enumCombo(const char* id, int& v, int count, const char* (*name)(E), float width = 12) {
    ImGui::SetNextItemWidth(emW(width));
    bool ch = false;
    if (ImGui::BeginCombo(id, name((E)v))) {
        for (int i = 0; i < count; ++i)
            if (ImGui::Selectable(name((E)i), i == v)) {
                v = i;
                ch = true;
            }
        ImGui::EndCombo();
    }
    return ch;
}

void labelled(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
}

// A row of the readout table: name, value.
void readout(const char* name, const std::string& value, const char* tip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}
std::string dbText(double v, const char* unit) {
    if (!std::isfinite(v)) return "-";
    char b[48];
    snprintf(b, sizeof b, "%.2f %s", v, unit);
    return b;
}
std::string numText(double v, const char* fmt) {
    if (!std::isfinite(v)) return "-";
    char b[48];
    snprintf(b, sizeof b, fmt, v);
    return b;
}

// A value where 0 means "not set" (an empty box with the hint).
void optionalSI(const char* id, double& v, const char* unit, float chars, const char* hint) {
    double x = v > 0 ? v : -1;
    if (InputSIOptional(id, &x, unit, emW(chars), hint)) v = x > 0 ? x : 0;
}

// The range of the finite values of v, with a margin of `pad` (for a plot
// that is fitted once for each result, and then pans and zooms freely).
void finiteRange(const std::vector<double>& v, double pad, double& lo, double& hi) {
    lo = INFINITY;
    hi = -INFINITY;
    for (double x : v)
        if (std::isfinite(x)) {
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
    if (!(hi >= lo)) lo = hi = 0;
    lo -= pad;
    hi += pad;
}

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
void App::openAnalysis(int kind) {
    anOpen_ = true;
    if (kind >= 0 && kind < kAnaKinds) anTabWant_ = kind;
}

bool App::setAnalysisOptions(const std::string& list, std::string& err) {
    AnaReq r = anReq_;
    r.reg.src.fs = fs_;
    // cursors=0|1 is the window's (the symbol cursors in the viewer), not a setting of a tab.
    std::string rest;
    size_t a = 0;
    while (a <= list.size()) {
        size_t e = list.find(',', a);
        if (e == std::string::npos) e = list.size();
        const std::string kv = list.substr(a, e - a);
        if (kv.rfind("cursors=", 0) == 0) anViewerCursors_ = kv.substr(8) != "0";
        else if (!kv.empty()) rest += (rest.empty() ? "" : ",") + kv;
        a = e + 1;
    }
    if (!anaSetOptions(r, rest, err)) return false;
    anReq_ = r;
    anMaxSamples_ = (double)r.reg.maxSamples;
    return true;
}

bool App::analysisBusy() const { return anWorker_ && anWorker_->busy(); }

void App::pollAnalysis() {
    if (!anWorker_) return;
    while (auto r = anWorker_->take()) {
        // A job that ran while a new file or format opened is for the old one.
        const Source& rs = r->req.reg.src;
        if (rs.file != file_ || !(rs.fp == fp_) || rs.fs != fs_ || rs.fc != fc_) continue;
        const int k = (int)r->req.kind;
        if (k >= 0 && k < kAnaKinds) anRes_[k] = r;
        // A new result can have the address of an old one.
        for (AnTex& t : anTex_) t.key = nullptr;
        anBitsFor_ = nullptr;
        std::fill(std::begin(anFitFor_), std::end(anFitFor_), nullptr);
        if (!r->err.empty()) LOG_DEBUG("analysis", "%s: %s", anaKindName(r->req.kind), r->err.c_str());
        else LOG_DEBUG("analysis", "%s: %lld samples in %.0f ms", anaKindName(r->req.kind), (long long)r->used, r->seconds * 1e3);
    }
}

void App::clearAnalysis() {
    for (auto& r : anRes_) r.reset();
    anBitsFor_ = nullptr;
    anPulseSel_ = -1;
    if (anWorker_) anWorker_->invalidate();
}

// The region and the channel, from the window's settings. False (and why)
// when there is no region.
bool App::analysisRegion(AnaRegion& g, std::string& why) const {
    g = AnaRegion();
    if (!file_) {
        why = "Open a file first.";
        return false;
    }
    g.src.file = file_;
    g.src.fp = fp_;
    g.src.fs = fs_;
    g.src.fc = fc_;
    double t0 = tmin_, t1 = tmax_;
    switch (anRegion_) {
        case 1:
            if (!tMarkers_) {
                why = "Turn on the time markers (View > Time markers, or T), and put them on the signal.";
                return false;
            }
            t0 = std::min(tm1_, tm2_);
            t1 = std::max(tm1_, tm2_);
            break;
        case 2:
            if (anBookmark_ < 0 || anBookmark_ >= (int)bookmarks_.size()) {
                why = "There is no bookmark. Add one with the bookmark button (or B), or select a bookmark.";
                return false;
            }
            t0 = bookmarks_[anBookmark_].t0;
            t1 = bookmarks_[anBookmark_].t1;
            break;
        case 3:
            t0 = tBegin();
            t1 = tEnd();
            break;
        default: break;
    }
    g.s0 = std::clamp<int64_t>((int64_t)std::floor(t0 * fs_), segS0_, segS1_);
    g.s1 = std::clamp<int64_t>((int64_t)std::ceil(t1 * fs_), segS0_, segS1_);
    if (anUseChan_ && demod_.chanEnable) {
        g.chanEnable = true;
        g.chanOffset = demod_.chanOffset;
        g.chanBw = demod_.chanBw;
        g.taps = demod_.taps;
        g.decim = demod_.decim;
    }
    g.maxSamples = (int64_t)std::clamp(anMaxSamples_, 1e4, 256e6);
    if (g.s1 - g.s0 < 16) {
        why = "The region is too short. Select a longer region.";
        return false;
    }
    return true;
}

// The time range of the correlation template, or false.
bool App::analysisTemplate(double& t0, double& t1, std::string& why) const {
    if (anTmplSrc_ == 0) {
        if (!tMarkers_) {
            why = "Turn on the time markers (T) and put them around the signal to find.";
            return false;
        }
        t0 = std::min(tm1_, tm2_);
        t1 = std::max(tm1_, tm2_);
        return true;
    }
    if (anTmplBookmark_ < 0 || anTmplBookmark_ >= (int)bookmarks_.size()) {
        why = "Select the bookmark that has the signal to find.";
        return false;
    }
    t0 = bookmarks_[anTmplBookmark_].t0;
    t1 = bookmarks_[anTmplBookmark_].t1;
    return true;
}

// ---------------------------------------------------------------------------
// File sessions: the window's state as the "analysis" object
// ---------------------------------------------------------------------------
namespace {
const char* kRegionKeys[] = {"view", "markers", "bookmark", "segment"};

// The index of name in names (an enum's name function), or def.
template <class E>
int indexOf(const std::string& name, int count, const char* (*fn)(E), int def) {
    for (int i = 0; i < count; ++i)
        if (name == fn((E)i)) return i;
    return def;
}
}  // namespace

JsonValue App::analysisToJson() const {
    const AnaReq& r = anReq_;
    auto n = [](double v) { return jsonNumber(v); };
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    auto q = [](const char* s) { return jsonQuote(s); };
    auto i = [](int v) { return std::to_string(v); };
    std::string j = "{\"open\": " + b(anOpen_) + ", \"tab\": " + q(anaKindName((AnaKind)anTab_)) +
                    ", \"region\": " + q(kRegionKeys[std::clamp(anRegion_, 0, 3)]) + ", \"bookmark\": " + i(anBookmark_) +
                    ", \"channel\": " + b(anUseChan_) + ", \"max_samples\": " + n(anMaxSamples_);
    j += ", \"measure\": {\"nfft\": " + i(r.measure.nfft) + ", \"window\": " + q(windowName((WindowType)r.measure.window)) + "}";
    j += ", \"spectrum\": {\"nfft\": " + i(r.spectrum.nfft) + ", \"window\": " + q(windowName((WindowType)r.spectrum.window)) +
         ", \"overlap\": " + n(r.spectrum.overlap) + ", \"max\": " + b(anSpecMax_) + ", \"min\": " + b(anSpecMin_) + "}";
    j += ", \"histogram\": {\"bins\": " + i(r.hist.bins) + ", \"show\": " + q(histQtyName((HistQty)anHistQty_)) +
         ", \"log\": " + b(anHistLog_) + "}";
    j += ", \"constellation\": {\"carrier\": " + q(carrierFixName((CarrierFix)r.cons.carrier)) + ", \"manual_hz\": " +
         n(r.cons.manualHz) + ", \"power\": " + i(r.cons.power) + ", \"normalize\": " + b(r.cons.normalize) +
         ", \"symbols\": " + b(anConsSymbols_) + ", \"density\": " + b(anConsDensity_) + "}";
    j += ", \"pulses\": {\"auto\": " + b(r.pulse.autoThreshold) + ", \"threshold_db\": " + n(r.pulse.thresholdDb) +
         ", \"hysteresis_db\": " + n(r.pulse.hysteresisDb) + ", \"min_width_s\": " + n(r.pulse.minWidthS) +
         ", \"smooth\": " + i(r.pulse.smooth) + "}";
    j += ", \"correlation\": {\"search\": " + b(r.corr.search) + ", \"template\": " + q(anTmplSrc_ ? "bookmark" : "markers") +
         ", \"template_bookmark\": " + i(anTmplBookmark_) + ", \"look_in\": " + q(anSearchView_ ? "view" : "segment") +
         ", \"threshold\": " + n(r.corr.threshold) + "}";
    j += ", \"symbols\": {\"detector\": " + q(symMethodName((SymMethod)r.sym.method)) + ", \"rate\": " + n(r.sym.rate) +
         ", \"min_rate\": " + n(r.sym.minRate) + ", \"max_rate\": " + n(r.sym.maxRate) + ", \"have_t0\": " +
         b(r.sym.haveT0) + ", \"t0_s\": " + n(r.sym.t0) + ", \"trace\": " + q(symTraceName((SymTrace)r.sym.trace)) +
         ", \"power\": " + i(r.sym.power) + ", \"viewer_cursors\": " + b(anViewerCursors_) + "}";
    j += ", \"cyclo\": {\"np\": " + i(r.cyclo.np) + ", \"blocks\": " + i(r.cyclo.maxP) + ", \"range_db\": " +
         n(anCycloRange_) + "}";
    j += ", \"demod\": {\"mode\": " + q(demodModeName((DemodMode)r.digital.mode)) + ", \"rate\": " + n(r.digital.rate) +
         ", \"auto\": " + b(r.digital.autoThreshold) + ", \"threshold\": " + n(r.digital.threshold) +
         ", \"differential\": " + b(r.digital.differential) + ", \"loop\": " + n(r.digital.loopGain) +
         ", \"hex\": " + b(anBitsHex_) + ", \"file\": " + jsonQuote(anBitsPath_) + "}}";
    JsonValue v;
    std::string err;
    if (!parseJson(j, v, err)) LOG_ERROR("analysis", "session state is not JSON: %s", err.c_str());
    return v;
}

void App::analysisFromJson(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) return;
    AnaReq& r = anReq_;
    const AnaReq def;
    auto num = [](const JsonValue* o, const char* k, double d, double lo, double hi) {
        const double x = o ? o->number(k, d) : d;
        return std::isfinite(x) ? std::clamp(x, lo, hi) : d;
    };
    auto pow2 = [&](const JsonValue* o, const char* k, int d, int lo, int hi) {
        return std::clamp(nextPow2((int64_t)num(o, k, d, lo, hi)), lo, hi);
    };
    auto win = [](const JsonValue* o, int d) {
        return o ? indexOf<WindowType>(o->string("window"), (int)WindowType::Count, windowName, d) : d;
    };
    anOpen_ = v.boolean("open");
    AnaKind k;
    anTab_ = anaKindFromName(v.string("tab"), k) ? (int)k : 0;
    anTabWant_ = anTab_;
    anRegion_ = 0;
    for (int i = 0; i < 4; ++i)
        if (v.string("region") == kRegionKeys[i]) anRegion_ = i;
    anBookmark_ = (int)num(&v, "bookmark", 0, 0, 1e6);
    anUseChan_ = v.boolean("channel", true);
    anMaxSamples_ = num(&v, "max_samples", 16e6, 1e4, 256e6);
    if (const JsonValue* o = v.get("measure")) {
        r.measure.nfft = pow2(o, "nfft", def.measure.nfft, 64, 1 << 20);
        r.measure.window = win(o, def.measure.window);
    }
    if (const JsonValue* o = v.get("spectrum")) {
        r.spectrum.nfft = pow2(o, "nfft", def.spectrum.nfft, 64, 1 << 20);
        r.spectrum.window = win(o, def.spectrum.window);
        r.spectrum.overlap = num(o, "overlap", def.spectrum.overlap, 0, 0.95);
        anSpecMax_ = o->boolean("max", true);
        anSpecMin_ = o->boolean("min", false);
    }
    if (const JsonValue* o = v.get("histogram")) {
        r.hist.bins = (int)num(o, "bins", def.hist.bins, 10, 4096);
        anHistQty_ = indexOf<HistQty>(o->string("show"), (int)HistQty::Count, histQtyName, 0);
        anHistLog_ = o->boolean("log");
    }
    if (const JsonValue* o = v.get("constellation")) {
        r.cons.carrier = indexOf<CarrierFix>(o->string("carrier"), (int)CarrierFix::Count, carrierFixName, def.cons.carrier);
        r.cons.manualHz = num(o, "manual_hz", 0, -1e12, 1e12);
        const int p = (int)num(o, "power", 4, 2, 8);
        r.cons.power = p == 2 || p == 8 ? p : 4;
        r.cons.normalize = o->boolean("normalize", true);
        anConsSymbols_ = o->boolean("symbols");
        anConsDensity_ = o->boolean("density", true);
    }
    if (const JsonValue* o = v.get("pulses")) {
        r.pulse.autoThreshold = o->boolean("auto", true);
        r.pulse.thresholdDb = num(o, "threshold_db", def.pulse.thresholdDb, -300, 300);
        r.pulse.hysteresisDb = num(o, "hysteresis_db", def.pulse.hysteresisDb, 0, 60);
        r.pulse.minWidthS = num(o, "min_width_s", 0, 0, 1e6);
        r.pulse.smooth = (int)num(o, "smooth", 1, 1, 65536);
    }
    if (const JsonValue* o = v.get("correlation")) {
        r.corr.search = o->boolean("search");
        anTmplSrc_ = o->string("template") == "bookmark" ? 1 : 0;
        anTmplBookmark_ = (int)num(o, "template_bookmark", 0, 0, 1e6);
        anSearchView_ = o->string("look_in") == "view";
        r.corr.threshold = num(o, "threshold", def.corr.threshold, 0.05, 1);
    }
    if (const JsonValue* o = v.get("symbols")) {
        r.sym.method = indexOf<SymMethod>(o->string("detector"), (int)SymMethod::Count, symMethodName, def.sym.method);
        r.sym.rate = num(o, "rate", 0, 0, 1e12);
        r.sym.minRate = num(o, "min_rate", 0, 0, 1e12);
        r.sym.maxRate = num(o, "max_rate", 0, 0, 1e12);
        r.sym.haveT0 = o->boolean("have_t0");
        r.sym.t0 = num(o, "t0_s", 0, -1e12, 1e12);
        r.sym.trace = indexOf<SymTrace>(o->string("trace"), (int)SymTrace::Count, symTraceName, def.sym.trace);
        const int p = (int)num(o, "power", 4, 2, 8);
        r.sym.power = p == 2 || p == 8 ? p : 4;
        anViewerCursors_ = o->boolean("viewer_cursors");
    }
    if (const JsonValue* o = v.get("cyclo")) {
        r.cyclo.np = pow2(o, "np", def.cyclo.np, 16, 256);
        r.cyclo.maxP = pow2(o, "blocks", def.cyclo.maxP, 16, 8192);
        anCycloRange_ = num(o, "range_db", 40, 5, 120);
    }
    if (const JsonValue* o = v.get("demod")) {
        r.digital.mode = indexOf<DemodMode>(o->string("mode"), (int)DemodMode::Count, demodModeName, def.digital.mode);
        r.digital.rate = num(o, "rate", 0, 0, 1e12);
        r.digital.autoThreshold = o->boolean("auto", true);
        r.digital.threshold = num(o, "threshold", 0, -1e12, 1e12);
        r.digital.differential = o->boolean("differential");
        r.digital.loopGain = num(o, "loop", def.digital.loopGain, 0, 0.5);
        anBitsHex_ = o->boolean("hex");
        const std::string f = o->string("file");
        if (!f.empty()) snprintf(anBitsPath_, sizeof anBitsPath_, "%s", f.c_str());
    }
}

// ---------------------------------------------------------------------------
// Textures for the density plots
// ---------------------------------------------------------------------------
void App::analysisTexture(int slot, const void* key, const std::vector<float>& v, int w, int h, bool logScale, float lo,
                          float hi) {
    AnTex& t = anTex_[slot];
    if (t.key == key && t.cmap == cmap_ && t.lo == lo && t.hi == hi && t.id) return;
    if (lutCmap_ != cmap_) buildLut();
    std::vector<uint32_t> px((size_t)w * h);
    float mx = 0;
    if (logScale)
        for (float x : v) mx = std::max(mx, x);
    const float lmx = std::log1p(std::max(mx, 1.f));
    for (size_t i = 0; i < px.size() && i < v.size(); ++i) {
        float f;
        if (logScale) f = v[i] > 0 ? 0.08f + 0.92f * std::log1p(v[i]) / lmx : 0.f;
        else f = (v[i] - lo) / std::max(1e-6f, hi - lo);
        const int k = std::clamp((int)(f * 255.f), 0, 255);
        px[i] = logScale && v[i] <= 0 ? IM_COL32(12, 12, 16, 255) : lut_[k];
    }
    win_.updateTexture(t.id, w, h, px.data());
    t.key = key;
    t.cmap = cmap_;
    t.lo = lo;
    t.hi = hi;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
void App::drawAnalysisWindow() {
    pollAnalysis();
    if (!anOpen_ || liveMode_) return;
    const float fs = ImGui::GetFontSize();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(fs * 62, fs * 42), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + fs * 4, vp->WorkPos.y + fs * 4), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Analysis###analysis", &anOpen_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    snapClipToWindow();
    if (!file_) {
        ImGui::TextDisabled("Open a file first. The analysis tools work on the file viewer's data.");
        ImGui::End();
        snapClipReset();
        return;
    }

    // --- the input ---
    labelled("Region");
    ImGui::SetNextItemWidth(emW(15));
    ImGui::Combo("##anregion", &anRegion_, kRegionNames, IM_ARRAYSIZE(kRegionNames));
    ImGui::SetItemTooltip("The samples to analyze: the time range of the view, between the time markers,\n"
                          "of a bookmark, or the whole loaded segment.");
    if (anRegion_ == 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(emW(18));
        const char* bp = anBookmark_ >= 0 && anBookmark_ < (int)bookmarks_.size() ? bookmarks_[anBookmark_].name.c_str() : "(none)";
        if (ImGui::BeginCombo("##anbm", bp)) {
            for (int i = 0; i < (int)bookmarks_.size(); ++i)
                if (ImGui::Selectable(bookmarks_[i].name.c_str(), i == anBookmark_)) anBookmark_ = i;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(0, fs);
    ImGui::Checkbox("Channel filter", &anUseChan_);
    ImGui::SetItemTooltip("On: analyze the output of the viewer's channel filter (when it is on).\n"
                          "Off: analyze the full band of the file.");
    ImGui::SameLine(0, fs);
    labelled("Max samples");
    InputSI("##anmax", &anMaxSamples_, "", emW(8), 4);
    ImGui::SetItemTooltip("The most samples that the histogram, constellation, correlation, symbol,\n"
                          "cyclostationary and demodulation tabs hold in memory (8 bytes each).\n"
                          "The measurement, spectrum and pulse tabs read the whole region in blocks.");

    AnaRegion reg;
    std::string why;
    const bool haveReg = analysisRegion(reg, why);
    {
        const ChannelReader rd(reg.src, reg.chanEnable, reg.chanOffset, reg.chanBw, reg.taps, reg.decim);
        const int64_t n = haveReg ? rd.outIndex(reg.s1) - rd.outIndex(reg.s0) : 0;
        ImGui::TextDisabled("%s to %s (%s), %s samples at %s%s", fmtSI(reg.s0 / fs_, "s", 7).c_str(),
                            fmtSI(reg.s1 / fs_, "s", 7).c_str(), fmtSI((reg.s1 - reg.s0) / fs_, "s", 4).c_str(),
                            fmtSI((double)n, "", 4).c_str(), fmtSI(rd.rate(), "S/s", 5).c_str(),
                            reg.chanEnable ? (", channel at " + fmtSI(rd.center(), "Hz", 7) + ", " +
                                              fmtSI(reg.chanBw, "Hz", 4) + " wide").c_str()
                                           : ", full band");
    }

    // --- the tabs ---
    int shown = -1;
    if (ImGui::BeginTabBar("##antabs")) {
        for (int k = 0; k < kAnaKinds; ++k) {
            const ImGuiTabItemFlags fl = anTabWant_ == k ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(anaKindLabel((AnaKind)k), nullptr, fl)) {
                shown = k;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    anTabWant_ = -1;
    if (shown < 0) {
        ImGui::End();
        snapClipReset();
        return;
    }
    anTab_ = shown;

    // --- the request for the open tab ---
    AnaReq req = anReq_;
    req.kind = (AnaKind)shown;
    req.reg = std::move(reg);
    bool ready = haveReg;
    if (req.kind == AnaKind::Spectrum) {
        req.spectrum.band = fMarkers_;
        req.spectrum.fLo = std::min(fm1_, fm2_);
        req.spectrum.fHi = std::max(fm1_, fm2_);
    }
    if (req.kind == AnaKind::Correlation && req.corr.search) {
        double a = 0, b = 0;
        ready = analysisTemplate(a, b, why);
        req.corr.tmplS0 = (int64_t)std::llround(a * fs_);
        req.corr.tmplS1 = (int64_t)std::llround(b * fs_);
        if (anSearchView_) {
            req.corr.searchS0 = std::clamp<int64_t>((int64_t)std::floor(tmin_ * fs_), segS0_, segS1_);
            req.corr.searchS1 = std::clamp<int64_t>((int64_t)std::ceil(tmax_ * fs_), segS0_, segS1_);
        } else {
            req.corr.searchS0 = segS0_;
            req.corr.searchS1 = segS1_;
        }
    }
    if (req.kind == AnaKind::Constellation && anConsSymbols_) {
        const auto& sr = anRes_[(int)AnaKind::Symbols];
        if (sr && sr->sym.rate > 0) {
            req.cons.symbolRate = sr->sym.rate;
            req.cons.symbolT0 = sr->sym.t0;
        }
    }
    if (!(req.kind == AnaKind::Constellation && anConsSymbols_)) {
        req.cons.symbolRate = 0;
        req.cons.symbolT0 = 0;
    }
    req = anaOnlyKind(req);
    if (ready) {
        const auto& cur = anRes_[shown];
        if (!cur || !(cur->req == req)) anWorker_->submit(req);
    }

    // --- status ---
    const std::shared_ptr<AnaRes> res = anRes_[shown];
    if (!ready) {
        ImGui::TextColored(kColWarn, "%s", why.c_str());
    } else if (anWorker_->busy()) {
        const float p = anWorker_->progress();
        if (p >= 0) ImGui::TextDisabled("Calculating... %d %%", (int)(p * 100));
        else ImGui::TextDisabled("Calculating...");
    } else if (res && !res->err.empty()) {
        ImGui::TextColored(kColWarn, "%s", res->err.c_str());
    } else if (res) {
        const bool cut = res->used < res->wanted;
        ImGui::TextDisabled("%s samples in %.0f ms%s", fmtSI((double)res->used, "", 4).c_str(), res->seconds * 1e3,
                            cut ? "  (the first part of the region only: see Max samples)" : "");
    } else {
        ImGui::TextDisabled(" ");
    }
    ImGui::Separator();

    const bool fresh = res && res->err.empty() && ready;
    switch ((AnaKind)shown) {
        case AnaKind::Measure: drawAnMeasure(fresh ? res.get() : nullptr); break;
        case AnaKind::Spectrum: drawAnSpectrum(fresh ? res.get() : nullptr); break;
        case AnaKind::Histogram: drawAnHistogram(fresh ? res.get() : nullptr); break;
        case AnaKind::Constellation: drawAnConstellation(fresh ? res.get() : nullptr); break;
        case AnaKind::Pulses: drawAnPulses(fresh ? res.get() : nullptr); break;
        case AnaKind::Correlation: drawAnCorrelation(fresh ? res.get() : nullptr); break;
        case AnaKind::Symbols: drawAnSymbols(fresh ? res.get() : nullptr); break;
        case AnaKind::Cyclo: drawAnCyclo(fresh ? res.get() : nullptr); break;
        case AnaKind::Demod: drawAnDemod(fresh ? res.get() : nullptr); break;
        default: break;
    }
    ImGui::End();
    snapClipReset();
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------
void App::drawAnMeasure(const AnaRes* r) {
    MeasureParams& p = anReq_.measure;
    labelled("FFT size");
    nfftCombo("##anmnfft", p.nfft);
    ImGui::SameLine();
    labelled("Window");
    windowCombo("##anmwin", p.window);
    if (!r) return;
    const MeasureRes& m = r->measure;
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginTable("##anmtab", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg, ImVec2(fs * 22, 0))) {
        readout("Mean power", dbText(m.meanDb, "dBFS"));
        readout("Peak power", dbText(m.peakDb, "dBFS"));
        readout("PAPR", dbText(m.paprDb, "dB"), "Peak power over mean power (crest factor)");
        readout("Carrier", fmtSI(m.carrierHz, "Hz", 9), "The peak of the spectrum, interpolated");
        readout("Centroid", fmtSI(m.centroidHz, "Hz", 9), "The power-weighted mean frequency (over the noise floor)");
        readout("Occupied BW (99 %)", fmtSI(m.obwHz, "Hz", 5), "The band that has 99 % of the power");
        readout("  from", fmtSI(m.obwLo, "Hz", 9));
        readout("  to", fmtSI(m.obwHi, "Hz", 9));
        readout("-26 dB bandwidth", fmtSI(m.bw26Hz, "Hz", 5), "From the lowest to the highest bin 26 dB under the peak");
        readout("Noise floor", dbText(m.noiseDb, "dBFS/bin"), "The 20th percentile of the spectrum bins");
        readout("SNR", dbText(m.snrDb, "dB"), "In the occupied bandwidth, over the noise floor");
        readout("DC offset", numText(m.dcI, "I %.5f") + numText(m.dcQ, ", Q %.5f"));
        readout("DC power", dbText(m.dcDb, "dBc"), "The DC power over the total power");
        readout("I/Q gain imbalance", dbText(m.gainImbDb, "dB"), "The I power over the Q power, after the DC is removed");
        readout("I/Q phase imbalance", numText(m.phaseImbDeg, "%.3f degrees"), "The angle from 90 degrees between I and Q");
        readout("RBW", fmtSI(m.rbwHz, "Hz", 5));
        ImGui::EndTable();
    }
    ImGui::SameLine();
    if (ImPlot::BeginPlot("##anmplot", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        // Fitted once for each result (issue #112): then it pans and zooms like
        // the viewer's spectrum. Double-click fits it again.
        double lo, hi;
        finiteRange(m.psdDb, 5, lo, hi);
        const ImPlotCond cond = anFitFor_[(int)AnaKind::Measure] != r ? ImPlotCond_Always : ImPlotCond_Once;
        anFitFor_[(int)AnaKind::Measure] = r;
        ImPlot::SetupAxes("Frequency", "dBFS");
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"Hz");
        if (!m.f.empty()) ImPlot::SetupAxesLimits(m.f.front(), m.f.back(), lo, hi, cond);
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("Spectrum", m.f.data(), m.psdDb.data(), (int)m.f.size());
        // Shaded down to the lowest bin. Not -INFINITY: ImPlot makes that the
        // current axis minimum, which a fit then moves down on each frame (issue #112).
        ImPlot::SetNextFillStyle(kColC, 0.12f);
        ImPlot::PlotShaded("99 %", std::vector<double>{m.obwLo, m.obwHi}.data(),
                           std::vector<double>{m.noiseDb, m.noiseDb}.data(), 2, lo + 5);
        ImPlot::SetNextLineStyle(kColC);
        double nx[2] = {m.f.empty() ? 0 : m.f.front(), m.f.empty() ? 0 : m.f.back()}, ny[2] = {m.noiseDb, m.noiseDb};
        ImPlot::PlotLine("Noise floor", nx, ny, 2);
        double cx[1] = {m.carrierHz};
        ImPlot::SetNextLineStyle(kColB);
        ImPlot::PlotInfLines("Carrier", cx, 1);
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Spectrum
// ---------------------------------------------------------------------------
void App::drawAnSpectrum(const AnaRes* r) {
    SpectrumParams& p = anReq_.spectrum;
    labelled("FFT size");
    nfftCombo("##ansnfft", p.nfft);
    ImGui::SameLine();
    labelled("Window");
    windowCombo("##answin", p.window);
    ImGui::SameLine();
    labelled("Overlap");
    double ov = p.overlap * 100;
    if (InputSI("##ansov", &ov, "%", emW(6), 3)) p.overlap = std::clamp(ov, 0.0, 95.0) / 100;
    ImGui::SameLine();
    ImGui::Checkbox("Max", &anSpecMax_);
    ImGui::SameLine();
    ImGui::Checkbox("Min", &anSpecMin_);
    if (!r) return;
    const SpectrumRes& s = r->spectrum;
    std::string line = "RBW " + fmtSI(s.rbwHz, "Hz", 4) + ", " + std::to_string(s.averages) + " averages, total " +
                       dbText(s.totalDb, "dBFS");
    if (r->req.spectrum.band)
        line += ".  Between the frequency markers: " + dbText(s.bandDb, "dBFS") + ", ACPR lower " +
                dbText(s.acprLoDb, "dBc") + ", upper " + dbText(s.acprHiDb, "dBc");
    else
        line += ".  Turn on the frequency markers (F) for the channel power and the ACPR.";
    ImGui::TextUnformatted(line.c_str());
    ImGui::SetItemTooltip("ACPR: the power in the bands of the same width below and above the band\n"
                          "between the frequency markers, relative to the power in that band.");
    if (ImPlot::BeginPlot("##ansplot", ImVec2(-1, -1))) {
        double lo, hi, l2, h2;
        finiteRange(s.avgDb, 5, lo, hi);
        if (anSpecMax_) {
            finiteRange(s.maxDb, 5, l2, h2);
            hi = std::max(hi, h2);
        }
        if (anSpecMin_) {
            finiteRange(s.minDb, 5, l2, h2);
            lo = std::max(std::min(lo, l2), hi - 200);  // an empty bin is -200 dB
        }
        const ImPlotCond cond = anFitFor_[(int)AnaKind::Spectrum] != r ? ImPlotCond_Always : ImPlotCond_Once;
        anFitFor_[(int)AnaKind::Spectrum] = r;
        ImPlot::SetupAxes("Frequency", "dBFS in one bin");
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"Hz");
        if (!s.f.empty()) ImPlot::SetupAxesLimits(s.f.front(), s.f.back(), lo, hi, cond);
        const int n = (int)s.f.size();
        if (anSpecMax_) {
            ImPlot::SetNextLineStyle(kColB);
            ImPlot::PlotLine("Max hold", s.f.data(), s.maxDb.data(), n);
        }
        if (anSpecMin_) {
            ImPlot::SetNextLineStyle(kColC);
            ImPlot::PlotLine("Min hold", s.f.data(), s.minDb.data(), n);
        }
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("Average", s.f.data(), s.avgDb.data(), n);
        if (r->req.spectrum.band) {
            const double lo = r->req.spectrum.fLo, hi = r->req.spectrum.fHi, w = hi - lo;
            double b[2] = {lo, hi};
            ImPlot::SetNextLineStyle(ImVec4(1.f, 0.6f, 0.9f, 1.f));
            ImPlot::PlotInfLines("Band", b, 2);
            double a[4] = {lo - w, lo, hi, hi + w};
            ImPlot::SetNextLineStyle(ImVec4(1.f, 0.6f, 0.9f, 0.35f));
            ImPlot::PlotInfLines("Adjacent", a, 4);
        }
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Histograms and CCDF
// ---------------------------------------------------------------------------
void App::drawAnHistogram(const AnaRes* r) {
    HistogramParams& p = anReq_.hist;
    labelled("Show");
    enumCombo<HistQty>("##anhq", anHistQty_, (int)HistQty::Count, histQtyName, 6);
    ImGui::SameLine();
    labelled("Bins");
    double b = p.bins;
    if (InputSI("##anhbins", &b, "", emW(6), 4)) p.bins = std::clamp((int)b, 10, 4096);
    ImGui::SameLine();
    ImGui::Checkbox("Log counts", &anHistLog_);
    if (!r) return;
    const HistogramRes& h = r->hist;
    const Histogram& g = h.h[anHistQty_];
    const char* units[] = {"FS", "FS", "FS", "Hz", "rad"};
    ImGui::Text("Mean %s, standard deviation %s.   CCDF: %.2f dB at 1 %%, %.2f dB at 0.1 %%, %.2f dB at 0.01 %%, peak %.2f dB",
                fmtSI(g.mean, units[anHistQty_], 5).c_str(), fmtSI(g.stdev, units[anHistQty_], 5).c_str(), h.papr1e2,
                h.papr1e3, h.papr1e4, h.peakDb);
    const float w = ImGui::GetContentRegionAvail().x;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    if (ImPlot::BeginPlot("##anhplot", ImVec2((w - sp) * 0.55f, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(histQtyName((HistQty)anHistQty_), "Count", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        if (anHistLog_) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)units[anHistQty_]);
        const int n = (int)g.counts.size();
        const double bw = (g.hi - g.lo) / std::max(1, n);
        std::vector<double> x(n), y(n);
        for (int i = 0; i < n; ++i) {
            x[i] = g.lo + (i + 0.5) * bw;
            y[i] = anHistLog_ ? std::max(0.5, g.counts[i]) : g.counts[i];
        }
        ImPlot::SetNextFillStyle(kColA, 0.8f);
        ImPlot::PlotBars("##h", x.data(), y.data(), n, bw);
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    if (ImPlot::BeginPlot("CCDF of the power##anccdf", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("dB over the mean power", "Probability", ImPlotAxisFlags_AutoFit, 0);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 1e-6, 1, ImPlotCond_Always);
        std::vector<double> y = h.ccdfProb;
        for (double& v : y) v = std::max(v, 1e-7);
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("Measured", h.ccdfDb.data(), y.data(), (int)y.size());
        // the CCDF of complex Gaussian noise, for reference
        std::vector<double> gx, gy;
        for (int i = 0; i <= 120; ++i) {
            const double x = i * 0.1;
            gx.push_back(x);
            gy.push_back(std::exp(-std::pow(10.0, x / 10)) / std::exp(-1.0));
        }
        ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.7f, 0.7f, 0.6f));
        ImPlot::PlotLine("Gaussian noise", gx.data(), gy.data(), (int)gx.size());
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Constellation
// ---------------------------------------------------------------------------
void App::drawAnConstellation(const AnaRes* r) {
    ConstellationParams& p = anReq_.cons;
    labelled("Carrier offset");
    enumCombo<CarrierFix>("##ancfix", p.carrier, (int)CarrierFix::Count, carrierFixName, 11);
    ImGui::SetItemTooltip("How to remove a carrier offset that is left after the channel filter:\n"
                          "None; Manual (type it); Mean frequency (FM, FSK, a tone);\n"
                          "x^N peak (M-PSK: also removes the mean phase).");
    if (p.carrier == (int)CarrierFix::Manual) {
        ImGui::SameLine();
        InputSI("##ancman", &p.manualHz, "Hz", emW(10));
    } else if (p.carrier == (int)CarrierFix::PowerN) {
        ImGui::SameLine();
        labelled("N");
        int pi = p.power == 2 ? 0 : p.power == 8 ? 2 : 1;
        ImGui::SetNextItemWidth(emW(5));
        if (ImGui::Combo("##ancpow", &pi, "2\0" "4\0" "8\0")) p.power = pi == 0 ? 2 : pi == 2 ? 8 : 4;
        ImGui::SetItemTooltip("2: BPSK, 4: QPSK and QAM, 8: 8-PSK");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Normalize", &p.normalize);
    ImGui::SameLine();
    ImGui::Checkbox("Symbols only", &anConsSymbols_);
    {
        const auto& sr = anRes_[(int)AnaKind::Symbols];
        const bool have = sr && sr->sym.rate > 0;
        ImGui::SetItemTooltip(have ? "One point at each symbol center, with the rate and the timing of the Symbols tab."
                                   : "One point at each symbol center. Open the Symbols tab first: it finds the rate and the timing.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Density", &anConsDensity_);
    if (!r) return;
    const ConstellationRes& c = r->cons;
    ImGui::Text("Removed offset %s, RMS magnitude %.4f, magnitude deviation %.4f (%.1f %%)%s",
                fmtSI(c.removedHz, "Hz", 6).c_str(), c.rmsMag, c.magStdev, 100 * c.magStdev / std::max(1e-12, c.rmsMag),
                r->req.cons.symbolRate > 0 ? ", one point per symbol" : "");
    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    if (ImPlot::BeginPlot("##ancplot", ImVec2(side, side), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("I", "Q");
        ImPlot::SetupAxesLimits(-c.extent, c.extent, -c.extent, c.extent, ImPlotCond_Always);
        if (anConsDensity_ && c.bins > 0) {
            analysisTexture(0, &c, c.density, c.bins, c.bins, true, 0, 0);
            ImPlot::PlotImage("##dens", anTex_[0].id, ImPlotPoint(-c.extent, -c.extent), ImPlotPoint(c.extent, c.extent));
        } else {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 1.5f, kColA, 0, kColA);
            ImPlot::PlotScatter("##pts", c.xs.data(), c.ys.data(), (int)c.xs.size());
        }
        double z[2] = {0, 0};
        ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.2f));
        ImPlot::PlotInfLines("##x0", z, 1);
        ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.2f));
        ImPlot::PlotInfLines("##y0", z, 1, ImPlotInfLinesFlags_Horizontal);
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Pulses
// ---------------------------------------------------------------------------
void App::drawAnPulses(const AnaRes* r) {
    PulseParams& p = anReq_.pulse;
    ImGui::Checkbox("Auto threshold", &p.autoThreshold);
    ImGui::SetItemTooltip("On: the threshold is the value below over the noise floor.\nOff: the threshold is the value below, in dBFS.");
    ImGui::SameLine();
    labelled(p.autoThreshold ? "Over the noise" : "Threshold");
    InputSI("##anpthr", &p.thresholdDb, "dB", emW(7), 4);
    ImGui::SameLine();
    labelled("Hysteresis");
    InputSI("##anphys", &p.hysteresisDb, "dB", emW(6), 3);
    ImGui::SameLine();
    labelled("Min width");
    InputSI("##anpmin", &p.minWidthS, "s", emW(8), 4);
    ImGui::SameLine();
    labelled("Smooth");
    double sm = p.smooth;
    if (InputSI("##anpsm", &sm, "", emW(5), 4)) p.smooth = std::clamp((int)sm, 1, 65536);
    ImGui::SetItemTooltip("Moving average of the power, in samples, before the threshold");
    if (!r) return;
    const PulseRes& pr = r->pulse;
    std::string groups;
    if (pr.priGroups.size() == 1) groups = "fixed PRI";
    else if (pr.priGroups.size() > 1 && pr.priGroups.size() <= 8) groups = std::to_string(pr.priGroups.size()) + " PRIs (stagger)";
    else if (pr.priGroups.size() > 8) groups = "varying PRI (jitter)";
    ImGui::Text("%zu pulses%s.  Noise %s, threshold %s.  Mean width %s, mean PRI %s (PRF %s), duty cycle %.3g %%%s%s",
                pr.pulses.size(), pr.truncated ? " (the first only)" : "", dbText(pr.noiseDb, "dBFS").c_str(),
                dbText(pr.thresholdDb, "dBFS").c_str(), fmtSI(pr.meanWidth, "s", 4).c_str(),
                fmtSI(pr.meanPri, "s", 5).c_str(), fmtSI(pr.meanPri > 0 ? 1 / pr.meanPri : 0, "Hz", 5).c_str(),
                100 * pr.dutyCycle, groups.empty() ? "" : ", ", groups.c_str());
    if (pr.priGroups.size() > 1 && pr.priGroups.size() <= 8) {
        std::string s = "PRIs:";
        for (double v : pr.priGroups) s += " " + fmtSI(v, "s", 5);
        ImGui::TextDisabled("%s", s.c_str());
    }
    ImGui::BeginDisabled(pr.pulses.empty());
    if (ImGui::Button("Add as bookmarks")) {
        const size_t n = std::min<size_t>(pr.pulses.size(), 1000);
        for (size_t i = 0; i < n; ++i) {
            const Pulse& u = pr.pulses[i];
            char name[64];
            snprintf(name, sizeof name, "Pulse %zu @ %s", i + 1, fmtSI(u.toa, "s", 6).c_str());
            bookmarks_.push_back({name, u.toa - 0.25 * u.width, u.toa + 1.25 * u.width});
        }
        status_ = "Added " + std::to_string(n) + " pulse bookmarks" + (pr.pulses.size() > n ? " (the first 1000)" : "");
    }
    ImGui::SetItemTooltip("A bookmark for each pulse (the first 1000), from a quarter width before it to a quarter width after it");
    ImGui::SameLine();
    if (ImGui::Button("Copy table")) ImGui::SetClipboardText(analysisReport(*r).c_str());
    ImGui::SetItemTooltip("All the pulses as CSV text");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Click a pulse to show it in the viewer.");

    const float h = ImGui::GetContentRegionAvail().y;
    const float w = ImGui::GetContentRegionAvail().x;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::BeginTable("##anptab", 9,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                          ImVec2(w * 0.55f, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* c : {"#", "TOA", "Width", "PRI", "Peak", "Frequency", "Chirp", "Rise", "Fall"}) ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)pr.pulses.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const Pulse& u = pr.pulses[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[32];
                snprintf(id, sizeof id, "%d##p%d", i + 1, i);
                if (ImGui::Selectable(id, anPulseSel_ == i, ImGuiSelectableFlags_SpanAllColumns)) {
                    anPulseSel_ = i;
                    setView(u.toa - 0.5 * u.width, u.toa + 1.5 * u.width);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fmtSI(u.toa, "s", 8).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fmtSI(u.width, "s", 4).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::isfinite(u.pri) ? fmtSI(u.pri, "s", 5).c_str() : "-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(dbText(u.peakDb, "dBFS").c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fmtSI(u.freqHz, "Hz", 8).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fmtSI(u.chirpHzPerS, "Hz/s", 3).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::isfinite(u.rise) ? fmtSI(u.rise, "s", 3).c_str() : "-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::isfinite(u.fall) ? fmtSI(u.fall, "s", 3).c_str() : "-");
            }
        }
        ImGui::EndTable();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float ph = (h - ImGui::GetStyle().ItemSpacing.y) * 0.5f;
    if (ImPlot::BeginPlot("Pulses aligned on the leading edge##anpov", ImVec2(w * 0.45f - sp, ph), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Time from the TOA", "dBFS", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"s");
        const double floor = pr.noiseDb - 10;
        for (size_t i = 0; i < pr.overlayDb.size(); ++i) {
            const auto& row = pr.overlayDb[i];
            std::vector<double> y(row.size());
            for (size_t k = 0; k < row.size(); ++k) y[k] = std::max((double)row[k], floor);
            const bool sel = (int)i == anPulseSel_;
            ImPlot::SetNextLineStyle(sel ? kColB : ImVec4(kColA.x, kColA.y, kColA.z, 0.25f), sel ? 2.f : 1.f);
            char id[24];
            snprintf(id, sizeof id, "##o%zu", i);
            ImPlot::PlotLine(id, pr.overlayT.data(), y.data(), (int)y.size());
        }
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("PRI histogram##anpri", ImVec2(w * 0.45f - sp, ph), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("PRI", "Count", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"s");
        std::vector<double> pri;
        for (const Pulse& u : pr.pulses)
            if (std::isfinite(u.pri)) pri.push_back(u.pri);
        if (!pri.empty()) {
            ImPlot::SetNextFillStyle(kColA, 0.8f);
            ImPlot::PlotHistogram("##pri", pri.data(), (int)pri.size(), 100);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndGroup();
}

// ---------------------------------------------------------------------------
// Correlation
// ---------------------------------------------------------------------------
void App::drawAnCorrelation(const AnaRes* r) {
    CorrelationParams& p = anReq_.corr;
    int mode = p.search ? 1 : 0;
    ImGui::RadioButton("Autocorrelation of the region", &mode, 0);
    ImGui::SetItemTooltip("Shows how the region repeats: frame lengths, preambles, the PRI, the OFDM cyclic prefix");
    ImGui::SameLine();
    ImGui::RadioButton("Find a template", &mode, 1);
    ImGui::SetItemTooltip("Finds each part of the file that looks like the template (normalized correlation)");
    p.search = mode == 1;
    if (p.search) {
        labelled("Template");
        ImGui::SetNextItemWidth(emW(14));
        ImGui::Combo("##antsrc", &anTmplSrc_, "Time markers\0Bookmark\0");
        if (anTmplSrc_ == 1) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(emW(18));
            const char* bp = anTmplBookmark_ >= 0 && anTmplBookmark_ < (int)bookmarks_.size()
                                 ? bookmarks_[anTmplBookmark_].name.c_str() : "(none)";
            if (ImGui::BeginCombo("##antbm", bp)) {
                for (int i = 0; i < (int)bookmarks_.size(); ++i)
                    if (ImGui::Selectable(bookmarks_[i].name.c_str(), i == anTmplBookmark_)) anTmplBookmark_ = i;
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        labelled("Look in");
        int sv = anSearchView_ ? 1 : 0;
        ImGui::SetNextItemWidth(emW(14));
        if (ImGui::Combo("##ansrange", &sv, "Whole segment\0View\0")) anSearchView_ = sv == 1;
        ImGui::SameLine();
        labelled("Threshold");
        InputSI("##ancthr", &p.threshold, "", emW(6), 3);
        p.threshold = std::clamp(p.threshold, 0.05, 1.0);
        ImGui::SetItemTooltip("The normalized correlation of a hit: 1 = the same as the template");
    }
    if (!r) return;
    const CorrelationRes& c = r->corr;
    if (!c.err.empty()) {
        ImGui::TextColored(kColWarn, "%s", c.err.c_str());
        return;
    }
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    if (c.search) {
        ImGui::Text("%zu hits%s for a template of %s", c.peaks.size(), c.truncated ? " (the first only)" : "",
                    fmtSI(c.tmplLen, "s", 4).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(c.peaks.empty());
        if (ImGui::Button("Add as bookmarks##hits")) {
            const size_t n = std::min<size_t>(c.peaks.size(), 1000);
            for (size_t i = 0; i < n; ++i) {
                char name[64];
                snprintf(name, sizeof name, "Hit %zu @ %s", i + 1, fmtSI(c.peaks[i].x, "s", 6).c_str());
                bookmarks_.push_back({name, c.peaks[i].x, c.peaks[i].x + c.tmplLen});
            }
            status_ = "Added " + std::to_string(n) + " hit bookmarks";
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy hits")) ImGui::SetClipboardText(analysisReport(*r).c_str());
        ImGui::EndDisabled();
    } else {
        std::string s = "Strongest lags:";
        for (size_t i = 0; i < std::min<size_t>(5, c.peaks.size()); ++i) {
            char b[80];
            snprintf(b, sizeof b, "  %s (%.3f)", fmtSI(c.peaks[i].x, "s", 5).c_str(), c.peaks[i].v);
            s += b;
        }
        if (c.peaks.empty()) s += " none";
        ImGui::TextUnformatted(s.c_str());
    }
    const float th = ImGui::GetContentRegionAvail().y;
    if (ImPlot::BeginPlot("##ancorr", ImVec2(w * (c.search ? 0.65f : 0.72f), th), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(c.search ? "Time" : "Lag", c.search ? "Score" : "|R| / R(0)", ImPlotAxisFlags_AutoFit, 0);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1.05, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"s");
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("##c", c.x.data(), c.y.data(), (int)c.x.size());
        if (c.search) {
            double t[1] = {r->req.corr.threshold};
            ImPlot::SetNextLineStyle(kColB);
            ImPlot::PlotInfLines("Threshold", t, 1, ImPlotInfLinesFlags_Horizontal);
        }
        std::vector<double> px, py;
        for (const CorrPeak& q : c.peaks) {
            px.push_back(q.x);
            py.push_back(q.v);
        }
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3, kColB, 0, kColB);
        ImPlot::PlotScatter("Peaks", px.data(), py.data(), (int)px.size());
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    if (ImGui::BeginTable("##ancpk", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(-1, th))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(c.search ? "Time" : "Lag");
        ImGui::TableSetupColumn(c.search ? "Score" : "|R|");
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)c.peaks.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[64];
                snprintf(id, sizeof id, "%s##k%d", fmtSI(c.peaks[i].x, "s", 7).c_str(), i);
                if (ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (c.search) setView(c.peaks[i].x - 0.25 * c.tmplLen, c.peaks[i].x + 1.25 * c.tmplLen);
                    else ImGui::SetClipboardText(fmtSI(c.peaks[i].x, "s", 9).c_str());
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", c.peaks[i].v);
            }
        ImGui::EndTable();
    }
    (void)h;
}

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------
void App::drawAnSymbols(const AnaRes* r) {
    SymbolParams& p = anReq_.sym;
    labelled("Detector");
    enumCombo<SymMethod>("##ansm", p.method, (int)SymMethod::Count, symMethodName, 12);
    ImGui::SetItemTooltip("What shows the symbol clock:\nEnvelope: shaped PSK and QAM\nEnvelope changes: OOK and ASK\n"
                          "Frequency changes: FSK, and PSK with sharp edges");
    ImGui::SameLine();
    labelled("Rate");
    optionalSI("##ansrate", p.rate, "Bd", 10, "estimate");
    ImGui::SetItemTooltip("Empty: use the estimate. Type a rate to set the cursors yourself.");
    ImGui::SameLine();
    labelled("Search");
    optionalSI("##ansmin", p.minRate, "Bd", 8, "auto");
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    optionalSI("##ansmax", p.maxRate, "Bd", 8, "auto");
    ImGui::SameLine();
    labelled("Trace");
    enumCombo<SymTrace>("##anstr", p.trace, (int)SymTrace::Count, symTraceName, 5);
    ImGui::SameLine();
    labelled("x^N");
    int pi = p.power == 2 ? 0 : p.power == 8 ? 2 : 1;
    ImGui::SetNextItemWidth(emW(5));
    if (ImGui::Combo("##anspow", &pi, "2\0" "4\0" "8\0")) p.power = pi == 0 ? 2 : pi == 2 ? 8 : 4;
    if (!r) return;
    const SymbolRes& s = r->sym;
    ImGui::Text("Estimate %s (line %.1f dB over the median)%s.  x^%d: peak at %s, carrier offset %s (%.1f dB)",
                fmtSI(s.estRate, "Bd", 7).c_str(), s.lineSnrDb, s.lineSnrDb < 6 ? ", weak: try another detector" : "",
                r->req.sym.power, fmtSI(s.powPeakHz, "Hz", 6).c_str(), fmtSI(s.powCarrierHz, "Hz", 6).c_str(), s.powPeakDb);
    if (p.haveT0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Use the estimated timing")) p.haveT0 = false;
    }
    ImGui::BeginDisabled(s.symV.empty());
    ImGui::Checkbox("Cursors in the viewer", &anViewerCursors_);
    ImGui::SetItemTooltip("Show the symbol cursors on the I/Q, AM, FM and PM plots of the viewer.\n"
                          "There, drag S0 to move the timing, and the second handle to change the rate.");
    ImGui::SameLine();
    if (ImGui::Button("Copy symbol values")) {
        std::string t;
        t.reserve(s.symV.size() * 10);
        char b[48];
        for (size_t i = 0; i < std::min<size_t>(s.symV.size(), 100000); ++i) {
            snprintf(b, sizeof b, "%s%.6g", i ? "," : "", s.symV[i]);
            t += b;
        }
        ImGui::SetClipboardText(t.c_str());
        status_ = "Copied " + std::to_string(std::min<size_t>(s.symV.size(), 100000)) + " symbol values";
    }
    ImGui::SetItemTooltip("The trace at each cursor (the first 100000), separated by commas");
    ImGui::SameLine();
    if (ImGui::Button("Use in Demodulation")) {
        anReq_.digital.rate = s.rate;
        anTabWant_ = (int)AnaKind::Demod;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu symbols at %s. Drag the red cursor to move the timing.", s.symV.size(), fmtSI(s.rate, "Bd", 7).c_str());

    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    const float sp = ImGui::GetStyle().ItemSpacing.x, gy = ImGui::GetStyle().ItemSpacing.y;
    const float th = (h - gy) * 0.5f;
    // the trace and the cursors
    if (ImPlot::BeginPlot("##anstrace", ImVec2(w * 0.62f, th), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Time from the region start", symTraceName((SymTrace)r->req.sym.trace), 0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"s");
        const double T = s.rate > 0 ? 1 / s.rate : 0;
        if (T > 0) ImPlot::SetupAxisLimits(ImAxis_X1, 0, std::min<double>(40 * T, s.traceT.empty() ? 1 : s.traceT.back()), ImPlotCond_Once);
        ImPlot::SetupFinish();
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        // only the samples in view, at most 20000 of them
        const double rate = r->rate;
        const int64_t n = (int64_t)s.traceY.size();
        const int64_t a = std::clamp<int64_t>((int64_t)std::floor(lim.X.Min * rate) - 1, 0, n);
        const int64_t b = std::clamp<int64_t>((int64_t)std::ceil(lim.X.Max * rate) + 2, a, n);
        const int64_t step = std::max<int64_t>(1, (b - a) / 20000);
        std::vector<double> x, y;
        for (int64_t k = a; k < b; k += step) {
            x.push_back(s.traceT[k]);
            y.push_back(s.traceY[k]);
        }
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("##tr", x.data(), y.data(), (int)x.size());
        if (T > 0 && (lim.X.Max - lim.X.Min) / T < 2000) {
            std::vector<double> cx, cy;
            const double t0 = s.t0 - r->t0;
            const double first = t0 + std::ceil((lim.X.Min - t0) / T) * T;
            for (int k = 0; first + k * T <= lim.X.Max; ++k) cx.push_back(first + k * T);
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.18f));
            ImPlot::PlotInfLines("##cur", cx.data(), (int)cx.size());
            for (double t : cx) {
                const int64_t k = std::clamp<int64_t>((int64_t)std::llround(t * rate), 0, std::max<int64_t>(0, n - 1));
                cy.push_back(n ? s.traceY[k] : 0);
            }
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3, kColB, 0, kColB);
            ImPlot::PlotScatter("##sym", cx.data(), cy.data(), (int)cx.size());
            // the first cursor in view moves the timing
            double drag = cx.empty() ? t0 : cx.front();
            if (ImPlot::DragLineX(1, &drag, kColCursor, 2)) {
                p.t0 = drag + r->t0;
                p.haveT0 = true;
                if (!(p.rate > 0)) p.rate = s.rate;
            }
        }
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    if (ImPlot::BeginPlot("Eye##aneye", ImVec2(-1, th), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Symbols", symTraceName((SymTrace)r->req.sym.trace), ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        const double half = r->req.sym.eyeSymbols / 2.0;
        if (s.eyeW > 0) {
            analysisTexture(1, &s, s.eye, s.eyeW, s.eyeH, true, 0, 0);
            ImPlot::PlotImage("##eye", anTex_[1].id, ImPlotPoint(-half, s.eyeLo), ImPlotPoint(half, s.eyeHi));
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Symbol clock line##anline", ImVec2(w * 0.5f - sp / 2, th), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Rate", "dB", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"Bd");
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("##l", s.lineF.data(), s.lineDb.data(), (int)s.lineF.size());
        double e[1] = {s.estRate};
        ImPlot::SetNextLineStyle(kColB);
        ImPlot::PlotInfLines("Estimate", e, 1);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    char title[48];
    snprintf(title, sizeof title, "Spectrum of x^%d##anpow", r->req.sym.power);
    if (ImPlot::BeginPlot(title, ImVec2(-1, th), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Frequency from the channel center", "dB", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"Hz");
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("##p", s.powF.data(), s.powDb.data(), (int)s.powF.size());
        double e[1] = {s.powPeakHz};
        ImPlot::SetNextLineStyle(kColB);
        ImPlot::PlotInfLines("Peak", e, 1);
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Symbol cursors on the viewer's time plots
// ---------------------------------------------------------------------------
bool App::symbolCursorsReady() const {
    const auto& r = anRes_[(int)AnaKind::Symbols];
    return r && r->err.empty() && (r->sym.rate > 0 || anReq_.sym.rate > 0);
}

void App::drawSymbolCursors() {
    if (!anViewerCursors_ || liveMode_ || !symbolCursorsReady()) return;
    const SymbolRes& s = anRes_[(int)AnaKind::Symbols]->sym;
    SymbolParams& p = anReq_.sym;
    // The settings first: a drag shows at once, before the tab calculates again.
    const double rate = p.rate > 0 ? p.rate : s.rate;
    const double t0 = p.haveT0 ? p.t0 : s.t0;
    if (!(rate > 0) || !std::isfinite(t0)) return;
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    const SymbolCursors c = symbolCursors(rate, t0, lim.X.Min, lim.X.Max);
    if (c.xs.empty()) return;  // none in view, or too dense to show
    ImPlot::SetNextLineStyle(ImVec4(1.f, 1.f, 1.f, 0.22f));
    ImPlot::PlotInfLines("##symcur", c.xs.data(), (int)c.xs.size(), ImPlotItemFlags_NoFit);
    const ImVec4 colB(1.f, 0.6f, 0.2f, 0.8f);
    double a = c.s0, b = c.handle;
    const bool movedA = ImPlot::DragLineX(40, &a, kColCursor, 2.f);
    const bool movedB = ImPlot::DragLineX(41, &b, colB, 2.f);
    ImPlot::TagX(a, kColCursor, "S0");
    ImPlot::TagX(b, colB, "S%d", c.n);
    if (movedA && a != c.s0) {
        p.t0 = a;
        p.haveT0 = true;
        if (!(p.rate > 0)) p.rate = rate;
    } else if (movedB && b != c.handle) {
        if (const double r = rateFromHandle(c.s0, b, c.n); r > 0) {
            p.rate = r;
            p.t0 = c.s0;
            p.haveT0 = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Cyclostationary
// ---------------------------------------------------------------------------
void App::drawAnCyclo(const AnaRes* r) {
    CycloParams& p = anReq_.cyclo;
    labelled("Channels");
    int ni = p.np == 32 ? 0 : p.np == 128 ? 2 : p.np == 256 ? 3 : 1;
    ImGui::SetNextItemWidth(emW(6));
    if (ImGui::Combo("##ancnp", &ni, "32\0" "64\0" "128\0" "256\0")) p.np = 32 << ni;
    ImGui::SetItemTooltip("The FFT size of the channelizer: the frequency resolution is the rate / this.\n"
                          "Large values are slower (the time goes up as its square).");
    ImGui::SameLine();
    labelled("Max blocks");
    int pi = 0;
    for (int v = 64; v < p.maxP; v *= 2) ++pi;
    ImGui::SetNextItemWidth(emW(7));
    if (ImGui::Combo("##ancp", &pi, "64\0" "128\0" "256\0" "512\0" "1024\0" "2048\0" "4096\0")) p.maxP = 64 << pi;
    ImGui::SetItemTooltip("The most channelizer blocks: the cycle frequency resolution is the rate / (blocks x channels / 4).");
    ImGui::SameLine();
    labelled("Range");
    InputSI("##ancrng", &anCycloRange_, "dB", emW(6), 3);
    anCycloRange_ = std::clamp(anCycloRange_, 5.0, 120.0);
    if (!r) return;
    const CycloRes& c = r->cyclo;
    std::string s = "Strongest cycle frequencies:";
    for (size_t i = 0; i < std::min<size_t>(5, c.peaks.size()); ++i) s += "  " + fmtSI(c.peaks[i].x, "Hz", 6) + numText(c.peaks[i].v, " (%.1f dB)");
    ImGui::TextUnformatted(s.c_str());
    ImGui::TextDisabled("%d channels, %d blocks of %d, %s samples. A symbol rate shows as a line at that cycle frequency.",
                        c.np, c.p, c.l, fmtSI((double)c.samples, "", 4).c_str());
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    const float gy = ImGui::GetStyle().ItemSpacing.y;
    if (ImPlot::BeginPlot("Spectral correlation##ancscd", ImVec2(w, (h - gy) * 0.6f), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Cycle frequency (alpha)", "Frequency", 0, 0);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"Hz");
        ImPlot::SetupAxisFormat(ImAxis_Y1, formatValueSI, (void*)"Hz");
        ImPlot::SetupAxesLimits(c.aLo, c.aHi, c.fLo, c.fHi, ImPlotCond_Once);
        if (c.na > 0) {
            analysisTexture(2, &c, c.db, c.na, c.nf, false, (float)-anCycloRange_, 0.f);
            ImPlot::PlotImage("##scd", anTex_[2].id, ImPlotPoint(c.aLo, c.fLo), ImPlotPoint(c.aHi, c.fHi));
        }
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Cyclic profile (the largest value at each alpha)##ancprof", ImVec2(w, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Cycle frequency (alpha)", "dB", 0, 0);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)"Hz");
        ImPlot::SetupAxisLimits(ImAxis_X1, c.aLo, c.aHi, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -anCycloRange_, 3, ImPlotCond_Always);
        ImPlot::SetNextLineStyle(kColA);
        ImPlot::PlotLine("##prof", c.alpha.data(), c.profileDb.data(), (int)c.alpha.size());
        std::vector<double> px, py;
        for (const CorrPeak& q : c.peaks) {
            px.push_back(q.x);
            py.push_back(q.v);
        }
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3, kColB, 0, kColB);
        ImPlot::PlotScatter("Peaks", px.data(), py.data(), (int)px.size());
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Digital demodulation
// ---------------------------------------------------------------------------
void App::drawAnDemod(const AnaRes* r) {
    DigitalParams& p = anReq_.digital;
    labelled("Mode");
    enumCombo<DemodMode>("##andm", p.mode, (int)DemodMode::Count, demodModeName, 7);
    ImGui::SameLine();
    labelled("Rate");
    optionalSI("##andrate", p.rate, "Bd", 10, "estimate");
    ImGui::SameLine();
    if (p.mode == (int)DemodMode::OOK || p.mode == (int)DemodMode::FSK) {
        ImGui::Checkbox("Auto threshold", &p.autoThreshold);
        if (!p.autoThreshold) {
            ImGui::SameLine();
            InputSI("##andthr", &p.threshold, p.mode == (int)DemodMode::FSK ? "Hz" : "FS", emW(9));
        }
    } else {
        ImGui::Checkbox("Differential", &p.differential);
        ImGui::SetItemTooltip("A symbol is the phase change from the symbol before it.\n"
                              "This removes the phase ambiguity of the carrier recovery.");
    }
    ImGui::SameLine();
    labelled("Timing loop");
    InputSI("##andg", &p.loopGain, "", emW(6), 3);
    p.loopGain = std::clamp(p.loopGain, 0.0, 0.5);
    ImGui::SetItemTooltip("The gain of the Gardner timing loop. 0 = fixed timing from the estimate.");
    if (!r) return;
    const DigitalRes& d = r->digital;
    ImGui::Text("%zu symbols, %zu bits%s at %s%s", d.symT.size(), d.bits.size(), d.truncated ? " (the first only)" : "",
                fmtSI(d.rate, "Bd", 7).c_str(),
                p.mode >= (int)DemodMode::BPSK ? (", carrier offset " + fmtSI(d.carrierHz, "Hz", 6)).c_str()
                                               : (", threshold " + fmtSI(d.threshold, p.mode == (int)DemodMode::FSK ? "Hz" : "FS", 5)).c_str());
    // the bits as text, or as hexadecimal
    ImGui::Checkbox("Hexadecimal", &anBitsHex_);
    ImGui::SameLine();
    if (ImGui::Button("Copy bits")) {
        ImGui::SetClipboardText(anBitsText_.c_str());
        status_ = "Copied the bits";
    }
    ImGui::SameLine();
    labelled("File");
    ImGui::SetNextItemWidth(emW(30));
    ImGui::InputText("##anbitpath", anBitsPath_, sizeof anBitsPath_);
    ImGui::SetItemTooltip("A .txt file gets the bits as 0 and 1 characters; another file gets them packed\n"
                          "into bytes, the first bit in the most significant bit. wvt does not replace a file.");
    ImGui::SameLine();
    if (ImGui::Button("Save bits")) {
        const std::string path = expandHome(anBitsPath_);
        anBitsMsgErr_ = true;
        if (path.empty()) {
            anBitsMsg_ = "Type a file name.";
        } else if (fileExists(path)) {
            anBitsMsg_ = "The file exists. Type a new name.";
        } else if (FILE* f = fopen(path.c_str(), "wbx")) {
            const bool txt = path.size() > 4 && path.compare(path.size() - 4, 4, ".txt") == 0;
            if (txt) {
                for (size_t i = 0; i < d.bits.size(); ++i) {
                    fputc('0' + d.bits[i], f);
                    if (i % 64 == 63) fputc('\n', f);
                }
                fputc('\n', f);
            } else {
                for (size_t i = 0; i < d.bits.size(); i += 8) {
                    uint8_t b = 0;
                    for (size_t k = 0; k < 8; ++k) b = (uint8_t)(b << 1 | (i + k < d.bits.size() ? d.bits[i + k] : 0));
                    fputc(b, f);
                }
            }
            const bool ok = fclose(f) == 0;
            anBitsMsgErr_ = !ok;
            anBitsMsg_ = ok ? "Wrote " + std::to_string(d.bits.size()) + " bits to " + path : "Could not write " + path;
            LOG_INFO("analysis", "%s", anBitsMsg_.c_str());
        } else {
            anBitsMsg_ = "Could not make " + path + ": " + errnoText(errno);
        }
    }
    if (!anBitsMsg_.empty()) {
        ImGui::SameLine();
        if (anBitsMsgErr_) ImGui::TextColored(kColWarn, "%s", anBitsMsg_.c_str());
        else ImGui::TextDisabled("%s", anBitsMsg_.c_str());
    }
    if (anBitsFor_ != r || anBitsForHex_ != anBitsHex_) {
        anBitsFor_ = r;
        anBitsForHex_ = anBitsHex_;
        anBitsText_.clear();
        const size_t n = std::min<size_t>(d.bits.size(), 200000);
        if (anBitsHex_) {
            static const char* hx = "0123456789abcdef";
            for (size_t i = 0; i < n; i += 4) {
                int v = 0;
                for (size_t k = 0; k < 4; ++k) v = v << 1 | (i + k < n ? d.bits[i + k] : 0);
                anBitsText_ += hx[v];
                if (i % 256 == 252) anBitsText_ += '\n';
                else if (i % 32 == 28) anBitsText_ += ' ';
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                anBitsText_ += (char)('0' + d.bits[i]);
                if (i % 64 == 63) anBitsText_ += '\n';
                else if (i % 8 == 7) anBitsText_ += ' ';
            }
        }
    }
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    const float gy = ImGui::GetStyle().ItemSpacing.y;
    ImGui::InputTextMultiline("##anbits", anBitsText_.data(), anBitsText_.size() + 1, ImVec2(w * 0.5f, h),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    const bool psk = p.mode >= (int)DemodMode::BPSK;
    if (psk) {
        const float side = std::min(ImGui::GetContentRegionAvail().x, h);
        if (ImPlot::BeginPlot("Symbols##andsym", ImVec2(side, side), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("I", "Q", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            const size_t m = std::min<size_t>(d.soft.size(), 20000);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 1.5f, kColA, 0, kColA);
            ImPlot::PlotScatter("##s", d.soft.data(), d.softQ.data(), (int)m);
            snapNotePlot(SnapPlot::Analysis);
            ImPlot::EndPlot();
        }
    } else if (ImPlot::BeginPlot("Decision values##andsoft", ImVec2(-1, h), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Symbol", p.mode == (int)DemodMode::FSK ? "Hz from the threshold" : "Amplitude from the threshold",
                          0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, std::min<double>(200, (double)d.soft.size()), ImPlotCond_Once);
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2, kColA, 0, kColA);
        ImPlot::PlotLine("##soft", d.soft.data(), (int)d.soft.size());
        double z[1] = {0};
        ImPlot::SetNextLineStyle(kColB);
        ImPlot::PlotInfLines("Threshold", z, 1, ImPlotInfLinesFlags_Horizontal);
        snapNotePlot(SnapPlot::Analysis);
        ImPlot::EndPlot();
    }
    (void)gy;
}
