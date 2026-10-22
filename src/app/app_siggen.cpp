// Signal generator window: CW / pulsed / playback waveform parameters, a
// preview, and the three outputs (file, live loopback, USRP transmit).
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "common/ui_util.h"
#include "common/log.h"
#include "app/platform_x11.h"

namespace {
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColTx(1.f, 0.32f, 0.32f, 1.f);
const ImVec4 kColEnv(0.40f, 0.90f, 0.50f, 1.f);
const ImVec4 kColI(0.35f, 0.70f, 1.00f, 1.f);
const ImVec4 kColQ(1.00f, 0.55f, 0.25f, 1.f);

const SampleFormat kFileFmts[] = {SampleFormat::CF32, SampleFormat::CI16, SampleFormat::CI8, SampleFormat::CU8};
const char* kFileFmtNames[] = {"cf32 (float, exact)", "ci16", "ci8", "cu8 (RTL-SDR style)"};




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
    out = std::move(v);
    return true;
}

// Red-tinted button for transmit actions.

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
    snprintf(sgPlayPath_, sizeof sgPlayPath_, "%s", sg_.playPath.c_str());
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
        if (sgFileFailed_ && sgFileSt_.error != "cancelled") LOG_ERROR("app", "%s", sgFileMsg_.c_str());
    }
    if (tx_ && tx_->running()) {
        txStats_ = tx_->stats();
        if (!txStats_.error.empty()) stopTx("Transmitter stopped: " + txStats_.error);
        else if (txStats_.finished)
            stopTx("The chain finished after " + std::to_string(txStats_.chainPasses) +
                   (txStats_.chainPasses == 1 ? " pass" : " passes") + ": transmitter stopped");  // (not an error)
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
    if (checkParams(sg_).ok()) sigGenBus().publish(sg_);  // a recording not loaded yet loads in the stream's thread
    return startDevice(c);
}

