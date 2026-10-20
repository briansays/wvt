// Live device mode: device/recording toolbar, live spectrum and scrolling
// waterfall, status bar.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "app.h"
#include "log.h"
#include "platform_x11.h"

namespace {
const int kFftSizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

const ImVec4 kColHover(1.f, 1.f, 1.f, 0.45f);
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColRec(1.f, 0.32f, 0.32f, 1.f);

float fieldW(float chars) { return ImGui::GetFontSize() * chars * 0.55f + ImGui::GetStyle().FramePadding.x * 2; }
float stepW(float chars) { return fieldW(chars) + 2 * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x); }

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Gain limits to show before a device is open.
GainRange defaultGain(const std::string& driver) {
    if (driver == "rtlsdr") return {0, 49.6, 0.1, {}, true};
    if (driver == "uhd") return {0, 76, 1, {}, true};
    return {0, 60, 1, {}, true};
}

const char* driverName(const std::string& d) {
    if (d == "rtlsdr") return "RTL-SDR";
    if (d == "uhd") return "USRP";
    if (d == "siggen") return "Signal generator";
    return "Simulator";
}

// Tooltip that also shows over a disabled item.
void tooltipAlways(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}
}  // namespace

// ---------------------------------------------------------------------------
// Mode switching & device control
// ---------------------------------------------------------------------------
bool App::liveRecording() const { return live_ && live_->recorder().active(); }

void App::drawSourceSelector() {
    label("Source", fieldW(8));
    int mode = liveMode_ ? 1 : 0;
    const char* names[] = {"File", "Device"};
    ImGui::BeginDisabled(liveRecording());
    if (ImGui::Combo("##source", &mode, names, 2)) {
        if (mode == 1 && !liveMode_) enterLive();
        else if (mode == 0 && liveMode_) leaveLive();
    }
    ImGui::EndDisabled();
    tooltipAlways("File: analyze a recording.\nDevice: stream live from an RTL-SDR or USRP B200/B205.");
}

void App::enterLive() {
    stopAudio();
    fileFs_ = fs_;
    fileFc_ = fc_;
    liveMode_ = true;
    liveViewFc_ = liveViewFs_ = NAN;
    if (!devicesScanned_) scanDevices();
    status_ = "Pick a device, set the frequency and sample rate, then press Start";
    win_.setTitle("wvt - live");
}

void App::leaveLive() {
    stopAudio();
    if (live_) live_->stop();
    liveMode_ = false;
    fs_ = fileFs_;
    fc_ = fileFc_;
    onSourceChanged(true);
    status_ = file_ ? "Back to " + file_->path() : "No file loaded - type a path in the File box and press Enter";
}

void App::scanDevices() {
    const bool first = !devicesScanned_;
    devices_ = enumerateDevices();
    devicesScanned_ = true;
    bool found = false;
    for (const auto& d : devices_) found |= d.driver == devCfg_.driver && d.args == devCfg_.args;
    // The first scan picks the first real device (the simulator is listed last).
    if ((first || !found) && !devices_.empty()) selectDevice(devices_.front());
}

void App::selectDevice(const DeviceInfo& d) {
    const bool driverChanged = d.driver != devCfg_.driver;
    devCfg_.driver = d.driver;
    devCfg_.args = d.args;
    snprintf(devArgs_, sizeof devArgs_, "%s", d.args.c_str());
    if (driverChanged) {
        std::string err, warn;
        if (!checkSampleRate(d.driver, devCfg_.rate, err, warn)) devCfg_.rate = d.driver == "uhd" ? 10e6 : 2.4e6;
        GainRange g = defaultGain(d.driver);
        devCfg_.gain = std::clamp(devCfg_.gain, g.min, g.max);
        if (d.driver == "uhd" && !antenna_[0]) snprintf(antenna_, sizeof antenna_, "RX2");
    }
}

bool App::restartDevice() {
    if (!live_) live_ = std::make_unique<LiveEngine>([this] { win_.wake(); });
    devCfg_.args = devArgs_;
    devCfg_.antenna = antenna_;
    std::string err;
    if (!live_->start(devCfg_, err)) {
        liveErr_ = err;
        status_ = "Device error: " + err;
        LOG_ERROR("app", "device error: %s", err.c_str());
        return false;
    }
    liveErr_.clear();
    liveStats_ = live_->stats();
    status_ = "Streaming from " + liveStats_.description;
    win_.setTitle("wvt - " + liveStats_.description);
    return true;
}

