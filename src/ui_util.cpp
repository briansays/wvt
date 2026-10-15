#include "ui_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "implot.h"

namespace {
struct Prefix {
    double scale;
    const char* sym;
};
const Prefix kPrefixes[] = {{1e12, "T"}, {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""},
                            {1e-3, "m"}, {1e-6, "u"}, {1e-9, "n"}, {1e-12, "p"}};

const Prefix& pickPrefix(double mag) {
    if (!(mag > 0) || !std::isfinite(mag)) return kPrefixes[4];
    for (const auto& p : kPrefixes)
        if (mag >= p.scale * 0.9999999) return p;
    return kPrefixes[8];
}

void trimZeros(char* s) {
    char* e = strchr(s, 'e');
    if (e || !strchr(s, '.')) return;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '0') s[--n] = 0;
    if (n > 0 && s[n - 1] == '.') s[--n] = 0;
}
}  // namespace

std::string fmtSI(double v, const char* unit, int sig) {
    if (!std::isfinite(v)) return "--";
    const Prefix& p = pickPrefix(std::fabs(v));
    char num[64];
    snprintf(num, sizeof num, "%.*f", std::max(0, sig - 1 - (int)std::floor(std::log10(std::max(1e-300, std::fabs(v / p.scale))))), v / p.scale);
    if (v == 0) snprintf(num, sizeof num, "0");
    trimZeros(num);
    char out[96];
    snprintf(out, sizeof out, "%s %s%s", num, p.sym, unit ? unit : "");
    std::string s(out);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool parseSI(const char* s, double& out) {
    while (*s && std::isspace((unsigned char)*s)) ++s;
    if (!*s) return false;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return false;
    while (*end && std::isspace((unsigned char)*end)) ++end;
    double mult = 1;
    switch (*end) {
        case 'T': mult = 1e12; break;
        case 'G': case 'g': mult = 1e9; break;
        case 'M': mult = 1e6; break;
        case 'k': case 'K': mult = 1e3; break;
        case 'm': mult = 1e-3; break;
        case 'u': mult = 1e-6; break;
        case 'n': mult = 1e-9; break;
        case 'p': mult = 1e-12; break;
        default: break;
    }
    if ((unsigned char)end[0] == 0xC2 && (unsigned char)end[1] == 0xB5) mult = 1e-6;  // UTF-8 micro sign
    out = v * mult;
    return std::isfinite(out);
}

bool InputSI(const char* label, double* v, const char* unit, float width, int sig) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", fmtSI(*v, unit, sig).c_str());
    ImGui::SetNextItemWidth(width);
    bool enter = ImGui::InputText(label, buf, sizeof buf,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                      ImGuiInputTextFlags_CharsNoBlank);
    bool commit = enter || ImGui::IsItemDeactivatedAfterEdit();
    if (commit) {
        double nv;
        if (parseSI(buf, nv) && nv != *v) {
            *v = nv;
            return true;
        }
    }
    return false;
}

int formatAxisSI(double value, char* buf, int size, void* user) {
    const AxisFmt* a = (const AxisFmt*)user;
    double lo = *a->lo - a->offset, hi = *a->hi - a->offset;
    double mag = std::max(std::fabs(lo), std::fabs(hi));
    double span = std::fabs(hi - lo);
    const Prefix& p = pickPrefix(mag);
    double spanScaled = span / p.scale;
    int dec = spanScaled > 0 ? (int)std::ceil(-std::log10(spanScaled / 6.0)) : 0;
    dec = std::clamp(dec, 0, 9);
    double v = (value - a->offset) / p.scale;
    if (std::fabs(v) < std::pow(10.0, -dec) * 0.5) v = 0;  // avoid "-0"
    return snprintf(buf, size, "%.*f %s%s", dec, v, p.sym, a->unit);
}

int formatValueSI(double value, char* buf, int size, void* unit) {
    std::string s = fmtSI(value, (const char*)unit, 4);
    return snprintf(buf, size, "%s", s.c_str());
}

bool ScrollbarH(const char* id, double total, double& start, double span, float width, float height) {
    ImGui::PushID(id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sb", ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), ImGui::GetColorU32(ImGuiCol_ScrollbarBg),
                      st.ScrollbarRounding);
    bool changed = false;
    if (total > 0 && width > 4) {
        double frac = std::clamp(span / total, 0.0, 1.0);
        float thumbW = std::max(10.f, (float)(frac * width));
        float travel = width - thumbW;
        double maxStart = std::max(0.0, total - span);
        float tx = p0.x + (maxStart > 0 ? (float)(start / maxStart) * travel : 0.f);
        tx = std::clamp(tx, p0.x, p0.x + travel);
        ImGuiStorage* stg = ImGui::GetStateStorage();
        ImGuiID grabId = ImGui::GetID("grab");
        float mx = ImGui::GetIO().MousePos.x;
        if (ImGui::IsItemActivated()) {
            if (mx >= tx && mx <= tx + thumbW) {
                stg->SetFloat(grabId, mx - tx);  // grabbed the thumb
            } else {
                // click on the track: page toward the click
                start += (mx < tx ? -1 : 1) * span;
                stg->SetFloat(grabId, -1.f);
                changed = true;
            }
        }
        if (active && ImGui::IsMouseDragging(0, 0.f)) {
            float g = stg->GetFloat(grabId, -1.f);
            if (g >= 0 && travel > 0) {
                float nx = std::clamp(mx - g, p0.x, p0.x + travel);
                start = (nx - p0.x) / travel * maxStart;
                changed = true;
            }
        }
        if (hovered && ImGui::GetIO().MouseWheel != 0) {
            start -= ImGui::GetIO().MouseWheel * span * 0.25;
            changed = true;
        }
        start = std::clamp(start, 0.0, maxStart);
        ImU32 col = ImGui::GetColorU32(active ? ImGuiCol_ScrollbarGrabActive
                                              : hovered ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab);
        float txn = p0.x + (maxStart > 0 ? (float)(start / maxStart) * travel : 0.f);
        dl->AddRectFilled(ImVec2(txn, p0.y + 2), ImVec2(txn + thumbW, p0.y + height - 2), col, st.ScrollbarRounding);
    }
    ImGui::PopID();
    return changed;
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SetupStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 3;
    s.GrabRounding = 3;
    s.ScrollbarRounding = 3;
    s.FramePadding = ImVec2(5, 3);
    s.ItemSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 14;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.20f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.30f, 0.42f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.26f, 0.34f, 1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.38f, 0.52f, 1.f);
    s.ScaleAllSizes(scale);
    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.PlotPadding = ImVec2(18 * scale, 6 * scale);
    ps.LabelPadding = ImVec2(4, 2);
    ps.FitPadding = ImVec2(0, 0.08f);
    ps.LineWeight = 1.2f;
    ps.Colors[ImPlotCol_PlotBg] = ImVec4(0.05f, 0.05f, 0.06f, 1.f);
}
