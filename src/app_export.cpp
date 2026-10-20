// Region export window and waterfall selection (issue #18,
// docs/region-export-plan.md): pick a time range and a band, check what will
// be written, and export it in the background.
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

#include "app.h"
#include "log.h"
#include "ui_util.h"

namespace {
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColOk(0.45f, 0.85f, 0.50f, 1.f);
const ImVec4 kColRegion(0.95f, 0.95f, 0.30f, 1.f);

enum ExportFill { FillView = 0, FillChannel, FillMarkers };

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}




std::string baseName(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
}  // namespace

bool App::exportBusy() const { return exJob_ && exSt_.active; }

ExtractConfig App::exportConfig() const {
    ExtractConfig c;
    c.src = Source{file_, fp_, fs_, fc_};
    c.s0 = (int64_t)std::llround(std::min(exT0_, exT1_) * fs_);
    c.s1 = (int64_t)std::llround(std::max(exT0_, exT1_) * fs_);
    c.limit0 = segS0_;
    c.limit1 = segS1_;
    c.fullBand = exFullBand_;
    c.fLo = std::min(exLo_, exHi_);
    c.fHi = std::max(exLo_, exHi_);
    c.ci16 = exCi16_;
    c.sigmf = exSigmf_;
    c.path = exPathAuto_ ? "" : exPath_;
    c.sourceStart = fileStart_;
    return c;
}

// Fills the region from the view, the channel or the markers.
void App::exportFrom(int what) {
    if (!file_) return;
    exOpen_ = true;
    exMsg_.clear();
    double t0 = std::max(tmin_, tBegin()), t1 = std::min(tmax_, tEnd());
    if (what == FillMarkers && tMarkers_) {
        t0 = std::clamp(std::min(tm1_, tm2_), tBegin(), tEnd());
        t1 = std::clamp(std::max(tm1_, tm2_), tBegin(), tEnd());
    }
    exT0_ = t0;
    exT1_ = t1;
    if (what == FillView) {
        exFullBand_ = true;
    } else if (what == FillChannel) {
        if (!demod_.chanEnable) {
            exMsg_ = "The channel filter is off: turn it on (C) to export its band.";
            exMsgErr_ = true;
            return;
        }
        exFullBand_ = false;
        exLo_ = fc_ + demod_.chanOffset - demod_.chanBw / 2;
        exHi_ = fc_ + demod_.chanOffset + demod_.chanBw / 2;
    } else if (what == FillMarkers) {
        exFullBand_ = !fMarkers_;
        if (fMarkers_) {
            exLo_ = std::min(fm1_, fm2_);
            exHi_ = std::max(fm1_, fm2_);
        }
        if (!tMarkers_ && !fMarkers_) {
            exMsg_ = "The time and frequency markers are off (T, F): used the view.";
            exMsgErr_ = false;
        }
    }
    static const char* names[] = {"view", "channel", "markers"};
    LOG_INFO("app", "export region from the %s: %s to %s, %s", names[what], fmtSI(exT0_, "s", 9).c_str(),
             fmtSI(exT1_, "s", 9).c_str(),
             exFullBand_ ? "full band" : (fmtSI(exLo_, "Hz", 9) + " to " + fmtSI(exHi_, "Hz", 9)).c_str());
}

void App::startExport(bool overwrite) {
    ExtractConfig c = exportConfig();
    c.path = exPath_;
    c.overwrite = overwrite;
    exJob_ = std::make_unique<ExtractJob>();
    std::string err;
    if (!exJob_->start(c, err)) {
        exJob_.reset();
        exMsg_ = "Export not started: " + err;
        exMsgErr_ = true;
        LOG_ERROR("extract", "%s", exMsg_.c_str());
        return;
    }
    exSt_ = exJob_->status();
    exMsg_.clear();
}

void App::pollExport() {
    if (!exJob_) return;
    const bool was = exSt_.active;
    exSt_ = exJob_->status();
    if (!was || exSt_.active) return;
    if (exSt_.cancelled) {
        exMsg_ = "Export cancelled; the partial file was deleted.";
        exMsgErr_ = false;
    } else if (!exSt_.error.empty()) {
        exMsg_ = "Export failed: " + exSt_.error;
        exMsgErr_ = true;
        LOG_ERROR("extract", "%s", exMsg_.c_str());
    } else {
        exMsg_ = "Wrote " + baseName(exSt_.dataPath) + " (" + fmtSI((double)exSt_.bytes, "B", 4) + ", " +
                 fmtSI(exSt_.seconds, "s", 3) + (exSt_.gpu ? ", filter on the GPU" : "") + ")";
        if (exSt_.clipped > 0) exMsg_ += "; " + std::to_string(exSt_.clipped) + " ci16 values clipped";
        exMsgErr_ = exSt_.clipped > 0;
        status_ = exMsg_;
    }
    exPlanAt_ = 0;  // the file now exists: plan again
}

