// Signal generator: chain playback (issue #27, docs/design/siggen-chain-plan.md).
// A list of recordings that play in turn, each with its own segment, offset,
// level, repeats and gap, looped forever or a set number of times.
#include <algorithm>
#include <cmath>
#include <string>

#include "app/app.h"
#include "io/recorder.h"
#include "common/ui_util.h"

namespace {
const ImVec4 kColWarn(1.f, 0.8f, 0.3f, 1.f);
const ImVec4 kColErr(1.f, 0.42f, 0.36f, 1.f);
const ImVec4 kColEnv(0.40f, 0.90f, 0.50f, 1.f);
// Item shading in the preview, in turn.
const ImU32 kItemCols[] = {IM_COL32(90, 160, 255, 48), IM_COL32(255, 160, 70, 48), IM_COL32(190, 120, 255, 48),
                           IM_COL32(90, 220, 200, 48)};

std::string baseName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

std::string trimmed(const char* s) {
    std::string p = s;
    while (!p.empty() && std::isspace((unsigned char)p.back())) p.pop_back();
    while (!p.empty() && std::isspace((unsigned char)p.front())) p.erase(0, 1);
    return p;
}
}  // namespace

bool App::chainLoaded(int* loaded) const {
    int n = 0;
    for (size_t i = 0; i < sg_.chain.size(); ++i) n += cachedPlayback(chainItemParams(sg_, i)) != nullptr;
    if (loaded) *loaded = n;
    return !sg_.chain.empty() && n == (int)sg_.chain.size();
}

void App::addChainItem(const std::string& path, bool fromViewer, bool markers) {
    if (path.empty()) return;
    WaveformParams& p = sg_;
    const bool txOn = tx_ && tx_->running();
    const bool lockRate = txOn || sgFileSt_.active || (sigGenLoopback() && liveRecording());
    // The first item sets the generator's rate and center, so it plays as recorded.
    if (p.chain.empty()) {
        const FileMetaGuess g = guessMeta(expandHome(path));
        const bool haveRate = fromViewer || g.haveRate, haveFc = fromViewer || g.haveFc;
        const double rate = fromViewer ? fs_ : g.rate, fc = fromViewer ? fc_ : g.fc;
        if (haveRate && !lockRate && p.rate != rate) {
            p.rate = rate;
            if (sigGenLoopback()) startLoopback();
        }
        if (haveFc && !txOn) p.fc = fc;
    }
    if (std::string err; !fromViewer && !metaReadable(guessMeta(expandHome(path)), err)) {
        sgChainNote_ = err;
        sgChainNoteErr_ = true;
        LOG_ERROR("app", "chain: %s", err.c_str());
        return;
    }
    ChainItem c = makeChainItem(path, p, &sgChainNote_);
    if (fromViewer) {  // read it the way the viewer does, overrides included
        c.fp = fp_;
        c.fileRate = fs_;
        c.offset = std::fabs(fc_ - p.fc) < p.rate / 2 ? fc_ - p.fc : 0;
        sgChainNote_ = "settings from the viewer";
        if (markers) {
            c.start = std::max(0.0, std::min(tm1_, tm2_));
            c.len = std::fabs(tm2_ - tm1_);
        }
    }
    sgChainNoteErr_ = false;
    p.chain.push_back(c);
    sgChainSel_ = (int)p.chain.size() - 1;
}

