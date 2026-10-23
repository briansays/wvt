#include "io/session.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>

#include "common/json.h"
#include "common/log.h"
#include "io/recorder.h"

namespace {
// Session files are a few kB; anything larger is a recording.
constexpr off_t kMaxSessionBytes = 4 << 20;

std::string dirOf(const std::string& path) {
    const size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? "." : sl == 0 ? "/" : path.substr(0, sl);
}

bool exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::string absolute(const std::string& path) {
    std::string p = expandHome(path);
    if (!p.empty() && p[0] == '/') return p;
    char cwd[PATH_MAX];
    return getcwd(cwd, sizeof cwd) ? std::string(cwd) + "/" + p : p;
}

// "cf32_le": the short format name, as in SigMF and the chain files.
std::string datatype(const FormatParams& fp) {
    std::string f = formatName(fp.fmt);
    return f.substr(0, f.find(' ')) + (fp.bigEndian ? "_be" : "_le");
}

bool readText(const std::string& path, std::string& text, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path + ": " + errnoText(errno);
        return false;
    }
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    return true;
}

std::string b(bool v) { return v ? "true" : "false"; }
}  // namespace

std::string defaultSessionPath(const std::string& recordingPath) {
    std::string base = recordingPath;
    for (const char* ext : {".sigmf-meta", ".sigmf-data", ".sigmf"}) {
        const size_t n = std::string(ext).size();
        if (base.size() > n && base.compare(base.size() - n, n, ext) == 0) {
            base.resize(base.size() - n);
            return base + ".wvtsession.json";
        }
    }
    const size_t dot = base.find_last_of('.'), sl = base.find_last_of('/');
    if (dot != std::string::npos && (sl == std::string::npos || dot > sl + 1)) base.resize(dot);
    return base + ".wvtsession.json";
}

bool isSessionFile(const std::string& path) {
    const std::string full = expandHome(path);
    struct stat st{};
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > kMaxSessionBytes || st.st_size < 2) return false;
    std::string text, err;
    if (!readText(full, text, err)) return false;
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || text[first] != '{') return false;
    JsonValue v;
    return parseJson(text, v, err) && v.get("wvt_session");
}

std::string sessionToJson(const Session& s) {
    auto n = [](double v) { return jsonNumber(v); };
    std::string j = "{\n  \"wvt_session\": 1,\n";
    j += "  \"recording\": {\"path\": " + jsonQuote(s.file) + ", \"relative_path\": " + jsonQuote(s.fileRel) +
         ", \"datatype\": \"" + datatype(s.fp) + "\", \"header_bytes\": " + std::to_string(s.fp.headerBytes) +
         ", \"swap_iq\": " + b(s.fp.swapIQ) + ",\n                \"sample_rate\": " + n(s.rate) +
         ", \"center_freq\": " + n(s.fc) + ", \"segment_s\": [" + n(s.seg0) + ", " + n(s.seg1) + "]},\n";
    j += "  \"view\": {\"t0_s\": " + n(s.t0) + ", \"t1_s\": " + n(s.t1) + ", \"f0_hz\": " + n(s.f0) + ", \"f1_hz\": " + n(s.f1) +
         ", \"relative_freq\": " + b(s.relFreq) + "},\n";
    j += "  \"waterfall\": {\"nfft\": " + std::to_string(s.nfft) + ", \"window\": " + jsonQuote(s.window) +
         ", \"overlap_pct\": " + n(s.overlapPct) + ", \"avg\": " + std::to_string(s.avg) + ", \"max_rows\": " +
         std::to_string(s.maxRows) + ", \"max_cols\": " + std::to_string(s.maxCols) + ",\n                \"db_min\": " +
         n(s.dbMin) + ", \"db_max\": " + n(s.dbMax) + ", \"auto_db\": " + b(s.autoDb) + ", \"colormap\": " + jsonQuote(s.colormap) +
         ", \"show_peak\": " + b(s.showPeak) + "},\n";
    j += "  \"layout\": {\"left_frac\": " + n(s.leftFrac) + ", \"spectrum_frac\": " + n(s.specFrac) + ", \"plot_height\": " +
         n(s.plotH) + ", \"overview\": " + b(s.showOverview) + ", \"iq\": " + b(s.showIQ) + ", \"am\": " + b(s.showAM) +
         ", \"fm\": " + b(s.showFM) + ", \"pm\": " + b(s.showPM) + ",\n             \"auto_y\": [" + b(s.autoY[0]) + ", " +
         b(s.autoY[1]) + ", " + b(s.autoY[2]) + ", " + b(s.autoY[3]) + "]},\n";
    j += "  \"channel\": {\"enabled\": " + b(s.chanEnable) + ", \"offset_hz\": " + n(s.chanOffset) + ", \"bandwidth_hz\": " +
         n(s.chanBw) + ", \"taps\": " + std::to_string(s.taps) + ", \"decim\": " + std::to_string(s.decim) +
         ",\n              \"am_mode\": " + std::to_string(s.amMode) + ", \"am_smooth\": " + std::to_string(s.amSmooth) +
         ", \"fm_smooth\": " + std::to_string(s.fmSmooth) + ", \"fm_relative\": " + b(s.fmRelative) + ", \"pm_mode\": " +
         std::to_string(s.pmMode) + ", \"pm_smooth\": " + std::to_string(s.pmSmooth) + ", \"pm_degrees\": " + b(s.pmDegrees) +
         ",\n              \"squelch\": " + b(s.squelch) + ", \"squelch_db\": " + n(s.squelchDb) + "},\n";
    j += "  \"markers\": {\"time\": " + b(s.tMarkers) + ", \"t1_s\": " + n(s.tm1) + ", \"t2_s\": " + n(s.tm2) +
         ", \"freq\": " + b(s.fMarkers) + ", \"f1_hz\": " + n(s.fm1) + ", \"f2_hz\": " + n(s.fm2) + "},\n";
    // Only the markers that are set, and only the values their plot has.
    j += "  \"points\": {\"shown\": " + b(s.points);
    for (int k = 0; k < 2; ++k) {
        const PointMarker& p = s.pt[k];
        if (!p.set()) continue;
        j += std::string(", \"p") + char('1' + k) + "\": {\"plot\": " + jsonQuote(measPlotName(p.plot));
        if (measHasTime(p.plot)) j += ", \"t_s\": " + n(p.t);
        if (measHasFreq(p.plot)) j += ", \"f_hz\": " + n(p.f);
        j += '}';
    }
    j += "},\n";
    j += "  \"bookmarks\": [";
    for (size_t i = 0; i < s.bookmarks.size(); ++i)
        j += std::string(i ? "," : "") + "\n    {\"name\": " + jsonQuote(s.bookmarks[i].name) + ", \"t0_s\": " +
             n(s.bookmarks[i].t0) + ", \"t1_s\": " + n(s.bookmarks[i].t1) + "}";
    j += s.bookmarks.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return j;
}

