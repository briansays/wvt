#pragma once
#include <memory>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "engine.h"
#include "extract.h"
#include "imgui.h"
#include "implot.h"
#include "live.h"
#include "siggen_out.h"
#include "snapshot.h"
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
    // File > Quit or Ctrl+Q
    bool quitRequested() const { return quit_; }
    void setView(double t0, double t1);
    // Loads only [t0, t1) seconds of the file; -1 = file start / end. Kept for
    // the next file. Returns false (and keeps the old segment) on a bad value.
    bool setSegment(double t0, double t1);
    void setChannel(double offset, double bw);

    // Live device mode (app_live.cpp)
    bool startDevice(const StreamConfig& cfg);
    bool startRecording(const RecordConfig& cfg);
    bool isLive() const { return liveMode_; }
    int liveRowsReceived() const { return liveFilled_; }

    // Signal generator window (app_siggen.cpp)
    void openSigGen(const WaveformParams* p = nullptr);
    bool sigGenPreviewReady() const { return sgPrev_ != nullptr; }

    // Audio window (app_audio.cpp)
    void openAudio(const AudioParams* p = nullptr, bool play = false);
    void setAudioOutput(const std::string& device, double volumeDb);
    // Playing or exporting: the main loop keeps running (the playhead moves).
    bool audioBusy() const;
    // A region export is running (issue #18).
    bool exportBusy() const;
    // Opens the export window with the channel's band when the channel
    // filter is on, else with the view.
    void openExport() { exportFrom(demod_.chanEnable ? 1 : 0); }
    // Plot snapshots (issue #13): call after the frame is rendered. Writes the
    // PNG files when an export is due on this frame.
    void afterRender();
    // Opens the snapshot window (File > Save plot snapshots..., the status bar button).
    void openSnapshot() { snapOpen_ = true; }