// ---------------------------------------------------------------------------
// Toolbar button & window
// ---------------------------------------------------------------------------
void App::drawExportButton() {
    if (liveMode_) return;
    ImGui::SameLine();
    const bool busy = exportBusy();
    ImGui::BeginDisabled(!file_);
    if (ImGui::Button(busy ? "Export *" : "Export")) {
        exOpen_ = !exOpen_;
        if (exOpen_ && exT1_ <= exT0_) exportFrom(FillView);
    }
    ImGui::EndDisabled();
    tooltipAlways(busy ? "An export is running. Click to open or close the export window (Ctrl+E)."
                       : "Export a time range and a band of the file to a new file (Ctrl+E).\n"
                         "Right-click the waterfall to select the region with the mouse.");
}

void App::drawExportWindow() {
    pollExport();
    if (!exOpen_ || liveMode_) return;
    const float fs = ImGui::GetFontSize();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(fs * 30, 0), ImGuiCond_FirstUseEver);
    // On the right, left of the audio window when that is open.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - fs * 31 - (auOpen_ ? fs * 31 : 0), vp->WorkPos.y + fs * 5),
                            ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Export region###export", &exOpen_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (!file_) {
        ImGui::TextDisabled("Open a file first.");
        ImGui::End();
        return;
    }
    const bool busy = exportBusy();
    ImGui::BeginDisabled(busy);

    // --- fill from ---
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Fill from");
    ImGui::SameLine();
    if (ImGui::Button("View")) exportFrom(FillView);
    tooltipAlways("The time range of the view, full band");
    ImGui::SameLine();
    if (ImGui::Button("Channel")) exportFrom(FillChannel);
    tooltipAlways("The channel filter's band (offset and width), over the view's time range");
    ImGui::SameLine();
    if (ImGui::Button("Markers")) exportFrom(FillMarkers);
    tooltipAlways("The time markers (T) and frequency markers (F), where they are on");
    ImGui::SameLine();
    if (ImGui::Button("Select...")) exSelecting_ = true;
    tooltipAlways("Drag a box on the waterfall (Esc cancels)");

    if (beginForm("##exform", 6.5f)) {
        // --- time ---
        const char* tTip = "Time range in seconds of file time, inside the loaded segment.";
        if (exInSamples_) {
            double s0 = std::llround(exT0_ * fs_), s1 = std::llround(exT1_ * fs_);
            row("Start", tTip);
            if (InputSI("##exs0", &s0, "", -FLT_MIN)) exT0_ = std::round(s0) / fs_;
            row("End", tTip);
            if (InputSI("##exs1", &s1, "", -FLT_MIN)) exT1_ = std::round(s1) / fs_;
        } else {
            row("Start", tTip);
            InputSI("##ext0", &exT0_, "s", -FLT_MIN);
            row("End", tTip);
            InputSI("##ext1", &exT1_, "s", -FLT_MIN);
        }
        row("");
        ImGui::Checkbox("in samples", &exInSamples_);
        ImGui::SameLine();
        const double len = std::fabs(exT1_ - exT0_);
        ImGui::TextDisabled("length %s, %s samples", fmtSI(len, "s", 6).c_str(), fmtSI(std::round(len * fs_), "", 6).c_str());

        // --- band ---
        row("Band");
        ImGui::Checkbox("Full band", &exFullBand_);
        tooltipAlways("On: copy the samples unchanged (same format, rate and center frequency).\n"
                      "Off: only the band below, mixed to 0 Hz, filtered and decimated.");
        if (!exFullBand_) {
            ImGui::SameLine();
            ImGui::Checkbox("center / width", &exCenterBw_);
            if (exCenterBw_) {
                double c = 0.5 * (exLo_ + exHi_), w = std::fabs(exHi_ - exLo_);
                bool ch = false;
                row("Center");
                ch |= InputSI("##exc", &c, "Hz", -FLT_MIN);
                row("Width");
                ch |= InputSI("##exw", &w, "Hz", -FLT_MIN);
                if (ch) {
                    exLo_ = c - std::fabs(w) / 2;
                    exHi_ = c + std::fabs(w) / 2;
                }
            } else {
                row("Low");
                InputSI("##exlo", &exLo_, "Hz", -FLT_MIN);
                row("High");
                InputSI("##exhi", &exHi_, "Hz", -FLT_MIN);
            }
        }

        // --- output ---
        row("Format");
        if (exFullBand_) {
            ImGui::TextDisabled("%s (the source format)", formatName(fp_.fmt));
        } else {
            int f = exCi16_ ? 1 : 0;
            ImGui::RadioButton("cf32", &f, 0);
            ImGui::SameLine();
            ImGui::RadioButton("ci16", &f, 1);
            exCi16_ = f == 1;
            tooltipAlways("ci16 is half the size; values above full scale are clipped (counted).");
        }
        row("File type");
        int t = exSigmf_ ? 0 : 1;
        ImGui::RadioButton("SigMF", &t, 0);
        ImGui::SameLine();
        ImGui::RadioButton("raw", &t, 1);
        exSigmf_ = t == 0;
        tooltipAlways("SigMF: .sigmf-data + .sigmf-meta with the rate, frequency and source.\n"
                      "raw: the samples only; the name carries the rate and frequency.");

        // The plan: again when the region changes, and each second (free space, existing files).
        ExtractConfig c = exportConfig();
        if (!(c == exPlanCfg_) || nowSec() - exPlanAt_ > 1.0) {
            exPlanCfg_ = c;
            exPlanAt_ = nowSec();
            exPlan_ = planExtract(c);
            if (exPathAuto_ && exPlan_.rate > 0) {
                snprintf(exPath_, sizeof exPath_, "%s",
                         defaultExtractPath(file_->path(), std::min(exT0_, exT1_), exPlan_.center, exPlan_.rate).c_str());
                c.path = exPath_;
                exPlan_ = planExtract(c);
            }
        }
        row("File", "Output path without the extension. Edit it to choose your own name.");
        if (ImGui::InputText("##expath", exPath_, sizeof exPath_)) {
            exPathAuto_ = false;
            exPlanAt_ = 0;
        }
        if (!exPathAuto_) {
            row("");
            if (ImGui::SmallButton("Default name")) {
                exPathAuto_ = true;
                exPlanAt_ = 0;
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    // --- what will be written ---
    ImGui::Separator();
    const ExtractPlan& p = exPlan_;
    if (!p.err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kColErr);
        ImGui::TextWrapped("%s", p.err.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%s at %s, %s, %s samples, %s", fmtSI(p.rate, "S/s", 6).c_str(), fmtSI(p.center, "Hz", 9).c_str(),
                            formatName(p.fmt), fmtSI((double)p.samples, "", 6).c_str(), fmtSI(p.bytes, "B", 4).c_str());
        if (exFullBand_) ImGui::TextDisabled("Copy of the samples, unchanged");
        else
            ImGui::TextDisabled("Mixed by %s, %d taps, decimation %d%s", fmtSI(-p.offset, "Hz", 6).c_str(), p.taps, p.decim,
                                gpuChannelUsable() ? " (GPU)" : "");
        ImGui::TextDisabled("%s", baseName(p.dataPath).c_str());
        if (p.exists) ImGui::TextColored(kColErr, "This file exists; Export asks before replacing it.");
    }

    // --- export / progress ---
    if (busy) {
        char txt[64];
        snprintf(txt, sizeof txt, "%.0f %%", exSt_.progress * 100);
        ImGui::ProgressBar((float)exSt_.progress, ImVec2(fs * 18, 0), txt);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            exJob_->cancel();
            pollExport();
        }
        tooltipAlways("Stop the export and delete the partial file");
    } else {
        ImGui::BeginDisabled(!p.err.empty());
        if (ImGui::Button("Export", ImVec2(fs * 6, 0))) {
            if (p.exists) ImGui::OpenPopup("Replace?###exreplace");
            else startExport(false);
        }
        ImGui::EndDisabled();
    }
    if (ImGui::BeginPopupModal("Replace?###exreplace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s exists. Replace it?", baseName(p.dataPath).c_str());
        if (ImGui::Button("Replace")) {
            startExport(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!exMsg_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, exMsgErr_ ? kColErr : kColOk);
        ImGui::TextWrapped("%s", exMsg_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Waterfall: menu, crosshair selection, region
// ---------------------------------------------------------------------------
// Inside the waterfall plot. A right-click without a drag asks for the menu
// (right-drag stays the box zoom). In crosshair mode a left-drag draws the
// region; the axes are locked meanwhile (see drawLeft).
void App::waterfallExportInput(bool& openMenu) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImPlot::IsPlotHovered();
    if (hovered && !exSelecting_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < io.MouseDragThreshold * io.MouseDragThreshold)
        openMenu = true;
    if (!exSelecting_) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        exSelecting_ = exDragging_ = false;
        return;
    }
    const ImPlotPoint m = ImPlot::GetPlotMousePos();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    if (hovered) {
        // A crosshair (the X11 layer does not change the system cursor).
        const ImVec2 pm = ImPlot::PlotToPixels(m), lo = ImPlot::GetPlotPos(), sz = ImPlot::GetPlotSize();
        const ImU32 col = ImGui::GetColorU32(kColRegion);
        dl->AddLine(ImVec2(lo.x, pm.y), ImVec2(lo.x + sz.x, pm.y), col);
        dl->AddLine(ImVec2(pm.x, lo.y), ImVec2(pm.x, lo.y + sz.y), col);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            exDragging_ = true;
            exDragT_ = m.y;
            exDragF_ = m.x;
        }
    }
    if (exDragging_) {
        const ImVec2 a = ImPlot::PlotToPixels(exDragF_, exDragT_), b = ImPlot::PlotToPixels(m);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(kColRegion.x, kColRegion.y, kColRegion.z, 0.15f)));
        dl->AddRect(a, b, ImGui::GetColorU32(kColRegion), 0, 0, 1.5f);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            exDragging_ = exSelecting_ = false;
            if (m.y != exDragT_ && m.x != exDragF_) {
                exT0_ = std::clamp(std::min(m.y, exDragT_), tBegin(), tEnd());
                exT1_ = std::clamp(std::max(m.y, exDragT_), tBegin(), tEnd());
                exLo_ = std::clamp(std::min(m.x, exDragF_), fc_ - fs_ / 2, fc_ + fs_ / 2);
                exHi_ = std::clamp(std::max(m.x, exDragF_), fc_ - fs_ / 2, fc_ + fs_ / 2);
                exFullBand_ = false;
                exOpen_ = true;
                exMsg_.clear();
                LOG_INFO("app", "export region selected: %s to %s, %s to %s", fmtSI(exT0_, "s", 9).c_str(),
                         fmtSI(exT1_, "s", 9).c_str(), fmtSI(exLo_, "Hz", 9).c_str(), fmtSI(exHi_, "Hz", 9).c_str());
            }
        }
    }
    ImPlot::PopPlotClipRect();
}

