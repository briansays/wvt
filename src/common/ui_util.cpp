#include "common/ui_util.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
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
    const char* unit = end;  // after the SI prefix: only a unit may follow (issue #46)
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
    if (mult != 1) ++unit;
    if ((unsigned char)end[0] == 0xC2 && (unsigned char)end[1] == 0xB5) {  // UTF-8 micro sign
        mult = 1e-6;
        unit = end + 2;
    }
    // A unit such as "Hz", "MS/s" or "dBFS" is fine; "12abc3", "1.5.2" or "4:3" is not.
    for (; *unit; ++unit)
        if (!std::isalpha((unsigned char)*unit) && *unit != '/' && *unit != '%' && !std::isspace((unsigned char)*unit))
            return false;
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
    // Room on the right for half of the last tick label, and around the plot area.
    ps.PlotPadding = ImVec2(24 * scale, 10 * scale);
    ps.LabelPadding = ImVec2(5 * scale, 3 * scale);
    ps.FitPadding = ImVec2(0, 0.08f);
    ps.LineWeight = 1.2f;
    ps.Colors[ImPlotCol_PlotBg] = ImVec4(0.05f, 0.05f, 0.06f, 1.f);
}

void tooltipAlways(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

bool beginForm(const char* id, float labelEm, float widthEm) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp, ImVec2(ImGui::GetFontSize() * widthEm, 0))) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * labelEm);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void row(const char* label, const char* tip) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip) tooltipAlways(tip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool redButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.22f, 0.22f, 1.f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

bool settingsButton(const char* popupId, const char* tip) {
    // The font has no gear glyph, so the gear is drawn: a ring with 8 teeth.
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(popupId);
    const bool clicked = ImGui::Button("##gear", ImVec2(h, h));
    ImGui::PopID();
    if (tip) tooltipAlways(tip);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float r = h * 0.24f;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 8; ++i) {
        const float ang = i * 3.14159265f / 4, ca = std::cos(ang), sa = std::sin(ang);
        dl->AddLine(ImVec2(c.x + ca * r, c.y + sa * r), ImVec2(c.x + ca * r * 1.45f, c.y + sa * r * 1.45f), col, h * 0.11f);
    }
    dl->AddCircle(c, r, col, 16, h * 0.09f);
    dl->AddCircleFilled(c, r * 0.35f, col, 12);
    if (clicked) ImGui::OpenPopup(popupId);
    return clicked;
}

namespace {
float colorbarPadX() { return ImGui::GetFontSize() * 0.3f; }
}  // namespace

float colorbarWidth() {
    // ImPlot's bar is 20 px wide; the rest is the tick labels and the padding.
    return 20 + ImPlot::GetStyle().LabelPadding.x + ImGui::CalcTextSize("-888 dB").x + 2 * colorbarPadX() + 2;
}

void colorbar(const char* id, float dbMin, float dbMax, float height, int cmap) {
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(colorbarPadX(), ImPlot::GetStyle().PlotPadding.y));
    ImPlot::ColormapScale(id, dbMin, dbMax, ImVec2(colorbarWidth(), height), "%g dB", 0, cmap);
    ImPlot::PopStyleVar();
}

std::string elideMiddle(const std::string& text, float maxW) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    const char* dots = "...";
    // Keep the end (a file name) a little longer than the start.
    for (size_t keep = text.size(); keep > 0; --keep) {
        const size_t head = keep * 2 / 5, tail = keep - head;
        std::string t = text.substr(0, head) + dots + text.substr(text.size() - tail);
        if (ImGui::CalcTextSize(t.c_str()).x <= maxW) return t;
    }
    return dots;
}

