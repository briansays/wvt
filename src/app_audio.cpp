// Audio window: demodulates one channel of the open file or the live stream
// and plays it, records it to WAV, or exports a file range to WAV.
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

#include "app.h"
#include "log.h"
#include "platform_x11.h"

namespace {
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColRec(1.f, 0.32f, 0.32f, 1.f);
const ImVec4 kColHead(0.30f, 1.00f, 0.45f, 1.f);
const ImVec4 kColAudio(1.00f, 0.62f, 0.20f, 1.f);

constexpr double kMinFollowSpan = 0.25;  // s

const char* kRanges[] = {"From view start", "View", "Time markers", "Whole file"};

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

bool isFm(int mode) { return mode == (int)AudioMode::NFM || mode == (int)AudioMode::WFM; }

std::string baseName(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
void App::openAudio(const AudioParams* p, bool play) {
    if (p) {
        au_ = *p;
        if (auFollow_) {
            demod_.chanEnable = true;
            demod_.chanBw = au_.bw;
        }
    }
    auOpen_ = true;
    if (play) playAudio();
}

void App::setAudioOutput(const std::string& device, double volumeDb) {
    auDevice_ = device;
    auVolDb_ = std::clamp(volumeDb, -60.0, 20.0);
}

bool App::audioBusy() const { return (audio_ && audio_->running()) || auExportSt_.active; }

void App::setAudioMode(int mode) {
    au_.mode = mode;
    au_.bw = audioModeDefaultBw((AudioMode)mode);
    au_.deemph = mode == (int)AudioMode::WFM ? (int)Deemphasis::Us75 : (int)Deemphasis::Off;
    if (auFollow_) {
        demod_.chanEnable = true;
        demod_.chanBw = std::min(au_.bw, fs_);
    }
}

// The play range of the file, in samples, inside the loaded segment.
void App::playRange(int64_t& s0, int64_t& s1) const {
    auto smp = [&](double t) { return std::clamp<int64_t>((int64_t)std::llround(t * fs_), segS0_, segS1_); };
    switch (auRange_) {
        case 0: s0 = smp(tmin_); s1 = segS1_; break;
        case 1: s0 = smp(tmin_); s1 = smp(tmax_); break;
        case 2: s0 = smp(std::min(tm1_, tm2_)); s1 = smp(std::max(tm1_, tm2_)); break;
        default: s0 = segS0_; s1 = segS1_; break;
    }
}

const char* App::rangeName(int r) const {
    return r == 3 && segmentActive() ? "Whole segment" : kRanges[r];
}

void App::syncAudioChannel() {
    if (auFollow_ && demod_.chanEnable) {
        au_.offset = demod_.chanOffset;
        au_.bw = demod_.chanBw;
    }
}

bool App::playAudio(int64_t from) {
    if (!audio_) audio_ = std::make_unique<AudioEngine>([this] { win_.wake(); });
    if (auFollow_ && !demod_.chanEnable) {
        demod_.chanEnable = true;
        demod_.chanBw = std::min(au_.bw, fs_);
    }
    syncAudioChannel();
    audio_->setVolume(auVolDb_, auMute_);
    std::string err;
    bool ok;
    if (liveMode_) {
        if (!live_ || !live_->running()) {
            auErr_ = "start a device first";
            return false;
        }
        ok = audio_->startLive(live_->ring(), live_->format(), live_->sampleRate(), au_, auDevice_, AudioOutput::Require, err);
        if (ok) audio_->noteFresh(live_->freshFrom());
    } else {
        int64_t s0, s1;
        playRange(s0, s1);
        if (from >= 0 && (from < s0 || from >= s1)) {
            s0 = from;
            s1 = segS1_;
        }
        ok = audio_->startFile(Source{file_, fp_, fs_, fc_}, s0, s1, from >= 0 ? from : s0, auLoop_, au_, auDevice_, err);
    }
    auErr_ = ok ? "" : err;
    if (!ok) {
        status_ = "Audio: " + err;
        LOG_ERROR("app", "audio: %s", err.c_str());
    }
    auStats_ = audio_->stats();
    return ok;
}

void App::stopAudio(const std::string& why) {
    if (audio_ && audio_->running()) {
        audio_->stop();
        if (!why.empty()) auMsg_ = why;
    }
    auStats_ = {};
    auPlayhead_ = NAN;
}

void App::pollAudio() {
    const bool wasExporting = auExportSt_.active;
    if (auExport_) auExportSt_ = auExport_->status();
    if (wasExporting && !auExportSt_.active) {
        auMsg_ = auExportSt_.error.empty() ? "Wrote " + baseName(auExportSt_.path) + " (" + fmtSI(auExportSt_.seconds, "s", 4) + ")"
                                           : "WAV export " + auExportSt_.error + ": " + baseName(auExportSt_.path);
        status_ = auMsg_;
        if (!auExportSt_.error.empty() && auExportSt_.error != "cancelled") LOG_ERROR("app", "%s", auMsg_.c_str());
    }
    if (!audio_ || !audio_->running()) {
        auPlayhead_ = NAN;
        return;
    }
    // Follow source changes: a new file or format stops playback; a restarted
    // device (new ring) restarts it.
    if (audio_->isLive()) {
        if (!liveMode_ || !live_ || !live_->running()) {
            stopAudio("Audio stopped (device stopped)");
            return;
        }
        if (live_->ring() != audio_->liveRing() && !playAudio()) return;
        audio_->noteFresh(live_->freshFrom());
    } else if (liveMode_ || !(audio_->fileSource() == Source{file_, fp_, fs_, fc_})) {
        stopAudio("Audio stopped (source changed)");
        return;
    }
    syncAudioChannel();
    audio_->setParams(au_);
    audio_->setVolume(auVolDb_, auMute_);
    audio_->setLoop(auLoop_);
    auStats_ = audio_->stats();

    const double t = nowSec();
    auPeakHold_ = std::max(auStats_.peakDb, auPeakHold_ - (float)(20 * (t - auPeakAt_)));
    auPeakAt_ = t;

    if (auDragHead_) {
        if (!ImGui::IsMouseDown(0)) {
            auDragHead_ = false;
            audio_->seek((int64_t)std::llround(auPlayhead_ * fs_));
        }
        return;
    }
    const double prev = auPlayhead_;
    auPlayhead_ = !auStats_.live && auStats_.pos >= 0 ? auStats_.pos / fs_ : NAN;
    // Follow playhead: page forward when it crosses the right edge of the view.
    // Short views would page faster than the plots can be computed.
    if (auFollowView_ && tmax_ - tmin_ >= kMinFollowSpan && std::isfinite(auPlayhead_) && std::isfinite(prev) &&
        prev <= tmax_ && auPlayhead_ > tmax_) {
        const double span = tmax_ - tmin_;
        setView(auPlayhead_ - 0.05 * span, auPlayhead_ + 0.95 * span);
    }
}

// ---------------------------------------------------------------------------
// Plot overlays
// ---------------------------------------------------------------------------
void App::drawAudioOverlay() {
    if (!auOpen_ || auFollow_) return;
    const double c0 = fc_ + au_.offset;
    const double lo = c0 - au_.bw / 2, hi = c0 + au_.bw / 2;
    ImPlot::PushPlotClipRect();
    ImPlotRect lim = ImPlot::GetPlotLimits();
    ImVec2 a = ImPlot::PlotToPixels(lo, lim.Y.Min), b = ImPlot::PlotToPixels(hi, lim.Y.Max);
    ImPlot::GetPlotDrawList()->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
                                             ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(255, 160, 50, 40));
    ImPlot::PopPlotClipRect();
    double c = c0;
    if (ImPlot::DragLineX(40, &c, kColAudio, 1.5f)) au_.offset = c - fc_;
}

void App::drawPlayhead(bool vertical) {
    if (!std::isfinite(auPlayhead_)) return;
    double t = auPlayhead_;
    if (vertical ? ImPlot::DragLineX(50, &t, kColHead, 2.f) : ImPlot::DragLineY(50, &t, kColHead, 2.f)) {
        auDragHead_ = true;
        auPlayhead_ = std::clamp(t, tBegin(), tEnd());
    }
}

// Shift+click on a time plot or the waterfall plays from there.
void App::playFrom(double t) {
    const int64_t s = std::clamp<int64_t>((int64_t)std::llround(t * fs_), segS0_, segS1_);
    if (audio_ && audio_->running() && !audio_->isLive()) {
        int64_t s0, s1;
        playRange(s0, s1);
        if (s >= s0 && s < s1) {
            audio_->setPaused(false);
            audio_->seek(s);
            return;
        }
    }
    auOpen_ = true;
    playAudio(s);
}

// ---------------------------------------------------------------------------
// Toolbar button & window
// ---------------------------------------------------------------------------
void App::drawAudioButton() {
    ImGui::SameLine();
    const bool on = audio_ && audio_->running();
    if (ImGui::Button(on ? "Audio *" : "Audio")) auOpen_ = !auOpen_;
    tooltipAlways(on ? "Audio is playing. Click to open or close the audio window (Ctrl+D)."
                     : "Open or close the audio window: demodulate and play a channel (Ctrl+D)");
}

void App::drawAudioWindow() {
    if (!auOpen_) return;
    const float fs = ImGui::GetFontSize();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(fs * 30, std::min(vp->WorkSize.y - fs * 4, fs * 40)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - fs * 31 - (sgOpen_ ? fs * 35 : 0), vp->WorkPos.y + fs * 3),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 22, fs * 10), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Audio###audio", &auOpen_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    const bool running = audio_ && audio_->running();
    const auto& st = auStats_;

