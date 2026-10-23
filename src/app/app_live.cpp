// Live device mode: device/recording toolbar, live spectrum and scrolling
// waterfall, status bar.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "common/log.h"
#include "app/platform_x11.h"

namespace {
const int kFftSizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

const ImVec4 kColHover(1.f, 1.f, 1.f, 0.45f);
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColRec(1.f, 0.32f, 0.32f, 1.f);

float fieldW(float chars) { return ImGui::GetFontSize() * chars * 0.55f + ImGui::GetStyle().FramePadding.x * 2; }
// The colour scale's dB min and max fields, Auto and the options button
float dbGroupW() {
    const ImGuiStyle& st = ImGui::GetStyle();
    return fieldW(5) * 2 + ImGui::GetFrameHeight() * 2 + st.ItemInnerSpacing.x * 5 + ImGui::CalcTextSize("Auto").x;
}

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Gain limits to show before a device is open.
GainRange defaultGain(const std::string& driver) {
    if (driver == "rtlsdr") return {0, 49.6, 0.1, {}, true};
    if (driver == "uhd") return {0, 76, 1, {}, true};
    if (driver == "bb") return {-130, 20, 1, {}, false, "Ref level", "dBm"};
    if (driver == "skiq") return {0, 76, 1, {}, true};  // the M.2; the open card gives its own
    return {0, 60, 1, {}, true};
}

const char* driverName(const std::string& d) {
    if (d == "rtlsdr") return "RTL-SDR";
    if (d == "uhd") return "USRP";
    if (d == "bb") return "BB60";
    if (d == "skiq") return "Sidekiq";
    if (d == "siggen") return "Signal generator";
    return "Simulator";
}

}  // namespace

// ---------------------------------------------------------------------------
// Mode switching & device control
// ---------------------------------------------------------------------------
bool App::liveRecording() const { return live_ && live_->recorder().active(); }

