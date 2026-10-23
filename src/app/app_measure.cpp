// Point measurements (issue #80): two point markers, P1 and P2, and the
// differences between them. M or Alt+click on a plot puts a marker at the
// pointer; on a time plot it goes on the trace (to a peak or a valley when
// one is near). Each marker can be dragged. The status bar and a label next
// to P2 show P2 minus P1.
#include <algorithm>
#include <cmath>

#include "app/app.h"

namespace {
const ImVec4 kColPt[2] = {ImVec4(1.00f, 1.00f, 1.00f, 1.f), ImVec4(0.40f, 1.00f, 0.95f, 1.f)};
const char* const kPtName[2] = {"P1", "P2"};
constexpr float kSnapPx = 8;      // a placed marker moves to a peak or a valley this near
constexpr float kSnapGainPx = 3;  // ... when that is this much nearer the pointer

bool timePlot(MeasPlot p) { return p == MeasPlot::I || p == MeasPlot::Q || p == MeasPlot::AM || p == MeasPlot::FM || p == MeasPlot::PM; }

// The plot a marker is drawn on: I and Q share the I/Q plot.
MeasPlot frameOf(MeasPlot p) { return p == MeasPlot::Q ? MeasPlot::I : p; }

const char* valueName(MeasPlot p) {
    switch (p) {
        case MeasPlot::I: return "I";
        case MeasPlot::Q: return "Q";
        case MeasPlot::AM: return "AM";
        case MeasPlot::FM: return "FM";
        case MeasPlot::PM: return "PM";
        default: return "P";  // power (waterfall, spectrum)
    }
}

// The key of a marker on its plot's X axis (the waterfall: frequency).
double keyOf(const PointMarker& m) { return measHasTime(m.plot) && m.plot != MeasPlot::Waterfall ? m.t : m.f; }

// The demodulator settings that change a trace's values; not the samples in view.
DemodReq valueKey(DemodReq r) {
    r.s0 = r.s1 = 0;
    r.maxDense = 0;
    r.displayPoints = 0;
    return r;
}
}  // namespace

bool App::pointSeries(MeasPlot p, const std::vector<double>*& xs, const std::vector<double>*& ys) const {
    const Series* s = nullptr;
    if (dm_) {
        switch (p) {
            case MeasPlot::I: s = &dm_->i; break;
            case MeasPlot::Q: s = &dm_->q; break;
            case MeasPlot::AM: s = &dm_->am; break;
            case MeasPlot::FM: s = &dm_->fm; break;
            case MeasPlot::PM: s = &dm_->pm; break;
            default: break;
        }
    }
    if (s) {
        xs = &s->t;
        ys = &s->y;
        return true;
    }
    if (p == MeasPlot::Spectrum && wf_) {
        xs = &wf_->specF;
        ys = &wf_->specAvg;
        return true;
    }
    return false;
}

// The value at the marker from the plotted data, when a sample is near (the
// marker is in the view, and not in a gap of a sparse view). Else, for a time
// plot, the value last read with the same demodulator settings, or NaN.
double App::pointValue(int k) const {
    const PointMarker& m = pts_[k];
    if (m.plot == MeasPlot::Waterfall) return wfValueAt(m.t, m.f);
    const std::vector<double>* xs = nullptr;
    const std::vector<double>* ys = nullptr;
    double v = NAN;
    if (pointSeries(m.plot, xs, ys)) {
        const double key = keyOf(m);
        const ptrdiff_t i = nearestIndex(*xs, key);
        if (i >= 0 && i < (ptrdiff_t)ys->size()) {
            double step = INFINITY;  // the spacing of the samples here
            if (i > 0) step = (*xs)[i] - (*xs)[i - 1];
            if (i + 1 < (ptrdiff_t)xs->size()) step = std::min(step, (*xs)[i + 1] - (*xs)[i]);
            if (!std::isfinite(step)) step = 0;
            const double tol = std::max(1.5 * step, timePlot(m.plot) ? 2 * ptSecPerPx_ : 0.0);
            if (std::fabs((*xs)[i] - key) <= tol) v = (*ys)[i];
        }
    }
    if (!timePlot(m.plot)) return v;
    PtCache& c = ptCache_[k];
    if (std::isfinite(v)) {
        c = {v, valueKey(dm_->req), m};
        return v;
    }
    return dm_ && c.at == m && c.req == valueKey(dm_->req) ? c.v : NAN;
}

