#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui_internal.h"
#include "log.h"
#include "platform_x11.h"

namespace {
const int kFftSizes[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const struct {
    const char* name;
    double span;
} kSpans[] = {{"1 us", 1e-6},  {"10 us", 1e-5}, {"100 us", 1e-4}, {"1 ms", 1e-3},   {"10 ms", 1e-2},
              {"100 ms", 0.1}, {"1 s", 1.0},    {"10 s", 10.0},   {"1 min", 60.0}, {"All", -1}};

const ImVec4 kColI(0.35f, 0.70f, 1.00f, 1.f);
const ImVec4 kColQ(1.00f, 0.55f, 0.25f, 1.f);
const ImVec4 kColAM(0.40f, 0.90f, 0.50f, 1.f);
const ImVec4 kColFM(0.95f, 0.80f, 0.30f, 1.f);
const ImVec4 kColPM(0.85f, 0.50f, 1.00f, 1.f);
const ImVec4 kColChan(0.20f, 0.90f, 0.90f, 1.f);
const ImVec4 kColT(1.00f, 0.35f, 0.35f, 1.f);
const ImVec4 kColF(1.00f, 0.60f, 0.90f, 1.f);
const ImVec4 kColHover(1.f, 1.f, 1.f, 0.45f);
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);

float fieldW(float chars) { return ImGui::GetFontSize() * chars * 0.55f + ImGui::GetStyle().FramePadding.x * 2; }
// width for inputs with +/- step buttons
float stepW(float chars) { return fieldW(chars) + 2 * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x); }
}  // namespace

App::App(X11Window& win) : win_(win) {
    auto wake = [this] { win_.wake(); };
    wfWorker_ = std::make_unique<Worker<WaterfallReq, WaterfallRes>>(computeWaterfall, wake, "waterfall");
    dmWorker_ = std::make_unique<Worker<DemodReq, DemodRes>>(computeDemod, wake, "demod");
    ovWorker_ = std::make_unique<Worker<OverviewReq, OverviewRes>>(computeOverview, wake, "overview");
    cmap_ = ImPlotColormap_Viridis;
    demod_.chanBw = 50e3;
    status_ = "No file loaded - type a path in the File box and press Enter (or run: wvt <file>)";
    snprintf(recPrefix_, sizeof recPrefix_, "%s", recCfg_.prefix.c_str());
    sgPrevWorker_ = std::make_unique<Worker<SigPreviewReq, SigPreviewRes>>(computeSigPreview, wake, "siggen preview");
    sgFile_ = std::make_unique<FileGenJob>();
    snprintf(sgPrefix_, sizeof sgPrefix_, "~/wvt-recordings/siggen");
    snprintf(txArgs_, sizeof txArgs_, "%s", txCfg_.args.c_str());
    snprintf(auPrefix_, sizeof auPrefix_, "~/wvt-recordings/audio");
}

App::~App() {
    tx_.reset();  // stop transmitting first
    audio_.reset();
    auExport_.reset();
    sgFile_.reset();
    sgPrevWorker_.reset();
    live_.reset();
    win_.destroyTexture(liveTex_);
    wfWorker_.reset();
    dmWorker_.reset();
    ovWorker_.reset();
    win_.destroyTexture(wfTex_);
}

bool App::workersBusy() const { return wfWorker_->busy() || dmWorker_->busy() || ovWorker_->busy(); }

// While a device streams, the main loop keeps running at frame rate.
bool App::busy() const { return workersBusy() || (live_ && live_->running()) || sigGenBusy(); }

// ---------------------------------------------------------------------------
// Source / view management
// ---------------------------------------------------------------------------
bool App::openFile(const std::string& path, const CliOverrides& o) {
    FileMetaGuess g = guessMeta(path);
    std::string err;
    auto f = IqFile::open(g.dataPath, err);
    if (!f) {
        LOG_ERROR("app", "%s", err.c_str());
        status_ = err;
        return false;
    }
    stopAudio("Audio stopped (new file)");
    file_ = f;
    snprintf(pathBuf_, sizeof pathBuf_, "%s", g.dataPath.c_str());
    if (g.haveFmt) { fp_.fmt = g.fmt; fp_.bigEndian = g.bigEndian; }
    if (g.haveRate) fs_ = g.rate;
    if (g.haveFc) fc_ = g.fc;
    if (o.haveFmt) { fp_.fmt = o.fmt; fp_.bigEndian = o.bigEndian; }
    if (o.haveRate) fs_ = o.rate;
    if (o.haveFc) fc_ = o.fc;
    fp_.headerBytes = o.header >= 0 ? o.header : 0;
    if (o.swapIQ) fp_.swapIQ = true;
    metaNote_ = g.source.empty() ? "no metadata found - set format / rate below" : "settings from " + g.source;
    status_ = "Opened " + g.dataPath + " (" + metaNote_ + ")";
    LOG_INFO("app", "opened %s: %s%s, %s at %s, %s (%s)", g.dataPath.c_str(), formatName(fp_.fmt), fp_.bigEndian ? " big-endian" : "",
             fmtSI(fs_, "S/s", 6).c_str(), fmtSI(fc_, "Hz", 9).c_str(), fmtSI((double)f->sizeBytes(), "B", 4).c_str(), metaNote_.c_str());
    bookmarks_.clear();
    bookmarkSel_ = -1;
    onSourceChanged(true);
    return true;
}

void App::onSourceChanged(bool resetView) {
    nSamples_ = file_ ? file_->numSamples(fp_) : 0;
    dur_ = fs_ > 0 ? nSamples_ / fs_ : 0;
    if (resetView) {
        tmin_ = 0;
        tmax_ = dur_ > 0 ? dur_ : 1;
        fmin_ = fc_ - fs_ / 2;
        fmax_ = fc_ + fs_ / 2;
        tm1_ = dur_ * 0.25;
        tm2_ = dur_ * 0.75;
        fm1_ = fc_ - fs_ / 8;
        fm2_ = fc_ + fs_ / 8;
        fitSpecY_ = true;
    }
    wf_.reset();
    dm_.reset();
    ov_.reset();
    std::string name = file_ ? file_->path() : "";
    size_t sl = name.find_last_of('/');
    if (sl != std::string::npos) name = name.substr(sl + 1);
    win_.setTitle(file_ ? "wvt - " + name : "wvt - IQ waveform viewer");
}