bool App::startTx() {
    txCfg_.args = txArgs_;
    txCfg_.antenna = txAntenna_;
    txCfg_.fc = sg_.fc;
    txCfg_.rate = sg_.rate;
    sigGenBus().publish(sg_);
    if (!tx_) tx_ = std::make_unique<TxEngine>();
    std::string err;
    if (!tx_->start(txCfg_, err)) {
        txErr_ = err;
        status_ = "Transmit failed: " + err;
        LOG_ERROR("app", "transmit failed: %s", err.c_str());
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
        if (why.rfind("Transmitter stopped: ", 0) == 0) {
            txErr_ = why.substr(21);
            LOG_ERROR("app", "transmitter stopped: %s", txErr_.c_str());
        }
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
    if (beginForm("##sgout", 8.5f)) {
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
                      "USRP transmit: send it over the air with a USRP.");
        ImGui::EndTable();
    }
    ImGui::Separator();

    if (ImGui::BeginChild("##sgbody", ImVec2(0, 0), ImGuiChildFlags_None)) {
        drawSigGenWaveform();

        sgCheck_ = checkParams(sg_);
        // A recording is published only once it's loaded (the preview worker loads it), so the
        // streaming outputs pick it up from the cache instead of stalling on the disk.
        int loaded = 0;
        sgReady_ = sgCheck_.ok() && (sg_.mode != GenMode::Playback || cachedPlayback(sg_)) &&
                   (sg_.mode != GenMode::Chain || chainLoaded(&loaded));
        const bool playErr = sgCheck_.ok() && !sgReady_ && sgPrev_ && sgPrev_->req.p == sg_ && !sgPrev_->error.empty();
        ImGui::PushTextWrapPos(0);
        for (const auto& e : sgCheck_.errors) ImGui::TextColored(kColErr, "%s", e.c_str());
        if (playErr) ImGui::TextColored(kColErr, "%s: %s", sg_.mode == GenMode::Chain ? "Chain" : "Recording", sgPrev_->error.c_str());
        for (const auto& w : sgCheck_.warnings) ImGui::TextColored(kColWarn, "%s", w.c_str());
        if (!sgReady_ && (txOn || sigGenLoopback()) && (!sgCheck_.ok() || playErr))
            ImGui::TextColored(kColErr, "Not applied: the output keeps the last valid settings.");
        if (sgCheck_.ok()) {
            ImGui::TextDisabled("%s", describeParams(sg_).c_str());
            if (!sgReady_ && !playErr) {
                if (sg_.mode == GenMode::Chain) ImGui::TextDisabled("Loading the chain (%d of %d)...", loaded, (int)sg_.chain.size());
                else ImGui::TextDisabled("Loading the recording...");
            }
            if (sgReady_) sigGenBus().publish(sg_);
            sgPrevWorker_->submit({sg_, sgPrevTrain_ && sg_.mode == GenMode::Pulsed});
        }
        ImGui::PopTextWrapPos();

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

    if (ImGui::CollapsingHeader("Carrier", ImGuiTreeNodeFlags_DefaultOpen) && beginForm("##sgcarrier", 8.5f)) {
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
            if (!tx_->setCenterFreq(p.fc, err)) {
                LOG_ERROR("app", "transmit tune failed: %s", err.c_str());
                // Show where the transmitter is, not the frequency that failed (issue #40).
                txStats_ = tx_->stats();
                p.fc = txStats_.fc;
                if (!txStats_.error.empty()) stopTx("Transmitter stopped: " + txStats_.error);
                else status_ = "Transmit tune failed: " + err;
            }
        }
        tooltipAlways("RF center frequency: the transmit tuning and the file's frequency.\n"
                      "The signal itself sits at center + offset.");

        const bool chain = p.mode == GenMode::Chain;
        const bool fSweep = p.sweeping() && p.fSweep != SweepShape::Off;
        const bool aSweep = p.sweeping() && p.aSweep != SweepShape::Off;
        row("Offset");
        ImGui::BeginDisabled(fSweep || chain);
        InputSI("##sgoff", &p.offset, "Hz", -FLT_MIN);
        ImGui::EndDisabled();
        tooltipAlways(chain    ? "Each chain item has its own offset (Chain section)"
                      : fSweep ? "The frequency sweep sets the frequency (Sweep rows)"
                               : "Carrier offset from the center frequency, inside +-rate/2.\n"
                                 "A non-zero offset keeps the signal away from the USRP's LO leakage at the center.");

        row("Amplitude");
        ImGui::BeginDisabled(aSweep || chain);
        ImGui::InputDouble("##sgamp", &p.ampDb, 1, 6, "%.1f dBFS");
        ImGui::EndDisabled();
        tooltipAlways(chain    ? "Each chain item has its own level (Chain section)"
                      : aSweep ? "The level sweep sets the amplitude (Sweep rows)"
                               : "Peak amplitude relative to full scale (0 dBFS)");

        row("Start phase");
        ImGui::InputDouble("##sgph", &p.phaseDeg, 15, 90, "%.1f deg");

        row("Mode");
        int mode = (int)p.mode;
        ImGui::RadioButton("CW", &mode, (int)GenMode::CW);
        ImGui::SameLine();
        ImGui::RadioButton("Pulsed", &mode, (int)GenMode::Pulsed);
        ImGui::SameLine();
        ImGui::RadioButton("Playback", &mode, (int)GenMode::Playback);
        tooltipAlways("Playback: a canned waveform from a SigMF or raw IQ recording, e.g. one this generator wrote");
        ImGui::SameLine();
        ImGui::RadioButton("Chain", &mode, (int)GenMode::Chain);
        tooltipAlways("Chain: several recordings played in turn, each with its own offset, level, plays and gap,\n"
                      "looped forever or a set number of times");
        p.mode = (GenMode)mode;
        ImGui::EndTable();
    }

    if (p.mode == GenMode::CW) {
        if (ImGui::CollapsingHeader("CW", ImGuiTreeNodeFlags_DefaultOpen) && beginForm("##sgcw", 8.5f)) {
            drawSweepRows(p);
            row("Drift");
            ImGui::BeginDisabled(p.fSweep != SweepShape::Off);
            InputSI("##sgdrift", &p.driftHzPerSec, "Hz/s", -FLT_MIN);
            ImGui::EndDisabled();
            tooltipAlways("Linear frequency drift, e.g. to test a tracking loop. 0 = steady tone.\n"
                          "Not used while the frequency sweeps.");
            ImGui::EndTable();
        }
    } else if (p.mode == GenMode::Playback) {
        drawSigGenPlayback();
    } else if (p.mode == GenMode::Chain) {
        drawSigGenChain();
    } else {
        if (ImGui::CollapsingHeader("Pulse", ImGuiTreeNodeFlags_DefaultOpen) && beginForm("##sgpulse", 8.5f)) {
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
        // Pulse-to-pulse frequency and level sweeps (frequency agility, scan modulation).
        if (ImGui::CollapsingHeader("Sweep", p.sweeping() ? ImGuiTreeNodeFlags_DefaultOpen : 0) && beginForm("##sgpsweep", 8.5f)) {
            drawSweepRows(p);
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Impairments") && beginForm("##sgimp", 8.5f)) {
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
// Playback
// ---------------------------------------------------------------------------
void App::setPlaybackFile(const std::string& path, bool fromViewer) {
    std::string pth = path;
    while (!pth.empty() && std::isspace((unsigned char)pth.back())) pth.pop_back();
    while (!pth.empty() && std::isspace((unsigned char)pth.front())) pth.erase(0, 1);
    if (pth.empty()) return;
    FileMetaGuess g = guessMeta(expandHome(pth));
    WaveformParams& p = sg_;
    p.playPath = g.dataPath;
    snprintf(sgPlayPath_, sizeof sgPlayPath_, "%s", p.playPath.c_str());
    p.playFp = {};
    if (g.haveFmt) {
        p.playFp.fmt = g.fmt;
        p.playFp.bigEndian = g.bigEndian;
    }
    if (fromViewer) {  // read it the way the viewer does, overrides included
        p.playFp = fp_;
        g.haveRate = g.haveFc = true;
        g.rate = fs_;
        g.fc = fc_;
        g.source = "the viewer";
    }
    p.playRate = g.haveRate ? g.rate : p.rate;
    p.playStart = p.playLen = 0;
    // Play it back as recorded: same rate, and at its own frequency. A running
    // output keeps its rate and tuning (the recording is resampled instead).
    const bool txOn = tx_ && tx_->running();
    const bool lockRate = txOn || sgFileSt_.active || (sigGenLoopback() && liveRecording());
    if (g.haveRate && !lockRate && p.rate != g.rate) {
        p.rate = g.rate;
        if (sigGenLoopback()) startLoopback();
    }
    if (g.haveFc && !txOn) {
        p.fc = g.fc;
        p.offset = 0;
    }
    forgetPlayback();  // Load always rereads the file
    sgPlayNote_ = g.source.empty() ? "no metadata found: set the format and rate below" : "settings from " + g.source;
    sgPlayNoteWarn_ = false;
    if (std::string err; !fromViewer && !metaReadable(g, err)) {
        sgPlayNote_ = err + ": set the format below";
        sgPlayNoteWarn_ = true;
        LOG_WARN("app", "playback: %s", err.c_str());
    }
}

void App::drawSigGenPlayback() {
    WaveformParams& p = sg_;
    if (!ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen) || !beginForm("##sgplay", 8.5f)) return;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float half = (ImGui::GetContentRegionAvail().x - st.ItemInnerSpacing.x) / 2;

    row("Recording", "A SigMF (.sigmf-meta / .sigmf-data) or raw IQ file. Enter or Load reads it and its metadata.");
    const float loadW = ImGui::CalcTextSize("Load").x + st.FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(loadW + st.ItemInnerSpacing.x));
    bool load = ImGui::InputText("##sgplaypath", sgPlayPath_, sizeof sgPlayPath_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    load |= ImGui::Button("Load##sgplay");
    if (load) setPlaybackFile(sgPlayPath_);

    // Shortcuts: the generator's last file, and the viewer's file and time markers.
    const bool viewerFile = file_ && !liveMode_;
    if (!sgLastFile_.empty() || viewerFile) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        if (!sgLastFile_.empty()) {
            if (ImGui::SmallButton("Last generated")) setPlaybackFile(sgLastFile_);
            tooltipAlways(sgLastFile_.c_str());
            ImGui::SameLine();
        }
        if (viewerFile) {
            if (ImGui::SmallButton("Viewer's file")) setPlaybackFile(file_->path(), true);
            tooltipAlways("Play the file open in the main window, read with the viewer's format and rate");
            if (p.playPath == file_->path()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Viewer's markers")) {
                    p.playStart = std::max(0.0, std::min(tm1_, tm2_));
                    p.playLen = std::fabs(tm2_ - tm1_);
                }
                tooltipAlways("Play only the part between the viewer's time markers (T)");
            }
        }
    }
    if (!sgPlayNote_.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        if (sgPlayNoteWarn_) ImGui::TextColored(kColWarn, "%s", sgPlayNote_.c_str());
        else ImGui::TextDisabled("%s", sgPlayNote_.c_str());
    }

    row("Format", "Sample format of the recording");
    if (ImGui::BeginCombo("##sgplayfmt", (std::string(formatName(p.playFp.fmt)) + (p.playFp.bigEndian ? " BE" : "")).c_str())) {
        for (int i = 0; i < (int)SampleFormat::Count; ++i)
            if (ImGui::Selectable(formatName((SampleFormat)i), (int)p.playFp.fmt == i)) p.playFp.fmt = (SampleFormat)i;
        ImGui::Separator();
        ImGui::Checkbox("Big-endian", &p.playFp.bigEndian);
        ImGui::Checkbox("Swap I/Q", &p.playFp.swapIQ);
        ImGui::EndCombo();
    }

    row("File rate", "The recording's sample rate. If it differs from the output rate, the recording is resampled.");
    double fr = p.playRate > 0 ? p.playRate : p.rate;
    if (InputSI("##sgplayrate", &fr, "Sps", -FLT_MIN) && fr > 0) p.playRate = fr;

    row("Segment", "Start and length of the part to play, in the recording's time. Length 0 = to the end.");
    InputSI("##sgplay0", &p.playStart, "s", half);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    InputSI("##sgplaylen", &p.playLen, "s", -FLT_MIN);

    row("Repeat", "Loop the recording, with this much silence between plays. Unticked: play it once.");
    ImGui::Checkbox("Loop##sgplayloop", &p.playLoop);
    ImGui::SameLine();
    ImGui::BeginDisabled(!p.playLoop);
    InputSI("##sgplaygap", &p.playGap, "s", -FLT_MIN);
    ImGui::EndDisabled();

    row("Start delay", "Silence before the first play");
    InputSI("##sgplaydelay", &p.startDelay, "s", -FLT_MIN);

    // What was loaded, from the preview worker.
    const auto r = sgPrev_;
    if (r && r->req.p == p && r->error.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s of %s, peak %.1f dBFS", fmtSI(r->playSeconds, "s", 4).c_str(),
                            fmtSI(r->fileSeconds, "s", 4).c_str(), r->playPeakDb);
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep its level")) p.ampDb = std::min(0.0, std::round(r->playPeakDb * 10) / 10);
        tooltipAlways("Amplitude scales the recording's peak. This sets it to the recorded peak, so it plays at its own level.");
        if (r->playOutPeakDb > 0) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kColWarn, "Resampling overshoot peaks at %+.1f dBFS: lower the amplitude to avoid clipping",
                               r->playOutPeakDb);
        }
    }
    ImGui::EndTable();
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
    if (!r->error.empty()) {
        ImGui::TextDisabled(stale ? "loading..." : "no preview: the recording could not be loaded");
        return;
    }
    static const char* kUnitS = "s";
    static const char* kUnitHz = "Hz";

    if (!r->sweepF.empty()) drawSweepPreview(*r);
    if (r->req.p.mode == GenMode::Chain) {
        drawChainPreview(*r);
    } else if (!r->t.empty() && ImPlot::BeginPlot("##sgtime", ImVec2(-1, fs * 9), ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)kUnitS);
        ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
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
    ImGui::TextDisabled("spectrum: %s bins, power per bin%s%s", fmtSI(r->rbw, "Hz", 3).c_str(),
                        r->hold ? ", max-hold over the sweep" : "", stale ? "  (updating)" : "");
}