std::string App::pointValueText(MeasPlot p, double v) const {
    if (!std::isfinite(v)) return "?";
    char b[64];
    switch (p) {
        case MeasPlot::FM: return fmtSI(v, "Hz", 5);
        case MeasPlot::AM:
            snprintf(b, sizeof b, dm_ && dm_->req.amMode == (int)AmMode::Decibel ? "%.4g dB" : "%.4g", v);
            break;
        case MeasPlot::PM: snprintf(b, sizeof b, "%.4g %s", v, dm_ && dm_->req.pmDegrees ? "deg" : "rad"); break;
        case MeasPlot::Waterfall:
        case MeasPlot::Spectrum: snprintf(b, sizeof b, "%.1f dB", v); break;
        default: snprintf(b, sizeof b, "%.4g", v); break;
    }
    return b;
}

// One marker: "t=... f=... AM=...".
std::string App::pointText(int k) const {
    const PointMarker& m = pts_[k];
    std::string s;
    if (measHasTime(m.plot)) s += "t=" + fmtSI(m.t, "s", 7);
    if (measHasFreq(m.plot)) s += std::string(s.empty() ? "" : " ") + "f=" + fmtSI(m.f - (relFreq_ ? fc_ : 0), "Hz", 8);
    return s + " " + valueName(m.plot) + "=" + pointValueText(m.plot, pointValue(k));
}

// P2 minus P1, for each quantity the two markers share. `full` adds 1/dT and
// the samples (the status bar).
std::string App::pointDeltas(bool full) const {
    const PointMarker &a = pts_[0], &b = pts_[1];
    if (!a.set() || !b.set()) return "";
    std::string s;
    auto add = [&](const std::string& part) { s += (s.empty() ? "" : "  ") + part; };
    if (measHasTime(a.plot) && measHasTime(b.plot)) {
        const double dt = b.t - a.t;
        std::string part = "dT=" + fmtSI(dt, "s", 6);
        if (full)
            part += " (1/dT=" + fmtSI(dt != 0 ? 1 / std::fabs(dt) : 0, "Hz", 5) + ", " +
                    std::to_string((long long)std::llround(dt * fs_)) + " samples)";
        add(part);
    }
    if (measHasFreq(a.plot) && measHasFreq(b.plot)) add("dF=" + fmtSI(b.f - a.f, "Hz", 6));
    if (measSameValue(a.plot, b.plot)) {
        const double va = pointValue(0), vb = pointValue(1);
        const char* name = a.plot == b.plot ? valueName(a.plot) : "A";  // I with Q: amplitude
        add(std::string("d") + name + "=" + pointValueText(a.plot, vb - va));
    }
    return s;
}

std::string App::pointReadout() const {
    if (!pointsShown_ || liveMode_) return "";
    const bool p1 = pts_[0].set(), p2 = pts_[1].set();
    if (p1 && p2) {
        const std::string d = pointDeltas(true);
        return "P1-P2 " + (d.empty() ? "(no shared axis)" : d);
    }
    if (p1 || p2) return std::string(p1 ? "P1 " : "P2 ") + pointText(p1 ? 0 : 1);
    return "";
}

void App::clearPoints() {
    pts_[0] = pts_[1] = PointMarker{};
    nextPt_ = 0;
}

