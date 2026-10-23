#include "dsp/measure.h"

#include <algorithm>
#include <cmath>

bool measHasTime(MeasPlot p) { return p != MeasPlot::None && p != MeasPlot::Spectrum; }

bool measHasFreq(MeasPlot p) { return p == MeasPlot::Waterfall || p == MeasPlot::Spectrum; }

bool measSameValue(MeasPlot a, MeasPlot b) {
    auto iq = [](MeasPlot p) { return p == MeasPlot::I || p == MeasPlot::Q; };
    return a != MeasPlot::None && (a == b || (iq(a) && iq(b)));
}

namespace {
const struct {
    MeasPlot p;
    const char* name;
} kNames[] = {{MeasPlot::Waterfall, "waterfall"}, {MeasPlot::Spectrum, "spectrum"}, {MeasPlot::I, "i"}, {MeasPlot::Q, "q"},
              {MeasPlot::AM, "am"},               {MeasPlot::FM, "fm"},             {MeasPlot::PM, "pm"}};
}  // namespace

const char* measPlotName(MeasPlot p) {
    for (const auto& n : kNames)
        if (n.p == p) return n.name;
    return "";
}

MeasPlot measPlotFromName(const std::string& name) {
    for (const auto& n : kNames)
        if (name == n.name) return n.p;
    return MeasPlot::None;
}

ptrdiff_t nearestIndex(const std::vector<double>& xs, double v) {
    if (xs.empty() || !std::isfinite(v)) return -1;
    auto it = std::lower_bound(xs.begin(), xs.end(), v);
    ptrdiff_t i = it - xs.begin();
    if (i >= (ptrdiff_t)xs.size()) i = (ptrdiff_t)xs.size() - 1;
    if (i > 0 && std::fabs(xs[i - 1] - v) <= std::fabs(xs[i] - v)) --i;
    return i;
}

ptrdiff_t snapIndex(const std::vector<double>& xs, const std::vector<double>& ys, double v, double halfWidth,
                    double yHint, double minGain) {
    const ptrdiff_t n = (ptrdiff_t)std::min(xs.size(), ys.size());
    ptrdiff_t best = nearestIndex(xs, v);
    if (best < 0 || best >= n || !std::isfinite(yHint) || !(halfWidth > 0)) return best < n ? best : n - 1;
    ptrdiff_t lo = -1, hi = -1;
    const ptrdiff_t a = std::lower_bound(xs.begin(), xs.begin() + n, v - halfWidth) - xs.begin();
    for (ptrdiff_t i = a; i < n && xs[i] <= v + halfWidth; ++i) {
        if (!std::isfinite(ys[i])) continue;
        if (lo < 0 || ys[i] < ys[lo]) lo = i;
        if (hi < 0 || ys[i] > ys[hi]) hi = i;
    }
    auto dist = [&](ptrdiff_t i) { return std::isfinite(ys[i]) ? std::fabs(ys[i] - yHint) : INFINITY; };
    const ptrdiff_t nearest = best;
    for (ptrdiff_t c : {hi, lo})
        if (c >= 0 && dist(c) < dist(best) && dist(c) + minGain <= dist(nearest)) best = c;
    return best;
}
