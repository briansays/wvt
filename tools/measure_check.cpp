// Point measurement self-test (issue #80): the nearest sample, the snap to
// the top or the bottom of a pulse, the rules for which differences apply,
// and the plot names in session files.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/measure.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
}  // namespace

int main() {
    printf("nearest sample\n");
    const std::vector<double> xs = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    check(nearestIndex({}, 1) == -1, "no samples: -1");
    check(nearestIndex(xs, NAN) == -1, "NaN: -1");
    check(nearestIndex(xs, 3.4) == 3 && nearestIndex(xs, 3.6) == 4, "3.4 -> 3, 3.6 -> 4");
    check(nearestIndex(xs, 3.5) == 3, "half way: the earlier sample");
    check(nearestIndex(xs, -5) == 0 && nearestIndex(xs, 50) == 9, "before / after the data: the first / last");

    printf("snap\n");
    // A pulse: low 0, high 1 from x = 3 to 6, a peak of 1.2 at x = 5.
    const std::vector<double> ys = {0, 0, 0.5, 1, 1, 1.2, 1, 0, 0, 0};
    check(snapIndex(xs, ys, 4, 2, 1.3) == 5, "a click above the pulse: its peak");
    check(snapIndex(xs, ys, 2, 2, 0) == 0, "a click below, near the edge: the low level");
    check(snapIndex(xs, ys, 2, 2, 0.5) == 2, "a click on the edge: stays on the edge");
    check(snapIndex(xs, ys, 2, 0, 1.3) == 2, "no window: the nearest sample");
    check(snapIndex(xs, ys, 2, 2, NAN) == 2, "no pointer value: the nearest sample");
    std::vector<double> gaps = ys;
    gaps[5] = NAN;  // squelched
    check(snapIndex(xs, gaps, 4, 2, 1.3) == 4, "a blanked sample is not a peak: stays on the plateau");
    const std::vector<double> flat = {0, 0, 1.01, 1, 1, 1, 1.005, 1, 0, 0};  // a flat top with noise
    check(snapIndex(xs, flat, 4, 2, 1.2, 0.1) == 4, "noise on a flat top: the marker stays (min gain)");
    check(snapIndex(xs, flat, 4, 2, 1.2) == 2, "... and moves to the noise peak without one");
    check(snapIndex(xs, ys, 4, 2, 1.3, 0.1) == 5, "a real peak is still taken with a min gain");
    check(snapIndex(xs, std::vector<double>(ys.begin(), ys.begin() + 4), 8, 2, 0) == 3,
          "fewer y than x: only the samples with both");

    printf("differences\n");
    check(measHasTime(MeasPlot::AM) && measHasTime(MeasPlot::Waterfall) && !measHasTime(MeasPlot::Spectrum),
          "time: time plots and the waterfall");
    check(measHasFreq(MeasPlot::Spectrum) && measHasFreq(MeasPlot::Waterfall) && !measHasFreq(MeasPlot::FM),
          "frequency: the spectrum and the waterfall");
    check(measSameValue(MeasPlot::I, MeasPlot::Q) && measSameValue(MeasPlot::FM, MeasPlot::FM),
          "I with Q, FM with FM: a value difference");
    check(!measSameValue(MeasPlot::AM, MeasPlot::FM) && !measSameValue(MeasPlot::Waterfall, MeasPlot::Spectrum) &&
              !measSameValue(MeasPlot::None, MeasPlot::None),
          "AM with FM, waterfall with spectrum, none: no value difference");

    printf("names\n");
    bool names = true;
    for (MeasPlot p : {MeasPlot::Waterfall, MeasPlot::Spectrum, MeasPlot::I, MeasPlot::Q, MeasPlot::AM, MeasPlot::FM,
                       MeasPlot::PM})
        names = names && measPlotFromName(measPlotName(p)) == p;
    check(names, "every plot name reads back");
    check(measPlotFromName("") == MeasPlot::None && measPlotFromName("xyz") == MeasPlot::None, "unknown: none");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