void App::setFs(double fs) {
    if (!(fs > 0)) return;
    double k = fs_ / fs;  // keep the same samples in view
    tmin_ *= k; tmax_ *= k; tm1_ *= k; tm2_ *= k;
    double df = fs / fs_;
    fmin_ = fc_ + (fmin_ - fc_) * df;
    fmax_ = fc_ + (fmax_ - fc_) * df;
    fs_ = fs;
    onSourceChanged(false);
}

void App::setFc(double fc) {
    double d = fc - fc_;
    fmin_ += d; fmax_ += d; fm1_ += d; fm2_ += d;
    fc_ = fc;
    onSourceChanged(false);
}

void App::setChannel(double offset, double bw) {
    demod_.chanEnable = true;
    demod_.chanOffset = offset;
    demod_.chanBw = std::clamp(bw, 1.0, fs_);
}

void App::setView(double t0, double t1) {
    if (t1 < t0) std::swap(t0, t1);
    double span = std::max(t1 - t0, 4.0 / std::max(1.0, fs_));
    double total = std::max(dur_, span);
    span = std::min(span, total);
    t0 = std::clamp(t0, 0.0, total - span);
    tmin_ = t0;
    tmax_ = t0 + span;
}

void App::pan(double frac) {
    double span = tmax_ - tmin_;
    setView(tmin_ + frac * span, tmax_ + frac * span);
}

void App::zoom(double factor) {
    double c = 0.5 * (tmin_ + tmax_), h = 0.5 * (tmax_ - tmin_) * factor;
    setView(c - h, c + h);
}

void App::submitRequests() {
    if (liveMode_ || !file_ || nSamples_ <= 0) return;
    Source src{file_, fp_, fs_, fc_};
    int64_t s0 = std::clamp<int64_t>((int64_t)std::floor(tmin_ * fs_), 0, nSamples_ - 1);
    int64_t s1 = std::clamp<int64_t>((int64_t)std::ceil(tmax_ * fs_), s0 + 1, nSamples_);

    WaterfallReq wr;
    wr.src = src;
    wr.s0 = s0;
    wr.s1 = s1;
    wr.nfft = nfft_;
    wr.window = window_;
    wr.overlap = overlapPct_ / 100.0;
    wr.maxRows = maxRows_;
    wr.maxCols = maxCols_;
    wr.avgPerRow = avg_;
    wfWorker_->submit(wr);

    if (showIQ_ || showAM_ || showFM_ || showPM_) {
        DemodReq dr = demod_;
        dr.src = src;
        dr.s0 = s0;
        dr.s1 = s1;
        dmWorker_->submit(dr);
    }
    OverviewReq orq;
    orq.src = src;
    ovWorker_->submit(orq);
}

void App::pollResults() {
    if (auto r = wfWorker_->take()) {
        wf_ = r;
        if (autoDb_) {
            dbMin_ = std::floor(r->p5 - 3);
            dbMax_ = std::ceil(std::max(r->p999 + 3, dbMin_ + 20.f));
        }
        if (fitSpecY_) needRecolor_ = true;
        needRecolor_ = true;
    }
    if (auto r = dmWorker_->take()) dm_ = r;
    if (auto r = ovWorker_->take()) ov_ = r;
    if (lutCmap_ != cmap_) {
        buildLut();
        needRecolor_ = true;
    }
    if (needRecolor_) recolor();
}

void App::buildLut() {
    lut_.resize(256);
    for (int i = 0; i < 256; ++i) lut_[i] = ImGui::ColorConvertFloat4ToU32(ImPlot::SampleColormap(i / 255.f, cmap_));
    lutCmap_ = cmap_;
}

void App::recolor() {
    needRecolor_ = false;
    if (!wf_ || wf_->rows == 0) return;
    const size_t n = wf_->db.size();
    rgba_.resize(n);
    const float lo = dbMin_, scale = 255.f / std::max(1e-3f, dbMax_ - dbMin_);
    for (size_t i = 0; i < n; ++i) {
        int k = (int)((wf_->db[i] - lo) * scale);
        rgba_[i] = lut_[std::clamp(k, 0, 255)];
    }
    win_.updateTexture(wfTex_, wf_->cols, wf_->rows, rgba_.data());
}

double App::valueAt(const Series& s, double t) const {
    if (s.t.empty() || !std::isfinite(t)) return NAN;
    auto it = std::lower_bound(s.t.begin(), s.t.end(), t);
    size_t i = (size_t)(it - s.t.begin());
    if (i >= s.t.size()) i = s.t.size() - 1;
    if (i > 0 && std::fabs(s.t[i - 1] - t) < std::fabs(s.t[i] - t)) --i;
    return s.y[i];
}

float App::wfValueAt(double t, double f) const {
    if (!wf_ || wf_->rows == 0 || !std::isfinite(t) || !std::isfinite(f)) return NAN;
    int r = (int)((t - wf_->t0) / (wf_->t1 - wf_->t0) * wf_->rows);
    int c = (int)((f - wf_->f0) / (wf_->f1 - wf_->f0) * wf_->cols);
    if (r < 0 || r >= wf_->rows || c < 0 || c >= wf_->cols) return NAN;
    return wf_->db[(size_t)r * wf_->cols + c];
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------
void App::flow(float w) {
    if (!flowFirst_) {
        float x2 = ImGui::GetItemRectMax().x;
        float maxX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (x2 + ImGui::GetStyle().ItemSpacing.x * 2 + w <= maxX) ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
    }
    flowFirst_ = false;
}

void App::label(const char* text, float fw) {
    float tw = ImGui::CalcTextSize(text, nullptr, true).x;
    flow(tw + ImGui::GetStyle().ItemInnerSpacing.x + fw);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(fw);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void App::frame() {
    pollResults();
    if (liveMode_) pollLive();
    pollSigGen();
    pollAudio();
    fFmt_.offset = relFreq_ ? fc_ : 0.0;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    nextHoverT_ = NAN;
    nextHoverF_ = NAN;
    liveNextHoverAgo_ = NAN;

    drawToolbar();
    ImGui::Separator();

    const float statusH = ImGui::GetFrameHeightWithSpacing();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float bodyH = std::max(100.f, avail.y - statusH);
    const float splitW = 6.f;
    float leftW = std::floor(std::clamp(leftFrac_, 0.15f, 0.85f) * (avail.x - splitW));

    if (liveMode_) {
        // Live mode is spectrum-only: the spectrum/waterfall pane takes the full width.
        ImGui::BeginChild("##live", ImVec2(0, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        drawLiveLeft();
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##left", ImVec2(leftW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        drawLeft();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton("##vsplit", ImVec2(splitW, bodyH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) leftFrac_ += ImGui::GetIO().MouseDelta.x / std::max(1.f, avail.x);
        {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, a.y), ImVec2(b.x - 2, b.y),
                                                      ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive
                                                                                               : ImGuiCol_Separator));
        }
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##right", ImVec2(0, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        drawRight();
        ImGui::EndChild();
    }

    drawStatusBar();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) sgOpen_ = !sgOpen_;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) auOpen_ = !auOpen_;
    handleShortcuts();
    ImGui::End();
    drawSigGenWindow();
    drawAudioWindow();

    hoverT_ = nextHoverT_;
    hoverF_ = nextHoverF_;
    liveHoverAgo_ = liveNextHoverAgo_;
    submitRequests();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
