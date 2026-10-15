#pragma once
#include <string>

#include "imgui.h"

// Engineering/SI formatting, e.g. fmtSI(2.4e6, "Hz") -> "2.4 MHz"
std::string fmtSI(double v, const char* unit, int sig = 6);
// Parses "2.4M", "915 MHz", "10m", "1.5e-3", "250u" ... returns false on garbage.
bool parseSI(const char* s, double& out);

// Text box that accepts SI suffixes. Applies on Enter or when focus leaves.
bool InputSI(const char* label, double* v, const char* unit, float width, int sig = 9);

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

// Horizontal scrollbar over [0,total] with a thumb at [start, start+span).
// Returns true if `start` was changed.
bool ScrollbarH(const char* id, double total, double& start, double span, float width, float height);

void HelpMarker(const char* text);
void SetupStyle(float scale);
