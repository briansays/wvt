// Signal generator window: CW / pulsed waveform parameters, a preview, and
// the three outputs (file, live loopback, USRP transmit).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "app.h"
#include "platform_x11.h"

namespace {
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColTx(1.f, 0.32f, 0.32f, 1.f);
const ImVec4 kColEnv(0.40f, 0.90f, 0.50f, 1.f);
const ImVec4 kColI(0.35f, 0.70f, 1.00f, 1.f);
const ImVec4 kColQ(1.00f, 0.55f, 0.25f, 1.f);

const SampleFormat kFileFmts[] = {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CI8, SampleFormat::CU8};
const char* kFileFmtNames[] = {"cf32 (float, exact)", "ci16", "ci8", "cu8 (RTL-SDR style)"};

void tooltipAlways(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

// Starts a "label | field" row in a two-column table; the field fills the column.
void row(const char* label, const char* tip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip) tooltipAlways(tip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool beginForm(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.5f);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

std::string staggerText(const std::vector<double>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + fmtSI(v[i], "s", 6);
    return s;
}

// "1m, 1.25 ms; 800u" -> {1e-3, 1.25e-3, 8e-4}
bool parseStagger(const char* text, std::vector<double>& out) {
    std::vector<double> v;
    std::string s = text, tok;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',' || s[i] == ';') {
            size_t a = tok.find_first_not_of(' '), b = tok.find_last_not_of(' ');
            if (a != std::string::npos) {
                double x;
                if (!parseSI(tok.substr(a, b - a + 1).c_str(), x)) return false;
                v.push_back(x);
            }
            tok.clear();
        } else {
            tok += s[i];
        }
    }
    if (v.empty()) return false;
    out = v;
    return true;
}

// Red-tinted button for transmit actions.
bool redButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.22f, 0.22f, 1.f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

bool haveTx() {
    for (const auto& d : sdrDrivers())
        if (d == "uhd") return true;
    return false;
}
}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
void App::openSigGen(const WaveformParams* p) {
    if (p) sg_ = *p;
    sgOpen_ = true;
    if (checkParams(sg_).ok()) sigGenBus().publish(sg_);
}

bool App::sigGenBusy() const { return (tx_ && tx_->running()) || sgFileSt_.active || sgPrevWorker_->busy(); }

bool App::sigGenLoopback() const { return live_ && live_->running() && devCfg_.driver == "siggen"; }

void App::pollSigGen() {
    if (auto r = sgPrevWorker_->take()) sgPrev_ = r;
    const bool wasActive = sgFileSt_.active;
    sgFileSt_ = sgFile_->status();
    if (wasActive && !sgFileSt_.active) {
        const auto& rs = sgFileSt_.rec;
        std::string name = rs.dataPath.substr(rs.dataPath.find_last_of('/') + 1);
        sgFileFailed_ = !sgFileSt_.error.empty();
        sgFileMsg_ = sgFileFailed_ ? "Generation " + sgFileSt_.error + ": " + name
                                   : "Wrote " + name + " (" + fmtSI(rs.seconds, "s", 4) + ", " +
                                         fmtSI((double)rs.bytes, "B", 4) + ")";
        sgLastFile_ = rs.metaPath.empty() ? rs.dataPath : rs.metaPath;
        status_ = sgFileMsg_;
    }
    if (tx_ && tx_->running()) {
        txStats_ = tx_->stats();
        if (!txStats_.error.empty()) stopTx("Transmitter stopped: " + txStats_.error);
    }
}

bool App::startLoopback() {
    StreamConfig c = devCfg_;
    c.driver = "siggen";
    c.args.clear();
    c.fc = sg_.fc;
    c.rate = sg_.rate;
    c.agc = false;
    c.gain = 30;  // unity
    if (checkParams(sg_).ok()) sigGenBus().publish(sg_);
    return startDevice(c);
}

bool App::startTx() {
    txCfg_.args = txArgs_;
    txCfg_.fc = sg_.fc;
    txCfg_.rate = sg_.rate;
    sigGenBus().publish(sg_);
    if (!tx_) tx_ = std::make_unique<TxEngine>();
    std::string err;
    if (!tx_->start(txCfg_, err)) {
        txErr_ = err;
        status_ = "Transmit failed: " + err;
        return false;
    }
    txErr_.clear();
    txStats_ = tx_->stats();
    status_ = "TRANSMITTING on " + fmtSI(txStats_.fc + sg_.offset, "Hz", 9) + " from " + txStats_.description;
    return true;
}