private:
    // UI sections
    void drawMenuBar();      // app_menu.cpp
    void drawHelpWindows();  // keyboard help and About (app_menu.cpp)
    void drawToolbar();
    void drawOverview(float h);
    void drawLeft();
    void drawRight();
    void drawStatusBar();
    void handleShortcuts();
    // GPU (issue #3): only in a GPU build
    std::string gpuTag(bool live) const;
    void drawStatusRight(const std::string& lead, const std::string& rest, bool live);
    float statusLeftW(const std::string& lead, const std::string& rest, bool live) const;

    // plot helpers
    bool beginTimePlot(const char* id, const char* ylabel, float h, bool autoY, ImPlotFormatter yfmt = nullptr,
                       void* yud = nullptr, bool legend = false);
    void endTimePlot(SnapPlot p);
    void plotHeader(const char* name, bool* show, const ImVec4& col, const char* readout);
    void drawChannelOverlay(bool vertical);
    void drawFreqMarkers();
    void drawTimeMarkersVertical();
    void drawTimeMarkersHorizontal();

    // flow layout helpers (label + field, wrapping to the next line when needed)
    void flowBegin() { flowFirst_ = true; }
    void flow(float w);
    void label(const char* text, float fieldW);

    // state changes
    std::string onSourceChanged(bool resetView);  // returns a segment note, or ""
    std::string applySegment();
    void resetTimeView();
    double tBegin() const { return fs_ > 0 ? segS0_ / fs_ : 0; }
    double tEnd() const { return fs_ > 0 ? segS1_ / fs_ : 0; }
    bool segmentActive() const { return file_ && (segS0_ > 0 || segS1_ < nSamples_); }
    void setFs(double fs);
    void setFc(double fc);
    void pan(double frac);
    void showAll();       // the whole file (or segment) and the full band; live: the full band
    void toggleExport();
    void addBookmark();  // opens (with the view) or closes the export window
    void zoom(double factor);
    void submitRequests();
    void pollResults();
    void recolor();
    void buildLut();
    double valueAt(const Series& s, double t) const;
    float wfValueAt(double t, double f) const;
    bool workersBusy() const;

    // live mode (app_live.cpp)
    void drawSourceSelector(float width);
    void drawDeviceToolbar();
    void drawRecordBar();
    void drawLiveLeft();
    void drawLiveStatusBar();
    void handleLiveShortcuts();
    void enterLive();
    void leaveLive();
    void scanDevices();
    void selectDevice(const DeviceInfo& d);
    bool restartDevice();
    void pollLive();
    void resetLiveHistory(uint64_t gen, int cols);
    void recolorLiveRow(int row);
    void recolorLiveAll();
    float liveValueAt(double ago, double f) const;
    bool liveRecording() const;

    // signal generator (app_siggen.cpp)
    void drawSigGenWindow();
    void drawSigGenWaveform();
    void drawSigGenPreview();
    void drawSweepPreview(const SigPreviewRes& r);
    void drawSweepRows(WaveformParams& p);
    void drawSigGenFile();
    void drawSigGenLoopback();
    void drawSigGenTx();
    void drawSigGenPlayback();
    void setPlaybackFile(const std::string& path, bool fromViewer = false);
    // chain playback (app_chain.cpp)
    void drawSigGenChain();
    void drawChainItem(ChainItem& c, const SigPreviewRes::ChainInfo* info);
    void drawChainPreview(const SigPreviewRes& r);
    void addChainItem(const std::string& path, bool fromViewer = false, bool markers = false);
    bool chainLoaded(int* loaded = nullptr) const;  // every item is in the playback cache
    void pollSigGen();
    bool sigGenBusy() const;
    bool sigGenLoopback() const;
    bool startLoopback();
    bool startTx();
    void stopTx(const std::string& why);

    // region export (app_export.cpp, issue #18)
    void drawExportWindow();
    void drawExportRegion(bool waterfall);  // the region on the waterfall, or the time range on a time plot
    void waterfallExportInput(bool& openMenu);  // crosshair selection on the waterfall
    void drawWaterfallMenu();
    void exportFrom(int what);  // ExportFill
    ExtractConfig exportConfig() const;
    void startExport(bool overwrite);
    void pollExport();

    // plot snapshots (app_snapshot.cpp, issue #13)
    void drawSnapshotWindow();
    void snapNote(SnapPlot p, ImVec2 a, ImVec2 b);  // where plot p is drawn on this frame
    void snapNotePlot(SnapPlot p, float top = -1);  // the current ImPlot frame (from top, if given)
    void snapNoteItem(SnapPlot p);                  // add the last item (the colour bar)
    void snapClipToWindow();  // plots noted from now on are clipped to the current window
    void snapClipReset();     // ... or not clipped (the default)
    bool snapVisible(SnapPlot p) const;
    std::string statusText() const;  // the status line, or a recent snapshot note
    void saveSnapshots();

    // audio (app_audio.cpp)
    void drawAudioWindow();
    void drawAudioOverlay();
    void drawPlayhead(bool vertical);
    void playFrom(double t);
    void pollAudio();
    bool playAudio(int64_t from = -1);
    void stopAudio(const std::string& why = {});
    void syncAudioChannel();
    void setAudioMode(int mode);
    void playRange(int64_t& s0, int64_t& s1) const;
    const char* rangeName(int r) const;  // audio range name ("Whole segment" when one is loaded)

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
    // Time segment (issue #16): as typed (-1 = file start / end), and the
    // samples [segS0_, segS1_) it selects. Views stay in file time.
    double segT0_ = -1, segT1_ = -1;
    int64_t segS0_ = 0, segS1_ = 0;
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

    // plot snapshots: the plot rectangles of the current frame, the clip of their
    // window, and the frame number they are from (only that frame's are valid).
    struct SnapArea {
        ImVec2 a, b, clipA, clipB;
        int frame = -1;
    };
    SnapArea snapArea_[kSnapPlots];
    bool snapOpen_ = false;
    bool snapSel_[kSnapPlots] = {true, true, true, true, true, true, true};
    char snapDir_[1024] = "~/wvt-recordings/";
    int snapCountdown_ = 0;  // frames until the capture; 0 = none due
    ImVec2 snapMouse_;       // the pointer, put back after the capture
    std::string snapErr_;      // the folder could not be made
    float snapHeaderTop_ = 0;  // top of the current time plot's header
    ImVec2 snapClipA_, snapClipB_;  // the clip for snapNote
    std::string snapNoteMsg_;
    bool snapNoteErr_ = false;
    double snapNoteUntil_ = 0;

    // region export
    bool exOpen_ = false;
    double exT0_ = 0, exT1_ = 0, exLo_ = 0, exHi_ = 0;  // seconds, absolute Hz
    bool exFullBand_ = true, exCi16_ = false, exSigmf_ = true, exInSamples_ = false, exCenterBw_ = false;
    char exPath_[1024] = {};
    bool exPathAuto_ = true;  // the name follows the region until it is edited
    std::unique_ptr<ExtractJob> exJob_;
    ExtractJob::Status exSt_;
    ExtractConfig exPlanCfg_;  // the config exPlan_ was made for
    ExtractPlan exPlan_;
    double exPlanAt_ = 0;
    std::string exMsg_;
    bool exMsgErr_ = false;
    bool exSelecting_ = false, exDragging_ = false;
    double exDragT_ = 0, exDragF_ = 0;
    double fileStart_ = 0;  // SigMF start time of the open file (s since 1970), 0 = unknown

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
    bool focusPath_ = false;  // File > Open: put the cursor in the path box
    bool quit_ = false;
    bool helpOpen_ = false, aboutOpen_ = false;
    bool showOverview_ = true;

    // --- live device mode ---
    bool liveMode_ = false;
    std::unique_ptr<LiveEngine> live_;
    LiveEngine::Stats liveStats_;
    StreamConfig devCfg_;
    std::vector<DeviceInfo> devices_;
    bool devicesScanned_ = false;
    char devArgs_[256] = {};
    char antenna_[32] = {};
    std::string liveErr_;
    double fileFs_ = 1e6, fileFc_ = 0;  // file-mode values while live mode borrows fs_/fc_
    double liveViewFc_ = NAN, liveViewFs_ = NAN;

    RecordConfig recCfg_;
    char recPrefix_[1024] = {};
    std::string recErr_;
    bool recWasActive_ = false;  // for logging a recording that fails on its own
    std::string lastRecording_;  // path to reopen in file mode

    LiveParams liveParams_;
    int liveHistory_ = 600;  // waterfall rows kept
    int specAvgRows_ = 8;    // spectrum average time constant in rows
    bool liveAutoDb_ = true;
    double liveAutoAt_ = 0;

    // waterfall history: a ring of liveRows_ x liveCols_, newest row at liveHead_-1
    uint64_t liveGen_ = 0;
    int liveCols_ = 0, liveRows_ = 0, liveFilled_ = 0, liveHead_ = 0;
    double liveF0_ = 0, liveF1_ = 0;
    std::vector<float> liveDb_;
    std::vector<uint32_t> liveRgba_;
    ImTextureID liveTex_ = 0;
    bool liveTexDirty_ = false;
    int liveLutCmap_ = -1;
    float liveDbMin_ = 0, liveDbMax_ = 0;  // range the texture was colored with
    std::vector<double> liveSpecF_, liveAvgLin_, livePeakLin_, liveAvgDb_, livePeakDb_;
    bool liveFitY_ = true, liveFitSpecY_ = true;
    double liveHoverAgo_ = NAN, liveNextHoverAgo_ = NAN;

    // --- signal generator ---
    bool sgOpen_ = false;
    WaveformParams sg_;
    ParamCheck sgCheck_;
    bool sgReady_ = false;  // valid, and a playback recording is loaded
    char sgPlayPath_[1024] = {};
    std::string sgPlayNote_;
    char sgStagger_[256] = {};
    int sgOutput_ = 0;  // 0 file, 1 loopback, 2 transmit
    FileGenConfig sgFileCfg_;
    char sgPrefix_[1024] = {};
    bool sgByPulses_ = false;
    std::unique_ptr<FileGenJob> sgFile_;
    FileGenJob::Status sgFileSt_;
    std::string sgFileMsg_, sgLastFile_;
    bool sgFileFailed_ = false;
    std::unique_ptr<Worker<SigPreviewReq, SigPreviewRes>> sgPrevWorker_;
    std::shared_ptr<SigPreviewRes> sgPrev_;
    bool sgPrevTrain_ = false;
    char sgChainAdd_[1024] = {};
    char sgChainFile_[1024] = {};
    int sgChainSel_ = -1;  // item whose details show
    std::string sgChainNote_;
    bool sgChainNoteErr_ = false;
    std::unique_ptr<TxEngine> tx_;
    TxEngine::Stats txStats_;
    TxConfig txCfg_;
    char txArgs_[256] = {};
    bool txArmed_ = false;
    std::string txErr_;

    // --- audio ---
    bool auOpen_ = false;
    AudioParams au_;
    bool auFollow_ = true;  // use the viewer's channel (demod_)
    int auRange_ = 0;       // 0 from view start, 1 view, 2 time markers, 3 whole file
    bool auLoop_ = false, auFollowView_ = true;
    double auVolDb_ = -6;
    bool auMute_ = false;
    std::unique_ptr<AudioEngine> audio_;
    AudioEngine::Stats auStats_;
    std::string auErr_, auMsg_;
    std::vector<AudioDeviceInfo> auDevices_;
    bool auDevicesScanned_ = false;
    std::string auDevice_;  // "" = system default
    char auPrefix_[1024] = {};
    std::unique_ptr<WavExportJob> auExport_;
    WavExportJob::Status auExportSt_;
    float auPeakHold_ = -200;
    double auPeakAt_ = 0;
    double auPlayhead_ = NAN;  // seconds; file playback only
    bool auDragHead_ = false;
};