bool App::startDevice(const StreamConfig& cfg) {
    if (!liveMode_) enterLive();
    devCfg_ = cfg;
    snprintf(devArgs_, sizeof devArgs_, "%s", cfg.args.c_str());
    snprintf(antenna_, sizeof antenna_, "%s", cfg.antenna.c_str());
    return restartDevice();
}

bool App::startRecording(const RecordConfig& cfg) {
    recCfg_ = cfg;
    snprintf(recPrefix_, sizeof recPrefix_, "%s", cfg.prefix.c_str());
    std::string err;
    if (!live_ || !live_->startRecording(cfg, err)) {
        recErr_ = live_ ? err : "start a device before recording";
        status_ = "Recording failed: " + recErr_;
        LOG_ERROR("app", "recording failed: %s", recErr_.c_str());
        return false;
    }
    recErr_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
void App::drawDeviceToolbar() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const bool running = live_ && live_->running();
    const bool rec = liveRecording();
    const bool recRaw = rec && !live_->recorder().sigmf();
    std::string err;

    // --- device ---
    std::string cur = std::string(driverName(devCfg_.driver)) + (devCfg_.args.empty() ? "" : " " + devCfg_.args);
    for (const auto& d : devices_)
        if (d.driver == devCfg_.driver && d.args == devCfg_.args) cur = d.label;
    label("Device", fieldW(30));
    ImGui::BeginDisabled(rec);
    if (ImGui::BeginCombo("##dev", cur.c_str())) {
        for (const auto& d : devices_)
            if (ImGui::Selectable(d.label.c_str(), d.driver == devCfg_.driver && d.args == devCfg_.args)) {
                selectDevice(d);
                if (running) restartDevice();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    if (ImGui::Button("Rescan")) scanDevices();
    tooltipAlways("Look for attached devices again");
    if (devCfg_.driver == "uhd") {
        label("Args", fieldW(22));
        if (ImGui::InputText("##args", devArgs_, sizeof devArgs_, ImGuiInputTextFlags_EnterReturnsTrue) && running)
            restartDevice();
        tooltipAlways("UHD device arguments, e.g. type=b200 or serial=31A2B3C (Enter applies)");
    }
    ImGui::EndDisabled();

    // --- tuning ---
    label("Center freq", fieldW(15));
    double fc = running ? liveStats_.fc : devCfg_.fc;
    ImGui::BeginDisabled(recRaw);
    if (InputSI("##lfc", &fc, "Hz", fieldW(15))) {
        devCfg_.fc = fc;
        if (running && !live_->setCenterFreq(fc, err)) {
            status_ = "Tune failed: " + err;
            LOG_ERROR("app", "tune failed: %s", err.c_str());
        }
    }
    ImGui::EndDisabled();
    tooltipAlways(recRaw ? "A raw recording can't store a frequency change.\nStop recording, or record SigMF instead."
                         : "Accepts SI suffixes, e.g. 915M, 100.1M, 7.074M");
    label("Sample rate", fieldW(13));
    double rate = running ? liveStats_.rate : devCfg_.rate;
    ImGui::BeginDisabled(rec);
    if (InputSI("##lfs", &rate, "Sps", fieldW(13))) {
        std::string warn;
        if (!checkSampleRate(devCfg_.driver, rate, err, warn)) {
            status_ = err;
            LOG_WARN("app", "%s", err.c_str());
        } else {
            devCfg_.rate = rate;
            if (!warn.empty()) {
                status_ = warn;
                LOG_WARN("app", "%s", warn.c_str());
            }
            if (running) restartDevice();
        }
    }
    ImGui::EndDisabled();
    tooltipAlways(rec ? "A recording has a single sample rate. Stop recording to change it."
                      : devCfg_.driver == "rtlsdr" ? "RTL-SDR: 225-300 kS/s or 0.9-3.2 MS/s (2.4 MS/s or less is reliable)"
                                                   : "USRP B200/B205: up to 56 MS/s on USB 3");

    // --- gain ---
    const GainRange g = running ? live_->gainRange() : defaultGain(devCfg_.driver);
    label("Gain", fieldW(12));
    float gain = (float)devCfg_.gain;
    ImGui::BeginDisabled(devCfg_.agc);
    if (ImGui::SliderFloat("##gain", &gain, (float)g.min, (float)g.max, "%.1f dB")) {
        double v = gain;
        if (!g.steps.empty()) {  // snap to the tuner's discrete gains
            double best = g.steps.front();
            for (double s : g.steps)
                if (std::fabs(s - v) < std::fabs(best - v)) best = s;
            v = best;
        }
        devCfg_.gain = v;
        if (running && !live_->setGain(devCfg_.agc, devCfg_.gain, err)) {
            status_ = "Gain failed: " + err;
            LOG_ERROR("app", "gain failed: %s", err.c_str());
        }
    }
    ImGui::EndDisabled();
    if (g.agc) {
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        if (ImGui::Checkbox("AGC", &devCfg_.agc) && running && !live_->setGain(devCfg_.agc, devCfg_.gain, err)) {
            status_ = "Gain failed: " + err;
            LOG_ERROR("app", "gain failed: %s", err.c_str());
        }
    }

    // --- driver-specific (these restart the stream) ---
    bool restart = false;
    ImGui::BeginDisabled(rec);
    if (devCfg_.driver == "rtlsdr") {
        flow(ImGui::GetFrameHeight() * 3.5f);
        restart |= ImGui::Checkbox("Bias-T", &devCfg_.biasTee);
        tooltipAlways("Sends 4.5 V up the antenna cable to power an LNA or active antenna.\nOnly enable it if your antenna side expects DC.");
        label("Direct", fieldW(4));
        const char* ds[] = {"Off", "I", "Q"};
        restart |= ImGui::Combo("##ds", &devCfg_.directSampling, ds, 3);
        tooltipAlways("Direct sampling for HF below 24 MHz. The RTL-SDR v3 uses the Q branch.");
        label("PPM", stepW(4));
        if (ImGui::InputInt("##ppm", &devCfg_.ppm)) {
            devCfg_.ppm = std::clamp(devCfg_.ppm, -200, 200);
            restart = true;
        }
        tooltipAlways("Frequency correction of the crystal (the RTL-SDR v3 TCXO is usually within 1-2 ppm)");
    } else if (devCfg_.driver == "uhd") {
        label("Antenna", fieldW(7));
        if (ImGui::BeginCombo("##ant", antenna_[0] ? antenna_ : "RX2")) {
            for (const char* a : {"RX2", "TX/RX"})
                if (ImGui::Selectable(a, strcmp(a, antenna_) == 0)) {
                    snprintf(antenna_, sizeof antenna_, "%s", a);
                    restart = true;
                }
            ImGui::EndCombo();
        }
        label("Bandwidth", fieldW(12));
        if (InputSI("##bw", &devCfg_.bandwidth, "Hz", fieldW(12))) restart = true;
        tooltipAlways("Analog filter bandwidth, 200 kHz - 56 MHz. 0 = automatic (follows the sample rate).");
    }
    ImGui::EndDisabled();
    if (restart && running) restartDevice();

    // --- start / stop ---
    flow(ImGui::GetFrameHeight() * 3);
    if (!running) {
        if (ImGui::Button("Start")) restartDevice();
    } else if (ImGui::Button("Stop")) {
        live_->stop();
        status_ = "Stopped";
    }
    if (running && !liveStats_.notes.empty()) {
        flow(ImGui::CalcTextSize(liveStats_.notes.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColWarn, "%s", liveStats_.notes.c_str());
    } else if (!running && !liveErr_.empty()) {
        flow(ImGui::CalcTextSize(liveErr_.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColErr, "%s", liveErr_.c_str());
    }

    // --- row 2: recording ---
    ImGui::Spacing();
    flowBegin();
    drawRecordBar();
}

void App::drawRecordBar() {
    const bool running = live_ && live_->running();
    const bool rec = liveRecording();

    ImGui::BeginDisabled(rec);
    label("Record to", fieldW(34));
    ImGui::InputText("##recprefix", recPrefix_, sizeof recPrefix_);
    tooltipAlways("Path prefix for recordings. Date, time, frequency and rate are appended,\ne.g. ~/wvt-recordings/wvt_20261016_142233_915.000MHz_2.4MSps.sigmf-data.\nMissing folders are created.");
    label("Format", fieldW(7));
    int fmt = recCfg_.sigmf ? 0 : 1;
    const char* fmts[] = {"SigMF", "Raw"};
    if (ImGui::Combo("##recfmt", &fmt, fmts, 2)) recCfg_.sigmf = fmt == 0;
    tooltipAlways("SigMF: .sigmf-data + .sigmf-meta with rate, frequency, retunes and dropped-sample notes.\nRaw: bare samples; rate and frequency are in the file name.");
    label("Samples", fieldW(12));
    std::string native = "native";
    if (running)
        native += liveStats_.fmt == SampleFormat::CU8 ? " (cu8)" : liveStats_.fmt == SampleFormat::CF32 ? " (cf32)" : " (ci16)";
    int smp = recCfg_.cf32 ? 1 : 0;
    const char* smps[] = {native.c_str(), "cf32"};
    if (ImGui::Combo("##recsmp", &smp, smps, 2)) recCfg_.cf32 = smp == 1;
    tooltipAlways("native: exactly what the device sends (smallest, lossless).\ncf32: complex float32, 2-4x larger, for tools that only read float.");
    label("Limit", fieldW(9));
    InputSI("##reclim", &recCfg_.maxSeconds, "s", fieldW(9));
    tooltipAlways("Stop recording after this long. 0 = no limit.");
    ImGui::EndDisabled();

    flow(ImGui::GetFrameHeight() * 5);
    if (!rec) {
        ImGui::BeginDisabled(!running);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.f));
        if (ImGui::Button("Record")) {
            RecordConfig c = recCfg_;
            c.prefix = recPrefix_;
            startRecording(c);
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        tooltipAlways(running ? "Start recording the stream" : "Start a device first");
    } else if (ImGui::Button("Stop recording")) {
        live_->stopRecording();
    }

    const Recorder::Status rs = live_ ? live_->recorder().status() : Recorder::Status{};
    std::string text;
    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (rs.active) {
        char b[160];
        snprintf(b, sizeof b, "REC %s  %s  buffer %.0f%%", fmtSI(rs.seconds, "s", 4).c_str(),
                 fmtSI((double)rs.bytes, "B", 4).c_str(), rs.ringFill * 100);
        text = b;
        if (rs.gaps > 0) text += "  gaps " + std::to_string(rs.gaps);
        col = kColRec;
    } else if (!recErr_.empty()) {
        text = recErr_;
        col = kColErr;
    } else if (!rs.dataPath.empty()) {
        std::string name = rs.dataPath.substr(rs.dataPath.find_last_of('/') + 1);
        text = "Saved " + name + " (" + fmtSI(rs.seconds, "s", 4) + ", " + fmtSI((double)rs.bytes, "B", 4) +
               (rs.gaps ? ", " + std::to_string(rs.gaps) + " gaps" : "") + ")";
        if (rs.failed) {
            text = "Recording stopped: " + rs.stopReason + " - " + name;
            col = kColErr;
        }
        lastRecording_ = rs.metaPath.empty() ? rs.dataPath : rs.metaPath;
    }
    if (!text.empty()) {
        flow(ImGui::CalcTextSize(text.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col, "%s", text.c_str());
    }
    if (!rs.active && !lastRecording_.empty()) {
        flow(ImGui::CalcTextSize("Open recording").x + ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::Button("Open recording")) {
            std::string path = lastRecording_;
            leaveLive();
            openFile(path);
        }
        tooltipAlways("Stop the device and open the last recording in file mode");
    }
}

// ---------------------------------------------------------------------------
// Live data
// ---------------------------------------------------------------------------
void App::resetLiveHistory(uint64_t gen, int cols) {
    liveGen_ = gen;
    liveCols_ = cols;
    liveRows_ = liveHistory_;
    liveFilled_ = liveHead_ = 0;
    liveDb_.assign((size_t)liveRows_ * cols, -200.f);
    liveRgba_.assign((size_t)liveRows_ * cols, 0);
    liveSpecF_.clear();
    liveAvgLin_.clear();
    livePeakLin_.clear();
    liveAvgDb_.clear();
    livePeakDb_.clear();
    liveFitY_ = true;
    liveTexDirty_ = cols > 0;
}

void App::recolorLiveRow(int row) {
    const float lo = dbMin_, scale = 255.f / std::max(1e-3f, dbMax_ - dbMin_);
    const float* src = &liveDb_[(size_t)row * liveCols_];
    uint32_t* dst = &liveRgba_[(size_t)row * liveCols_];
    for (int c = 0; c < liveCols_; ++c) dst[c] = lut_[std::clamp((int)((src[c] - lo) * scale), 0, 255)];
}

void App::recolorLiveAll() {
    liveDbMin_ = dbMin_;
    liveDbMax_ = dbMax_;
    liveLutCmap_ = cmap_;
    const int rows = liveFilled_ < liveRows_ ? liveFilled_ : liveRows_;
    for (int r = 0; r < rows; ++r) recolorLiveRow(r);
    liveTexDirty_ = true;
}

void App::pollLive() {
    if (!live_ || !live_->running()) return;
    liveStats_ = live_->stats();
    // A recording that ends by itself (limit, disk full) has no UI event; catch its edge here.
    const Recorder::Status rs = live_->recorder().status();
    if (recWasActive_ && !rs.active && rs.failed) LOG_ERROR("app", "recording stopped: %s", rs.stopReason.c_str());
    recWasActive_ = rs.active;
    if (!liveStats_.error.empty()) {
        liveErr_ = liveStats_.error;
        status_ = "Device stopped: " + liveErr_;
        LOG_ERROR("app", "device stopped: %s", liveErr_.c_str());
        live_->stop();
        return;
    }
    fs_ = liveStats_.rate;
    fc_ = liveStats_.fc;
    if (fc_ != liveViewFc_ || fs_ != liveViewFs_) {
        fmin_ = fc_ - fs_ / 2;
        fmax_ = fc_ + fs_ / 2;
        fm1_ = fc_ - fs_ / 8;
        fm2_ = fc_ + fs_ / 8;
        liveViewFc_ = fc_;
        liveViewFs_ = fs_;
    }
    live_->setParams(liveParams_);

    LiveRows r;
    live_->takeRows(r);
    if (r.gen != liveGen_ || liveRows_ != liveHistory_ || (r.n > 0 && r.cols != liveCols_))
        resetLiveHistory(r.gen, r.n > 0 ? r.cols : 0);
    if (r.n > 0) {
        const int cols = liveCols_;
        liveF0_ = r.f0;
        liveF1_ = r.f1;
        const double a = 1.0 / std::max(1, specAvgRows_);
        for (int i = 0; i < r.n; ++i) {
            const float* lin = &r.lin[(size_t)i * cols];
            std::copy_n(&r.db[(size_t)i * cols], cols, &liveDb_[(size_t)liveHead_ * cols]);
            recolorLiveRow(liveHead_);
            if ((int)liveAvgLin_.size() != cols) {
                liveAvgLin_.assign(lin, lin + cols);
                livePeakLin_ = liveAvgLin_;
                liveFitSpecY_ = true;
            } else {
                for (int c = 0; c < cols; ++c) {
                    liveAvgLin_[c] += a * (lin[c] - liveAvgLin_[c]);
                    livePeakLin_[c] = std::max(livePeakLin_[c], (double)lin[c]);
                }
            }
            liveHead_ = (liveHead_ + 1) % liveRows_;
            liveFilled_ = std::min(liveFilled_ + 1, liveRows_);
        }
        liveSpecF_.resize(cols);
        liveAvgDb_.resize(cols);
        livePeakDb_.resize(cols);
        const double colHz = (liveF1_ - liveF0_) / cols;
        for (int c = 0; c < cols; ++c) {
            liveSpecF_[c] = liveF0_ + (c + 0.5) * colHz;
            liveAvgDb_[c] = 10.0 * std::log10(liveAvgLin_[c] + 1e-20);
            livePeakDb_[c] = 10.0 * std::log10(livePeakLin_[c] + 1e-20);
        }
        liveTexDirty_ = true;
    }

    // Auto-range the colors to the history twice a second.
    const double t = nowSec();
    if (liveAutoDb_ && liveFilled_ > 0 && t - liveAutoAt_ > 0.5) {
        liveAutoAt_ = t;
        const size_t total = (size_t)liveFilled_ * liveCols_, stride = std::max<size_t>(1, total / 100000);
        std::vector<float> s;
        s.reserve(total / stride + 1);
        for (size_t i = 0; i < total; i += stride) s.push_back(liveDb_[i]);
        auto pct = [&](double p) {
            size_t k = std::min(s.size() - 1, (size_t)(p * (s.size() - 1)));
            std::nth_element(s.begin(), s.begin() + k, s.end());
            return s[k];
        };
        const float lo = std::floor(pct(0.05) - 3), hi = std::ceil(std::max(pct(0.999) + 3, lo + 20.f));
        // Small changes would make the whole waterfall flicker.
        if (std::fabs(lo - dbMin_) >= 2 || std::fabs(hi - dbMax_) >= 2) {
            dbMin_ = lo;
            dbMax_ = hi;
        }
    }
    if (dbMin_ != liveDbMin_ || dbMax_ != liveDbMax_ || liveLutCmap_ != cmap_) recolorLiveAll();
    if (liveTexDirty_ && liveCols_ > 0) {
        win_.updateTexture(liveTex_, liveCols_, liveRows_, liveRgba_.data());
        liveTexDirty_ = false;
    }
}

float App::liveValueAt(double ago, double f) const {
    const double dt = liveStats_.rowPeriod > 0 ? liveStats_.rowPeriod : 1.0 / liveParams_.rowsPerSec;
    if (liveFilled_ == 0 || !std::isfinite(ago) || !std::isfinite(f)) return NAN;
    const int a = (int)std::floor(ago / dt);
    const int c = (int)((f - liveF0_) / (liveF1_ - liveF0_) * liveCols_);
    if (a < 0 || a >= liveFilled_ || c < 0 || c >= liveCols_) return NAN;
    const int row = ((liveHead_ - 1 - a) % liveRows_ + liveRows_) % liveRows_;
    return liveDb_[(size_t)row * liveCols_ + c];
}

// ---------------------------------------------------------------------------
// Spectrum + scrolling waterfall
// ---------------------------------------------------------------------------
void App::drawLiveLeft() {
    flowBegin();
    label("FFT", fieldW(7));
    char nf[16];
    snprintf(nf, sizeof nf, "%d", liveParams_.nfft);
    if (ImGui::BeginCombo("##lnfft", nf)) {
        for (int n : kFftSizes) {
            snprintf(nf, sizeof nf, "%d", n);
            if (ImGui::Selectable(nf, n == liveParams_.nfft)) liveParams_.nfft = n;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("FFT size: frequency resolution = Fs / N = %s", fmtSI(fs_ / liveParams_.nfft, "Hz", 4).c_str());
    label("Window", fieldW(14));
    if (ImGui::BeginCombo("##lwin", windowName((WindowType)liveParams_.window))) {
        for (int i = 0; i < (int)WindowType::Count; ++i)
            if (ImGui::Selectable(windowName((WindowType)i), i == liveParams_.window)) liveParams_.window = i;
        ImGui::EndCombo();
    }
    label("Avg", fieldW(8));
    ImGui::SliderInt("##lavg", &liveParams_.avg, 0, 64, liveParams_.avg == 0 ? "auto" : "%d");
    ImGui::SetItemTooltip("FFTs averaged into each waterfall row.\nauto: all samples since the previous row (up to 20 MS/s,\nor 100 MS/s on the GPU), so nothing is missed at RTL-SDR rates. Lower values show short bursts more sharply.");
    label("Rows/s", stepW(4));
    if (ImGui::InputDouble("##lrps", &liveParams_.rowsPerSec, 5, 10, "%.0f"))
        liveParams_.rowsPerSec = std::clamp(liveParams_.rowsPerSec, 1.0, 100.0);
    ImGui::SetItemTooltip("Waterfall scroll speed (rows per second)");
    label("History", stepW(5));
    if (ImGui::InputInt("##lhist", &liveHistory_, 100, 500)) liveHistory_ = std::clamp(liveHistory_, 64, 4096);
    ImGui::SetItemTooltip("Waterfall rows kept");
    label("Cols", stepW(5));
    if (ImGui::InputInt("##lcols", &liveParams_.maxCols, 256, 1024))
        liveParams_.maxCols = std::clamp(liveParams_.maxCols, 64, 8192);
    ImGui::SetItemTooltip("Maximum waterfall columns; FFT bins beyond this are max-pooled");
    drawGpuCheckbox();
    label("Spec avg", stepW(4));
    if (ImGui::InputInt("##lsavg", &specAvgRows_)) specAvgRows_ = std::clamp(specAvgRows_, 1, 200);
    ImGui::SetItemTooltip("Spectrum averaging time constant, in rows");
    label("dB min", stepW(5));
    if (ImGui::InputFloat("##ldbmin", &dbMin_, 1, 10, "%.0f")) liveAutoDb_ = false;
    label("dB max", stepW(5));
    if (ImGui::InputFloat("##ldbmax", &dbMax_, 1, 10, "%.0f")) liveAutoDb_ = false;
    flow(ImGui::GetFrameHeight() * 3);
    ImGui::Checkbox("Auto", &liveAutoDb_);
    ImGui::SetItemTooltip("Auto-range the color scale to the waterfall history");
    label("Colormap", fieldW(10));
    if (ImGui::BeginCombo("##lcmap", ImPlot::GetColormapName(cmap_))) {
        for (int i = 0; i < ImPlot::GetColormapCount(); ++i)
            if (ImGui::Selectable(ImPlot::GetColormapName(i), i == cmap_)) cmap_ = i;
        ImGui::EndCombo();
    }
    flow(ImGui::GetFrameHeight() * 9);
    ImGui::Checkbox("Peak", &showPeak_);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::SmallButton("reset")) livePeakLin_ = liveAvgLin_;
    ImGui::SetItemTooltip("Restart the peak hold");
    ImGui::SameLine();
    ImGui::Checkbox("Freq markers", &fMarkers_);
    ImGui::SetItemTooltip("Two draggable frequency cursors (F)");
    ImGui::SameLine();
    ImGui::Checkbox("Rel. freq", &relFreq_);
    ImGui::SetItemTooltip("Label frequency axes relative to the center frequency");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cbW = ImGui::GetFontSize() * 4.5f;
    const float plotW = avail.x - cbW - ImGui::GetStyle().ItemSpacing.x;
    float specH = std::max(80.f, avail.y * specFrac_);
    float wfH = std::max(80.f, avail.y - specH - 6);

    // --- spectrum ---
    if (liveFitSpecY_ && !liveAvgDb_.empty()) {
        ImPlot::SetNextAxisLimits(ImAxis_Y1, dbMin_ - 5, dbMax_ + 10, ImGuiCond_Always);
        liveFitSpecY_ = false;
    }
    if (ImPlot::BeginPlot("##lspectrum", ImVec2(plotW, specH), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisSI, &fFmt_);
        ImPlot::SetupAxisLinks(ImAxis_X1, &fmin_, &fmax_);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, fc_ - fs_, fc_ + fs_);
        ImPlot::SetupAxis(ImAxis_Y1, "dBFS");
        if (!liveSpecF_.empty()) {
            if (showPeak_) {
                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.4f, 0.3f, 0.6f));
                ImPlot::PlotLine("peak", liveSpecF_.data(), livePeakDb_.data(), (int)liveSpecF_.size());
            }
            ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.85f, 1.f, 1.f));
            ImPlot::PlotLine("avg", liveSpecF_.data(), liveAvgDb_.data(), (int)liveSpecF_.size());
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
    ImGui::TextDisabled("Live\nspectrum");

    ImGui::InvisibleButton("##lhsplit", ImVec2(avail.x, 6));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        specFrac_ = std::clamp(specFrac_ + ImGui::GetIO().MouseDelta.y / std::max(1.f, avail.y), 0.08f, 0.8f);

    // --- waterfall: newest row at the top, y = seconds ago ---
    const double dt = liveStats_.rowPeriod > 0 ? liveStats_.rowPeriod : 1.0 / liveParams_.rowsPerSec;
    const int H = liveRows_ > 0 ? liveRows_ : liveHistory_;
    const double span = H * dt;
    if (liveFitY_) {
        ImPlot::SetNextAxisLimits(ImAxis_Y1, 0, span, ImGuiCond_Always);
        liveFitY_ = false;
    }
    if (ImPlot::BeginPlot("##lwaterfall", ImVec2(plotW, wfH - 6), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatAxisSI, &fFmt_);
        ImPlot::SetupAxisLinks(ImAxis_X1, &fmin_, &fmax_);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, fc_ - fs_, fc_ + fs_);
        ImPlot::SetupAxis(ImAxis_Y1, "ago", ImPlotAxisFlags_Invert);
        ImPlot::SetupAxisFormat(ImAxis_Y1, formatValueSI, (void*)"s");
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, -0.02 * span, 1.02 * span);
        if (liveTex_ && liveFilled_ > 0 && liveRows_ > 0) {
            // The texture is a ring of rows; the newest is at liveHead_-1. ImPlot
            // maps uv0 to (xmin, ymax) and uv1 to (xmax, ymin).
            const int head = liveHead_;
            const float vh = (float)head / liveRows_;
            if (head > 0)
                ImPlot::PlotImage("##lwf0", liveTex_, ImPlotPoint(liveF0_, 0), ImPlotPoint(liveF1_, head * dt),
                                  ImVec2(0, 0), ImVec2(1, vh));
            if (liveFilled_ == liveRows_ && head < liveRows_)
                ImPlot::PlotImage("##lwf1", liveTex_, ImPlotPoint(liveF0_, head * dt),
                                  ImPlotPoint(liveF1_, liveRows_ * dt), ImVec2(0, vh), ImVec2(1, 1));
        }
        drawChannelOverlay(true);
        drawAudioOverlay();
        drawFreqMarkers();
        ImPlot::SetNextLineStyle(kColHover);
        if (std::isfinite(liveHoverAgo_))
            ImPlot::PlotInfLines("##hago", &liveHoverAgo_, 1, (int)ImPlotItemFlags_NoFit | (int)ImPlotInfLinesFlags_Horizontal);
        ImPlot::SetNextLineStyle(kColHover);
        if (std::isfinite(hoverF_)) ImPlot::PlotInfLines("##hf", &hoverF_, 1, ImPlotItemFlags_NoFit);
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint m = ImPlot::GetPlotMousePos();
            nextHoverF_ = m.x;
            liveNextHoverAgo_ = m.y;
            if (ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(0)) {
                demod_.chanEnable = true;
                demod_.chanOffset = m.x - fc_;
            }
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("##lcbar", dbMin_, dbMax_, ImVec2(cbW, wfH - 6), "%g dB", 0, cmap_);
}

// ---------------------------------------------------------------------------
// Status bar & keys
// ---------------------------------------------------------------------------
void App::drawLiveStatusBar() {
    std::string s;
    if (std::isfinite(hoverF_)) {
        s += "f=" + fmtSI(hoverF_ - (relFreq_ ? fc_ : 0), "Hz", 8) + "  ";
        if (std::isfinite(liveHoverAgo_)) {
            s += fmtSI(liveHoverAgo_, "s", 3) + " ago  ";
            float v = liveValueAt(liveHoverAgo_, hoverF_);
            if (std::isfinite(v)) {
                char b[32];
                snprintf(b, sizeof b, "%.1f dB", v);
                s += b;
            }
        }
    } else {
        s = status_;
    }
    if (fMarkers_) s += "   | dF=" + fmtSI(std::fabs(fm2_ - fm1_), "Hz", 6);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.c_str());

    std::string right;
    if (live_ && live_->running()) {
        const auto& t = liveStats_;
        right = t.description + " | " + fmtSI(t.rate, "S/s", 4) + " @ " + fmtSI(t.fc, "Hz", 9);
        char b[64];
        snprintf(b, sizeof b, " | FFT sees %.0f%%%s", std::min(1.0, t.analyzed) * 100, t.gpu ? " (GPU)" : "");
        right += b;
        if (t.gapEvents > 0)
            right += " | overflows " + std::to_string(t.gapEvents) +
                     (t.dropped > 0 ? " (" + fmtSI((double)t.dropped, "S", 3) + " lost)" : "");
        const auto rs = live_->recorder().status();
        if (rs.active) right += " | REC " + fmtSI(rs.seconds, "s", 4);
    } else {
        right = "not streaming";
    }
    char fps[64];
    snprintf(fps, sizeof fps, "%s | %.0f fps", win_.usingGL() ? "GLX" : "software", ImGui::GetIO().Framerate);
    drawStatusRight(right + " | ", fps, true);
}

void App::handleLiveShortcuts() {
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fMarkers_ = !fMarkers_;
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        fmin_ = fc_ - fs_ / 2;
        fmax_ = fc_ + fs_ / 2;
    }
}