void App::stopTx(const std::string& why) {
    if (tx_) tx_->stop();
    txArmed_ = false;
    if (!why.empty()) {
        status_ = why;
        if (why.rfind("Transmitter stopped: ", 0) == 0) txErr_ = why.substr(21);
    }
}

// ---------------------------------------------------------------------------
// Toolbar button
// ---------------------------------------------------------------------------
void App::drawSigGenButton() {
    ImGui::SameLine();
    const bool txOn = tx_ && tx_->running();
    if (txOn) {
        if (redButton("TX ON")) sgOpen_ = true;
        tooltipAlways("The signal generator is transmitting. Click to open it.");
    } else {
        if (ImGui::Button(sgOpen_ ? "Signal gen *" : "Signal gen")) sgOpen_ = !sgOpen_;
        tooltipAlways("Open or close the signal generator (Ctrl+G)");
    }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
void App::drawSigGenWindow() {
    if (!sgOpen_) {
        // Closing the window ends a transmission: nothing should transmit unseen.
        if (tx_ && tx_->running()) stopTx("Transmitter stopped (generator window closed)");
        return;
    }
    const float fs = ImGui::GetFontSize();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(fs * 34, std::min(vp->WorkSize.y - fs * 4, fs * 56)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - fs * 35, vp->WorkPos.y + fs * 3), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 24, fs * 10), ImVec2(FLT_MAX, FLT_MAX));
    const bool txOn = tx_ && tx_->running();
    if (txOn) ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
    if (txOn) ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.45f, 0.10f, 0.10f, 1.f));
    const bool open = ImGui::Begin(txOn ? "Signal Generator - TRANSMITTING###siggen" : "Signal Generator###siggen", &sgOpen_,
                                   ImGuiWindowFlags_NoSavedSettings);
    if (txOn) ImGui::PopStyleColor(2);
    if (!open) {
        ImGui::End();
        return;
    }

    // --- output selector ---
    const bool fileActive = sgFileSt_.active;
    if (beginForm("##sgout")) {
        row("Output");
        const char* outs[] = {"File (SigMF / raw)", "Loopback (live view)", "USRP transmit"};
        ImGui::BeginDisabled(txOn || fileActive);
        if (ImGui::BeginCombo("##sgoutput", outs[sgOutput_])) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(outs[i], sgOutput_ == i)) sgOutput_ = i;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        tooltipAlways("File: write the waveform to disk.\nLoopback: show it in the live spectrum and waterfall.\n"
                      "USRP transmit: send it over the air with a B200/B205.");
        ImGui::EndTable();
    }
    ImGui::Separator();

    if (ImGui::BeginChild("##sgbody", ImVec2(0, 0), ImGuiChildFlags_None)) {
        drawSigGenWaveform();

        sgCheck_ = checkParams(sg_);
        for (const auto& e : sgCheck_.errors) ImGui::TextColored(kColErr, "%s", e.c_str());
        for (const auto& w : sgCheck_.warnings) ImGui::TextColored(kColWarn, "%s", w.c_str());
        if (!sgCheck_.ok() && (txOn || sigGenLoopback()))
            ImGui::TextColored(kColErr, "Not applied: the output keeps the last valid settings.");
        if (sgCheck_.ok()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", describeParams(sg_).c_str());
            ImGui::PopTextWrapPos();
            sigGenBus().publish(sg_);
            sgPrevWorker_->submit({sg_, sgPrevTrain_ && sg_.mode == GenMode::Pulsed});
        }

        if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) drawSigGenPreview();
        const char* outHdr[] = {"File output", "Loopback", "USRP transmit"};
        ImGui::PushID(sgOutput_);
        if (ImGui::CollapsingHeader(outHdr[sgOutput_], ImGuiTreeNodeFlags_DefaultOpen)) {
            if (sgOutput_ == 0) drawSigGenFile();
            else if (sgOutput_ == 1) drawSigGenLoopback();
            else drawSigGenTx();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Waveform parameters
// ---------------------------------------------------------------------------
void App::drawSigGenWaveform() {
    const bool txOn = tx_ && tx_->running();
    const bool fileActive = sgFileSt_.active;
    WaveformParams& p = sg_;

    if (ImGui::CollapsingHeader("Carrier", ImGuiTreeNodeFlags_DefaultOpen) && beginForm("##sgcarrier")) {
        row("Sample rate");
        double rate = p.rate;
        const bool lockRate = txOn || fileActive || (sigGenLoopback() && liveRecording());
        ImGui::BeginDisabled(lockRate);
        if (InputSI("##sgrate", &rate, "Sps", -FLT_MIN) && rate > 0) {
            p.rate = rate;
            if (sigGenLoopback()) startLoopback();
        }
        ImGui::EndDisabled();
        tooltipAlways(lockRate ? "Stop the output to change the sample rate"
                               : "Sample rate of the generated stream (also the transmit rate)");

        row("Center freq");
        if (InputSI("##sgfc", &p.fc, "Hz", -FLT_MIN) && txOn) {
            std::string err;
            if (!tx_->setCenterFreq(p.fc, err)) status_ = "Transmit tune failed: " + err;
        }
        tooltipAlways("RF center frequency: the transmit tuning and the file's frequency.\n"
                      "The signal itself sits at center + offset.");

        row("Offset");
        InputSI("##sgoff", &p.offset, "Hz", -FLT_MIN);
        tooltipAlways("Carrier offset from the center frequency, inside +-rate/2.\n"
                      "A non-zero offset keeps the signal away from the USRP's LO leakage at the center.");

        row("Amplitude");
        ImGui::InputDouble("##sgamp", &p.ampDb, 1, 6, "%.1f dBFS");
        tooltipAlways("Peak amplitude relative to full scale (0 dBFS)");

        row("Start phase");
        ImGui::InputDouble("##sgph", &p.phaseDeg, 15, 90, "%.1f deg");

        row("Mode");
        int mode = (int)p.mode;
        ImGui::RadioButton("CW", &mode, (int)GenMode::CW);
        ImGui::SameLine();
        ImGui::RadioButton("Pulsed", &mode, (int)GenMode::Pulsed);
        p.mode = (GenMode)mode;
        ImGui::EndTable();
    }

    if (p.mode == GenMode::CW) {
        if (ImGui::CollapsingHeader("CW", ImGuiTreeNodeFlags_DefaultOpen) && beginForm("##sgcw")) {
            row("Drift");
            InputSI("##sgdrift", &p.driftHzPerSec, "Hz/s", -FLT_MIN);
            tooltipAlways("Linear frequency drift, e.g. to test a tracking loop. 0 = steady tone.");
            ImGui::EndTable();
        }
    } else {
        if (ImGui::CollapsingHeader("Pulse", ImGuiTreeNodeFlags_DefaultOpen) && beginForm("##sgpulse")) {
            row("PRI / PRF", "Pulse repetition interval, or its inverse, the repetition frequency");
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemInnerSpacing.x) / 2;
            InputSI("##sgpri", &p.pri, "s", half);
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            double prf = p.pri > 0 ? 1 / p.pri : 0;
            if (InputSI("##sgprf", &prf, "Hz", -FLT_MIN) && prf > 0) p.pri = 1 / prf;

            row("Width / duty", "Pulse width (the whole on-time, edges included), or the duty cycle");
            InputSI("##sgpw", &p.pw, "s", half);
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            double duty = p.pri > 0 ? 100 * p.pw / p.pri : 0;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputDouble("##sgduty", &duty, 0, 0, "%.4g %%", ImGuiInputTextFlags_EnterReturnsTrue) && duty > 0)
                p.pw = p.pri * duty / 100;

            row("Edges");
            if (ImGui::BeginCombo("##sgedge", edgeName(p.edge))) {
                for (int i = 0; i < (int)EdgeShape::Count; ++i)
                    if (ImGui::Selectable(edgeName((EdgeShape)i), (int)p.edge == i)) {
                        p.edge = (EdgeShape)i;
                        if (p.edge != EdgeShape::Rect && p.rise == 0 && p.fall == 0) p.rise = p.fall = p.pw * 0.1;
                    }
                ImGui::EndCombo();
            }
            if (p.edge != EdgeShape::Rect) {
                row("Rise / fall", "Duration of each edge (0-100%)");
                InputSI("##sgrise", &p.rise, "s", half);
                ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                InputSI("##sgfall", &p.fall, "s", -FLT_MIN);
            }

            row("PRI pattern");
            if (ImGui::BeginCombo("##sgpat", priPatternName(p.priPattern))) {
                for (int i = 0; i < (int)PriPattern::Count; ++i)
                    if (ImGui::Selectable(priPatternName((PriPattern)i), (int)p.priPattern == i)) p.priPattern = (PriPattern)i;
                ImGui::EndCombo();
            }
            tooltipAlways("Fixed: every PRI the same.\nStagger: cycle through a list of PRIs.\n"
                          "Jitter: each PRI varies randomly around the set PRI.");
            if (p.priPattern == PriPattern::Stagger) {
                row("PRIs", "Comma-separated PRIs used in turn, e.g. 1m, 1.25m, 800u (Enter applies)");
                if (!ImGui::GetIO().WantTextInput)  // don't overwrite the list while it's being typed
                    snprintf(sgStagger_, sizeof sgStagger_, "%s", staggerText(p.stagger).c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText("##sgstag", sgStagger_, sizeof sgStagger_);
                if (ImGui::IsItemDeactivatedAfterEdit() && !parseStagger(sgStagger_, p.stagger))
                    status_ = "Could not read the stagger list (use e.g. 1m, 1.25m)";
            } else if (p.priPattern == PriPattern::Jitter) {
                row("Jitter / seed", "PRI varies uniformly by +- this percentage. The same seed repeats the sequence.");
                ImGui::SetNextItemWidth(half);
                ImGui::InputDouble("##sgjit", &p.jitterPct, 1, 5, "+-%.3g %%");
                ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                int seed = (int)p.seed;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##sgseed", &seed)) p.seed = (uint32_t)std::max(0, seed);
            }

            row("Burst", "Pulses per burst (0 = continuous train) and the extra dead time after each burst");
            int bp = p.burstPulses;
            ImGui::SetNextItemWidth(half);
            if (ImGui::InputInt("##sgbp", &bp)) p.burstPulses = std::max(0, bp);
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::BeginDisabled(p.burstPulses == 0);
            InputSI("##sggap", &p.burstGap, "s", -FLT_MIN);
            ImGui::EndDisabled();

            row("Start delay", "Silence before the first pulse");
            InputSI("##sgdelay", &p.startDelay, "s", -FLT_MIN);

            row("Modulation", "Modulation inside each pulse");
            if (ImGui::BeginCombo("##sgintra", intraModName(p.intra))) {
                for (int i = 0; i < (int)IntraMod::Count; ++i)
                    if (ImGui::Selectable(intraModName((IntraMod)i), (int)p.intra == i)) p.intra = (IntraMod)i;
                ImGui::EndCombo();
            }
            if (p.intra == IntraMod::Barker) {
                row("Code length");
                if (ImGui::BeginCombo("##sgbark", ("Barker-" + std::to_string(p.barkerLen)).c_str())) {
                    for (int n : barkerLengths())
                        if (ImGui::Selectable(("Barker-" + std::to_string(n)).c_str(), p.barkerLen == n)) p.barkerLen = n;
                    ImGui::EndCombo();
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("chip %s, bandwidth ~%s", fmtSI(p.pw / p.barkerLen, "s", 4).c_str(),
                                    fmtSI(p.barkerLen / p.pw, "Hz", 4).c_str());
            } else if (p.intra != IntraMod::None) {
                row("Chirp BW", "Frequency sweep of the chirp, centered on the offset");
                InputSI("##sgbw", &p.chirpBw, "Hz", -FLT_MIN);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("time-bandwidth product %.4g", p.chirpBw * p.pw);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Impairments") && beginForm("##sgimp")) {
        row("Noise");
        ImGui::Checkbox("##sgnoise", &p.noise);
        ImGui::SameLine();
        ImGui::BeginDisabled(!p.noise);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble("##sgsnr", &p.snrDb, 1, 10, "SNR %.1f dB");
        ImGui::EndDisabled();
        tooltipAlways("White Gaussian noise over the whole band. SNR = peak signal power / total noise power.");

        row("DC offset I / Q", "Constant offset in each branch, as a fraction of full scale");
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemInnerSpacing.x) / 2;
        ImGui::SetNextItemWidth(half);
        ImGui::InputDouble("##sgdci", &p.dcI, 0.001, 0.01, "%.4f");
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble("##sgdcq", &p.dcQ, 0.001, 0.01, "%.4f");

        row("IQ imbalance", "Q branch gain error and phase error (they create an image at -offset)");
        ImGui::SetNextItemWidth(half);
        ImGui::InputDouble("##sgiqg", &p.iqGainDb, 0.1, 1, "%.2f dB");
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble("##sgiqp", &p.iqPhaseDeg, 0.5, 5, "%.2f deg");

        row("Linewidth", "Carrier phase noise as a random walk with this 3 dB linewidth. 0 = clean carrier.");
        InputSI("##sglw", &p.linewidth, "Hz", -FLT_MIN);
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------
void App::drawSigGenPreview() {
    const float fs = ImGui::GetFontSize();
    if (sg_.mode == GenMode::Pulsed) {
        int train = sgPrevTrain_ ? 1 : 0;
        ImGui::RadioButton("One pulse", &train, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Pulse train", &train, 1);
        sgPrevTrain_ = train == 1;
    }
    const auto r = sgPrev_;
    if (!r) {
        ImGui::TextDisabled(sgCheck_.ok() ? "computing..." : "fix the settings above to see a preview");
        return;
    }
    const bool stale = !(r->req.p == sg_);
    static const char* kUnitS = "s";
    static const char* kUnitHz = "Hz";

    if (ImPlot::BeginPlot("##sgtime", ImVec2(-1, fs * 9), ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)kUnitS);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal);
        const int n = (int)r->t.size();
        if (r->iq) {
            ImPlot::SetNextLineStyle(kColI);
            ImPlot::PlotLine("I", r->t.data(), r->i.data(), n);
            ImPlot::SetNextLineStyle(kColQ);
            ImPlot::PlotLine("Q", r->t.data(), r->q.data(), n);
        }
        ImPlot::SetNextLineStyle(kColEnv, 1.5f);
        ImPlot::PlotLine(r->iq ? "|x|" : "|x| (peak)", r->t.data(), r->mag.data(), n);
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("##sgspec", ImVec2(-1, fs * 9), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) {
        double peak = -200, lo = 0;
        for (double d : r->db) peak = std::max(peak, d);
        lo = peak;
        for (double d : r->db) lo = std::min(lo, d);
        ImPlot::SetupAxes(nullptr, "dBFS", ImPlotAxisFlags_AutoFit, 0);
        ImPlot::SetupAxisLimits(ImAxis_Y1, std::max(lo, peak - 120) - 5, peak + 5, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)kUnitHz);
        ImPlot::SetNextLineStyle(kColI);
        ImPlot::PlotLine("spectrum", r->f.data(), r->db.data(), (int)r->f.size());
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled("spectrum: %s bins, power per bin%s", fmtSI(r->rbw, "Hz", 3).c_str(), stale ? "  (updating)" : "");
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
void App::drawSigGenFile() {
    const bool active = sgFileSt_.active;
    FileGenConfig& c = sgFileCfg_;
    ImGui::BeginDisabled(active);
    if (beginForm("##sgfile")) {
        row("Save to");
        ImGui::InputText("##sgprefix", sgPrefix_, sizeof sgPrefix_);
        tooltipAlways("Path prefix. Date, time, frequency and rate are appended, and missing folders are created.");

        row("Samples");
        int fi = 0;
        for (int i = 0; i < 4; ++i)
            if (kFileFmts[i] == c.fmt) fi = i;
        if (ImGui::Combo("##sgffmt", &fi, kFileFmtNames, 4)) c.fmt = kFileFmts[fi];
        tooltipAlways("cf32 keeps the waveform exactly. Integer formats quantize and clip at full scale.");

        row("Container");
        int cont = c.rec.sigmf ? 0 : 1;
        ImGui::RadioButton("SigMF", &cont, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Raw", &cont, 1);
        c.rec.sigmf = cont == 0;
        tooltipAlways("SigMF stores the rate, frequency and all generator settings in a .sigmf-meta file.\n"
                      "Raw is bare samples; rate and frequency go in the file name.");

        row("Length");
        if (sg_.mode == GenMode::Pulsed) {
            int by = sgByPulses_ ? 1 : 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            const char* bys[] = {"Duration", "Pulses"};
            if (ImGui::Combo("##sgby", &by, bys, 2)) sgByPulses_ = by == 1;
            ImGui::SameLine();
        }
        const bool byPulses = sgByPulses_ && sg_.mode == GenMode::Pulsed;
        if (byPulses) {
            long long n = std::max<long long>(1, c.pulses);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputScalar("##sgnp", ImGuiDataType_S64, &n)) c.pulses = std::max<long long>(1, n);
            if (c.pulses < 1) c.pulses = 100;
        } else {
            InputSI("##sglen", &c.seconds, "s", -FLT_MIN);
        }
        if (sg_.mode == GenMode::Pulsed && c.rec.sigmf) {
            row("Annotations");
            ImGui::Checkbox("Mark each pulse##sgann", &c.annotatePulses);
            tooltipAlways("Adds a SigMF annotation (start and length) for every pulse, up to 10000.");
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    const bool byPulses = sgByPulses_ && sg_.mode == GenMode::Pulsed;
    const double secs = byPulses ? (double)std::max<int64_t>(1, c.pulses) * sg_.meanPri() + sg_.startDelay : c.seconds;
    const double bytes = secs * sg_.rate * formatBytesPerSample(c.fmt);
    ImGui::TextDisabled("about %s, %s", fmtSI(secs, "s", 4).c_str(), fmtSI(bytes, "B", 3).c_str());

    if (!active) {
        ImGui::BeginDisabled(!sgCheck_.ok());
        if (ImGui::Button("Generate file")) {
            FileGenConfig run = c;
            run.rec.prefix = sgPrefix_;
            if (!byPulses) run.pulses = 0;
            std::string err;
            if (sgFile_->start(sg_, run, err)) {
                sgFileMsg_.clear();
                sgFileSt_ = sgFile_->status();
            } else {
                sgFileMsg_ = "Generation failed: " + err;
                sgFileFailed_ = true;
            }
        }
        ImGui::EndDisabled();
    } else {
        ImGui::ProgressBar((float)sgFileSt_.progress, ImVec2(ImGui::GetFontSize() * 14, 0));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) sgFile_->cancel();
    }
    if (!sgFileMsg_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(sgFileFailed_ ? kColErr : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", sgFileMsg_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!active && !sgLastFile_.empty() && !sgFileFailed_) {
        if (ImGui::Button("Open in viewer")) {
            std::string path = sgLastFile_;
            if (liveMode_) leaveLive();
            openFile(path);
        }
        tooltipAlways("Open the generated file in the main window (File mode)");
    }
}

void App::drawSigGenLoopback() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Streams the waveform into the main window's live spectrum and waterfall, as a "
                       "receiver tuned to the center frequency would see it. Changes apply as you make them. "
                       "Record it with the main toolbar's Record button.");
    ImGui::PopTextWrapPos();
    if (sigGenLoopback()) {
        ImGui::BeginDisabled(liveRecording());
        if (ImGui::Button("Stop loopback")) {
            live_->stop();
            status_ = "Loopback stopped";
        }
        ImGui::EndDisabled();
        tooltipAlways(liveRecording() ? "Stop the recording first" : "Stop streaming into the live view");
    } else {
        ImGui::BeginDisabled(!sgCheck_.ok() || liveRecording());
        if (ImGui::Button("Start loopback")) startLoopback();
        ImGui::EndDisabled();
        tooltipAlways(liveRecording() ? "A recording is running from another device" : "Switch the main window to the live view of the generator");
        if (sg_.rate > 20e6) ImGui::TextColored(kColWarn, "Loopback runs at up to 20 MS/s");
    }
}

void App::drawSigGenTx() {
    const float fs = ImGui::GetFontSize();
    if (!haveTx()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kColWarn, "This wvt was built without UHD, so it can't transmit. Install UHD "
                                     "(see README: Optional: device support) and rebuild.");
        ImGui::PopTextWrapPos();
        return;
    }
    const bool txOn = tx_ && tx_->running();

    if (beginForm("##sgtx")) {
        ImGui::BeginDisabled(txOn);
        row("Device args");
        ImGui::InputText("##txargs", txArgs_, sizeof txArgs_);
        tooltipAlways("UHD device arguments, e.g. type=b200 or serial=31A2B3C.\n"
                      "If the live view is receiving from the same USRP, both share it (full duplex).");
        row("Antenna");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("TX/RX");
        tooltipAlways("TX/RX is the only transmit port on the B200/B205.");
        row("Bandwidth");
        InputSI("##txbw", &txCfg_.bandwidth, "Hz", -FLT_MIN);
        tooltipAlways("Analog filter bandwidth. 0 = automatic (follows the sample rate).");
        ImGui::EndDisabled();

        row("TX gain");
        const GainRange g = txOn ? txStats_.range : GainRange{0, 89.75, 0.25, {}, false};
        float gain = (float)txCfg_.gain;
        if (ImGui::SliderFloat("##txgain", &gain, (float)g.min, (float)g.max, "%.2f dB")) {
            txCfg_.gain = gain;
            std::string err;
            if (txOn && !tx_->setGain(gain, err)) status_ = "Gain failed: " + err;
        }
        tooltipAlways("B200/B205 transmit gain, 0-89.75 dB. Start low: about 0 dBm comes out near the top of the range.");
        ImGui::EndTable();
    }

    // The B2xx derives RX and TX rates from one clock: they must match while both run.
    if (!txOn && live_ && live_->running() && devCfg_.driver == "uhd" && std::fabs(liveStats_.rate - sg_.rate) > 1) {
        ImGui::TextColored(kColWarn, "Receiving at %s: transmit must use the same rate.", fmtSI(liveStats_.rate, "S/s", 6).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Use it")) sg_.rate = liveStats_.rate;
    }

    if (!txOn) {
        if (ImGui::Checkbox("Arm transmitter", &txArmed_) && txArmed_) txErr_.clear();
        tooltipAlways("Safety catch: the transmit button works only while armed. Disarms after every transmission.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!txArmed_ || !sgCheck_.ok());
        if (redButton("Transmit...")) ImGui::OpenPopup("Start transmitting?");
        ImGui::EndDisabled();
        if (!txErr_.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kColErr, "%s", txErr_.c_str());
            ImGui::PopTextWrapPos();
        }
    } else {
        if (redButton("STOP TRANSMITTING", ImVec2(-FLT_MIN, fs * 2.2f))) stopTx("Transmitter stopped");
        const auto& s = txStats_;
        ImGui::TextColored(kColTx, "TX ON  %s  (signal at %s)", fmtSI(s.fc, "Hz", 9).c_str(),
                           fmtSI(s.fc + sg_.offset, "Hz", 9).c_str());
        ImGui::Text("%s, %s, gain %.2f dB", s.description.c_str(), fmtSI(s.rate, "S/s", 6).c_str(), s.gain);
        ImGui::Text("sent %s   underflows %lld   lost packets %lld", fmtSI((double)s.tx.sent, "S", 4).c_str(),
                    (long long)s.tx.underflows, (long long)s.tx.seqErrors);
        if (!s.notes.empty()) ImGui::TextColored(kColWarn, "%s", s.notes.c_str());
        if (!sgCheck_.ok()) ImGui::TextColored(kColWarn, "Still transmitting the last valid settings.");
    }

    if (ImGui::BeginPopupModal("Start transmitting?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(kColTx, "Transmit at %s", fmtSI(sg_.fc + sg_.offset, "Hz", 9).c_str());
        ImGui::Text("Center %s, %s, TX gain %.2f dB, antenna TX/RX", fmtSI(sg_.fc, "Hz", 9).c_str(),
                    fmtSI(sg_.rate, "S/s", 6).c_str(), txCfg_.gain);
        ImGui::Text("Device: %s", txArgs_);
        ImGui::PushTextWrapPos(fs * 30);
        ImGui::TextDisabled("%s", describeParams(sg_).c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Only transmit into a dummy load or an attenuated cable, or on a frequency and power "
                               "you are licensed for. Never connect the TX/RX port straight to a receiver input "
                               "without at least 30 dB of attenuation.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (redButton("Transmit")) {
            startTx();
            txArmed_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
