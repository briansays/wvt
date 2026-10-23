// Session file self-test (issue #76): a session written and read back gives
// the same state, the recording is found when its folder moves, session
// files are told apart from recordings and SigMF metadata, an existing file
// is kept unless replacing is asked for, and bad values are refused.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "io/session.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void write(const std::string& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

Session sample(const std::string& file) {
    Session s;
    s.file = file;
    s.fp.fmt = SampleFormat::CI16;
    s.fp.bigEndian = true;
    s.fp.headerBytes = 512;
    s.fp.swapIQ = true;
    s.rate = 2.4e6;
    s.fc = 915.123456789e6;
    s.seg0 = 1.5;
    s.seg1 = 9.25;
    s.t0 = 2.000125;
    s.t1 = 2.5;
    s.f0 = 914.5e6;
    s.f1 = 915.7e6;
    s.relFreq = true;
    s.nfft = 4096;
    s.window = "Hann";
    s.overlapPct = 75;
    s.avg = 3;
    s.maxRows = 512;
    s.maxCols = 1024;
    s.dbMin = -95.5;
    s.dbMax = -12.25;
    s.autoDb = false;
    s.colormap = "Plasma";
    s.showPeak = false;
    s.leftFrac = 0.6;
    s.specFrac = 0.3;
    s.plotH = 140;
    s.showOverview = false;
    s.showAM = false;
    s.autoY[2] = false;
    s.chanEnable = true;
    s.chanOffset = -250e3;
    s.chanBw = 12.5e3;
    s.taps = 255;
    s.decim = 16;
    s.amMode = 2;
    s.amSmooth = 4;
    s.fmSmooth = 5;
    s.fmRelative = true;
    s.pmMode = 0;
    s.pmSmooth = 6;
    s.pmDegrees = true;
    s.squelch = true;
    s.squelchDb = -72.5;
    s.tMarkers = true;
    s.fMarkers = true;
    s.tm1 = 2.1;
    s.tm2 = 2.3;
    s.fm1 = 914.9e6;
    s.fm2 = 915.2e6;
    s.points = true;
    s.pt[0] = {MeasPlot::Waterfall, 2.15, 915.01e6};
    s.pt[1] = {MeasPlot::FM, 2.25, 0};
    s.bookmarks = {{"burst \"A\"", 2.0, 2.1}, {"tail", 8.0, 9.0}};
    return s;
}