void App::drawSigGenChain() {
    WaveformParams& p = sg_;
    if (!ImGui::CollapsingHeader("Chain", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const ImGuiStyle& st = ImGui::GetStyle();
    const auto r = sgPrev_ && sgPrev_->req.p.chain == p.chain && sgPrev_->req.p.rate == p.rate ? sgPrev_ : nullptr;

    // --- the items ---
    int move = -1, dir = 0, remove = -1;
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_ScrollY;
    const float rowH = ImGui::GetFrameHeight();
    const float tableH = (rowH + st.CellPadding.y * 2) * (float)(std::clamp<int>((int)p.chain.size(), 1, 8) + 1) + 2;
    if (ImGui::BeginTable("##chain", 8, tf, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Length");
        ImGui::TableSetupColumn("Offset");
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Plays");
        ImGui::TableSetupColumn("Gap");
        ImGui::TableSetupColumn("##btns");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)p.chain.size(); ++i) {
            const ChainItem& c = p.chain[(size_t)i];
            const SigPreviewRes::ChainInfo* info = r && i < (int)r->chainItems.size() ? &r->chainItems[(size_t)i] : nullptr;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            char num[16];
            snprintf(num, sizeof num, "%d", i + 1);
            if (ImGui::Selectable(num, sgChainSel_ == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, rowH)))
                sgChainSel_ = sgChainSel_ == i ? -1 : i;
            ImGui::TableSetColumnIndex(1);
            const std::string name = baseName(c.path);
            const float nameW = ImGui::GetContentRegionAvail().x;
            if (info && !info->error.empty()) {
                ImGui::TextColored(kColErr, "%s", elideMiddle(name, nameW).c_str());
                tooltipAlways((c.path + "\n" + info->error).c_str());
            } else {
                ImGui::TextUnformatted(elideMiddle(name, nameW).c_str());
                tooltipAlways(c.path.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            if (info && info->error.empty()) ImGui::TextUnformatted(fmtSI(info->seconds, "s", 3).c_str());
            else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(fmtSI(c.offset, "Hz", 4).c_str());
            ImGui::TableSetColumnIndex(4);
            if (c.keepLevel) ImGui::TextDisabled("as rec.");
            else ImGui::Text("%.1f dBFS", c.ampDb);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", c.repeats);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(c.gapAfter > 0 ? fmtSI(c.gapAfter, "s", 3).c_str() : "0");
            ImGui::TableSetColumnIndex(7);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) move = i, dir = -1;
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(i + 1 == (int)p.chain.size());
            if (ImGui::SmallButton("v")) move = i, dir = 1;
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::SetItemTooltip("Remove from the chain");
            ImGui::PopID();
        }
        if (p.chain.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("no recordings yet: add one below");
        }
        ImGui::EndTable();
    }
    if (move >= 0) {
        std::swap(p.chain[(size_t)move], p.chain[(size_t)(move + dir)]);
        if (sgChainSel_ == move) sgChainSel_ = move + dir;
        else if (sgChainSel_ == move + dir) sgChainSel_ = move;
    }
    if (remove >= 0) {
        p.chain.erase(p.chain.begin() + remove);
        if (sgChainSel_ == remove) sgChainSel_ = -1;
        else if (sgChainSel_ > remove) --sgChainSel_;
    }
    if (sgChainSel_ >= (int)p.chain.size()) sgChainSel_ = -1;

    // --- the selected item ---
    if (sgChainSel_ >= 0) {
        const SigPreviewRes::ChainInfo* info =
            r && sgChainSel_ < (int)r->chainItems.size() ? &r->chainItems[(size_t)sgChainSel_] : nullptr;
        drawChainItem(p.chain[(size_t)sgChainSel_], info);
    } else if (!p.chain.empty()) {
        ImGui::TextDisabled("Click a row to edit its segment, offset, level, plays and gap.");
    }

    // --- adding, the loop, chain files ---
    if (beginForm("##chainadd", 8.5f)) {
        row("Add", "A SigMF (.sigmf-meta / .sigmf-data) or raw IQ file. Enter or Add reads its metadata.\n"
                   "The first recording also sets the generator's rate and center frequency.");
        const float addW = ImGui::CalcTextSize("Add").x + st.FramePadding.x * 2;
        ImGui::SetNextItemWidth(-(addW + st.ItemInnerSpacing.x));
        bool add = ImGui::InputText("##chainaddpath", sgChainAdd_, sizeof sgChainAdd_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        add |= ImGui::Button("Add##chain");
        if (add) {
            addChainItem(trimmed(sgChainAdd_));
            sgChainAdd_[0] = 0;
        }
        const bool viewerFile = file_ && !liveMode_;
        if (!sgLastFile_.empty() || viewerFile) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (!sgLastFile_.empty()) {
                if (ImGui::SmallButton("Last generated")) addChainItem(sgLastFile_);
                tooltipAlways(sgLastFile_.c_str());
                ImGui::SameLine();
            }
            if (viewerFile) {
                if (ImGui::SmallButton("Viewer's file")) addChainItem(file_->path(), true);
                tooltipAlways("Add the file open in the main window, read with the viewer's format and rate");
                ImGui::SameLine();
                if (ImGui::SmallButton("Viewer's markers")) addChainItem(file_->path(), true, true);
                tooltipAlways("Add the part of the viewer's file between its time markers (T)");
            }
        }
        if (!sgChainNote_.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushTextWrapPos(0);
            if (sgChainNoteErr_) ImGui::TextColored(kColErr, "%s", sgChainNote_.c_str());
            else ImGui::TextDisabled("%s", sgChainNote_.c_str());
            ImGui::PopTextWrapPos();
        }

        row("Loop", "Play the chain until the output stops, or this many times. A transmitter stops by itself "
                    "after the last pass.");
        int forever = p.chainLoops == 0 ? 1 : 0;
        if (ImGui::RadioButton("Forever", &forever, 1)) p.chainLoops = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Times", &forever, 0) && p.chainLoops == 0) p.chainLoops = 3;
        ImGui::SameLine();
        ImGui::BeginDisabled(p.chainLoops == 0);
        int n = std::max(1, p.chainLoops);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("##chainloops", &n) && p.chainLoops > 0) p.chainLoops = std::max(1, n);
        ImGui::EndDisabled();

        row("Start delay", "Silence before the first pass");
        InputSI("##chaindelay", &p.startDelay, "s", -FLT_MIN);

        row("Chain file", "Save the chain (recordings, settings, loop count, rate and center) to a .wvtchain.json\n"
                          "file, or load one. Recordings next to the chain file are stored relative to it.");
        const float bw = ImGui::CalcTextSize("Load").x + ImGui::CalcTextSize("Save").x + st.FramePadding.x * 4;
        ImGui::SetNextItemWidth(-(bw + st.ItemInnerSpacing.x * 2));
        ImGui::InputTextWithHint("##chainfile", "~/caps/my.wvtchain.json", sgChainFile_, sizeof sgChainFile_);
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        if (ImGui::Button("Load##chainfile")) {
            WaveformParams q = p;
            std::string err;
            const bool txOn = tx_ && tx_->running();
            const bool lockRate = txOn || sgFileSt_.active || (sigGenLoopback() && liveRecording());
            if (loadChain(trimmed(sgChainFile_), q, err)) {
                if (lockRate) q.rate = p.rate;
                if (txOn) q.fc = p.fc;
                const bool rateChanged = q.rate != p.rate;
                p = std::move(q);
                if (rateChanged && sigGenLoopback()) startLoopback();
                sgChainSel_ = -1;
                sgChainNote_ = "Loaded " + std::to_string(p.chain.size()) + " recordings from " + baseName(trimmed(sgChainFile_));
                sgChainNoteErr_ = false;
            } else {
                sgChainNote_ = err;
                sgChainNoteErr_ = true;
            }
        }
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        ImGui::BeginDisabled(p.chain.empty() || !sgChainFile_[0]);
        if (ImGui::Button("Save##chainfile")) {
            std::string path = trimmed(sgChainFile_), err;
            if (path.find(".json") == std::string::npos) path += ".wvtchain.json";
            if (saveChain(path, p, err)) {
                snprintf(sgChainFile_, sizeof sgChainFile_, "%s", path.c_str());
                sgChainNote_ = "Saved " + path;
                sgChainNoteErr_ = false;
            } else {
                sgChainNote_ = err;
                sgChainNoteErr_ = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    // --- totals ---
    if (r && r->error.empty() && !r->chainItems.empty())
        ImGui::TextDisabled("%s per pass, %s loaded (%s as cf32)", fmtSI(r->passSeconds, "s", 4).c_str(),
                            fmtSI((double)r->chainSamples, "S", 3).c_str(), fmtSI((double)r->chainSamples * 8, "B", 3).c_str());
}

void App::drawChainItem(ChainItem& c, const SigPreviewRes::ChainInfo* info) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::SeparatorText(("Item " + std::to_string(sgChainSel_ + 1) + ": " + baseName(c.path)).c_str());
    if (!beginForm("##chainitem", 8.5f)) return;
    const float half = (ImGui::GetContentRegionAvail().x - st.ItemInnerSpacing.x) / 2;

    row("Format", "Sample format of the recording");
    if (ImGui::BeginCombo("##chfmt", (std::string(formatName(c.fp.fmt)) + (c.fp.bigEndian ? " BE" : "")).c_str())) {
        for (int i = 0; i < (int)SampleFormat::Count; ++i)
            if (ImGui::Selectable(formatName((SampleFormat)i), (int)c.fp.fmt == i)) c.fp.fmt = (SampleFormat)i;
        ImGui::Separator();
        ImGui::Checkbox("Big-endian", &c.fp.bigEndian);
        ImGui::Checkbox("Swap I/Q", &c.fp.swapIQ);
        ImGui::EndCombo();
    }

    row("File rate", "The recording's sample rate. If it differs from the output rate, the recording is resampled.");
    double fr = c.fileRate > 0 ? c.fileRate : sg_.rate;
    if (InputSI("##chrate", &fr, "Sps", -FLT_MIN) && fr > 0) c.fileRate = fr;

    row("Segment", "Start and length of the part to play, in the recording's time. Length 0 = to the end.");
    InputSI("##ch0", &c.start, "s", half);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    InputSI("##chlen", &c.len, "s", -FLT_MIN);

    row("Offset", "Where the recording's 0 Hz lands, from the center frequency. Adding a recording puts it at "
                  "its recorded frequency when that is inside the band.");
    InputSI("##choff", &c.offset, "Hz", -FLT_MIN);

    row("Level", "As recorded: play the samples as they are. Otherwise the recording's peak plays at this level.");
    ImGui::Checkbox("As recorded##chkeep", &c.keepLevel);
    ImGui::SameLine();
    ImGui::BeginDisabled(c.keepLevel);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputDouble("##champ", &c.ampDb, 1, 6, "%.1f dBFS");
    ImGui::EndDisabled();

    row("Plays", "How many times in a row it plays, each followed by the gap");
    if (ImGui::InputInt("##chrep", &c.repeats)) c.repeats = std::max(1, c.repeats);

    row("Gap after", "Silence after each play");
    InputSI("##chgap", &c.gapAfter, "s", -FLT_MIN);

    if (info) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0);
        if (!info->error.empty()) {
            ImGui::TextColored(kColErr, "%s", info->error.c_str());
        } else {
            ImGui::TextDisabled("%s of %s, recorded peak %.1f dBFS", fmtSI(info->seconds, "s", 4).c_str(),
                                fmtSI(info->fileSeconds, "s", 4).c_str(), info->peakDb);
            if (info->outPeakDb > 0)
                ImGui::TextColored(kColWarn, "Plays up to %+.1f dBFS: lower its level to avoid clipping", info->outPeakDb);
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
}

// One pass: the envelope, with each play shaded and numbered.
void App::drawChainPreview(const SigPreviewRes& r) {
    static const char* kUnitS = "s";
    if (!ImPlot::BeginPlot("##sgchain", ImVec2(-1, ImGui::GetFontSize() * 9), ImPlotFlags_NoMenus)) return;
    double top = 0;
    for (double m : r.mag) top = std::max(top, m);
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, 0);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, std::max(top, 1e-3) * 1.15, ImPlotCond_Always);  // from 0: levels compare
    ImPlot::SetupAxisFormat(ImAxis_X1, formatValueSI, (void*)kUnitS);
    ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
    const double rate = r.req.p.rate;
    const size_t shown = std::min<size_t>(r.chainPlays.size(), 400);
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    auto box = [&](const SigGen::Pulse& pl, ImVec2& a, ImVec2& b) {
        a = ImPlot::PlotToPixels((double)pl.start / rate, lim.Y.Max);
        b = ImPlot::PlotToPixels((double)(pl.start + pl.len) / rate, lim.Y.Min);
    };
    ImPlot::PushPlotClipRect();
    for (size_t k = 0; k < shown; ++k) {
        ImVec2 a, b;
        box(r.chainPlays[k], a, b);
        ImPlot::GetPlotDrawList()->AddRectFilled(a, b, kItemCols[(size_t)std::max(0, r.chainPlays[k].item) % std::size(kItemCols)]);
    }
    ImPlot::PopPlotClipRect();
    ImPlot::SetNextLineStyle(kColEnv, 1.5f);
    ImPlot::PlotLine("|x| (peak)", r.t.data(), r.mag.data(), (int)r.t.size());
    // The item numbers go over the trace.
    ImPlot::PushPlotClipRect();
    for (size_t k = 0; k < shown; ++k) {
        ImVec2 a, b;
        box(r.chainPlays[k], a, b);
        if (b.x - a.x < ImGui::GetFontSize()) continue;
        const std::string label = std::to_string(r.chainPlays[k].item + 1);
        ImPlot::GetPlotDrawList()->AddText(ImVec2(a.x + 3, a.y + 2), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    }
    ImPlot::PopPlotClipRect();
    ImPlot::EndPlot();
}
