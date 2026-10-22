// Menu bar (issue #19, docs/design/ui-cleanup-plan.md): File / View / Tools / Help on
// the left; on the right the source and what runs in the background
// (transmitting, recording, audio, an export). Also the keyboard help and
// About windows.
#include <algorithm>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/build_info.h"
#include "gpu/gpu.h"
#include "app/platform_x11.h"
#include "common/ui_util.h"

namespace {
const ImVec4 kColRec(1.f, 0.32f, 0.32f, 1.f);

struct Key {
    const char* keys;
    const char* what;
};
const Key kFileKeys[] = {
    {"Left / Right", "Pan 10%"},
    {"PgUp / PgDn", "Back / forward one view"},
    {"Home / End", "Jump to the start / end"},
    {"+ / -", "Zoom in / out 2x"},
    {"A", "Show everything (the whole file or segment, full bandwidth)"},
    {"T", "Time markers"},
    {"Z", "Zoom to the time markers"},
    {"F", "Frequency markers"},
    {"C", "Channel filter on / off"},
    {"B", "Bookmark the view"},
    {"Esc", "Cancel a region selection"},
};
const Key kGlobalKeys[] = {
    {"Ctrl+E", "Export region window (file mode)"},
    {"Ctrl+G", "Signal generator window"},
    {"Ctrl+D", "Audio window"},
    {"Ctrl+Q", "Quit"},
};
const Key kTextKeys[] = {
    {"Ctrl+C / Ctrl+X", "Copy / cut the selected text to the system clipboard"},
    {"Ctrl+V", "Paste from the system clipboard"},
    {"Ctrl+A", "Select all the text in the field"},
};
const Key kMouse[] = {
    {"Wheel", "Zoom the plot under the mouse (over an axis: that axis only)"},
    {"Drag", "Pan"},
    {"Right-drag", "Zoom to a box"},
    {"Double-click", "Fit the plot"},
    {"Ctrl+click", "Tune the channel (spectrum and waterfall)"},
    {"Shift+click", "Play audio from there (waterfall and time plots)"},
    {"Right-click", "Waterfall: export menu. Time plot: its options."},
    {"Overview strip", "Click to jump, drag the box to move, drag its edges to resize"},
    {"Snapshot...", "Status bar: save the plots as PNG files"},
};

void keyTable(const char* id, const Key* keys, size_t n) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return;
    // The same key column width in every table, so they line up.
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed);
    for (size_t i = 0; i < n; ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(keys[i].keys);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(keys[i].what);
    }
    ImGui::EndTable();
}
}  // namespace

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    const bool rec = liveRecording();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open file...", nullptr, false, !rec)) {
            if (liveMode_) leaveLive();
            focusPath_ = true;
        }
        ImGui::SetItemTooltip("Type or paste the path in the File box and press Enter");
        if (ImGui::MenuItem("Export region...", "Ctrl+E", exOpen_, !liveMode_ && file_)) toggleExport();
        if (ImGui::MenuItem("Save plot snapshots...")) openSnapshot();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (!liveMode_) {
            ImGui::MenuItem("Time markers", "T", &tMarkers_);
            ImGui::SetItemTooltip("Two draggable time cursors shown on every plot");
        }
        ImGui::MenuItem("Frequency markers", "F", &fMarkers_);
        ImGui::SetItemTooltip("Two draggable frequency cursors on the spectrum and waterfall");
        ImGui::MenuItem("Relative frequency", nullptr, &relFreq_);
        ImGui::SetItemTooltip("Label frequency axes relative to the center frequency");
        ImGui::MenuItem("Peak hold", nullptr, &showPeak_);
        ImGui::SetItemTooltip("Show the peak-hold trace in the spectrum");
        if (!liveMode_) {
            ImGui::Separator();
            ImGui::MenuItem("Overview strip", nullptr, &showOverview_);
            ImGui::SetItemTooltip("The whole file's power over time, above the plots.\n"
                                  "Hidden by itself when the window is short.");
            ImGui::MenuItem("I / Q plot", nullptr, &showIQ_);
            ImGui::MenuItem("AM plot", nullptr, &showAM_);
            ImGui::MenuItem("FM plot", nullptr, &showFM_);
            ImGui::MenuItem("PM plot", nullptr, &showPM_);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show everything", "A")) showAll();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("Signal generator", "Ctrl+G", &sgOpen_);
        ImGui::MenuItem("Audio", "Ctrl+D", &auOpen_);
        if (gpuInfo().available) {
            ImGui::Separator();
            bool on = gpuEnabled();
            if (ImGui::MenuItem("Use the GPU", nullptr, &on)) {
                gpuSetEnabled(on);
                wfWorker_->invalidate();  // compute the view again on the new side
            }
            ImGui::SetItemTooltip("Compute the waterfall, the live spectrum and the channel filter on the %s.\n"
                                  "Off: use the CPU (to compare, or after a driver problem).",
                                  gpuInfo().model.c_str());
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard and mouse")) helpOpen_ = true;
        if (ImGui::MenuItem("About wvt")) aboutOpen_ = true;
        ImGui::EndMenu();
    }

    // --- right side: background activity, then the source ---
    const ImGuiStyle& st = ImGui::GetStyle();
    const bool txOn = tx_ && tx_->running();
    const bool auOn = audio_ && audio_->running();
    const Recorder::Status rs = rec ? live_->recorder().status() : Recorder::Status{};
    std::string recText, exText;
    if (rec) recText = "REC " + fmtSI(rs.seconds, "s", 3);
    if (exportBusy()) {
        char b[32];
        snprintf(b, sizeof b, "Export %.0f%%", exSt_.progress * 100);
        exText = b;
    }
    auto btnW = [&](const char* t) { return ImGui::CalcTextSize(t).x + st.FramePadding.x * 2 + st.ItemSpacing.x; };
    const float srcW = ImGui::CalcTextSize("Device").x + ImGui::GetFrameHeight() + st.FramePadding.x * 2;
    float w = ImGui::CalcTextSize("Source").x + st.ItemInnerSpacing.x + srcW;
    if (txOn) w += btnW("TX ON");
    if (rec) w += ImGui::CalcTextSize(recText.c_str()).x + st.ItemSpacing.x;
    if (auOn) w += btnW("Audio");
    if (!exText.empty()) w += btnW(exText.c_str());
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
    if (txOn) {
        if (redButton("TX ON")) sgOpen_ = true;
        tooltipAlways("The signal generator is transmitting. Click to open it.");
    }
    if (rec) {
        ImGui::TextColored(kColRec, "%s", recText.c_str());
        ImGui::SetItemTooltip("Recording to %s", rs.dataPath.c_str());
    }
    if (auOn) {
        if (ImGui::Button("Audio")) auOpen_ = true;
        tooltipAlways("Audio is playing. Click to open the audio window.");
    }
    if (!exText.empty()) {
        if (ImGui::Button(exText.c_str())) exOpen_ = true;
        tooltipAlways("An export is running. Click to open the export window.");
    }
    drawSourceSelector(srcW);
    ImGui::EndMenuBar();
}

