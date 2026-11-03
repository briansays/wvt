#pragma once
#include <string>

#include "imgui.h"

// Engineering/SI formatting, e.g. fmtSI(2.4e6, "Hz") -> "2.4 MHz"
std::string fmtSI(double v, const char* unit, int sig = 6);
// Parses "2.4M", "915 MHz", "10m", "1.5e-3", "250u" ... returns false on garbage.
bool parseSI(const char* s, double& out);

// Text box that accepts SI suffixes. Applies on Enter or when focus leaves.
bool InputSI(const char* label, double* v, const char* unit, float width, int sig = 9);
// The same for a value that can be unset: a negative *v shows as an empty box
// with `hint`, and an emptied box sets *v to -1.
bool InputSIOptional(const char* label, double* v, const char* unit, float width, const char* hint);

// ImPlot axis formatter: shared SI prefix for all ticks, chosen from the
// axis range so labels stay distinct at any zoom.
struct AxisFmt {
    const char* unit;
    const double* lo;
    const double* hi;
    double offset = 0;  // subtracted before formatting
};
int formatAxisSI(double value, char* buf, int size, void* user);
// Per-value SI formatter (for Y axes whose range isn't linked).
int formatValueSI(double value, char* buf, int size, void* unit);
// Ticks for the X axis `axis` (an ImAxis) of a plot `pixels` wide, linked to
// [*a.lo, *a.hi], in 1-2-5 steps spaced by the width of their labels. ImPlot's
// own ticks overlap on a narrow plot or at a large UI scale. Call it after
// SetupAxisFormat(axis, formatAxisSI, &a).
void setupSpacedTicks(int axis, const AxisFmt& a, float pixels);

// Horizontal scrollbar over [0,total] with a thumb at [start, start+span).
// Returns true if `start` was changed.
bool ScrollbarH(const char* id, double total, double& start, double span, float width, float height);

void HelpMarker(const char* text);

// Tooltip that also shows over a disabled item.
void tooltipAlways(const char* text);
// Two-column "label | field" forms: beginForm opens the table (labelEm = the
// label column width in font sizes; widthEm = the table width, needed inside a
// popup, 0 = the available width), row starts a row whose field fills the column.
bool beginForm(const char* id, float labelEm, float widthEm = 0);
void row(const char* label, const char* tip = nullptr);
// A button in the transmit / record red.
bool redButton(const char* label, const ImVec2& size = ImVec2(0, 0));
// A square button with a gear that opens the popup `popupId` (call
// ImGui::BeginPopup(popupId) after it). Returns true when it was clicked.
bool settingsButton(const char* popupId, const char* tip);
// The dB colour scale beside a waterfall, with a tighter padding than the
// plots. colorbarWidth() is its width ("%g dB" labels down to -999 dB).
float colorbarWidth();
void colorbar(const char* id, float dbMin, float dbMax, float height, int cmap);
// Shortens text in the middle ("/a/b/…/name.sigmf-data") to fit maxW pixels.
std::string elideMiddle(const std::string& text, float maxW);
// Draws `text`, shortened in the middle to fit maxW; the full text shows in a
// tooltip when it was shortened.
void textElided(const std::string& text, float maxW);
// One entry of a plot key: a short line in `col`, then `name` (outside the
// plot, so that the traces are not told apart by colour alone).
void traceKey(const ImVec4& col, const char* name);
// The spectrum traces (file and live) and their key.
inline const ImVec4 kColSpecAvg(0.6f, 0.85f, 1.f, 1.f);
inline const ImVec4 kColSpecPeak(0.9f, 0.4f, 0.3f, 0.6f);
void spectrumKey(const char* title, bool peak);
void SetupStyle(float scale);