void App::drawToolbar() {
    const ImGuiStyle& st = ImGui::GetStyle();
    // --- Row 1: file & format -------------------------------------------------
    flowBegin();
    drawSourceSelector();
    drawSigGenButton();
    drawAudioButton();
    if (liveMode_) {
        drawDeviceToolbar();
        return;
    }
    label("File", fieldW(46));
    if (ImGui::InputText("##path", pathBuf_, sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue))
        openFile(pathBuf_);
    ImGui::SameLine();
    if (ImGui::Button("Open")) openFile(pathBuf_);
    ImGui::SetItemTooltip("Open the path typed in the File box (Enter also works).\nSigMF .sigmf-meta/.sigmf-data, gqrx names and common extensions are auto-detected.");

    label("Format", fieldW(26));
    if (ImGui::BeginCombo("##fmt", formatName(fp_.fmt))) {
        for (int i = 0; i < (int)SampleFormat::Count; ++i)
            if (ImGui::Selectable(formatName((SampleFormat)i), (int)fp_.fmt == i)) {
                fp_.fmt = (SampleFormat)i;
                onSourceChanged(false);
            }
        ImGui::EndCombo();
    }
    label("Sample rate", fieldW(13));
    double fs = fs_;
    if (InputSI("##fs", &fs, "Sps", fieldW(13))) setFs(fs);
    ImGui::SetItemTooltip("Accepts SI suffixes, e.g. 2.4M, 250k, 20e6");
    label("Center freq", fieldW(15));
    double fc = fc_;
    if (InputSI("##fc", &fc, "Hz", fieldW(15))) setFc(fc);
    label("Header bytes", fieldW(8));
    long long hb = fp_.headerBytes;
    if (ImGui::InputScalar("##hdr", ImGuiDataType_S64, &hb)) {
        fp_.headerBytes = std::max<long long>(0, hb);
        onSourceChanged(false);
    }
    flow(ImGui::GetFrameHeight() * 9);
    if (ImGui::Checkbox("Swap I/Q", &fp_.swapIQ)) onSourceChanged(false);
    ImGui::SetItemTooltip("Swap I and Q (spectral inversion)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Big endian", &fp_.bigEndian)) onSourceChanged(false);
    if (file_) {
        std::string info = fmtSI((double)nSamples_, "S", 4) + " | " + fmtSI(dur_, "s", 5) + " | " +
                           fmtSI((double)file_->sizeBytes(), "B", 4);
        flow(ImGui::CalcTextSize(info.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", info.c_str());
    }

    // --- Row 2: navigation ----------------------------------------------------
    ImGui::Spacing();
    flowBegin();
    flow(ImGui::GetFrameHeight() * 10);
    double span = tmax_ - tmin_;
    if (ImGui::Button("|<")) setView(0, span);
    ImGui::SetItemTooltip("Jump to start (Home)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("<<")) pan(-1.0);
    ImGui::SetItemTooltip("Back one view (PgUp)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("<")) pan(-0.1);
    ImGui::SetItemTooltip("Back 10%% (Left)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button(">")) pan(0.1);
    ImGui::SetItemTooltip("Forward 10%% (Right)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button(">>")) pan(1.0);
    ImGui::SetItemTooltip("Forward one view (PgDn)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button(">|")) setView(dur_ - span, dur_);
    ImGui::SetItemTooltip("Jump to end (End)");
    ImGui::SameLine(0, st.ItemSpacing.x * 2);
    if (ImGui::Button(" + ")) zoom(0.5);
    ImGui::SetItemTooltip("Zoom in 2x (+)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button(" - ")) zoom(2.0);
    ImGui::SetItemTooltip("Zoom out 2x (-)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("All")) {
        setView(0, dur_);
        fmin_ = fc_ - fs_ / 2;
        fmax_ = fc_ + fs_ / 2;
    }
    ImGui::SetItemTooltip("Show the whole file and full bandwidth (A)");

    label("Span", fieldW(8));
    if (ImGui::BeginCombo("##spanp", "preset")) {
        for (auto& s : kSpans)
            if (ImGui::Selectable(s.name)) {
                if (s.span < 0) setView(0, dur_);
                else {
                    double c = 0.5 * (tmin_ + tmax_);
                    setView(c - s.span / 2, c + s.span / 2);
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    double spanEdit = span;
    if (InputSI("##span", &spanEdit, "s", fieldW(11))) {
        double c = 0.5 * (tmin_ + tmax_);
        setView(c - spanEdit / 2, c + spanEdit / 2);
    }
    label("Start", fieldW(11));
    double startEdit = tmin_;
    if (InputSI("##start", &startEdit, "s", fieldW(11))) setView(startEdit, startEdit + span);
    label("Sample", fieldW(12));
    long long smp = (long long)std::llround(tmin_ * fs_);
    if (ImGui::InputScalar("##smp", ImGuiDataType_S64, &smp, nullptr, nullptr, "%lld",
                           ImGuiInputTextFlags_EnterReturnsTrue))
        setView(smp / fs_, smp / fs_ + span);
    ImGui::SetItemTooltip("First sample index in view (press Enter to jump)");

    // bookmarks
    flow(fieldW(14) + ImGui::GetFrameHeight() * 3);
    if (ImGui::Button("Bookmark")) {
        char name[64];
        snprintf(name, sizeof name, "B%zu @ %s", bookmarks_.size() + 1, fmtSI(tmin_, "s", 5).c_str());
        bookmarks_.push_back({name, tmin_, tmax_});
        bookmarkSel_ = (int)bookmarks_.size() - 1;
    }
    ImGui::SetItemTooltip("Save the current time view (B)");
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(fieldW(14));
    const char* bmPreview = bookmarkSel_ >= 0 && bookmarkSel_ < (int)bookmarks_.size()
                                ? bookmarks_[bookmarkSel_].name.c_str() : "(go to)";
    if (ImGui::BeginCombo("##bm", bmPreview)) {
        for (int i = 0; i < (int)bookmarks_.size(); ++i)
            if (ImGui::Selectable(bookmarks_[i].name.c_str(), i == bookmarkSel_)) {
                bookmarkSel_ = i;
                setView(bookmarks_[i].t0, bookmarks_[i].t1);
            }
        ImGui::EndCombo();
    }

    flow(ImGui::GetFrameHeight() * 14);
    ImGui::Checkbox("Time markers", &tMarkers_);
    ImGui::SetItemTooltip("Two draggable time cursors shown on every plot (T)");
    ImGui::SameLine();
    ImGui::Checkbox("Freq markers", &fMarkers_);
    ImGui::SetItemTooltip("Two draggable frequency cursors on spectrum/waterfall (F)");
    ImGui::SameLine();
    ImGui::Checkbox("Rel. freq", &relFreq_);
    ImGui::SetItemTooltip("Label frequency axes relative to the center frequency");

    // --- Row 3: overview + scrollbar -----------------------------------------
    ImGui::Spacing();
    drawOverview(ImGui::GetFontSize() * 3.2f);
    double start = tmin_;
    if (ScrollbarH("##hscroll", dur_, start, tmax_ - tmin_, ImGui::GetContentRegionAvail().x,
                   ImGui::GetStyle().ScrollbarSize + 2))
        setView(start, start + (tmax_ - tmin_));
}

void App::drawOverview(float h) {
    if (!ImPlot::BeginPlot("##overview", ImVec2(-1, h),
                           ImPlotFlags_CanvasOnly | ImPlotFlags_NoFrame | ImPlotFlags_NoMenus))
        return;
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickLabels,
                      ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisLimits(ImAxis_X1, 0, std::max(dur_, 1e-9), ImPlotCond_Always);
    ImPlot::SetupFinish();
    ImPlotRect lim = ImPlot::GetPlotLimits();
    if (ov_ && !ov_->t.empty()) {
        ImPlot::SetNextFillStyle(ImVec4(0.3f, 0.55f, 0.9f, 0.5f));
        ImPlot::PlotShaded("##peak", ov_->t.data(), ov_->peak.data(), (int)ov_->t.size(), -INFINITY);
        ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.85f, 1.f, 1.f));
        ImPlot::PlotLine("##mean", ov_->t.data(), ov_->mean.data(), (int)ov_->t.size());
    }
    for (auto& b : bookmarks_) {
        ImPlot::SetNextLineStyle(ImVec4(1, 0.8f, 0.3f, 0.7f));
        ImPlot::PlotInfLines("##bm", &b.t0, 1, ImPlotItemFlags_NoFit);
    }
    double x1 = tmin_, x2 = tmax_, y1 = lim.Y.Min, y2 = lim.Y.Max;
    bool rectDragged = ImPlot::DragRect(0, &x1, &y1, &x2, &y2, ImVec4(1, 1, 1, 0.9f), ImPlotDragToolFlags_None);
    if (rectDragged) setView(std::min(x1, x2), std::max(x1, x2));
    ImPlot::PushPlotClipRect();
    {
        // dim everything outside the current view
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        ImVec2 p0 = ImPlot::GetPlotPos(), sz = ImPlot::GetPlotSize();
        float a = ImPlot::PlotToPixels(tmin_, 0).x, b = ImPlot::PlotToPixels(tmax_, 0).x;
        dl->AddRectFilled(p0, ImVec2(std::max(p0.x, a), p0.y + sz.y), IM_COL32(0, 0, 0, 110));
        dl->AddRectFilled(ImVec2(std::min(p0.x + sz.x, b), p0.y), ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(0, 0, 0, 110));
    }
    ImPlot::PopPlotClipRect();
    // click / drag outside the view rectangle to scrub
    if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(0) && !rectDragged) {
        double mx = ImPlot::GetPlotMousePos().x;
        if (mx < tmin_ || mx > tmax_) ovScrub_ = true;
    }
    if (ovScrub_) {
        if (!ImGui::IsMouseDown(0)) ovScrub_ = false;
        else {
            double mx = ImPlot::GetPlotMousePos().x, s = tmax_ - tmin_;
            setView(mx - s / 2, mx + s / 2);
        }
    }
    if (ImPlot::IsPlotHovered()) {
        double mx = ImPlot::GetPlotMousePos().x;
        ImGui::SetTooltip("%s\nclick to jump, drag box to move, drag edges to resize", fmtSI(mx, "s", 6).c_str());
    }
    ImPlot::EndPlot();
}

// ---------------------------------------------------------------------------
// Left: spectrum + waterfall
// ---------------------------------------------------------------------------
void App::drawChannelOverlay(bool /*vertical*/) {
    if (!demod_.chanEnable) return;
    double c = fc_ + demod_.chanOffset;
    double lo = c - demod_.chanBw / 2, hi = c + demod_.chanBw / 2;
    ImPlot::PushPlotClipRect();
    ImPlotRect lim = ImPlot::GetPlotLimits();
    ImVec2 a = ImPlot::PlotToPixels(lo, lim.Y.Min), b = ImPlot::PlotToPixels(hi, lim.Y.Max);
    ImPlot::GetPlotDrawList()->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
                                             ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(50, 230, 230, 40));
    ImPlot::PopPlotClipRect();
    if (ImPlot::DragLineX(10, &c, kColChan, 1.5f)) demod_.chanOffset = c - fc_;
    ImVec4 edge = kColChan;
    edge.w = 0.6f;
    if (ImPlot::DragLineX(11, &lo, edge, 1.f)) demod_.chanBw = std::max(1.0, 2 * (c - lo));
    if (ImPlot::DragLineX(12, &hi, edge, 1.f)) demod_.chanBw = std::max(1.0, 2 * (hi - c));
}

void App::drawFreqMarkers() {
    if (!fMarkers_) return;
    ImPlot::DragLineX(20, &fm1_, kColF, 1.f);
    ImPlot::DragLineX(21, &fm2_, kColF, 1.f);
    ImPlot::TagX(fm1_, kColF, "F1");
    ImPlot::TagX(fm2_, kColF, "F2");
}

void App::drawTimeMarkersVertical() {
    if (!tMarkers_) return;
    ImPlot::DragLineX(30, &tm1_, kColT, 1.f);
    ImPlot::DragLineX(31, &tm2_, kColT, 1.f);
}

void App::drawTimeMarkersHorizontal() {
    if (!tMarkers_) return;
    ImPlot::DragLineY(30, &tm1_, kColT, 1.f);
    ImPlot::DragLineY(31, &tm2_, kColT, 1.f);
    ImPlot::TagY(tm1_, kColT, "T1");
    ImPlot::TagY(tm2_, kColT, "T2");
}

void App::drawLeft() {
    // --- waterfall parameters ---
    flowBegin();
    label("FFT", fieldW(7));
    char nf[16];
    snprintf(nf, sizeof nf, "%d", nfft_);
    if (ImGui::BeginCombo("##nfft", nf)) {
        for (int n : kFftSizes) {
            snprintf(nf, sizeof nf, "%d", n);
            if (ImGui::Selectable(nf, n == nfft_)) nfft_ = n;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("FFT size: frequency resolution = Fs / N = %s", fmtSI(fs_ / nfft_, "Hz", 4).c_str());
    label("Window", fieldW(14));
    if (ImGui::BeginCombo("##win", windowName((WindowType)window_))) {
        for (int i = 0; i < (int)WindowType::Count; ++i)
            if (ImGui::Selectable(windowName((WindowType)i), i == window_)) window_ = i;
        ImGui::EndCombo();
    }
    label("Overlap %", stepW(6));
    if (ImGui::InputDouble("##ovl", &overlapPct_, 5, 25, "%.0f")) overlapPct_ = std::clamp(overlapPct_, 0.0, 95.0);
    ImGui::SetItemTooltip("FFT overlap. When the view holds more rows than 'Rows', each row averages\n"
                          "several FFTs at this spacing, so no sample falls between FFTs.");
    label("Avg", stepW(4));
    if (ImGui::InputInt("##avg", &avg_)) avg_ = std::clamp(avg_, 1, 64);
    if (wf_ && wf_->fftsPerRow > avg_)
        ImGui::SetItemTooltip("Minimum FFTs averaged per waterfall row (smooths noise).\n"
                              "Now %d: zoomed out, each row averages enough FFTs to cover its whole time slice.",
                              wf_->fftsPerRow);
    else
        ImGui::SetItemTooltip("Minimum FFTs averaged per waterfall row (smooths noise).\n"
                              "Zoomed out, each row averages enough FFTs to cover its whole time slice.");
    label("Rows", stepW(5));
    if (ImGui::InputInt("##rows", &maxRows_, 128, 512)) maxRows_ = std::clamp(maxRows_, 16, 8192);
    ImGui::SetItemTooltip("Maximum waterfall rows (time resolution of the image)");
    label("Cols", stepW(5));
    if (ImGui::InputInt("##cols", &maxCols_, 256, 1024)) maxCols_ = std::clamp(maxCols_, 64, 8192);
    ImGui::SetItemTooltip("Maximum image columns; FFT bins beyond this are max-pooled");
    label("dB min", stepW(5));
    if (ImGui::InputFloat("##dbmin", &dbMin_, 1, 10, "%.0f")) { autoDb_ = false; needRecolor_ = true; }
    label("dB max", stepW(5));
    if (ImGui::InputFloat("##dbmax", &dbMax_, 1, 10, "%.0f")) { autoDb_ = false; needRecolor_ = true; }
    flow(ImGui::GetFrameHeight() * 3);
    if (ImGui::Checkbox("Auto", &autoDb_) && autoDb_ && wf_) {
        dbMin_ = std::floor(wf_->p5 - 3);
        dbMax_ = std::ceil(std::max(wf_->p999 + 3, dbMin_ + 20.f));
        needRecolor_ = true;
    }
    ImGui::SetItemTooltip("Auto-range the color scale to the data in view");
    label("Colormap", fieldW(10));
    if (ImGui::BeginCombo("##cmap", ImPlot::GetColormapName(cmap_))) {
        for (int i = 0; i < ImPlot::GetColormapCount(); ++i)
            if (ImGui::Selectable(ImPlot::GetColormapName(i), i == cmap_)) cmap_ = i;
        ImGui::EndCombo();
    }
    flow(ImGui::GetFrameHeight() * 4);
    ImGui::Checkbox("Peak", &showPeak_);
    ImGui::SetItemTooltip("Show peak-hold trace in the spectrum");
    if (wf_ && wf_->coverage < 0.999) {
        char cov[32];
        snprintf(cov, sizeof cov, "FFT sees %.0f%%", 100 * wf_->coverage);
        flow(ImGui::CalcTextSize(cov).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColWarn, "%s", cov);
        ImGui::SetItemTooltip("The view is too wide to FFT every sample, so each row samples its time slice.\n"
                              "Short bursts can show in some rows and not others. Zoom in for full coverage.");
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cbW = ImGui::GetFontSize() * 4.5f;
    float specH = std::max(80.f, avail.y * specFrac_);
    float wfH = std::max(80.f, avail.y - specH - 6);

    // --- spectrum ---
    if (fitSpecY_ && wf_) {
        ImPlot::SetNextAxisLimits(ImAxis_Y1, dbMin_ - 5, dbMax_ + 10, ImGuiCond_Always);
        fitSpecY_ = false;
    }
    if (ImPlot::BeginPlot("##spectrum", ImVec2(avail.x - cbW - ImGui::GetStyle().ItemSpacing.x, specH),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisSI, &fFmt_);
        ImPlot::SetupAxisLinks(ImAxis_X1, &fmin_, &fmax_);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, fc_ - fs_, fc_ + fs_);
        ImPlot::SetupAxis(ImAxis_Y1, "dBFS");
        if (wf_ && !wf_->specF.empty()) {
            if (showPeak_) {
                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.4f, 0.3f, 0.6f));
                ImPlot::PlotLine("peak", wf_->specF.data(), wf_->specPeak.data(), (int)wf_->specF.size());
            }
            ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.85f, 1.f, 1.f));
            ImPlot::PlotLine("avg", wf_->specF.data(), wf_->specAvg.data(), (int)wf_->specF.size());
        }
        drawChannelOverlay(false);
        drawAudioOverlay();
        drawFreqMarkers();
        if (std::isfinite(hoverF_)) {
            ImPlot::SetNextLineStyle(kColHover);
            ImPlot::PlotInfLines("##hf", &hoverF_, 1, ImPlotItemFlags_NoFit);
        }
        if (ImPlot::IsPlotHovered()) nextHoverF_ = ImPlot::GetPlotMousePos().x;
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(0)) {
            demod_.chanEnable = true;
            demod_.chanOffset = ImPlot::GetPlotMousePos().x - fc_;
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Spectrum\nof view");
    ImGui::EndGroup();

    // splitter between spectrum and waterfall
    ImGui::InvisibleButton("##hsplit", ImVec2(avail.x, 6));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) specFrac_ = std::clamp(specFrac_ + ImGui::GetIO().MouseDelta.y / std::max(1.f, avail.y), 0.08f, 0.8f);

    // --- waterfall ---
    if (ImPlot::BeginPlot("##waterfall", ImVec2(avail.x - cbW - ImGui::GetStyle().ItemSpacing.x, wfH - 6),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisSI, &fFmt_);
        ImPlot::SetupAxisLinks(ImAxis_X1, &fmin_, &fmax_);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, fc_ - fs_, fc_ + fs_);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Invert);
        ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisSI, &tFmt_);
        ImPlot::SetupAxisLinks(ImAxis_Y1, &tmin_, &tmax_);
        double pad = std::max(dur_ * 0.02, 1.0 / std::max(1.0, fs_));
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, -pad, dur_ + pad);
        ImPlot::SetupAxisZoomConstraints(ImAxis_Y1, 4.0 / std::max(1.0, fs_), dur_ + 2 * pad);
        if (wf_ && wfTex_)
            // ImPlot maps uv0 to (xmin, ymax); the time axis is inverted, so flip V
            // to keep texture row 0 (earliest time) at the top.
            ImPlot::PlotImage("##wf", wfTex_, ImPlotPoint(wf_->f0, wf_->t0), ImPlotPoint(wf_->f1, wf_->t1),
                              ImVec2(0, 1), ImVec2(1, 0));
        drawChannelOverlay(true);
        drawAudioOverlay();
        drawFreqMarkers();
        drawTimeMarkersHorizontal();
        drawPlayhead(false);
        ImPlot::SetNextLineStyle(kColHover);
        if (std::isfinite(hoverT_)) ImPlot::PlotInfLines("##ht", &hoverT_, 1, (int)ImPlotItemFlags_NoFit | (int)ImPlotInfLinesFlags_Horizontal);
        ImPlot::SetNextLineStyle(kColHover);
        if (std::isfinite(hoverF_)) ImPlot::PlotInfLines("##hf", &hoverF_, 1, ImPlotItemFlags_NoFit);
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint m = ImPlot::GetPlotMousePos();
            nextHoverT_ = m.y;
            nextHoverF_ = m.x;
            if (ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(0)) {
                demod_.chanEnable = true;
                demod_.chanOffset = m.x - fc_;
            }
            if (ImGui::GetIO().KeyShift && ImGui::IsMouseClicked(0)) playFrom(m.y);
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("##cbar", dbMin_, dbMax_, ImVec2(cbW, wfH - 6), "%g dB", 0, cmap_);
}

// ---------------------------------------------------------------------------
// Right: channel + demodulated time series
// ---------------------------------------------------------------------------
bool App::beginTimePlot(const char* id, const char* ylabel, float h, bool autoY, ImPlotFormatter yfmt, void* yud,
                        bool legend) {
    ImPlotFlags flags = ImPlotFlags_NoTitle;
    if (!legend) flags |= ImPlotFlags_NoLegend;
    if (!ImPlot::BeginPlot(id, ImVec2(-1, h), flags)) return false;
    ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
    ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisSI, &tFmt_);
    ImPlot::SetupAxisLinks(ImAxis_X1, &tmin_, &tmax_);
    double pad = std::max(dur_ * 0.02, 1.0 / std::max(1.0, fs_));
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, -pad, dur_ + pad);
    ImPlot::SetupAxisZoomConstraints(ImAxis_X1, 4.0 / std::max(1.0, fs_), dur_ + 2 * pad);
    ImPlot::SetupAxis(ImAxis_Y1, ylabel, autoY ? (ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit) : 0);
    if (yfmt) ImPlot::SetupAxisFormat(ImAxis_Y1, yfmt, yud);
    if (legend) ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal);
    return true;
}

