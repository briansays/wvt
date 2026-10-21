// Plot snapshots (issue #13, docs/snapshots.md): the window that selects the
// plots and the folder, and the capture. The plots note where they are drawn on
// each frame; an export hides the pointer for two frames (no crosshair or
// tooltip in the picture), then crops each plot from the rendered frame.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "app.h"
#include "imgui_internal.h"
#include "implot_internal.h"
#include "log.h"
#include "platform_x11.h"
#include "recorder.h"

namespace {
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void App::snapClipToWindow() {
    const ImRect r = ImGui::GetCurrentWindow()->InnerClipRect;
    snapClipA_ = r.Min;
    snapClipB_ = r.Max;
}

void App::snapClipReset() {
    snapClipA_ = ImVec2(-FLT_MAX, -FLT_MAX);
    snapClipB_ = ImVec2(FLT_MAX, FLT_MAX);
}

void App::snapNote(SnapPlot p, ImVec2 a, ImVec2 b) {
    SnapArea& s = snapArea_[(int)p];
    s.a = a;
    s.b = b;
    s.clipA = snapClipA_;
    s.clipB = snapClipB_;
    s.frame = ImGui::GetFrameCount();
}

void App::snapNotePlot(SnapPlot p, float top) {
    const ImRect r = ImPlot::GetCurrentPlot()->FrameRect;
    snapNote(p, ImVec2(r.Min.x, top >= 0 ? std::min(top, r.Min.y) : r.Min.y), r.Max);
}

void App::snapNoteItem(SnapPlot p) {
    SnapArea& s = snapArea_[(int)p];
    if (s.frame != ImGui::GetFrameCount()) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    s.a = ImVec2(std::min(s.a.x, a.x), std::min(s.a.y, a.y));
    s.b = ImVec2(std::max(s.b.x, b.x), std::max(s.b.y, b.y));
}

bool App::snapVisible(SnapPlot p) const {
    const SnapArea& s = snapArea_[(int)p];
    return s.frame >= ImGui::GetFrameCount() - 1 && s.b.x > s.clipA.x && s.a.x < s.clipB.x && s.b.y > s.clipA.y &&
           s.a.y < s.clipB.y;
}

void App::drawSnapshotWindow() {
    if (snapOpen_ && !ImGui::IsPopupOpen("Save plot snapshots")) ImGui::OpenPopup("Save plot snapshots");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Save plot snapshots", &snapOpen_, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("Plots to save, as they show on the screen:");
    int count = 0;
    for (int i = 0; i < kSnapPlots; ++i) {
        const SnapPlot p = (SnapPlot)i;
        const bool vis = snapVisible(p);
        ImGui::BeginDisabled(!vis);
        bool on = vis && snapSel_[i];
        if (ImGui::Checkbox(snapPlotLabel(p), &on)) snapSel_[i] = on;
        ImGui::EndDisabled();
        if (!vis) {
            ImGui::SetItemTooltip("Not on the screen: hidden, or not in this mode");
        } else {
            const SnapArea& s = snapArea_[i];
            // A few pixels of the frame border under the edge do not count.
            const float tol = ImGui::GetFontSize() * 0.3f;
            if (s.a.y < s.clipA.y - tol || s.b.y > s.clipB.y + tol) {
                ImGui::SameLine();
                ImGui::TextColored(kColWarn, "partly hidden");
                ImGui::SetItemTooltip("Only the part on the screen is saved. Scroll the plots to show all of it.");
            }
        }
        count += on;
    }
    if (ImGui::SmallButton("All")) std::fill(std::begin(snapSel_), std::end(snapSel_), true);
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) std::fill(std::begin(snapSel_), std::end(snapSel_), false);

    ImGui::Spacing();
    ImGui::TextUnformatted("Folder");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
    ImGui::InputText("##snapdir", snapDir_, sizeof snapDir_);
    const std::string dir = expandHome(snapDir_);
    const std::string base = snapshotBase(liveMode_ || !file_ ? "" : file_->path());
    const int n = nextSnapshotIndex(dir, base);
    ImGui::TextDisabled("Files: %s_capture_<plot>_%d.png", base.c_str(), n);

    if (!snapErr_.empty()) ImGui::TextColored(kColWarn, "%s", snapErr_.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button("Export", ImVec2(ImGui::GetFontSize() * 6, 0))) {
        snapErr_.clear();
        if (makeDirs(dir, snapErr_)) {
            // Hide the pointer so that no crosshair or tooltip is in the picture: the
            // crosshair lags one frame, so the capture is two frames after this one.
            ImGuiIO& io = ImGui::GetIO();
            snapMouse_ = io.MousePos;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            snapCountdown_ = 3;
            snapOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 6, 0))) {
        snapErr_.clear();
        snapOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

std::string App::statusText() const {
    return !snapNoteMsg_.empty() && nowSec() < snapNoteUntil_ ? snapNoteMsg_ : status_;
}

void App::afterRender() {
    if (snapCountdown_ <= 0) return;
    if (--snapCountdown_ > 0) return;
    saveSnapshots();
    if (ImGui::IsMousePosValid(&snapMouse_)) ImGui::GetIO().AddMousePosEvent(snapMouse_.x, snapMouse_.y);
}

void App::saveSnapshots() {
    const auto t0 = std::chrono::steady_clock::now();
    RgbImage frame;
    win_.readFrame(frame.w, frame.h, frame.px);
    const std::string dir = expandHome(snapDir_);
    const std::string base = snapshotBase(liveMode_ || !file_ ? "" : file_->path());
    const int n = nextSnapshotIndex(dir, base);
    int saved = 0;
    std::string err, names;
    for (int i = 0; i < kSnapPlots; ++i) {
        const SnapPlot p = (SnapPlot)i;
        if (!snapSel_[i] || !snapVisible(p)) continue;
        const SnapArea& s = snapArea_[i];
        // The plot, clipped to its window (a time plot can be scrolled partly out).
        SnapRect r;
        r.x0 = (int)std::floor(std::max(s.a.x, s.clipA.x));
        r.y0 = (int)std::floor(std::max(s.a.y, s.clipA.y));
        r.x1 = (int)std::ceil(std::min(s.b.x, s.clipB.x));
        r.y1 = (int)std::ceil(std::min(s.b.y, s.clipB.y));
        const RgbImage img = cropImage(frame, r);
        const std::string path = snapshotPath(dir, base, p, n);
        std::string e;
        if (!writePng(path, img, e)) {
            LOG_ERROR("app", "snapshot %s: %s", snapPlotName(p), e.c_str());
            if (err.empty()) err = e;
            continue;
        }
        LOG_INFO("app", "snapshot %s written (%dx%d)", path.c_str(), img.w, img.h);
        names += std::string(names.empty() ? "" : ", ") + snapPlotName(p);
        ++saved;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG_DEBUG("app", "snapshots took %.0f ms", ms);
    snapNoteErr_ = !err.empty();
    if (snapNoteErr_)
        snapNoteMsg_ = "Snapshot failed: " + err;
    else
        snapNoteMsg_ = "Saved " + std::to_string(saved) + (saved == 1 ? " plot" : " plots") + " (" + names + ") as " +
                       base + "_capture_<plot>_" + std::to_string(n) + ".png in " + snapDir_;
    snapNoteUntil_ = nowSec() + 8;
}