bool sessionFromJson(const std::string& text, const std::string& sessionPath, Session& s, std::string& err) {
    JsonValue v;
    if (!parseJson(text, v, err)) return false;
    const JsonValue* rec = v.get("recording");
    if (!v.get("wvt_session") || !rec || rec->type != JsonValue::Type::Object) {
        err = "not a wvt session file";
        return false;
    }
    int64_t version = 0;
    if (!v.integer("wvt_session", 1, 1000, 1, version) || version > 1) {
        err = "session file version " + std::to_string(version) + " is newer than this wvt (1)";
        return false;
    }
    Session o;  // the defaults for anything the file leaves out
    o.file = rec->string("path");
    o.fileRel = rec->string("relative_path");
    if (o.file.empty() && o.fileRel.empty()) {
        err = "the session names no recording";
        return false;
    }
    if (!o.fileRel.empty() && o.fileRel[0] != '/') o.fileRel = dirOf(absolute(sessionPath)) + "/" + o.fileRel;
    bool be = false;
    if (!parseFormatName(rec->string("datatype", "cf32_le"), o.fp.fmt, be)) {
        err = "unknown datatype " + rec->string("datatype");
        return false;
    }
    o.fp.bigEndian = be;
    o.fp.swapIQ = rec->boolean("swap_iq");
    int64_t i64 = 0;
    // Whole numbers must fit: casting 1e30 or NaN is undefined.
    auto whole = [&](const JsonValue* obj, const char* key, int64_t lo, int64_t hi, int def, int& out) {
        if (!obj) return true;
        if (!obj->integer(key, lo, hi, def, i64)) {
            err = std::string(key) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi);
            return false;
        }
        out = (int)i64;
        return true;
    };
    if (!rec->integer("header_bytes", 0, int64_t(1) << 53, 0, i64)) {
        err = "header_bytes must be a whole number, 0 or more";
        return false;
    }
    o.fp.headerBytes = i64;
    o.rate = rec->number("sample_rate", o.rate);
    o.fc = rec->number("center_freq", o.fc);
    if (!(o.rate > 0) || !std::isfinite(o.fc)) {
        err = "the sample rate must be more than 0";
        return false;
    }
    if (const JsonValue* seg = rec->get("segment_s"); seg && seg->arr.size() == 2) {
        o.seg0 = seg->arr[0].num;
        o.seg1 = seg->arr[1].num;
    }
    if (const JsonValue* w = v.get("view")) {
        o.t0 = w->number("t0_s");
        o.t1 = w->number("t1_s");
        o.f0 = w->number("f0_hz");
        o.f1 = w->number("f1_hz");
        o.relFreq = w->boolean("relative_freq");
    }
    const JsonValue* wf = v.get("waterfall");
    if (!whole(wf, "nfft", 16, 1 << 20, o.nfft, o.nfft) || !whole(wf, "avg", 1, 1 << 16, o.avg, o.avg) ||
        !whole(wf, "max_rows", 16, 1 << 16, o.maxRows, o.maxRows) || !whole(wf, "max_cols", 16, 1 << 16, o.maxCols, o.maxCols))
        return false;
    if (wf) {
        o.window = wf->string("window", o.window);
        o.overlapPct = wf->number("overlap_pct", o.overlapPct);
        o.dbMin = wf->number("db_min", o.dbMin);
        o.dbMax = wf->number("db_max", o.dbMax);
        o.autoDb = wf->boolean("auto_db", o.autoDb);
        o.colormap = wf->string("colormap", o.colormap);
        o.showPeak = wf->boolean("show_peak", o.showPeak);
    }
    if (const JsonValue* l = v.get("layout")) {
        o.leftFrac = l->number("left_frac", o.leftFrac);
        o.specFrac = l->number("spectrum_frac", o.specFrac);
        o.plotH = l->number("plot_height", o.plotH);
        o.showOverview = l->boolean("overview", o.showOverview);
        o.showIQ = l->boolean("iq", o.showIQ);
        o.showAM = l->boolean("am", o.showAM);
        o.showFM = l->boolean("fm", o.showFM);
        o.showPM = l->boolean("pm", o.showPM);
        if (const JsonValue* a = l->get("auto_y"); a && a->arr.size() == 4)
            for (int k = 0; k < 4; ++k) o.autoY[k] = a->arr[k].type != JsonValue::Type::Bool || a->arr[k].b;
    }
    const JsonValue* ch = v.get("channel");
    if (!whole(ch, "taps", 0, 1 << 20, 0, o.taps) || !whole(ch, "decim", 0, 1 << 20, 0, o.decim) ||
        !whole(ch, "am_mode", 0, 2, o.amMode, o.amMode) || !whole(ch, "pm_mode", 0, 2, o.pmMode, o.pmMode) ||
        !whole(ch, "am_smooth", 1, 1 << 16, 1, o.amSmooth) || !whole(ch, "fm_smooth", 1, 1 << 16, 1, o.fmSmooth) ||
        !whole(ch, "pm_smooth", 1, 1 << 16, 1, o.pmSmooth))
        return false;
    if (ch) {
        o.chanEnable = ch->boolean("enabled");
        o.chanOffset = ch->number("offset_hz");
        o.chanBw = ch->number("bandwidth_hz", o.chanBw);
        o.fmRelative = ch->boolean("fm_relative");
        o.pmDegrees = ch->boolean("pm_degrees");
        o.squelch = ch->boolean("squelch");
        o.squelchDb = ch->number("squelch_db", o.squelchDb);
    }
    if (const JsonValue* m = v.get("markers")) {
        o.tMarkers = m->boolean("time");
        o.tm1 = m->number("t1_s");
        o.tm2 = m->number("t2_s");
        o.fMarkers = m->boolean("freq");
        o.fm1 = m->number("f1_hz");
        o.fm2 = m->number("f2_hz");
    }
    if (const JsonValue* p = v.get("points")) {
        o.points = p->boolean("shown");
        for (int k = 0; k < 2; ++k)
            if (const JsonValue* q = p->get(std::string("p") + char('1' + k)); q && q->type == JsonValue::Type::Object) {
                o.pt[k].plot = measPlotFromName(q->string("plot"));  // an unknown plot: not set
                o.pt[k].t = q->number("t_s");
                o.pt[k].f = q->number("f_hz");
            }
    }
    if (const JsonValue* bm = v.get("bookmarks"); bm && bm->type == JsonValue::Type::Array)
        for (const JsonValue& x : bm->arr) o.bookmarks.push_back({x.string("name"), x.number("t0_s"), x.number("t1_s")});
    s = std::move(o);
    return true;
}

bool saveSession(const std::string& path, const Session& s, bool overwrite, std::string& err) {
    const std::string full = expandHome(path);
    if (!overwrite && exists(full)) {
        err = full + " exists";
        return false;
    }
    Session out = s;
    // The recording path from the session's folder, when it is in that folder or below it.
    const std::string dir = dirOf(absolute(full)) + "/";
    out.fileRel = out.file.rfind(dir, 0) == 0 ? out.file.substr(dir.size()) : "";
    const std::string text = sessionToJson(out);
    const std::string tmp = full + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot write " + tmp + ": " + errnoText(errno);
        return false;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok || rename(tmp.c_str(), full.c_str()) != 0) {
        err = "cannot write " + full + ": " + errnoText(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool loadSession(const std::string& path, Session& s, std::string& err) {
    const std::string full = expandHome(path);
    std::string text;
    if (!readText(full, text, err)) return false;
    if (!sessionFromJson(text, full, s, err)) {
        err = full + ": " + err;
        return false;
    }
    return true;
}

std::string sessionRecording(const Session& s) {
    if (!s.fileRel.empty() && exists(s.fileRel)) return s.fileRel;
    return s.file;
}