void App::endTimePlot(const char* overlay) {
    drawTimeMarkersVertical();
    drawPlayhead(true);
    if (overlay && *overlay) {
        // measurement readout in the plot's top-left corner
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        ImVec2 p = ImPlot::GetPlotPos();
        p.x += 6;
        p.y += 4;
        ImVec2 sz = ImGui::CalcTextSize(overlay);
        ImPlot::PushPlotClipRect();
        dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1), ImVec2(p.x + sz.x + 3, p.y + sz.y + 1), IM_COL32(10, 10, 12, 190), 3);
        dl->AddText(p, IM_COL32(220, 220, 220, 255), overlay);
        ImPlot::PopPlotClipRect();
    }
    if (std::isfinite(hoverT_)) {
        ImPlot::SetNextLineStyle(kColHover);
        ImPlot::PlotInfLines("##ht", &hoverT_, 1, ImPlotItemFlags_NoFit);
    }
    if (ImPlot::IsPlotHovered()) {
        nextHoverT_ = ImPlot::GetPlotMousePos().x;
        if (ImGui::GetIO().KeyShift && ImGui::IsMouseClicked(0)) playFrom(nextHoverT_);
    }
    ImPlot::EndPlot();
}

void App::drawRight() {
    const ImGuiStyle& st = ImGui::GetStyle();
    // --- channel selection row ---
    flowBegin();
    flow(ImGui::GetFrameHeight() * 5);
    ImGui::Checkbox("Channel", &demod_.chanEnable);
    ImGui::SetItemTooltip("Shift, low-pass filter and decimate a channel before demodulating.\n"
                          "Drag the cyan lines in the waterfall/spectrum, or Ctrl+Click there to tune.");
    ImGui::BeginDisabled(!demod_.chanEnable);
    label("Offset", fieldW(13));
    InputSI("##coff", &demod_.chanOffset, "Hz", fieldW(13));
    ImGui::SetItemTooltip("Channel center relative to the capture center frequency");
    label("BW", fieldW(11));
    if (InputSI("##cbw", &demod_.chanBw, "Hz", fieldW(11))) demod_.chanBw = std::clamp(demod_.chanBw, 1.0, fs_);
    ImGui::SetItemTooltip("Channel filter bandwidth (two-sided)");
    label("Taps", stepW(5));
    if (ImGui::InputInt("##taps", &demod_.taps, 2, 32)) demod_.taps = std::clamp(demod_.taps, 0, 16383);
    ImGui::SetItemTooltip("FIR length, 0 = auto");
    label("Decim", stepW(5));
    if (ImGui::InputInt("##dec", &demod_.decim)) demod_.decim = std::clamp(demod_.decim, 0, 100000);
    ImGui::SetItemTooltip("Decimation factor, 0 = auto (output rate ~ 2x BW)");
    flow(ImGui::CalcTextSize("To peak").x + st.FramePadding.x * 2);
    if (ImGui::Button("To peak") && wf_ && !wf_->specF.empty()) {
        // strongest spectral peak within the visible frequency range
        int best = -1;
        for (int i = 0; i < (int)wf_->specF.size(); ++i)
            if (wf_->specF[i] >= fmin_ && wf_->specF[i] <= fmax_ && (best < 0 || wf_->specAvg[i] > wf_->specAvg[best]))
                best = i;
        if (best >= 0) demod_.chanOffset = wf_->specF[best] - fc_;
    }
    ImGui::SetItemTooltip("Center the channel on the strongest peak in the visible spectrum");
    ImGui::EndDisabled();
    flow(ImGui::GetFrameHeight() * 1.5f + ImGui::CalcTextSize("Squelch").x + stepW(6));
    ImGui::Checkbox("Squelch", &demod_.squelch);
    ImGui::SetItemTooltip("Blank FM/PM where the signal power is below the threshold");
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(stepW(6));
    ImGui::BeginDisabled(!demod_.squelch);
    ImGui::InputDouble("##sq", &demod_.squelchDb, 1, 10, "%.0f dB");
    ImGui::EndDisabled();
    label("Max pts", fieldW(10));
    long long md = demod_.maxDense;
    if (ImGui::InputScalar("##maxd", ImGuiDataType_S64, &md, nullptr, nullptr, "%lld",
                           ImGuiInputTextFlags_EnterReturnsTrue))
        demod_.maxDense = std::clamp<long long>(md, 10000, 200000000);
    ImGui::SetItemTooltip("Maximum demodulated samples processed at full resolution.\nWider views are sampled sparsely.");
    label("Plot H", fieldW(7));
    if (ImGui::InputFloat("##ph", &plotH_, 0, 0, "%.0f")) plotH_ = std::clamp(plotH_, 0.f, 2000.f);
    ImGui::SetItemTooltip("Plot height in pixels, 0 = fit the window");
    if (dm_) {
        char info[200];
        snprintf(info, sizeof info, "out %s | D=%d | taps=%d | %s pts", fmtSI(dm_->outRate, "Sps", 4).c_str(),
                 dm_->decim, dm_->taps, fmtSI((double)dm_->nOut, "", 4).c_str());
        flow(ImGui::CalcTextSize(info).x + ImGui::CalcTextSize("  SPARSE").x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", info);
        if (dm_->sparse) {
            ImGui::SameLine();
            ImGui::TextColored(kColWarn, "SPARSE");
            ImGui::SetItemTooltip("View exceeds 'Max pts': showing short demodulated blocks per pixel.\nZoom in (or raise Max pts) for full resolution.");
        }
    }
    ImGui::Separator();

    // --- scrollable plot stack ---
    ImGui::BeginChild("##plots", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    int nVis = (int)showIQ_ + showAM_ + showFM_ + showPM_;
    const float hdrH = ImGui::GetFrameHeightWithSpacing() * (ImGui::GetContentRegionAvail().x < fieldW(80) ? 2.f : 1.f);
    float availH = ImGui::GetContentRegionAvail().y;
    float h = plotH_ > 0 ? plotH_ : std::max(140.f, (availH - 4 * hdrH) / std::max(1, nVis) - st.ItemSpacing.y);

    static const char* amModes[] = {"Linear", "dB", "Normalized (m)"};
    static const char* pmModes[] = {"Wrapped", "Unwrapped", "Unwrapped, detrended"};

    auto header = [&](const char* name, bool* show, const ImVec4& col) {
        flowBegin();
        flow(ImGui::GetFrameHeight() * 6);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Checkbox(name, show);
        ImGui::PopStyleColor();
    };
    char buf[256];

    // I/Q
    ImGui::PushID("iq");
    header("I / Q", &showIQ_, kColI);
    flow(ImGui::GetFrameHeight() * 5);
    ImGui::Checkbox("Auto Y", &autoY_[0]);
    buf[0] = 0;
    if (dm_) {
        snprintf(buf, sizeof buf, "power  mean %.1f dBFS   min %.1f   max %.1f", dm_->sPwr.mean, dm_->sPwr.min, dm_->sPwr.max);
    }
    if (showIQ_ && beginTimePlot("##iqplot", "amplitude", h, autoY_[0], nullptr, nullptr, true)) {
        if (dm_) {
            ImPlot::SetNextLineStyle(kColI);
            ImPlot::PlotLine("I", dm_->i.t.data(), dm_->i.y.data(), (int)dm_->i.t.size());
            ImPlot::SetNextLineStyle(kColQ);
            ImPlot::PlotLine("Q", dm_->q.t.data(), dm_->q.y.data(), (int)dm_->q.t.size());
        }
        endTimePlot(buf);
    }
    ImGui::PopID();

    // AM
    ImGui::PushID("am");
    header("AM", &showAM_, kColAM);
    label("Mode", fieldW(13));
    ImGui::Combo("##amm", &demod_.amMode, amModes, 3);
    label("Smooth", stepW(5));
    if (ImGui::InputInt("##ams", &demod_.amSmooth)) demod_.amSmooth = std::clamp(demod_.amSmooth, 1, 100000);
    ImGui::SetItemTooltip("Moving-average length in output samples");
    flow(ImGui::GetFrameHeight() * 5);
    ImGui::Checkbox("Auto Y", &autoY_[1]);
    buf[0] = 0;
    if (dm_) {
        snprintf(buf, sizeof buf, "mod index m = %.3f   mean %.4g   min %.4g   max %.4g", dm_->amIndex, dm_->sAm.mean,
                 dm_->sAm.min, dm_->sAm.max);
    }
    static const char* amLabels[] = {"envelope", "envelope (dB)", "m(t)"};
    if (showAM_ && beginTimePlot("##amplot", amLabels[std::clamp(demod_.amMode, 0, 2)], h, autoY_[1])) {
        if (dm_) {
            ImPlot::SetNextLineStyle(kColAM);
            ImPlot::PlotLine("AM", dm_->am.t.data(), dm_->am.y.data(), (int)dm_->am.t.size());
        }
        endTimePlot(buf);
    }
    ImGui::PopID();

    // FM
    ImGui::PushID("fm");
    header("FM", &showFM_, kColFM);
    label("Smooth", stepW(5));
    if (ImGui::InputInt("##fms", &demod_.fmSmooth)) demod_.fmSmooth = std::clamp(demod_.fmSmooth, 1, 100000);
    ImGui::SetItemTooltip("Moving-average length in output samples (post-discriminator low-pass)");
    flow(ImGui::GetFrameHeight() * 8);
    ImGui::Checkbox("Rel. to channel", &demod_.fmRelative);
    ImGui::SetItemTooltip("Show frequency relative to the channel center instead of the capture center");
    ImGui::SameLine();
    ImGui::Checkbox("Auto Y", &autoY_[2]);
    buf[0] = 0;
    if (dm_) {
        double pk = std::max(std::fabs(dm_->sFm.max - dm_->sFm.mean), std::fabs(dm_->sFm.min - dm_->sFm.mean));
        snprintf(buf, sizeof buf, "mean %s   pk dev %s   rms dev %s", fmtSI(dm_->sFm.mean, "Hz", 5).c_str(),
                 fmtSI(pk, "Hz", 4).c_str(), fmtSI(dm_->sFm.rms, "Hz", 4).c_str());
    }
    static char hzUnit[] = "Hz";
    if (showFM_ && beginTimePlot("##fmplot", "inst. freq", h, autoY_[2], formatValueSI, hzUnit)) {
        if (dm_) {
            ImPlot::SetNextLineStyle(kColFM);
            ImPlot::PlotLine("FM", dm_->fm.t.data(), dm_->fm.y.data(), (int)dm_->fm.t.size());
        }
        endTimePlot(buf);
    }
    ImGui::PopID();

    // PM
    ImGui::PushID("pm");
    header("PM", &showPM_, kColPM);
    label("Mode", fieldW(18));
    ImGui::Combo("##pmm", &demod_.pmMode, pmModes, 3);
    ImGui::SetItemTooltip("Detrended removes the best-fit line, i.e. any residual carrier offset");
    label("Smooth", stepW(5));
    if (ImGui::InputInt("##pms", &demod_.pmSmooth)) demod_.pmSmooth = std::clamp(demod_.pmSmooth, 1, 100000);
    flow(ImGui::GetFrameHeight() * 9);
    ImGui::Checkbox("Degrees", &demod_.pmDegrees);
    ImGui::SameLine();
    ImGui::Checkbox("Auto Y", &autoY_[3]);
    const char* pu = demod_.pmDegrees ? "deg" : "rad";
    buf[0] = 0;
    if (dm_) {
        snprintf(buf, sizeof buf, "pk-pk %.4g %s   rms %.4g %s", dm_->sPm.max - dm_->sPm.min, pu, dm_->sPm.rms, pu);
    }
    if (showPM_ && beginTimePlot("##pmplot", demod_.pmDegrees ? "phase (deg)" : "phase (rad)", h, autoY_[3])) {
        if (dm_) {
            ImPlot::SetNextLineStyle(kColPM);
            ImPlot::PlotLine("PM", dm_->pm.t.data(), dm_->pm.y.data(), (int)dm_->pm.t.size());
        }
        endTimePlot(buf);
    }
    ImGui::PopID();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Status bar & shortcuts
// ---------------------------------------------------------------------------
void App::drawStatusBar() {
    ImGui::Separator();
    if (liveMode_) {
        drawLiveStatusBar();
        return;
    }
    std::string s;
    if (std::isfinite(hoverT_) || std::isfinite(hoverF_)) {
        if (std::isfinite(hoverT_)) s += "t=" + fmtSI(hoverT_, "s", 7) + " (#" + std::to_string((long long)std::llround(hoverT_ * fs_)) + ")  ";
        if (std::isfinite(hoverF_)) s += "f=" + fmtSI(hoverF_ - (relFreq_ ? fc_ : 0), "Hz", 8) + "  ";
        float v = wfValueAt(hoverT_, hoverF_);
        if (std::isfinite(v)) { char b[32]; snprintf(b, sizeof b, "%.1f dB  ", v); s += b; }
        if (dm_ && std::isfinite(hoverT_)) {
            char b[160];
            snprintf(b, sizeof b, "AM=%.4g  FM=%s  PM=%.4g", valueAt(dm_->am, hoverT_),
                     fmtSI(valueAt(dm_->fm, hoverT_), "Hz", 5).c_str(), valueAt(dm_->pm, hoverT_));
            s += b;
        }
    } else {
        s = status_;
    }
    if (tMarkers_) {
        double dt = std::fabs(tm2_ - tm1_);
        s += "   | dT=" + fmtSI(dt, "s", 6) + " (1/dT=" + fmtSI(dt > 0 ? 1 / dt : 0, "Hz", 5) + ", " +
             std::to_string((long long)std::llround(dt * fs_)) + " samples)";
    }
    if (fMarkers_) s += "   | dF=" + fmtSI(std::fabs(fm2_ - fm1_), "Hz", 6);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.c_str());
    const char* mode = win_.usingGL() ? "GLX" : "software";
    char right[96];
    snprintf(right, sizeof right, "%s%s | %.0f fps", workersBusy() ? "computing...  " : "", mode, ImGui::GetIO().Framerate);
    float rw = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - rw));
    ImGui::TextDisabled("%s", right);
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl) return;
    if (liveMode_) {
        handleLiveShortcuts();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pan(-0.1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pan(0.1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) pan(-1.0);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) pan(1.0);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) setView(0, tmax_ - tmin_);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) setView(dur_ - (tmax_ - tmin_), dur_);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) zoom(0.5);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) zoom(2.0);
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        setView(0, dur_);
        fmin_ = fc_ - fs_ / 2;
        fmax_ = fc_ + fs_ / 2;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) tMarkers_ = !tMarkers_;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fMarkers_ = !fMarkers_;
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) demod_.chanEnable = !demod_.chanEnable;
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        char name[64];
        snprintf(name, sizeof name, "B%zu @ %s", bookmarks_.size() + 1, fmtSI(tmin_, "s", 5).c_str());
        bookmarks_.push_back({name, tmin_, tmax_});
        bookmarkSel_ = (int)bookmarks_.size() - 1;
    }
    if (tMarkers_ && ImGui::IsKeyPressed(ImGuiKey_Z, false)) setView(std::min(tm1_, tm2_), std::max(tm1_, tm2_));
}