// In the menu bar.
void App::drawSourceSelector(float width) {
    ImGui::TextDisabled("Source");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(width);
    int mode = liveMode_ ? 1 : 0;
    const char* names[] = {"File", "Device"};
    ImGui::BeginDisabled(liveRecording());
    if (ImGui::Combo("##source", &mode, names, 2)) {
        if (mode == 1 && !liveMode_) enterLive();
        else if (mode == 0 && liveMode_) leaveLive();
    }
    ImGui::EndDisabled();
    tooltipAlways("File: analyze a recording.\nDevice: stream live from an RTL-SDR, a USRP, a Signal Hound BB60 or a Sidekiq.");
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

// UHD searches the network too, so the scan runs in the background; pollScan
// takes the result.
void App::scanDevices() {
    if (scanJob_.valid()) return;
    LOG_DEBUG("app", "scanning for devices");
    scanJob_ = std::async(std::launch::async, [] { return enumerateDevices(); });
}

void App::pollScan() {
    if (!scanJob_.valid() || scanJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const bool first = !devicesScanned_;
    devices_ = scanJob_.get();
    devicesScanned_ = true;
    LOG_INFO("app", "found %zu device entries", devices_.size());
    bool found = false;
    for (const auto& d : devices_) found |= d.driver == devCfg_.driver && d.args == devCfg_.args;
    // The first scan picks the first real device (the simulator is listed last),
    // unless one was chosen already (on the command line, or while it scanned).
    // A later scan picks again if the device it picked is gone. Not while a
    // device streams: an open BB60 is not in the list, and the selection must
    // stay the device that streams.
    const bool running = live_ && live_->running();
    if (!devPicked_ && (first || !found) && !running && !devices_.empty()) {
        selectDevice(devices_.front());
        devPicked_ = false;
    }
}

void App::selectDevice(const DeviceInfo& d) {
    const bool driverChanged = d.driver != devCfg_.driver;
    if (d.args != devCfg_.args || driverChanged) devCaps_ = {};
    devPicked_ = true;
    // The BB60's "gain" is a reference level in dBm: a value from another driver means nothing.
    const bool gainMeaningChanged = driverChanged && (d.driver == "bb" || devCfg_.driver == "bb");
    devCfg_.driver = d.driver;
    devCfg_.args = d.args;
    snprintf(devArgs_, sizeof devArgs_, "%s", d.args.c_str());
    if (driverChanged) {
        std::string err, warn;
        devCfg_.rate = snapSampleRate(d.driver, devCfg_.rate);
        if (!checkSampleRate(d.driver, devCfg_.rate, err, warn))
            devCfg_.rate = d.driver == "uhd" || d.driver == "bb" || d.driver == "skiq" ? 10e6 : 2.4e6;
        if (gainMeaningChanged) devCfg_.gain = d.driver == "bb" ? -20 : 30;
        GainRange g = defaultGain(d.driver);
        devCfg_.gain = std::clamp(devCfg_.gain, g.min, g.max);
        // A USRP starts on its default antenna, channel 0 and its default reference.
        antenna_[0] = 0;
        devCfg_.channel = 0;
        devCfg_.ref.clear();
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
    devCaps_ = live_->caps();
    status_ = "Streaming from " + liveStats_.description;
    win_.setTitle("wvt - " + liveStats_.description);
    return true;
}

bool App::startDevice(const StreamConfig& cfg) {
    if (!liveMode_) enterLive();
    devCfg_ = cfg;
    devPicked_ = true;
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
    pollScan();
    const bool scanning = scanJob_.valid();
    std::string cur = std::string(driverName(devCfg_.driver)) + (devCfg_.args.empty() ? "" : " " + devCfg_.args);
    for (const auto& d : devices_)
        if (d.driver == devCfg_.driver && d.args == devCfg_.args) cur = d.label;
    if (scanning && !devicesScanned_) cur = "Scanning...";
    // An open BB60 is not in the list: name the device that streams.
    if (running && !liveStats_.description.empty() && live_->config().driver == devCfg_.driver)
        cur = liveStats_.description;
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
    ImGui::BeginDisabled(scanning);
    if (ImGui::Button(scanning ? "Scanning" : "Rescan")) scanDevices();
    ImGui::EndDisabled();
    tooltipAlways("Look for attached devices again (USB, and USRPs on the network)");
    if (devCfg_.driver == "uhd") {
        label("Args", fieldW(22));
        if (ImGui::InputText("##args", devArgs_, sizeof devArgs_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            devCaps_ = {};
            if (running) restartDevice();
        }
        tooltipAlways("UHD device arguments (Enter applies), e.g.\n"
                      "  serial=31A2B3C               a USB B2xx\n"
                      "  addr=192.168.10.2            a network USRP (N3xx, X3xx, X4xx)\n"
                      "  addr=192.168.40.2,master_clock_rate=184.32e6\n"
                      "Empty: the first USRP UHD finds.");
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
    const auto rates = supportedRates(devCfg_.driver);
    if (!rates.empty()) {
        // Only discrete rates exist (decimations of one clock): pick from the list.
        ImGui::SetNextItemWidth(fieldW(13));
        if (ImGui::BeginCombo("##lfs", fmtSI(rate, "S/s", 6).c_str())) {
            for (double r : rates)
                if (ImGui::Selectable(fmtSI(r, "S/s", 6).c_str(), std::fabs(r - rate) < 1)) {
                    devCfg_.rate = r;
                    std::string warn;
                    if (checkSampleRate(devCfg_.driver, r, err, warn) && !warn.empty()) {
                        status_ = warn;
                        LOG_WARN("app", "%s", warn.c_str());
                    }
                    if (running) restartDevice();
                }
            ImGui::EndCombo();
        }
    } else if (InputSI("##lfs", &rate, "Sps", fieldW(13))) {
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
    std::string rateTip;
    if (rec) rateTip = "A recording has a single sample rate. Stop recording to change it.";
    else if (devCfg_.driver == "rtlsdr") rateTip = "RTL-SDR: 225-300 kS/s or 0.9-3.2 MS/s (2.4 MS/s or less is reliable)";
    else if (devCfg_.driver == "bb") rateTip = "BB60: 40 MS/s divided by a power of 2.\nAbove 10 MS/s the host CPU does a lot of filtering.";
    else if (devCfg_.driver == "uhd" && devCaps_.rate.valid())
        rateTip = "USRP " + devCaps_.product + ": " + fmtSI(devCaps_.rate.min, "S/s", 4) + " to " +
                  fmtSI(devCaps_.rate.max, "S/s", 4) + (devCaps_.usb ? " (USB 3 above about 8 MS/s)" : "") +
                  ". The USRP uses the nearest rate it can make.";
    else if (devCfg_.driver == "skiq" && devCaps_.rate.valid())
        rateTip = "Sidekiq " + devCaps_.product + ": " + fmtSI(devCaps_.rate.min, "S/s", 4) + " to " +
                  fmtSI(devCaps_.rate.max, "S/s", 4) + (devCaps_.usb ? ".\nOn USB the card streams much less than on PCIe." : ".");
    else if (devCfg_.driver == "skiq")
        rateTip = "Sidekiq: the range depends on the card (M.2 and mPCIe up to 61.44 MS/s, X2 and X4 245.76 MS/s).\n"
                  "On USB the card streams much less than on PCIe.";
    else if (devCfg_.driver == "uhd")
        rateTip = "USRP: the limit depends on the model (B2xx 61.44 MS/s on USB 3, N310 153.6 MS/s,\n"
                  "X310 200 MS/s, X410 500 MS/s). Network USRPs divide a fixed master clock.";
    else rateTip = "Up to 20 MS/s";
    tooltipAlways(rateTip.c_str());

    // --- gain ---
    const GainRange g = running ? live_->gainRange() : defaultGain(devCfg_.driver);
    label(g.label, fieldW(12));
    float gain = (float)devCfg_.gain;
    char gainFmt[16];
    snprintf(gainFmt, sizeof gainFmt, "%%.1f %s", g.unit);
    // A BB60 restarts its stream for a new reference level, so apply it when the drag ends.
    const bool applyOnRelease = devCfg_.driver == "bb";
    ImGui::BeginDisabled(devCfg_.agc && g.agc);
    if (ImGui::SliderFloat("##gain", &gain, (float)g.min, (float)g.max, gainFmt)) {
        double v = gain;
        if (!g.steps.empty()) {  // snap to the tuner's discrete gains
            double best = g.steps.front();
            for (double s : g.steps)
                if (std::fabs(s - v) < std::fabs(best - v)) best = s;
            v = best;
        }
        devCfg_.gain = v;
        if (running && !applyOnRelease && !live_->setGain(devCfg_.agc, devCfg_.gain, err)) {
            status_ = "Gain failed: " + err;
            LOG_ERROR("app", "gain failed: %s", err.c_str());
        }
    }
    if (running && applyOnRelease && ImGui::IsItemDeactivatedAfterEdit() && !live_->setGain(false, devCfg_.gain, err)) {
        status_ = "Reference level failed: " + err;
        LOG_ERROR("app", "reference level failed: %s", err.c_str());
    }
    ImGui::EndDisabled();
    if (devCfg_.driver == "bb")
        tooltipAlways("The strongest input you expect, in dBm. The BB60 sets its attenuator and amplifier from it\n"
                      "(unless Atten and Amp are set by hand in the options). Too low overloads the ADC;\n"
                      "too high raises the noise floor.");
    if (g.agc) {
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        if (ImGui::Checkbox("AGC", &devCfg_.agc) && running && !live_->setGain(devCfg_.agc, devCfg_.gain, err)) {
            status_ = "Gain failed: " + err;
            LOG_ERROR("app", "gain failed: %s", err.c_str());
        }
    }

    // --- driver-specific, in the options popup (these restart the stream) ---
    bool restart = false;
    if (devCfg_.driver == "rtlsdr" || devCfg_.driver == "uhd" || devCfg_.driver == "bb" || devCfg_.driver == "skiq") {
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        settingsButton("##devopts", devCfg_.driver == "rtlsdr" ? "Bias-T, direct sampling and frequency correction"
                                    : devCfg_.driver == "bb"   ? "Attenuator, amplifier, I/Q bandwidth and external reference"
                                    : devCfg_.driver == "skiq" ? "RF port, bandwidth and external reference"
                                                               : "Antenna, channel, reference and analog bandwidth");
    }
    if (ImGui::BeginPopup("##devopts")) {
        ImGui::BeginDisabled(rec);
        if (beginForm("##devform", 6.5f, 18)) {
            if (devCfg_.driver == "rtlsdr") {
                row("Bias-T", "Sends 4.5 V up the antenna cable to power an LNA or active antenna.\nOnly enable it if your antenna side expects DC.");
                restart |= ImGui::Checkbox("##biast", &devCfg_.biasTee);
                row("Direct", "Direct sampling for HF below 24 MHz. The RTL-SDR v3 uses the Q branch.");
                const char* ds[] = {"Off", "I", "Q"};
                restart |= ImGui::Combo("##ds", &devCfg_.directSampling, ds, 3);
                row("PPM", "Frequency correction of the crystal (the RTL-SDR v3 TCXO is usually within 1-2 ppm)");
                if (ImGui::InputInt("##ppm", &devCfg_.ppm)) {
                    devCfg_.ppm = std::clamp(devCfg_.ppm, -200, 200);
                    restart = true;
                }
            } else if (devCfg_.driver == "uhd") {
                // The lists come from the device once it has been opened.
                row("Antenna", "The receive port of the channel. Default: the device's default (RX2 on the B2xx).");
                if (ImGui::BeginCombo("##ant", antenna_[0] ? antenna_ : "Default")) {
                    if (ImGui::Selectable("Default", !antenna_[0])) {
                        antenna_[0] = 0;
                        restart = true;
                    }
                    std::vector<std::string> ants = devCaps_.antennas;
                    if (ants.empty()) ants = {"RX2", "TX/RX"};
                    for (const auto& a : ants)
                        if (ImGui::Selectable(a.c_str(), a == antenna_)) {
                            snprintf(antenna_, sizeof antenna_, "%s", a.c_str());
                            restart = true;
                        }
                    ImGui::EndCombo();
                }
                const int nch = std::max(1, devCaps_.channels);
                row("Channel", "Receive channel: the X310, N310 and X410 have several (one daughterboard or RF port\n"
                               "each). wvt streams one channel at a time.");
                if (ImGui::InputInt("##chan", &devCfg_.channel)) {
                    devCfg_.channel = std::clamp(devCfg_.channel, 0, devCaps_.channels > 0 ? nch - 1 : 15);
                    restart = true;
                }
                if (devCaps_.channels > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("of %d", nch);
                }
                row("Reference", "Clock and time reference: internal, external (10 MHz and PPS inputs) or gpsdo.\n"
                                 "Default: the device's default (internal).");
                if (ImGui::BeginCombo("##ref", devCfg_.ref.empty() ? "Default" : devCfg_.ref.c_str())) {
                    if (ImGui::Selectable("Default", devCfg_.ref.empty())) {
                        devCfg_.ref.clear();
                        restart = true;
                    }
                    std::vector<std::string> refs = devCaps_.refs;
                    if (refs.empty()) refs = {"internal", "external", "gpsdo"};
                    for (const auto& r : refs)
                        if (ImGui::Selectable(r.c_str(), r == devCfg_.ref)) {
                            devCfg_.ref = r;
                            restart = true;
                        }
                    ImGui::EndCombo();
                }
                const std::string bwTip =
                    "Analog filter bandwidth" +
                    (devCaps_.bandwidth.valid()
                         ? ", " + fmtSI(devCaps_.bandwidth.min, "Hz", 4) + " - " + fmtSI(devCaps_.bandwidth.max, "Hz", 4)
                         : std::string()) +
                    ". 0 = automatic (follows the sample rate). Not every daughterboard has one.";
                row("Bandwidth", bwTip.c_str());
                if (InputSI("##bw", &devCfg_.bandwidth, "Hz", ImGui::GetContentRegionAvail().x)) restart = true;
            } else if (devCfg_.driver == "bb") {
                row("Atten", "Input attenuator. Auto: chosen from the reference level.\n"
                             "Set Atten and Amp both by hand to override the reference level.");
                const char* att[] = {"Auto", "0 dB", "10 dB", "20 dB", "30 dB"};
                int a = devCfg_.bbAtten + 1;
                if (ImGui::Combo("##bbatt", &a, att, 5)) {
                    devCfg_.bbAtten = a - 1;
                    restart = true;
                }
                row("Amp", "Amplifier setting (0 = lowest gain; BB60D: 0, 5, 15, 20 dB). Auto: chosen from the\n"
                           "reference level.");
                const char* amp[] = {"Auto", "0", "1", "2", "3"};
                int m = devCfg_.bbAmp + 1;
                if (ImGui::Combo("##bbamp", &m, amp, 5)) {
                    devCfg_.bbAmp = m - 1;
                    restart = true;
                }
                const std::string bwTip =
                    "I/Q filter bandwidth. 0 = the widest for the rate (" +
                    fmtSI(bbMaxBandwidth(std::max(1, (int)std::lround(40e6 / devCfg_.rate))), "Hz", 4) +
                    " at " + fmtSI(devCfg_.rate, "S/s", 4) + ").";
                row("Bandwidth", bwTip.c_str());
                if (InputSI("##bw", &devCfg_.bandwidth, "Hz", ImGui::GetContentRegionAvail().x)) restart = true;
                row("Ext ref", "Lock to a 10 MHz reference on port 1.");
                restart |= ImGui::Checkbox("##bbref", &devCfg_.extRef);
            } else if (devCfg_.driver == "skiq") {
                // The port list comes from the card once it has been opened.
                row("RF port", "The receive port of handle A1. Default: the card's default.");
                if (ImGui::BeginCombo("##skport", antenna_[0] ? antenna_ : "Default")) {
                    if (ImGui::Selectable("Default", !antenna_[0])) {
                        antenna_[0] = 0;
                        restart = true;
                    }
                    for (const auto& a : devCaps_.antennas)
                        if (ImGui::Selectable(a.c_str(), a == antenna_)) {
                            snprintf(antenna_, sizeof antenna_, "%s", a.c_str());
                            restart = true;
                        }
                    ImGui::EndCombo();
                }
                row("Bandwidth", "RF bandwidth. 0 = 80 % of the sample rate.");
                if (InputSI("##bw", &devCfg_.bandwidth, "Hz", ImGui::GetContentRegionAvail().x)) restart = true;
                row("Ext ref", "Use the external reference clock input. The M.2 and mPCIe can't switch at run\n"
                               "time: set their reference with the SDK's ref_clock app.");
                restart |= ImGui::Checkbox("##skref", &devCfg_.extRef);
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
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

    // --- recording (wraps to the next row when there is no room) ---
    drawRecordBar();
}

void App::drawRecordBar() {
    const bool running = live_ && live_->running();
    const bool rec = liveRecording();

    // Record / Stop, then the settings in the options popup.
    flow(ImGui::CalcTextSize("Stop recording").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight() * 1.5f);
    if (!rec) {
        ImGui::BeginDisabled(!running);
        if (redButton("Record")) {
            RecordConfig c = recCfg_;
            c.prefix = recPrefix_;
            startRecording(c);
        }
        ImGui::EndDisabled();
        tooltipAlways(running ? "Start recording the stream" : "Start a device first");
    } else if (ImGui::Button("Stop recording")) {
        live_->stopRecording();
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    settingsButton("##recopts", "Where and how to record: path, format, samples and time limit");
    if (ImGui::BeginPopup("##recopts")) {
        ImGui::BeginDisabled(rec);
        if (beginForm("##recform", 5.5f, 30)) {
            row("Record to", "Path prefix for recordings. Date, time, frequency and rate are appended,\ne.g. ~/wvt-recordings/wvt_20261016_142233_915.000MHz_2.4MSps.sigmf-data.\nMissing folders are created.");
            ImGui::InputText("##recprefix", recPrefix_, sizeof recPrefix_);
            row("Format", "SigMF: .sigmf-data + .sigmf-meta with rate, frequency, retunes and dropped-sample notes.\nRaw: bare samples; rate and frequency are in the file name.");
            int fmt = recCfg_.sigmf ? 0 : 1;
            const char* fmts[] = {"SigMF", "Raw"};
            if (ImGui::Combo("##recfmt", &fmt, fmts, 2)) recCfg_.sigmf = fmt == 0;
            row("Samples", "native: exactly what the device sends (smallest, lossless).\ncf32: complex float32, 2-4x larger, for tools that only read float.");
            std::string native = "native";
            if (running)
                native += liveStats_.fmt == SampleFormat::CU8 ? " (cu8)" : liveStats_.fmt == SampleFormat::CF32 ? " (cf32)" : " (ci16)";
            int smp = recCfg_.cf32 ? 1 : 0;
            const char* smps[] = {native.c_str(), "cf32"};
            if (ImGui::Combo("##recsmp", &smp, smps, 2)) recCfg_.cf32 = smp == 1;
            row("Limit", "Stop recording after this long. 0 = no limit.");
            InputSI("##reclim", &recCfg_.maxSeconds, "s", ImGui::GetContentRegionAvail().x);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (!rec) {
        std::string where = std::string(recPrefix_) + (recCfg_.sigmf ? " (SigMF" : " (raw") + (recCfg_.cf32 ? ", cf32)" : ")");
        flow(ImGui::CalcTextSize(where.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", where.c_str());
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
    // The main settings on the row, the rest in the options popup.
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
    label("dB", dbGroupW());
    ImGui::SetNextItemWidth(fieldW(5));
    if (ImGui::InputFloat("##ldbmin", &dbMin_, 0, 0, "%.0f")) liveAutoDb_ = false;
    ImGui::SetItemTooltip("Bottom of the colour scale (dB)");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(fieldW(5));
    if (ImGui::InputFloat("##ldbmax", &dbMax_, 0, 0, "%.0f")) liveAutoDb_ = false;
    ImGui::SetItemTooltip("Top of the colour scale (dB)");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::Checkbox("Auto", &liveAutoDb_);
    ImGui::SetItemTooltip("Auto-range the color scale to the waterfall history");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    settingsButton("##lopts", "Averaging, scroll speed, history, image size, colormap and peak hold");
    if (ImGui::BeginPopup("##lopts")) {
        if (beginForm("##lform", 5.5f, 20)) {
            row("Avg", "FFTs averaged into each waterfall row.\nauto: all samples since the previous row (up to 20 MS/s,\nor 100 MS/s on the GPU), so nothing is missed at RTL-SDR rates. Lower values show short bursts more sharply.");
            ImGui::SliderInt("##lavg", &liveParams_.avg, 0, 64, liveParams_.avg == 0 ? "auto" : "%d");
            row("Rows/s", "Waterfall scroll speed (rows per second)");
            if (ImGui::InputDouble("##lrps", &liveParams_.rowsPerSec, 5, 10, "%.0f"))
                liveParams_.rowsPerSec = std::clamp(liveParams_.rowsPerSec, 1.0, 100.0);
            row("History", "Waterfall rows kept");
            if (ImGui::InputInt("##lhist", &liveHistory_, 100, 500)) liveHistory_ = std::clamp(liveHistory_, 64, 4096);
            row("Cols", "Maximum waterfall columns; FFT bins beyond this are max-pooled");
            if (ImGui::InputInt("##lcols", &liveParams_.maxCols, 256, 1024))
                liveParams_.maxCols = std::clamp(liveParams_.maxCols, 64, 8192);
            row("Spec avg", "Spectrum averaging time constant, in rows");
            if (ImGui::InputInt("##lsavg", &specAvgRows_)) specAvgRows_ = std::clamp(specAvgRows_, 1, 200);
            row("Colormap");
            if (ImGui::BeginCombo("##lcmap", ImPlot::GetColormapName(cmap_))) {
                for (int i = 0; i < ImPlot::GetColormapCount(); ++i)
                    if (ImGui::Selectable(ImPlot::GetColormapName(i), i == cmap_)) cmap_ = i;
                ImGui::EndCombo();
            }
            row("Peak hold", "Show the peak-hold trace in the spectrum");
            ImGui::Checkbox("##lpeak", &showPeak_);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) livePeakLin_ = liveAvgLin_;
            ImGui::SetItemTooltip("Restart the peak hold");
            ImGui::EndTable();
        }
        ImGui::EndPopup();
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cbW = colorbarWidth();
    const float plotW = avail.x - cbW - ImGui::GetStyle().ItemSpacing.x;
    float specH = std::max(ImGui::GetFontSize() * 7, avail.y * specFrac_);
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
        snapNotePlot(SnapPlot::Spectrum);
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
        snapNotePlot(SnapPlot::Waterfall);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    colorbar("##lcbar", dbMin_, dbMax_, wfH - 6, cmap_);
    snapNoteItem(SnapPlot::Waterfall);
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
        s = statusText();
    }
    if (fMarkers_) s += "   | dF=" + fmtSI(std::fabs(fm2_ - fm1_), "Hz", 6);

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
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(elideMiddle(s, statusLeftW(right + " | ", fps, true)).c_str());
    drawStatusRight(right + " | ", fps, true);
}

void App::handleLiveShortcuts() {
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fMarkers_ = !fMarkers_;
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) showAll();
}