    // --- transport ---
    const bool canPlay = liveMode_ ? (live_ && live_->running()) : (file_ != nullptr);
    if (!running || st.finished || (st.paused && !st.live)) {
        const bool resume = running && st.paused && !st.finished;
        ImGui::BeginDisabled(!canPlay);
        if (ImGui::Button(resume ? "Resume" : "Play", ImVec2(fs * 5, 0))) {
            if (resume) audio_->setPaused(false);
            else playAudio();
        }
        ImGui::EndDisabled();
        tooltipAlways(canPlay ? (liveMode_ ? "Play the selected channel of the live stream"
                                           : "Play the selected range of the file (Shift+click a plot to play from there)")
                              : (liveMode_ ? "Start a device first" : "Open a file first"));
    } else if (!st.live) {
        if (ImGui::Button("Pause", ImVec2(fs * 5, 0))) audio_->setPaused(true);
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Live", ImVec2(fs * 5, 0));
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop", ImVec2(fs * 4, 0))) stopAudio();
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        // Level meter: peak hold in dBFS, -60..0
        const float frac = std::clamp((auPeakHold_ + 60.f) / 60.f, 0.f, 1.f);
        char txt[48];
        if (running && auPeakHold_ > -100) snprintf(txt, sizeof txt, "%.0f dBFS", auPeakHold_);
        else snprintf(txt, sizeof txt, "-");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, auPeakHold_ > -1 ? kColErr : ImVec4(0.30f, 0.75f, 0.40f, 1.f));
        ImGui::ProgressBar(running ? frac : 0.f, ImVec2(-FLT_MIN, 0), txt);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Output level (after volume)");
    }

    // source / position
    if (liveMode_) {
        ImGui::TextDisabled("Live: %s", live_ && live_->running() ? liveStats_.description.c_str() : "no device streaming");
    } else if (file_) {
        ImGui::TextDisabled("File: %s", baseName(file_->path()).c_str());
        if (running && std::isfinite(auPlayhead_)) {
            float pos = (float)auPlayhead_;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##pos", &pos, (float)tBegin(), (float)tEnd(), fmtSI(pos, "s", 5).c_str())) {
                auPlayhead_ = pos;
                auDragHead_ = true;
            }
            ImGui::SetItemTooltip("Play position (also draggable on the plots)");
        }
    } else {
        ImGui::TextDisabled("No file loaded");
    }

    // --- demodulator ---
    ImGui::SeparatorText("Demodulator");
    if (beginForm("##audem")) {
        row("Mode");
        if (ImGui::BeginCombo("##aumode", audioModeName((AudioMode)au_.mode))) {
            for (int i = 0; i < (int)AudioMode::Count; ++i)
                if (ImGui::Selectable(audioModeName((AudioMode)i), i == au_.mode)) setAudioMode(i);
            ImGui::EndCombo();
        }
        row("Channel", "Follow: use the viewer's channel (the cyan lines; Ctrl+click the spectrum or waterfall to tune).\n"
                       "Otherwise the audio channel is the orange line.");
        ImGui::Checkbox("Follow viewer channel", &auFollow_);
        row("Offset", "Passband center relative to the center frequency.\nUSB: the carrier is at the lower edge; LSB: at the upper edge.");
        ImGui::BeginDisabled(auFollow_);
        InputSI("##auoff", &au_.offset, "Hz", -FLT_MIN);
        ImGui::EndDisabled();
        row("Bandwidth");
        ImGui::BeginDisabled(auFollow_);
        if (InputSI("##aubw", &au_.bw, "Hz", -FLT_MIN)) au_.bw = std::clamp(au_.bw, 50.0, std::max(50.0, fs_));
        ImGui::EndDisabled();
        if (au_.mode == (int)AudioMode::CW) {
            row("BFO pitch", "Tone pitch of a carrier at the passband center");
            float bfo = (float)au_.bfo;
            if (ImGui::SliderFloat("##aubfo", &bfo, 200, 1500, "%.0f Hz")) au_.bfo = bfo;
        }
        if (isFm(au_.mode)) {
            row("De-emphasis", "50 us: Europe and most of the world; 75 us: Americas, Korea");
            if (ImGui::BeginCombo("##audeem", deemphasisName((Deemphasis)au_.deemph))) {
                for (int i = 0; i < (int)Deemphasis::Count; ++i)
                    if (ImGui::Selectable(deemphasisName((Deemphasis)i), i == au_.deemph)) au_.deemph = i;
                ImGui::EndCombo();
            }
        } else {
            row("AGC", au_.mode == (int)AudioMode::AM ? "Normalize the audio by the carrier level" : "Automatic gain control");
            ImGui::Checkbox("##auagc", &au_.agc);
        }
        row("Gain");
        float g = (float)au_.gainDb;
        if (ImGui::SliderFloat("##augain", &g, -20, 60, "%.0f dB")) au_.gainDb = g;
        row("Squelch", "Mute the audio while the channel power is below the threshold");
        ImGui::Checkbox("##ausq", &au_.squelch);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(!au_.squelch);
        float sq = (float)au_.squelchDb;
        if (ImGui::SliderFloat("##ausqdb", &sq, -140, 0, "%.0f dBFS")) au_.squelchDb = sq;
        ImGui::EndDisabled();
        if (running) {
            row("Channel power");
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%.1f dBFS", st.chanDb);
            if (au_.squelch) {
                ImGui::SameLine();
                if (st.squelchOpen) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "open");
                else ImGui::TextDisabled("closed");
            }
        }
        ImGui::EndTable();
    }

    // --- output ---
    ImGui::SeparatorText("Output");
    if (beginForm("##auout")) {
        row("Volume");
        ImGui::Checkbox("Mute", &auMute_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        float v = (float)auVolDb_;
        if (ImGui::SliderFloat("##auvol", &v, -60, 20, "%.0f dB")) auVolDb_ = v;
        row("Device", "Sound output device (applies on the next Play)");
        if (!audioOutputBuilt()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kColWarn, "built without playback");
            tooltipAlways("This wvt was built with WVT_WITH_AUDIO=OFF. WAV export still works.");
        } else if (ImGui::BeginCombo("##audev", auDevice_.empty() ? "System default" : auDevice_.c_str())) {
            if (!auDevicesScanned_) {
                std::string err;
                auDevices_ = listAudioDevices(err);
                auDevicesScanned_ = true;
                if (!err.empty()) {
                    auErr_ = err;
                    LOG_ERROR("app", "%s", err.c_str());
                }
            }
            if (ImGui::Selectable("System default", auDevice_.empty())) auDevice_.clear();
            for (const auto& d : auDevices_)
                if (ImGui::Selectable(d.name.c_str(), d.name == auDevice_)) auDevice_ = d.name;
            if (ImGui::Selectable("(rescan)")) auDevicesScanned_ = false;
            ImGui::EndCombo();
        }
        if (!liveMode_) {
            row("Range", "Part of the file to play and to export (always inside the loaded segment)");
            if (ImGui::BeginCombo("##aurange", rangeName(auRange_))) {
                for (int i = 0; i < IM_ARRAYSIZE(kRanges); ++i)
                    if (ImGui::Selectable(rangeName(i), i == auRange_)) auRange_ = i;
                ImGui::EndCombo();
            }
            row("");
            ImGui::Checkbox("Loop", &auLoop_);
            ImGui::SameLine();
            ImGui::Checkbox("Follow playhead", &auFollowView_);
            ImGui::SetItemTooltip("Page the view forward when the playhead leaves it\n(views of %s or more)",
                                  fmtSI(kMinFollowSpan, "s", 3).c_str());
        }
        ImGui::EndTable();
    }

    // --- WAV ---
    ImGui::SeparatorText("WAV (48 kHz, 16-bit mono)");
    if (beginForm("##auwav")) {
        row("File prefix", "Date, time, frequency and mode are added to the name");
        ImGui::InputText("##auprefix", auPrefix_, sizeof auPrefix_);
        ImGui::EndTable();
    }
    const double chanHz = fc_ + au_.offset;
    if (liveMode_) {
        const bool rec = running && st.recording;
        if (rec) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
            if (ImGui::Button("Stop recording")) audio_->stopWav();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(kColRec, "REC %s", fmtSI(st.wavSeconds, "s", 4).c_str());
            ImGui::SetItemTooltip("%s", st.wavPath.c_str());
        } else {
            ImGui::BeginDisabled(!(live_ && live_->running()));
            if (ImGui::Button("Record audio")) {
                bool ok = running;
                if (!ok) {
                    // Without playback, record anyway (no sound device needed).
                    if (!audio_) audio_ = std::make_unique<AudioEngine>([this] { win_.wake(); });
                    if (auFollow_ && !demod_.chanEnable) {
                        demod_.chanEnable = true;
                        demod_.chanBw = std::min(au_.bw, fs_);
                    }
                    syncAudioChannel();
                    std::string err;
                    audio_->setVolume(auVolDb_, auMute_);
                    ok = audio_->startLive(live_->ring(), live_->format(), live_->sampleRate(), au_, auDevice_, AudioOutput::Optional, err);
                    if (ok) audio_->noteFresh(live_->freshFrom());
                    else {
                        auErr_ = err;
                        LOG_ERROR("app", "audio: %s", err.c_str());
                    }
                }
                std::string err;
                if (ok && !audio_->startWav(wavPath(auPrefix_, chanHz, audioModeName((AudioMode)au_.mode)), err)) {
                    auErr_ = err;
                    LOG_ERROR("app", "WAV recording: %s", err.c_str());
                } else if (ok) {
                    auErr_.clear();
                }
            }
            ImGui::EndDisabled();
            tooltipAlways("Record the demodulated audio to a WAV file (plays too, if a sound device is available)");
        }
        if (!st.wavError.empty()) ImGui::TextColored(kColErr, "WAV: %s", st.wavError.c_str());
    } else {
        if (auExportSt_.active) {
            ImGui::ProgressBar((float)auExportSt_.progress, ImVec2(-fs * 5, 0));
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) auExport_->cancel();
        } else {
            ImGui::BeginDisabled(!file_);
            if (ImGui::Button("Save WAV")) {
                if (!auExport_) auExport_ = std::make_unique<WavExportJob>();
                syncAudioChannel();
                WavExportConfig c;
                c.src = Source{file_, fp_, fs_, fc_};
                playRange(c.s0, c.s1);
                c.params = au_;
                c.path = wavPath(auPrefix_, chanHz, audioModeName((AudioMode)au_.mode));
                std::string err;
                if (!auExport_->start(c, err)) {
                    auErr_ = err;
                    LOG_ERROR("app", "WAV export: %s", err.c_str());
                } else {
                    auErr_.clear();
                    auExportSt_ = auExport_->status();
                }
            }
            ImGui::EndDisabled();
            int64_t s0, s1;
            playRange(s0, s1);
            ImGui::SameLine();
            ImGui::TextDisabled("%s, %s", rangeName(auRange_), fmtSI(fs_ > 0 ? (s1 - s0) / fs_ : 0, "s", 4).c_str());
            tooltipAlways("Demodulate the range to a WAV file, faster than real time");
        }
    }
    if (!auMsg_.empty()) ImGui::TextDisabled("%s", auMsg_.c_str());

    // --- status ---
    ImGui::Separator();
    if (!auErr_.empty()) ImGui::TextColored(kColErr, "%s", auErr_.c_str());
    if (running) {
        if (!st.hasOutput) ImGui::TextColored(kColWarn, "No sound output (%s); recording only", st.error.c_str());
        std::string s = fmtSI(st.latency, "s", 3) + " latency | IF " + fmtSI(st.ifRate, "Sps", 3);
        char b[96];
        snprintf(b, sizeof b, " | DSP %.0f%%", st.load * 100);
        s += b;
        if (st.live) {
            snprintf(b, sizeof b, " | drift %+.0f ppm", st.ppm);
            s += b;
        }
        ImGui::TextDisabled("%s", s.c_str());
        if (st.underruns || st.resyncs) {
            ImGui::TextColored(kColWarn, "underruns %lld  resyncs %lld", (long long)st.underruns, (long long)st.resyncs);
            ImGui::SetItemTooltip("Underrun: the sound card ran out of audio.\nResync: the demodulator fell behind the stream and skipped ahead.");
        }
        if (st.load > 0.8) ImGui::TextColored(kColWarn, "DSP behind: lower the sample rate");
        if (!st.live && st.finished) ImGui::TextDisabled("End of range");
    } else if (auExportSt_.active) {
        ImGui::TextDisabled("Exporting %s", baseName(auExportSt_.path).c_str());
    }
    ImGui::End();
}
