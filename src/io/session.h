#pragma once
// File sessions (issue #76): the state of the file viewer in a JSON file
// (.wvtsession.json), so that a person can go back to the same recording,
// view, plot settings and channel later. File > Save File Session writes
// one; the File box (or the command line) opens one like a recording.
//
// This module only reads and writes the file; the App maps its state to
// and from a Session (app_session.cpp).
#include <string>
#include <vector>

#include "dsp/measure.h"
#include "io/iq_file.h"

struct Session {
    // The recording. `file` is the absolute path; `fileRel` is the path from
    // the session file's folder, tried first, so that a folder with the
    // recording and its session can be moved.
    std::string file, fileRel;
    FormatParams fp;
    double rate = 1e6, fc = 0;
    double seg0 = -1, seg1 = -1;  // the time segment in s (-1 = file start / end)

    // The view (time in s, frequency in absolute Hz).
    double t0 = 0, t1 = 0, f0 = 0, f1 = 0;
    bool relFreq = false;

    // The waterfall and the spectrum.
    int nfft = 1024;
    std::string window = "Blackman-Harris";  // windowName()
    double overlapPct = 50;
    int avg = 1, maxRows = 1024, maxCols = 2048;
    double dbMin = -110, dbMax = -20;
    bool autoDb = true;
    std::string colormap = "Viridis";  // an ImPlot colormap name
    bool showPeak = true;

    // The layout and the plots.
    double leftFrac = 0.45, specFrac = 0.25, plotH = 0;
    bool showOverview = true, showIQ = true, showAM = true, showFM = true, showPM = true;
    bool autoY[4] = {true, true, true, true};

    // The channel and the demodulators.
    bool chanEnable = false;
    double chanOffset = 0, chanBw = 100e3;
    int taps = 0, decim = 0;
    int amMode = 0, amSmooth = 1, fmSmooth = 1, pmMode = 1, pmSmooth = 1;
    bool fmRelative = false, pmDegrees = false, squelch = false;
    double squelchDb = -60;

    // Markers and bookmarks.
    bool tMarkers = false, fMarkers = false;
    double tm1 = 0, tm2 = 0, fm1 = 0, fm2 = 0;
    bool points = false;   // the point markers (issue #80) are shown
    PointMarker pt[2];     // P1, P2
    struct Bookmark {
        std::string name;
        double t0 = 0, t1 = 0;
    };
    std::vector<Bookmark> bookmarks;
};

// "rec.sigmf-meta" -> "rec.wvtsession.json", beside the recording.
std::string defaultSessionPath(const std::string& recordingPath);

// True if `path` is a wvt session file: a small JSON object with
// "wvt_session". A recording is never mistaken for one (it fails the size,
// the first byte or the JSON parse).
bool isSessionFile(const std::string& path);

std::string sessionToJson(const Session& s);
// `sessionPath` makes the relative recording path absolute.
bool sessionFromJson(const std::string& text, const std::string& sessionPath, Session& s, std::string& err);

// Writes through a temporary file and a rename. An existing file is
// replaced only if `overwrite`.
bool saveSession(const std::string& path, const Session& s, bool overwrite, std::string& err);
bool loadSession(const std::string& path, Session& s, std::string& err);

// The recording to open: fileRel beside the session if it exists, else file.
std::string sessionRecording(const Session& s);