// The sweep rows of the CW and Pulse sections (inside their form table).
void App::drawSweepRows(WaveformParams& p) {
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemInnerSpacing.x) / 2;
    auto shapeCombo = [](const char* id, SweepShape& v, bool stepped) {
        if (!ImGui::BeginCombo(id, sweepShapeName(v))) return;
        for (int i = 0; i < (int)SweepShape::Count; ++i)
            if ((stepped || (SweepShape)i != SweepShape::Stepped) && ImGui::Selectable(sweepShapeName((SweepShape)i), (int)v == i))
                v = (SweepShape)i;
        ImGui::EndCombo();
    };
    row("Freq sweep", "Ramp up: From to To, then again. Ramp down: To to From. Triangle: From to To and back.\n"
                      "Stepped: From, From + step, ... up to To, each for one dwell. Repeats until the output stops.\n"
                      "Pulsed: each pulse takes the frequency the sweep has when it starts.");
    shapeCombo("##sgfsweep", p.fSweep, true);
    if (p.fSweep != SweepShape::Off) {
        row("From / to", "Sweep limits as offsets from the center frequency, inside +-rate/2");
        InputSI("##sgffrom", &p.fFrom, "Hz", half);
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        InputSI("##sgfto", &p.fTo, "Hz", -FLT_MIN);
        if (p.fSweep == SweepShape::Stepped) {
            row("Step / dwell", "Frequency step, and how long each frequency is held");
            InputSI("##sgfstep", &p.fStep, "Hz", half);
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            InputSI("##sgfdwell", &p.fDwell, "s", -FLT_MIN);
        }
    }
    row("Level sweep", "Sweeps the amplitude in dB over the same period. Ramp up: From to To. Ramp down: To to From.\n"
                       "Triangle: From to To and back. Pulsed: each pulse takes the level the sweep has when it starts.");
    shapeCombo("##sgasweep", p.aSweep, false);
    if (p.aSweep != SweepShape::Off) {
        row("From / to", "Level limits, dBFS");
        ImGui::SetNextItemWidth(half);
        ImGui::InputDouble("##sgafrom", &p.aFrom, 0, 0, "%.1f dBFS");
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble("##sgato", &p.aTo, 0, 0, "%.1f dBFS");
    }
    if (p.sweeping()) {
        row("Period", "The time of one sweep, shared by the frequency and level sweeps");
        if (p.fSweep == SweepShape::Stepped) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s (%d steps x dwell)", fmtSI((double)p.sweepSamples() / p.rate, "s", 4).c_str(),
                                p.sweepSteps());
        } else {
            InputSI("##sgsper", &p.sweepPeriod, "s", -FLT_MIN);
        }
    }
}

