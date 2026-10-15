#pragma once
#include <memory>
#include <string>
#include <vector>

#include "engine.h"
#include "imgui.h"
#include "implot.h"
#include "ui_util.h"

class X11Window;

struct CliOverrides {
    bool haveFmt = false;
    SampleFormat fmt = SampleFormat::CF32;
    bool bigEndian = false;
    bool haveRate = false;
    double rate = 0;
    bool haveFc = false;
    double fc = 0;
    int64_t header = -1;
    bool swapIQ = false;
};

class App {
public:
    explicit App(X11Window& win);
    ~App();
    bool openFile(const std::string& path, const CliOverrides& o = {});
    void frame();
    bool busy() const;
    void setView(double t0, double t1);
    void setChannel(double offset, double bw);

private:
    // UI sections
    void drawToolbar();
    void drawOverview(float h);
    void drawLeft();
    void drawRight();
    void drawStatusBar();
    void handleShortcuts();

    // plot helpers
    bool beginTimePlot(const char* id, const char* ylabel, float h, bool autoY, ImPlotFormatter yfmt = nullptr,
                       void* yud = nullptr, bool legend = false);
    void endTimePlot(const char* overlay = nullptr);
    void drawChannelOverlay(bool vertical);
    void drawFreqMarkers();
    void drawTimeMarkersVertical();
    void drawTimeMarkersHorizontal();

    // flow layout helpers (label + field, wrapping to the next line when needed)
    void flowBegin() { flowFirst_ = true; }
    void flow(float w);
    void label(const char* text, float fieldW);

    // state changes
    void onSourceChanged(bool resetView);
    void setFs(double fs);
    void setFc(double fc);
    void pan(double frac);
    void zoom(double factor);
    void submitRequests();
    void pollResults();
    void recolor();
    void buildLut();
    double valueAt(const Series& s, double t) const;
    float wfValueAt(double t, double f) const;

    X11Window& win_;
    std::unique_ptr<Worker<WaterfallReq, WaterfallRes>> wfWorker_;
    std::unique_ptr<Worker<DemodReq, DemodRes>> dmWorker_;
    std::unique_ptr<Worker<OverviewReq, OverviewRes>> ovWorker_;

    // source
    std::shared_ptr<IqFile> file_;
    char pathBuf_[2048] = {};
    FormatParams fp_;
    double fs_ = 1e6, fc_ = 0;
    int64_t nSamples_ = 0;
    double dur_ = 0;
    std::string status_;
    std::string metaNote_;

    // linked view ranges (shared by all plots through ImPlot axis links)
    double tmin_ = 0, tmax_ = 1;
    double fmin_ = -0.5e6, fmax_ = 0.5e6;
    bool relFreq_ = false;
    AxisFmt tFmt_{"s", &tmin_, &tmax_};
    AxisFmt fFmt_{"Hz", &fmin_, &fmax_};

    // waterfall parameters
    int nfft_ = 1024;
    int window_ = (int)WindowType::BlackmanHarris;
    double overlapPct_ = 50;
    int maxRows_ = 1024;
    int maxCols_ = 2048;
    int avg_ = 1;
    float dbMin_ = -110, dbMax_ = -20;
    bool autoDb_ = true;
    int cmap_ = 0;
    bool showPeak_ = true;
    float leftFrac_ = 0.45f;
    float specFrac_ = 0.25f;
    bool fitSpecY_ = true;

    // demodulation parameters (src/s0/s1 filled in per frame)
    DemodReq demod_;
    bool showIQ_ = true, showAM_ = true, showFM_ = true, showPM_ = true;
    bool autoY_[4] = {true, true, true, true};
    float plotH_ = 0;  // 0 = fit to window

    // markers & hover
    bool tMarkers_ = false, fMarkers_ = false;
    double tm1_ = 0, tm2_ = 0, fm1_ = 0, fm2_ = 0;
    double hoverT_ = NAN, hoverF_ = NAN, nextHoverT_ = NAN, nextHoverF_ = NAN;

    // overview scrubbing
    bool ovScrub_ = false;

    struct Bookmark {
        std::string name;
        double t0, t1;
    };
    std::vector<Bookmark> bookmarks_;
    int bookmarkSel_ = -1;

    // results
    std::shared_ptr<WaterfallRes> wf_;
    std::shared_ptr<DemodRes> dm_;
    std::shared_ptr<OverviewRes> ov_;
    ImTextureID wfTex_ = 0;
    bool needRecolor_ = false;
    std::vector<uint32_t> lut_;
    int lutCmap_ = -1;
    std::vector<uint32_t> rgba_;

    bool flowFirst_ = true;
};