void App::drawWaterfallMenu() {
    if (!ImGui::BeginPopup("##wfmenu")) return;
    ImGui::BeginDisabled(liveMode_ || !file_ || exportBusy());
    if (ImGui::MenuItem("Select a region to export...")) exSelecting_ = true;
    if (ImGui::MenuItem("Export this view...")) exportFrom(FillView);
    if (ImGui::MenuItem("Export the channel...", nullptr, false, demod_.chanEnable)) exportFrom(FillChannel);
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Show everything", "A")) {
        setView(tBegin(), tEnd());
        fmin_ = fc_ - fs_ / 2;
        fmax_ = fc_ + fs_ / 2;
    }
    ImGui::EndPopup();
}

// The region while the export window is open: on the waterfall a draggable
// box (or two time lines for a full-band export), on a time plot a shaded
// time range.
void App::drawExportRegion(bool waterfall) {
    if (!exOpen_ || liveMode_ || !file_ || exSelecting_) return;
    const ImVec4 col = kColRegion;
    if (!waterfall) {
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        const ImVec2 a = ImPlot::PlotToPixels(std::min(exT0_, exT1_), lim.Y.Max), b = ImPlot::PlotToPixels(std::max(exT0_, exT1_), lim.Y.Min);
        ImPlot::PushPlotClipRect();
        ImPlot::GetPlotDrawList()->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.08f)));
        ImPlot::PopPlotClipRect();
        return;
    }
    if (exportBusy()) {
        // Show it, but it cannot move while it is being written.
        const ImVec2 a = ImPlot::PlotToPixels(exFullBand_ ? fc_ - fs_ / 2 : exLo_, exT0_),
                     b = ImPlot::PlotToPixels(exFullBand_ ? fc_ + fs_ / 2 : exHi_, exT1_);
        ImPlot::PushPlotClipRect();
        ImPlot::GetPlotDrawList()->AddRect(a, b, ImGui::GetColorU32(col), 0, 0, 1.5f);
        ImPlot::PopPlotClipRect();
        return;
    }
    if (exFullBand_) {
        ImPlot::DragLineY(60, &exT0_, col, 1.5f);
        ImPlot::DragLineY(61, &exT1_, col, 1.5f);
    } else {
        ImPlot::DragRect(62, &exLo_, &exT0_, &exHi_, &exT1_, col);
        exLo_ = std::clamp(exLo_, fc_ - fs_ / 2, fc_ + fs_ / 2);
        exHi_ = std::clamp(exHi_, fc_ - fs_ / 2, fc_ + fs_ / 2);
    }
    exT0_ = std::clamp(exT0_, tBegin(), tEnd());
    exT1_ = std::clamp(exT1_, tBegin(), tEnd());
}