// One period of a sweep: the frequency (left axis) and the level (right axis).
void App::drawSweepPreview(const SigPreviewRes& r) {
    static const char* kUnitS = "s";
    static const char* kUnitHz = "Hz";
    const WaveformParams& p = r.req.p;
    const bool fs = p.fSweep != SweepShape::Off, as = p.aSweep != SweepShape::Off;
    if (!ImPlot::BeginPlot("##sgsweep", ImVec2(-1, ImGui::GetFontSize() * 9), ImPlotFlags_NoMenus)) return;
    ImPlot::SetupAxes(nullptr, fs ? "Hz" : "dBFS", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)kUnitS);
    if (fs) ImPlot::SetupAxisFormat(ImAxis_Y1, formatValueSI, (void*)kUnitHz);
    if (fs && as) ImPlot::SetupAxis(ImAxis_Y2, "dBFS", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
    ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
    const int n = (int)r.sweepT.size();
    if (fs) {
        ImPlot::SetNextLineStyle(kColI, 1.5f);
        ImPlot::PlotLine("frequency", r.sweepT.data(), r.sweepF.data(), n);
    }
    if (as) {
        if (fs) ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        ImPlot::SetNextLineStyle(kColEnv, 1.5f);
        ImPlot::PlotLine("level", r.sweepT.data(), r.sweepDb.data(), n);
    }
    ImPlot::EndPlot();
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
void App::drawSigGenFile() {
    const bool active = sgFileSt_.active;
    FileGenConfig& c = sgFileCfg_;
    const bool counted = sg_.mode != GenMode::CW || sg_.sweeping();  // the length can be a count of events
    ImGui::BeginDisabled(active);
    if (beginForm("##sgfile", 8.5f)) {
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
        const bool playOnce = sg_.mode == GenMode::Playback && !sg_.playLoop;
        if (counted) {
            int by = sgByPulses_ ? 1 : 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            const char* bys[] = {"Duration", sg_.mode == GenMode::Pulsed ? "Pulses"
                                             : sg_.mode == GenMode::CW   ? "Sweeps"
                                             : sg_.mode == GenMode::Chain ? "Passes"
                                             : playOnce                  ? "One play"
                                                                         : "Repeats"};
            if (ImGui::Combo("##sgby", &by, bys, 2)) sgByPulses_ = by == 1;
            if (sg_.mode == GenMode::Playback)
                tooltipAlways("Repeats / One play: the file holds whole plays of the recording, gaps included");
            ImGui::SameLine();
        }
        const bool byPulses = sgByPulses_ && counted;
        if (byPulses && playOnce) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("start delay + the recording");
            if (c.pulses < 1) c.pulses = 1;
        } else if (byPulses) {
            long long n = std::max<long long>(1, c.pulses);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputScalar("##sgnp", ImGuiDataType_S64, &n)) c.pulses = std::max<long long>(1, n);
            if (c.pulses < 1) c.pulses = 100;
        } else {
            InputSI("##sglen", &c.seconds, "s", -FLT_MIN);
        }
        if (counted && c.rec.sigmf) {
            row("Annotations");
            ImGui::Checkbox(("Mark each " + std::string(sg_.mode == GenMode::Chain ? "item" : eventName(sg_)) + "##sgann").c_str(),
                            &c.annotatePulses);
            tooltipAlways("Adds a SigMF annotation (start and length) for every pulse, sweep, play of the recording\n"
                          "or chain item, up to 10000.");
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    const bool byPulses = sgByPulses_ && counted;
    double secs = c.seconds;
    const double pass = sgPrev_ && sgPrev_->req.p.chain == sg_.chain ? sgPrev_->passSeconds : 0;
    if (sg_.mode == GenMode::Chain) {
        const int64_t passes = byPulses ? std::max<int64_t>(1, c.pulses) : 0;
        const int64_t limit = sg_.chainLoops > 0 ? (passes ? std::min<int64_t>(passes, sg_.chainLoops) : sg_.chainLoops) : passes;
        secs = limit ? sg_.startDelay + (double)limit * pass : c.seconds;
        if (!byPulses) secs = std::min(secs, c.seconds);
    } else if (byPulses && sg_.mode == GenMode::CW) {
        secs = (double)std::max<int64_t>(1, c.pulses) * (double)sg_.sweepSamples() / sg_.rate;
    } else if (byPulses && sg_.mode == GenMode::Pulsed) {
        secs = (double)std::max<int64_t>(1, c.pulses) * sg_.meanPri() + sg_.startDelay;
    } else if (byPulses) {
        const double play = sgPrev_ && sgPrev_->req.p == sg_ ? sgPrev_->playSeconds : 0;
        secs = sg_.startDelay + (sg_.playLoop ? (double)std::max<int64_t>(1, c.pulses) * (play + sg_.playGap) : play);
    }
    const double bytes = secs * sg_.rate * formatBytesPerSample(c.fmt);
    ImGui::TextDisabled("about %s, %s", fmtSI(secs, "s", 4).c_str(), fmtSI(bytes, "B", 3).c_str());

    if (!active) {
        ImGui::BeginDisabled(!sgReady_);
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
        if (sg_.mode == GenMode::Chain) {
            const int n = sigGenBus().chainPasses();
            if (sigGenBus().chainFinished())
                ImGui::TextDisabled("The chain finished after %d %s: the loopback is silent", n, n == 1 ? "pass" : "passes");
            else if (n > 0)
                ImGui::TextDisabled("Pass %d%s", n, sg_.chainLoops > 0 ? (" of " + std::to_string(sg_.chainLoops)).c_str() : "");
        }
    } else {
        ImGui::BeginDisabled(!sgReady_ || liveRecording());
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

    if (beginForm("##sgtx", 8.5f)) {
        ImGui::BeginDisabled(txOn);
        row("Device args");
        ImGui::InputText("##txargs", txArgs_, sizeof txArgs_);
        tooltipAlways("UHD device arguments, e.g. serial=31A2B3C (USB) or addr=192.168.10.2 (network).\n"
                      "Empty: the first USRP UHD finds.\n"
                      "If the live view is receiving from the same USRP, both share it (full duplex).");
        row("Antenna");
        ImGui::InputTextWithHint("##txant", "default", txAntenna_, sizeof txAntenna_);
        tooltipAlways("The transmit port. Empty: the channel's first transmit port\n"
                      "(TX/RX on the B2xx and N3xx, TX/RX0 on the X410).");
        row("Channel");
        if (ImGui::InputInt("##txchan", &txCfg_.channel)) txCfg_.channel = std::clamp(txCfg_.channel, 0, 15);
        tooltipAlways("Transmit channel: the X310, N310 and X410 have several.");
        row("Reference");
        if (ImGui::BeginCombo("##txref", txCfg_.ref.empty() ? "Default" : txCfg_.ref.c_str())) {
            if (ImGui::Selectable("Default", txCfg_.ref.empty())) txCfg_.ref.clear();
            for (const char* r : {"internal", "external", "gpsdo"})
                if (ImGui::Selectable(r, txCfg_.ref == r)) txCfg_.ref = r;
            ImGui::EndCombo();
        }
        tooltipAlways("Clock and time reference: internal, external (10 MHz and PPS) or gpsdo.\n"
                      "It applies to the whole USRP, so the live view on the same USRP uses it too.");
        row("Bandwidth");
        InputSI("##txbw", &txCfg_.bandwidth, "Hz", -FLT_MIN);
        tooltipAlways("Analog filter bandwidth. 0 = automatic (follows the sample rate).");
        ImGui::EndDisabled();

        row("TX gain");
        const TxLimits& lim = txCfg_.limits;
        const GainRange g = txOn ? txStats_.range : GainRange{0, 89.75, 0.25, {}, false};
        const double gMax = std::isnan(lim.maxGain) ? g.max : std::clamp(lim.maxGain, g.min, g.max);
        if (!txOn && txCfg_.gain > gMax) txCfg_.gain = gMax;
        float gain = (float)txCfg_.gain;
        if (ImGui::SliderFloat("##txgain", &gain, (float)g.min, (float)gMax, "%.2f dB",
                               ImGuiSliderFlags_AlwaysClamp)) {
            txCfg_.gain = gain;
            std::string err;
            if (txOn && !tx_->setGain(gain, err)) {
                status_ = "Gain failed: " + err;
                LOG_ERROR("app", "transmit gain failed: %s", err.c_str());
            }
        }
        tooltipAlways(txOn ? "Transmit gain of this USRP. Start low."
                           : "Transmit gain (B200/B205: 0-89.75 dB; the slider shows the device's range while it\n"
                             "transmits). Start low: a B2xx gives about 0 dBm near the top of the range.");
        if (lim.any()) {
            row("Limits");
            ImGui::TextWrapped("%s", describeTxLimits(lim).c_str());
            tooltipAlways("Set with --tx-max-gain and --tx-allow, or WVT_TX_MAX_GAIN and WVT_TX_ALLOW.\n"
                          "wvt refuses a gain above the maximum, and a frequency where the band\n"
                          "center +- rate/2 is not inside an allowed band.");
        }
        ImGui::EndTable();
    }

    // The B2xx derives RX and TX rates from one clock: they must match while both run.
    if (!txOn && live_ && live_->running() && devCfg_.driver == "uhd" && devCaps_.usb &&
        std::fabs(liveStats_.rate - sg_.rate) > 1) {
        ImGui::TextColored(kColWarn, "Receiving at %s: transmit must use the same rate.", fmtSI(liveStats_.rate, "S/s", 6).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Use it")) sg_.rate = liveStats_.rate;
    }

    if (!txOn) {
        if (ImGui::Checkbox("Arm transmitter", &txArmed_) && txArmed_) txErr_.clear();
        tooltipAlways("Safety catch: the transmit button works only while armed. Disarms after every transmission.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!txArmed_ || !sgReady_);
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
        ImGui::Text("%s, %s, gain %.2f dB, antenna %s", s.description.c_str(), fmtSI(s.rate, "S/s", 6).c_str(), s.gain,
                    s.antenna.c_str());
        ImGui::Text("sent %s   underflows %lld   lost packets %lld", fmtSI((double)s.tx.sent, "S", 4).c_str(),
                    (long long)s.tx.underflows, (long long)s.tx.seqErrors);
        if (!s.notes.empty()) ImGui::TextColored(kColWarn, "%s", s.notes.c_str());
        if (sg_.mode == GenMode::Chain && s.chainPasses > 0)
            ImGui::Text("Pass %d%s", s.chainPasses, sg_.chainLoops > 0 ? (" of " + std::to_string(sg_.chainLoops)).c_str() : "");
        if (!sgReady_) ImGui::TextColored(kColWarn, "Still transmitting the last valid settings.");
    }

    if (ImGui::BeginPopupModal("Start transmitting?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(kColTx, "Transmit at %s", fmtSI(sg_.fc + sg_.offset, "Hz", 9).c_str());
        ImGui::Text("Center %s, %s, TX gain %.2f dB, antenna %s, channel %d", fmtSI(sg_.fc, "Hz", 9).c_str(),
                    fmtSI(sg_.rate, "S/s", 6).c_str(), txCfg_.gain, txAntenna_[0] ? txAntenna_ : "default",
                    txCfg_.channel);
        ImGui::Text("Device: %s", txArgs_[0] ? txArgs_ : "the first USRP found");
        std::string limErr;
        const bool inLimits = txGainAllowed(txCfg_.limits, txCfg_.gain, limErr) &&
                              txFreqAllowed(txCfg_.limits, sg_.fc, sg_.rate, limErr);
        if (txCfg_.limits.any()) ImGui::Text("Limits: %s", describeTxLimits(txCfg_.limits).c_str());
        ImGui::PushTextWrapPos(fs * 30);
        ImGui::TextDisabled("%s", describeParams(sg_).c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Only transmit into a dummy load or an attenuated cable, or on a frequency and power "
                               "you are licensed for. Never connect a transmit port straight to a receiver input "
                               "without at least 30 dB of attenuation.");
        if (!inLimits) {
            ImGui::Spacing();
            ImGui::TextColored(kColErr, "Not allowed: %s", limErr.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::BeginDisabled(!inLimits);
        const bool go = redButton("Transmit");
        ImGui::EndDisabled();
        if (go) {
            startTx();
            txArmed_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
