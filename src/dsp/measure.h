#pragma once
// Point measurements (issue #80): two point markers, P1 and P2, put on the
// plots. A marker on a time plot (I, Q, AM, FM, PM) keeps only its time and
// sits on the trace; a marker on the waterfall keeps its time and frequency;
// a marker on the spectrum keeps its frequency. The time and frequency axes
// are shared, so dT and dF are measured between any two plots that have
// them; a value difference only between markers on the same quantity.
//
// This module has no ImGui: the App draws the markers (app_measure.cpp).
#include <cstddef>
#include <string>
#include <vector>

enum class MeasPlot { None, Waterfall, Spectrum, I, Q, AM, FM, PM };

struct PointMarker {
    MeasPlot plot = MeasPlot::None;
    double t = 0;  // seconds (time plots, waterfall)
    double f = 0;  // absolute Hz (waterfall, spectrum)
    bool set() const { return plot != MeasPlot::None; }
    bool operator==(const PointMarker&) const = default;
};

bool measHasTime(MeasPlot p);
bool measHasFreq(MeasPlot p);
// The same value quantity (and unit): I and Q are one (amplitude).
bool measSameValue(MeasPlot a, MeasPlot b);

// "waterfall", "spectrum", "i", "q", "am", "fm", "pm" (session files); "" for None.
const char* measPlotName(MeasPlot p);
MeasPlot measPlotFromName(const std::string& name);  // None when unknown

// Index of the x nearest `v` in the sorted `xs`, or -1 when `xs` is empty or
// `v` is not finite.
ptrdiff_t nearestIndex(const std::vector<double>& xs, double v);

// Where a marker placed at `v` (with the pointer at value `yHint`) goes: the
// sample nearest `v`, or the largest or smallest finite y within
// [v - halfWidth, v + halfWidth], whichever y is nearest `yHint`. So a click
// near the top of a pulse goes to the top, and a click on an edge stays on
// the edge. A peak or a valley is taken only when its y is at least
// `minGain` nearer `yHint` than the nearest sample's (so noise on a flat top
// does not move the marker). -1 when there is no sample.
ptrdiff_t snapIndex(const std::vector<double>& xs, const std::vector<double>& ys, double v, double halfWidth,
                    double yHint, double minGain = 0);
