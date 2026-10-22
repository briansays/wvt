// File sessions (issue #76): File > Save File Session writes the state of
// the file viewer to a .wvtsession.json file; the File box (or the command
// line) opens one like a recording and puts that state back.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "app/app.h"
#include "common/log.h"
#include "implot.h"
#include "io/session.h"

namespace {
// A float as the decimal a person typed: 0.45f, not 0.44999998807907104.
double tidy(float v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.7g", v);
    return strtod(buf, nullptr);
}
}  // namespace

Session App::sessionFromState() const {
    Session s;
    s.file = file_ ? file_->path() : "";
    if (!s.file.empty() && s.file[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) s.file = std::string(cwd) + "/" + s.file;
    }
    s.fp = fp_;
    s.rate = fs_;
    s.fc = fc_;
    s.seg0 = segT0_;
    s.seg1 = segT1_;
    s.t0 = tmin_;
    s.t1 = tmax_;
    s.f0 = fmin_;
    s.f1 = fmax_;
    s.relFreq = relFreq_;
    s.nfft = nfft_;
    s.window = windowName((WindowType)window_);
    s.overlapPct = overlapPct_;
    s.avg = avg_;
    s.maxRows = maxRows_;
    s.maxCols = maxCols_;
    s.dbMin = tidy(dbMin_);
    s.dbMax = tidy(dbMax_);
    s.autoDb = autoDb_;
    s.colormap = ImPlot::GetColormapName(cmap_);
    s.showPeak = showPeak_;
    s.leftFrac = tidy(leftFrac_);
    s.specFrac = tidy(specFrac_);
    s.plotH = tidy(plotH_);
    s.showOverview = showOverview_;
    s.showIQ = showIQ_;
    s.showAM = showAM_;
    s.showFM = showFM_;
    s.showPM = showPM_;
    std::copy(std::begin(autoY_), std::end(autoY_), s.autoY);
    s.chanEnable = demod_.chanEnable;
    s.chanOffset = demod_.chanOffset;
    s.chanBw = demod_.chanBw;
    s.taps = demod_.taps;
    s.decim = demod_.decim;
    s.amMode = demod_.amMode;
    s.amSmooth = demod_.amSmooth;
    s.fmSmooth = demod_.fmSmooth;
    s.fmRelative = demod_.fmRelative;
    s.pmMode = demod_.pmMode;
    s.pmSmooth = demod_.pmSmooth;
    s.pmDegrees = demod_.pmDegrees;
    s.squelch = demod_.squelch;
    s.squelchDb = demod_.squelchDb;
    s.tMarkers = tMarkers_;
    s.fMarkers = fMarkers_;
    s.tm1 = tm1_;
    s.tm2 = tm2_;
    s.fm1 = fm1_;
    s.fm2 = fm2_;
    for (const Bookmark& b : bookmarks_) s.bookmarks.push_back({b.name, b.t0, b.t1});
    return s;
}

bool App::saveSession(const std::string& path, bool overwrite, std::string& err) const {
    if (!file_ || liveMode_) {
        err = "no file is open";
        return false;
    }
    return ::saveSession(expandHome(path), sessionFromState(), overwrite, err);
}