// Puts a marker at (x, y) on the plot `frame` (the current ImPlot plot): the
// first free marker, else the older one.
void App::placePoint(MeasPlot frame, double x, double y) {
    PointMarker m;
    m.plot = frame;
    if (frame == MeasPlot::Waterfall) {
        m.t = y;
        m.f = x;
    } else {
        const std::vector<double>* xs = nullptr;
        const std::vector<double>* ys = nullptr;
        if (frame == MeasPlot::I && dm_ && dm_->i.y.size() == dm_->i.t.size() && dm_->q.y.size() == dm_->q.t.size()) {
            // The trace nearer the pointer.
            const ptrdiff_t i = nearestIndex(dm_->i.t, x), q = nearestIndex(dm_->q.t, x);
            if (i >= 0 && q >= 0 && std::fabs(dm_->q.y[q] - y) < std::fabs(dm_->i.y[i] - y)) m.plot = MeasPlot::Q;
        }
        double key = x;
        if (pointSeries(m.plot, xs, ys)) {
            const ImVec2 px = ImPlot::PlotToPixels(x, y);
            const ImPlotPoint d = ImPlot::PixelsToPlot(ImVec2(px.x + kSnapPx, px.y + kSnapGainPx));
            const double hw = std::fabs(d.x - x);
            const ptrdiff_t i = snapIndex(*xs, *ys, x, hw, y, std::fabs(d.y - y));
            if (i >= 0 && std::fabs((*xs)[i] - x) <= hw) key = (*xs)[i];
        }
        (frame == MeasPlot::Spectrum ? m.f : m.t) = key;
    }
    int k = !pts_[0].set() ? 0 : !pts_[1].set() ? 1 : nextPt_;
    pts_[k] = m;
    nextPt_ = k ^ 1;
    pointsShown_ = true;
}

// M, or Alt+click, on the hovered plot.
void App::pointInput(MeasPlot frame) {
    if (liveMode_ || !file_ || exSelecting_ || !ImPlot::IsPlotHovered()) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyShift) return;
    const bool key = !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_M, false);
    const bool click = io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (!key && !click) return;
    const ImPlotPoint m = ImPlot::GetPlotMousePos();
    placePoint(frame, m.x, m.y);
}

// Inside the plot `frame`: its markers as points (P2 with the differences),
// and the other plots' markers as faint lines on the axes this plot shares.
void App::drawPoints(MeasPlot frame) {
    if (liveMode_) return;
    if (timePlot(frame)) {
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        ptSecPerPx_ = (lim.X.Max - lim.X.Min) / std::max(1.f, ImPlot::GetPlotSize().x);
    }
    if (!pointsShown_) return;
    for (int k = 0; k < 2; ++k) {
        PointMarker& m = pts_[k];
        if (!m.set()) continue;
        ImVec4 faint = kColPt[k];
        faint.w = 0.45f;
        double x = keyOf(m), y = m.plot == MeasPlot::Waterfall ? m.t : pointValue(k);
        if (frameOf(m.plot) == frame && std::isfinite(y)) {
            const double x0 = x, y0 = y;
            if (ImPlot::DragPoint(70 + k, &x, &y, kColPt[k], 5, ImPlotDragToolFlags_Delayed)) {
                // On a trace only the time moves; the value follows the trace.
                if (m.plot == MeasPlot::Waterfall) m.t = y;
                (m.plot == MeasPlot::Waterfall || m.plot == MeasPlot::Spectrum ? m.f : m.t) = x;
            }
            const std::string d = k == 1 ? pointDeltas(false) : "";
            ImPlot::Annotation(x0, y0, faint, ImVec2(10, -10), true, "%s%s%s", kPtName[k], d.empty() ? "" : "  ", d.c_str());
            continue;
        }
        // A line where the marker is: on its own plot when its value is not known
        // (off the view), and on the other plots that share its time or frequency.
        const char* id = k ? "##p2" : "##p1";
        auto line = [&](double v, bool horizontal) {
            ImPlot::SetNextLineStyle(faint, 1.f);
            ImPlot::PlotInfLines(id, &v, 1, (int)ImPlotItemFlags_NoFit | (horizontal ? (int)ImPlotInfLinesFlags_Horizontal : 0));
            if (horizontal) ImPlot::TagY(v, faint, "%s", kPtName[k]);
            else ImPlot::TagX(v, faint, "%s", kPtName[k]);
        };
        if (timePlot(frame) && measHasTime(m.plot)) line(m.t, false);
        else if (frame == MeasPlot::Waterfall) {
            if (measHasTime(m.plot)) line(m.t, true);
            if (measHasFreq(m.plot)) line(m.f, false);
        } else if (frame == MeasPlot::Spectrum && measHasFreq(m.plot)) line(m.f, false);
    }
}