void App::drawHelpWindows() {
    const float fs = ImGui::GetFontSize();
    if (helpOpen_) {
        ImGui::SetNextWindowSize(ImVec2(fs * 34, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Keyboard and mouse###help", &helpOpen_,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SeparatorText("File mode");
            keyTable("##kfile", kFileKeys, std::size(kFileKeys));
            ImGui::SeparatorText("Everywhere");
            keyTable("##kglobal", kGlobalKeys, std::size(kGlobalKeys));
            ImGui::SeparatorText("In a text field");
            keyTable("##ktext", kTextKeys, std::size(kTextKeys));
#ifdef __APPLE__
            ImGui::TextDisabled("The Command key also works as Ctrl.");
#endif
            ImGui::SeparatorText("Mouse");
            keyTable("##kmouse", kMouse, std::size(kMouse));
            ImGui::TextDisabled("Keys don't act while a text box has the focus.");
        }
        ImGui::End();
    }
    if (aboutOpen_) {
        if (ImGui::Begin("About wvt###about", &aboutOpen_,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("wvt - IQ waveform viewer");
            ImGui::Separator();
            ImGui::Text("Version: %s", buildVersionTag());  // blank on a commit with no version tag
            ImGui::Text("Build: %s", buildGitHash());
            const GpuInfo& g = gpuInfo();
            ImGui::Text("Renderer: %s", win_.usingGL() ? "GLX" : "software");
            if (!g.built) ImGui::TextUnformatted("GPU: not built in (CPU only)");
            else if (!g.available) ImGui::Text("GPU (%s): not used, %s", g.backend.c_str(), g.reason.c_str());
            else ImGui::Text("GPU (%s): %s, %s", g.backend.c_str(), g.name.c_str(), g.version.c_str());
            ImGui::Text("Dear ImGui %s, ImPlot %s", IMGUI_VERSION, IMPLOT_VERSION);
        }
        ImGui::End();
    }
}