bool same(const Session& a, const Session& b) {
    bool ok = a.fp == b.fp && a.rate == b.rate && a.fc == b.fc && a.seg0 == b.seg0 && a.seg1 == b.seg1 && a.t0 == b.t0 &&
              a.t1 == b.t1 && a.f0 == b.f0 && a.f1 == b.f1 && a.relFreq == b.relFreq && a.nfft == b.nfft &&
              a.window == b.window && a.overlapPct == b.overlapPct && a.avg == b.avg && a.maxRows == b.maxRows &&
              a.maxCols == b.maxCols && a.dbMin == b.dbMin && a.dbMax == b.dbMax && a.autoDb == b.autoDb &&
              a.colormap == b.colormap && a.showPeak == b.showPeak && a.leftFrac == b.leftFrac &&
              a.specFrac == b.specFrac && a.plotH == b.plotH && a.showOverview == b.showOverview && a.showIQ == b.showIQ &&
              a.showAM == b.showAM && a.showFM == b.showFM && a.showPM == b.showPM && a.chanEnable == b.chanEnable &&
              a.chanOffset == b.chanOffset && a.chanBw == b.chanBw && a.taps == b.taps && a.decim == b.decim &&
              a.amMode == b.amMode && a.amSmooth == b.amSmooth && a.fmSmooth == b.fmSmooth && a.fmRelative == b.fmRelative &&
              a.pmMode == b.pmMode && a.pmSmooth == b.pmSmooth && a.pmDegrees == b.pmDegrees && a.squelch == b.squelch &&
              a.squelchDb == b.squelchDb && a.tMarkers == b.tMarkers && a.fMarkers == b.fMarkers && a.tm1 == b.tm1 &&
              a.tm2 == b.tm2 && a.fm1 == b.fm1 && a.fm2 == b.fm2 && a.points == b.points && a.pt[0] == b.pt[0] &&
              a.pt[1] == b.pt[1] && a.bookmarks.size() == b.bookmarks.size();
    for (int k = 0; k < 4; ++k) ok = ok && a.autoY[k] == b.autoY[k];
    for (size_t k = 0; ok && k < a.bookmarks.size(); ++k)
        ok = a.bookmarks[k].name == b.bookmarks[k].name && a.bookmarks[k].t0 == b.bookmarks[k].t0 &&
             a.bookmarks[k].t1 == b.bookmarks[k].t1;
    return ok;
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/wvt_session_check_XXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    const std::string dir = tmpl;
    const std::string rec = dir + "/caps/rec_915MHz.cs16";
    (void)!system(("mkdir -p " + dir + "/caps").c_str());
    write(rec, std::string(4096, '\x01'));

    printf("names\n");
    check(defaultSessionPath("/d/cap.sigmf-meta") == "/d/cap.wvtsession.json", "SigMF: cap.wvtsession.json");
    check(defaultSessionPath("/d/rec_915MHz.cs16") == "/d/rec_915MHz.wvtsession.json", "an extension is replaced");
    check(defaultSessionPath("/d.x/noext") == "/d.x/noext.wvtsession.json", "a dot in the folder is not an extension");

    printf("write and read\n");
    const std::string sess = dir + "/caps/rec.wvtsession.json";
    std::string err;
    const Session s = sample(rec);
    check(saveSession(sess, s, false, err), "saved: " + err);
    Session r;
    check(loadSession(sess, r, err), "read back: " + err);
    check(same(s, r), "every value read back is the same");
    check(sessionRecording(r) == rec, "the recording: " + sessionRecording(r));
    std::ifstream in(sess);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(text.find("\"relative_path\": \"rec_915MHz.cs16\"") != std::string::npos, "the recording beside it is saved by name");

    printf("files\n");
    check(isSessionFile(sess), "a session file is a session");
    check(!isSessionFile(rec), "a recording is not");
    write(dir + "/caps/rec.sigmf-meta", R"({"global": {"core:datatype": "ci16_le"}, "captures": []})");
    check(!isSessionFile(dir + "/caps/rec.sigmf-meta"), "SigMF metadata (JSON) is not");
    write(dir + "/caps/brace.bin", "{" + std::string(1000, '\x7f'));
    check(!isSessionFile(dir + "/caps/brace.bin"), "a recording that starts with '{' is not");
    check(!isSessionFile(dir + "/none.json"), "a missing file is not");

    printf("replace\n");
    check(!saveSession(sess, s, false, err) && err.find("exists") != std::string::npos, "an existing file is kept: " + err);
    check(saveSession(sess, s, true, err), "... and replaced when asked");

    printf("moved folder\n");
    const std::string moved = dir + "/moved";
    (void)!system(("mv " + dir + "/caps " + moved).c_str());
    check(loadSession(moved + "/rec.wvtsession.json", r, err) && sessionRecording(r) == moved + "/rec_915MHz.cs16",
          "the recording is found beside the moved session: " + sessionRecording(r));

    printf("bad files\n");
    auto bad = [&](const std::string& body) { return !sessionFromJson(body, dir + "/x.wvtsession.json", r, err); };
    const std::string recJson = R"("recording": {"path": "/x.cf32", "sample_rate": 1e6})";
    check(bad("{\"wvt_chain\": 1, " + recJson + "}"), "not a session: refused");
    check(bad("{\"wvt_session\": 2, " + recJson + "}") && err.find("newer") != std::string::npos, "a newer version: " + err);
    check(bad(R"({"wvt_session": 1, "recording": {"sample_rate": 1e6}})"), "no recording: refused");
    check(bad(R"({"wvt_session": 1, "recording": {"path": "/x", "sample_rate": 0}})"), "rate 0: refused");
    check(bad(R"({"wvt_session": 1, "recording": {"path": "/x", "datatype": "ri16_le", "sample_rate": 1e6}})"),
          "unknown datatype: refused");
    check(bad("{\"wvt_session\": 1, " + recJson + ", \"waterfall\": {\"nfft\": 1e30}}"), "nfft 1e30: refused: " + err);
    check(!bad("{\"wvt_session\": 1, " + recJson + "}") && r.nfft == 1024 && r.window == "Blackman-Harris",
          "only the recording: the other values are the defaults");
    check(!r.points && !r.pt[0].set() && !r.pt[1].set(), "... and no point markers (a session from before issue #80)");
    check(!bad("{\"wvt_session\": 1, " + recJson + R"(, "points": {"shown": true, "p1": {"plot": "xyz", "t_s": 1}}})") &&
              r.points && !r.pt[0].set(),
          "a point marker on an unknown plot is not set");

    (void)!system(("rm -rf " + dir).c_str());
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