bool App::openSession(const std::string& path) {
    Session s;
    std::string err;
    if (!loadSession(path, s, err)) {
        LOG_ERROR("app", "%s", err.c_str());
        status_ = "Session not opened: " + err;
        return false;
    }
    // The recording, with the session's format, rate and frequency.
    CliOverrides o;
    o.haveFmt = true;
    o.fmt = s.fp.fmt;
    o.bigEndian = s.fp.bigEndian;
    o.haveRate = true;
    o.rate = s.rate;
    o.haveFc = true;
    o.fc = s.fc;
    o.header = s.fp.headerBytes;
    o.swapIQ = s.fp.swapIQ;
    segT0_ = s.seg0;  // openFile() loads this segment
    segT1_ = s.seg1;
    const std::string rec = sessionRecording(s);
    if (!openFileOnly(rec, o)) {
        status_ = "Session " + path + ": " + status_;
        return false;
    }

    nfft_ = s.nfft;
    for (int w = 0; w < (int)WindowType::Count; ++w)
        if (s.window == windowName((WindowType)w)) window_ = w;
    overlapPct_ = std::clamp(s.overlapPct, 0.0, 95.0);
    avg_ = s.avg;
    maxRows_ = s.maxRows;
    maxCols_ = s.maxCols;
    dbMin_ = (float)s.dbMin;
    dbMax_ = (float)s.dbMax;
    autoDb_ = s.autoDb;
    if (const int c = ImPlot::GetColormapIndex(s.colormap.c_str()); c >= 0) cmap_ = c;
    showPeak_ = s.showPeak;
    leftFrac_ = (float)std::clamp(s.leftFrac, 0.1, 0.9);
    specFrac_ = (float)std::clamp(s.specFrac, 0.05, 0.9);
    plotH_ = (float)std::max(0.0, s.plotH);
    showOverview_ = s.showOverview;
    showIQ_ = s.showIQ;
    showAM_ = s.showAM;
    showFM_ = s.showFM;
    showPM_ = s.showPM;
    std::copy(std::begin(s.autoY), std::end(s.autoY), autoY_);
    demod_.chanEnable = s.chanEnable;
    demod_.chanOffset = s.chanOffset;
    demod_.chanBw = std::clamp(s.chanBw, 1.0, fs_);
    demod_.taps = s.taps;
    demod_.decim = s.decim;
    demod_.amMode = s.amMode;
    demod_.amSmooth = s.amSmooth;
    demod_.fmSmooth = s.fmSmooth;
    demod_.fmRelative = s.fmRelative;
    demod_.pmMode = s.pmMode;
    demod_.pmSmooth = s.pmSmooth;
    demod_.pmDegrees = s.pmDegrees;
    demod_.squelch = s.squelch;
    demod_.squelchDb = s.squelchDb;
    tMarkers_ = s.tMarkers;
    fMarkers_ = s.fMarkers;
    tm1_ = s.tm1;
    tm2_ = s.tm2;
    fm1_ = s.fm1;
    fm2_ = s.fm2;
    bookmarks_.clear();
    for (const Session::Bookmark& b : s.bookmarks) bookmarks_.push_back({b.name, b.t0, b.t1});
    bookmarkSel_ = -1;
    // The view last: setView() keeps it inside the segment.
    if (s.t1 > s.t0) setView(s.t0, s.t1);
    if (s.f1 > s.f0) {
        fmin_ = s.f0;
        fmax_ = s.f1;
    }
    relFreq_ = s.relFreq;
    needRecolor_ = true;
    status_ = "Opened session " + path;
    LOG_INFO("app", "opened session %s: %s", path.c_str(), rec.c_str());
    return true;
}

void App::openSaveSession() {
    if (!file_ || liveMode_) return;
    snprintf(sessPath_, sizeof sessPath_, "%s", defaultSessionPath(file_->path()).c_str());
    sessOverwrite_ = false;
    sessMsg_.clear();
    sessOpen_ = true;
}

void App::drawSessionWindow() {
    if (!sessOpen_) return;
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 34, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Save File Session###session", &sessOpen_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("Saves the recording, its format, rate and segment, the view, the plot settings, the channel, "
                           "the markers and the bookmarks. To open it again, type its path in the File box.");
        ImGui::Spacing();
        ImGui::TextUnformatted("File");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##sesspath", sessPath_, sizeof sessPath_);
        ImGui::Checkbox("Replace an existing file", &sessOverwrite_);
        if (!sessMsg_.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(sessMsgErr_ ? ImVec4(1.f, 0.4f, 0.4f, 1.f) : ImVec4(0.5f, 0.9f, 0.5f, 1.f), "%s",
                               sessMsg_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!file_ || !sessPath_[0]);
        if (ImGui::Button("Save", ImVec2(fs * 6, 0))) {
            std::string err;
            const std::string path = expandHome(sessPath_);
            sessMsgErr_ = !saveSession(path, sessOverwrite_, err);
            if (sessMsgErr_) {
                sessMsg_ = err + (err.size() > 7 && err.compare(err.size() - 7, 7, " exists") == 0
                                      ? ": tick \"Replace an existing file\", or change the name" : "");
                LOG_ERROR("app", "session not saved: %s", err.c_str());
            } else {
                sessMsg_ = "Saved " + path;
                status_ = sessMsg_;
                LOG_INFO("app", "saved session %s", path.c_str());
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(fs * 6, 0))) sessOpen_ = false;
    }
    ImGui::End();
}
