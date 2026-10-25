// wvt - IQ waveform visualization tool (X11)
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <climits>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app/app.h"
#include "common/build_info.h"
#include "app/cli.h"
#include "app/config.h"
#include "dsp/extract.h"
#include "gpu/gpu.h"
#include "imgui.h"
#include "implot.h"
#include "common/log.h"
#include "app/platform_x11.h"
#include "common/ui_util.h"

extern const unsigned char g_embedded_font[];
extern const size_t g_embedded_font_size;

static volatile sig_atomic_t g_interrupted = 0;

// Records from a device without a window. Prints progress once a second.
static int runHeadless(const StreamConfig& dev, const RecordConfig& rc) {
    LiveEngine live(nullptr);
    std::string err;
    if (!live.start(dev, err)) {
        LOG_ERROR("record", "%s", err.c_str());
        return 1;
    }
    auto st = live.stats();
    logStatus("record", "streaming from %s, %s at %s", st.description.c_str(), fmtSI(st.rate, "S/s", 6).c_str(),
            fmtSI(st.fc, "Hz", 9).c_str());
    if (!st.notes.empty()) LOG_WARN("record", "%s", st.notes.c_str());
    if (!live.startRecording(rc, err)) {
        LOG_ERROR("record", "%s", err.c_str());
        return 1;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    signal(SIGTERM, [](int) { g_interrupted = 1; });
    logStatus("record", "recording to %s (Ctrl+C to stop)", live.recorder().status().dataPath.c_str());
    auto next = std::chrono::steady_clock::now();
    while (!g_interrupted && live.recorder().active()) {
        next += std::chrono::milliseconds(200);
        std::this_thread::sleep_until(next);
        if (!live.stats().error.empty()) break;
        static int tick = 0;
        if (++tick % 5 == 0) {
            auto rs = live.recorder().status();
            logProgress("record", "%s  %s  buffer %.0f%%  gaps %d   ", fmtSI(rs.seconds, "s", 4).c_str(),
                    fmtSI((double)rs.bytes, "B", 4).c_str(), rs.ringFill * 100, rs.gaps);
        }
    }
    std::string devErr = live.stats().error;
    live.stopRecording();
    auto rs = live.recorder().status();
    live.stop();
    logProgressEnd();
    logStatus("record", "wrote %s (%s, %s%s)", rs.dataPath.c_str(), fmtSI(rs.seconds, "s", 5).c_str(),
            fmtSI((double)rs.bytes, "B", 4).c_str(), rs.gaps ? (", " + std::to_string(rs.gaps) + " gaps").c_str() : "");
    if (!rs.metaPath.empty()) logStatus("record", "metadata %s", rs.metaPath.c_str());
    if (!devErr.empty()) LOG_ERROR("record", "device error: %s", devErr.c_str());
    if (rs.failed) LOG_ERROR("record", "recording failed: %s", rs.stopReason.c_str());
    return rs.failed || !devErr.empty() ? 1 : 0;
}

// Writes a generated waveform to a file and exits.
static int runGenFile(const WaveformParams& p, const FileGenConfig& c) {
    FileGenJob job;
    std::string err;
    if (!job.start(p, c, err)) {
        LOG_ERROR("siggen", "%s", err.c_str());
        return 1;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    while (job.status().active) {
        if (g_interrupted) job.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto st = job.status();
    if (!st.error.empty()) {
        LOG_ERROR("siggen", "generation %s: %s", st.error.c_str(), st.rec.dataPath.c_str());
        return 1;
    }
    logStatus("siggen", "wrote %s (%s, %s)", st.rec.dataPath.c_str(), fmtSI(st.rec.seconds, "s", 5).c_str(),
            fmtSI((double)st.rec.bytes, "B", 4).c_str());
    if (!st.rec.metaPath.empty()) logStatus("siggen", "metadata %s", st.rec.metaPath.c_str());
    return 0;
}

// What --tx is about to send, for --tx-dry-run and the confirmation (issue #49).
static std::string txPlan(const WaveformParams& p, const TxConfig& c, double seconds) {
    char gain[32];
    snprintf(gain, sizeof gain, "%.2f dB", c.gain);
    std::string len = "until Ctrl+C";
    if (seconds > 0) len = fmtSI(seconds, "s", 6);
    else if (p.mode == GenMode::Chain && p.chainLoops > 0) len = std::to_string(p.chainLoops) + " chain passes";
    const std::string ch = std::to_string(c.channel);
    return "  signal     " + describeParams(p) + "\n" +
           "  frequency  " + fmtSI(c.fc + p.offset, "Hz", 9) + " (center " + fmtSI(c.fc, "Hz", 9) + ", band " +
           fmtSI(c.fc - c.rate / 2, "Hz", 9) + " to " + fmtSI(c.fc + c.rate / 2, "Hz", 9) + ")\n" +
           "  rate       " + fmtSI(c.rate, "S/s", 6) + "\n" +
           "  gain       " + gain + "\n" +
           "  device     " + (c.args.empty() ? "the first USRP found" : c.args) + ", antenna " +
           (c.antenna.empty() ? "default" : c.antenna) + ", channel " + ch + "\n" +
           "  duration   " + len + "\n" +
           "  limits     " + describeTxLimits(c.limits) + "\n";
}

// Checks the limits, then prints the plan and asks (issue #49): "yes" on a
// terminal, or --tx-confirm when stdin is not one. 0 = transmit, else the
// exit status.
static int confirmTx(const WaveformParams& p, const TxConfig& c, double seconds, bool dryRun, bool confirmed) {
    std::string err;
    if (!txGainAllowed(c.limits, c.gain, err) || !txFreqAllowed(c.limits, c.fc, c.rate, err)) {
        LOG_ERROR("tx", "%s", err.c_str());
        return 2;
    }
    printf("%s:\n%s", dryRun ? "--tx would transmit (dry run: no device is opened)" : "wvt is about to TRANSMIT",
           txPlan(p, c, seconds).c_str());
    fflush(stdout);
    if (dryRun || confirmed) return dryRun ? -1 : 0;
    if (!isatty(STDIN_FILENO)) {
        LOG_ERROR("tx", "stdin is not a terminal, so wvt cannot ask: add --tx-confirm to transmit from a script");
        return 2;
    }
    printf("Type yes to transmit: ");
    fflush(stdout);
    char line[64] = "";
    if (!fgets(line, sizeof line, stdin) || strcmp(line, "yes\n") != 0) {
        logStatus("tx", "not transmitting");
        return 1;
    }
    return 0;
}

// Transmits a generated waveform until Ctrl+C or the duration ends.
static int runTx(const WaveformParams& p, const TxConfig& cfg, double seconds) {
    sigGenBus().publish(p);
    TxEngine tx;
    std::string err;
    if (!tx.start(cfg, err)) {
        LOG_ERROR("tx", "%s", err.c_str());
        return 1;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    signal(SIGTERM, [](int) { g_interrupted = 1; });
    auto st = tx.stats();
    logStatus("tx", "TRANSMITTING %s at %s (center %s), %s, gain %.2f dB on %s (Ctrl+C stops)",
            describeParams(p).c_str(), fmtSI(st.fc + p.offset, "Hz", 9).c_str(), fmtSI(st.fc, "Hz", 9).c_str(),
            fmtSI(st.rate, "S/s", 6).c_str(), st.gain, st.description.c_str());
    const auto t0 = std::chrono::steady_clock::now();
    int tick = 0;
    while (!g_interrupted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        st = tx.stats();
        if (!st.error.empty()) break;
        if (st.finished) {
            logProgressEnd();
            logStatus("tx", "the chain finished after %d passes", st.chainPasses);
            break;
        }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (seconds > 0 && el >= seconds) break;
        if (++tick % 5 == 0)
            logProgress("tx", "TX %s  sent %s  underflows %lld   ", fmtSI(el, "s", 4).c_str(),
                    fmtSI((double)st.tx.sent, "S", 4).c_str(), (long long)st.tx.underflows);
    }
    tx.stop();
    logProgressEnd();
    logStatus("tx", "transmitter stopped (sent %s, underflows %lld, lost packets %lld)",
            fmtSI((double)st.tx.sent, "S", 4).c_str(), (long long)st.tx.underflows, (long long)st.tx.seqErrors);
    if (!st.error.empty()) LOG_ERROR("tx", "transmit error: %s", st.error.c_str());
    return st.error.empty() ? 0 : 1;
}

static bool parseAudioMode(const std::string& v, int& mode) {
    for (int i = 0; i < (int)AudioMode::Count; ++i) {
        std::string n = audioModeName((AudioMode)i);
        for (auto& c : n) c = (char)tolower(c);
        if (v == n) {
            mode = i;
            return true;
        }
    }
    return false;
}

// Demodulates a file (or its --segment, or the --view range inside it) to a
// WAV file and exits.
static int runAudioExport(const std::string& path, const CliOverrides& ov, bool haveView, double t0, double t1,
                          double seg0, double seg1, const AudioParams& ap, const std::string& out) {
    FileMetaGuess g = guessMeta(path);
    std::string err;
    if (!ov.haveFmt && !metaReadable(g, err)) {
        LOG_ERROR("audio", "%s; -f FMT reads it anyway", err.c_str());
        return 1;
    }
    auto f = IqFile::open(g.dataPath, err);
    if (!f) {
        LOG_ERROR("audio", "%s", err.c_str());
        return 1;
    }
    Source src;
    src.file = f;
    if (g.haveFmt) { src.fp.fmt = g.fmt; src.fp.bigEndian = g.bigEndian; }
    if (g.haveRate) src.fs = g.rate;
    if (g.haveFc) src.fc = g.fc;
    if (ov.haveFmt) { src.fp.fmt = ov.fmt; src.fp.bigEndian = ov.bigEndian; }
    if (ov.haveRate) src.fs = ov.rate;
    if (ov.haveFc) src.fc = ov.fc;
    src.fp.headerBytes = ov.header >= 0 ? ov.header : 0;
    src.fp.swapIQ = ov.swapIQ;
    if (!g.haveRate && !ov.haveRate) LOG_WARN("audio", "sample rate unknown, assuming %s (use -r)", fmtSI(src.fs, "S/s", 4).c_str());
    const int64_t n = f->numSamples(src.fp);
    SegmentRange seg = segmentRange(seg0, seg1, src.fs, n);
    if (!seg.err.empty()) {
        LOG_ERROR("audio", "%s", seg.err.c_str());
        return 2;
    }
    if (!seg.warn.empty()) LOG_WARN("audio", "%s", seg.warn.c_str());
    if (!seg.whole(n))
        LOG_INFO("audio", "segment %s to %s", fmtSI(seg.s0 / src.fs, "s", 6).c_str(), fmtSI(seg.s1 / src.fs, "s", 6).c_str());
    WavExportConfig c;
    c.src = src;
    c.s0 = haveView ? std::clamp<int64_t>((int64_t)std::llround(t0 * src.fs), seg.s0, seg.s1) : seg.s0;
    c.s1 = haveView ? std::clamp<int64_t>((int64_t)std::llround(t1 * src.fs), seg.s0, seg.s1) : seg.s1;
    c.params = ap;
    c.path = expandHome(out);
    WavExportJob job;
    if (!job.start(c, err)) {
        LOG_ERROR("audio", "%s", err.c_str());
        return 1;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    while (job.status().active) {
        if (g_interrupted) job.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto st = job.status();
    if (!st.error.empty()) {
        LOG_ERROR("audio", "audio export %s: %s", st.error.c_str(), st.path.c_str());
        return 1;
    }
    logStatus("audio", "wrote %s (%s %s at %s, %s)", st.path.c_str(), audioModeName((AudioMode)ap.mode),
            fmtSI(ap.bw, "Hz", 4).c_str(), fmtSI(src.fc + ap.offset, "Hz", 9).c_str(), fmtSI(st.seconds, "s", 5).c_str());
    return 0;
}

// Exports a time range (and band) of a file to a new file (issue #18).
static int runExtract(const std::string& path, const CliOverrides& ov, double seg0, double seg1, bool haveRange, double t0,
                      double t1, bool haveBand, double fLo, double fHi, bool ci16, bool sigmf, bool overwrite,
                      const std::string& out) {
    FileMetaGuess g = guessMeta(path);
    std::string err;
    if (!ov.haveFmt && !metaReadable(g, err)) {
        LOG_ERROR("extract", "%s; -f FMT reads it anyway", err.c_str());
        return 1;
    }
    auto f = IqFile::open(g.dataPath, err);
    if (!f) {
        LOG_ERROR("extract", "%s", err.c_str());
        return 1;
    }
    ExtractConfig c;
    Source& src = c.src;
    src.file = f;
    if (g.haveFmt) { src.fp.fmt = g.fmt; src.fp.bigEndian = g.bigEndian; }
    if (g.haveRate) src.fs = g.rate;
    if (g.haveFc) src.fc = g.fc;
    if (ov.haveFmt) { src.fp.fmt = ov.fmt; src.fp.bigEndian = ov.bigEndian; }
    if (ov.haveRate) src.fs = ov.rate;
    if (ov.haveFc) src.fc = ov.fc;
    src.fp.headerBytes = ov.header >= 0 ? ov.header : 0;
    src.fp.swapIQ = ov.swapIQ;
    if (!g.haveRate && !ov.haveRate) LOG_WARN("extract", "sample rate unknown, assuming %s (use -r)", fmtSI(src.fs, "S/s", 4).c_str());
    const int64_t n = f->numSamples(src.fp);
    SegmentRange seg = segmentRange(seg0, seg1, src.fs, n);
    if (!seg.err.empty()) {
        LOG_ERROR("extract", "%s", seg.err.c_str());
        return 2;
    }
    if (!seg.warn.empty()) LOG_WARN("extract", "%s", seg.warn.c_str());
    c.limit0 = seg.s0;
    c.limit1 = seg.s1;
    c.s0 = haveRange ? (int64_t)std::llround(t0 * src.fs) : seg.s0;
    c.s1 = haveRange ? (int64_t)std::llround(t1 * src.fs) : seg.s1;
    c.fullBand = !haveBand;
    c.fLo = fLo;
    c.fHi = fHi;
    c.ci16 = ci16;
    c.sigmf = sigmf;
    c.overwrite = overwrite;
    c.sourceStart = g.startTime;
    c.path = out;
    ExtractJob job;
    if (!job.start(c, err)) {
        LOG_ERROR("extract", "%s", err.c_str());
        return 2;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    int tick = 0;
    while (job.status().active) {
        if (g_interrupted) job.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (++tick % 25 == 0) logProgress("extract", "%.0f %%", job.status().progress * 100);
    }
    logProgressEnd();
    const auto st = job.status();
    if (st.cancelled) {
        LOG_ERROR("extract", "export cancelled; the output was deleted");
        return 1;
    }
    if (!st.error.empty()) {
        LOG_ERROR("extract", "export failed: %s", st.error.c_str());
        return 1;
    }
    logStatus("extract", "wrote %s (%lld samples, %s)", st.dataPath.c_str(), (long long)st.samples,
              fmtSI((double)st.bytes, "B", 4).c_str());
    return 0;
}

// Records the demodulated audio of a live stream to WAV until Ctrl+C or the duration ends.
static int runAudioLive(const StreamConfig& dev, const AudioParams& ap, const std::string& out, double seconds) {
    LiveEngine live(nullptr);
    std::string err;
    if (!live.start(dev, err)) {
        LOG_ERROR("audio", "%s", err.c_str());
        return 1;
    }
    auto st = live.stats();
    AudioEngine audio;
    if (!audio.startLive(live.ring(), live.format(), live.sampleRate(), ap, "", AudioOutput::None, err) ||
        !audio.startWav(expandHome(out), err)) {
        LOG_ERROR("audio", "%s", err.c_str());
        return 1;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    signal(SIGTERM, [](int) { g_interrupted = 1; });
    logStatus("audio", "recording %s %s at %s from %s to %s (Ctrl+C to stop)", audioModeName((AudioMode)ap.mode),
            fmtSI(ap.bw, "Hz", 4).c_str(), fmtSI(st.fc + ap.offset, "Hz", 9).c_str(), st.description.c_str(), out.c_str());
    int tick = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto as = audio.stats();
        if (g_interrupted || !live.stats().error.empty() || !as.recording) break;
        if (seconds > 0 && as.wavSeconds >= seconds) break;
        if (++tick % 5 == 0)
            logProgress("audio", "%s  channel %.1f dBFS  resyncs %lld   ", fmtSI(as.wavSeconds, "s", 4).c_str(), as.chanDb,
                    (long long)as.resyncs);
    }
    auto as = audio.stats();
    audio.stop();
    std::string devErr = live.stats().error;
    live.stop();
    logProgressEnd();
    logStatus("audio", "wrote %s (%s)", as.wavPath.c_str(), fmtSI(as.wavSeconds, "s", 5).c_str());
    if (!as.wavError.empty()) LOG_ERROR("audio", "WAV error: %s", as.wavError.c_str());
    if (!devErr.empty()) LOG_ERROR("audio", "device error: %s", devErr.c_str());
    return as.wavError.empty() && devErr.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    CliOverrides ov;
    std::string path, shotPath, sessionOut;
    bool software = false, showHelp = false;
    bool haveChan = false, haveView = false, haveSeg = false;
    std::string extractOut;
    bool haveExtract = false, haveExtractBand = false, extractCi16 = false, extractOverwrite = false, openExportWin = false;
    double extract0 = 0, extract1 = 0, extractLo = 0, extractHi = 0;
    double chanOff = 0, chanBw = 0, view0 = 0, view1 = 0, seg0 = -1, seg1 = -1;
    auto parsePair = [](const char* s, double& a, double& b) {
        std::string str(s);
        size_t c = str.find(':');
        return c != std::string::npos && parseSI(str.substr(0, c).c_str(), a) && parseSI(str.substr(c + 1).c_str(), b);
    };
    StreamConfig dev;
    bool haveDevice = false, haveRecord = false, noGui = false, haveGain = false;
    RecordConfig rc;
    WaveformParams gp;
    FileGenConfig gc;
    TxConfig txc;
    bool haveGen = false, openGen = false, haveGenOut = false, haveTx = false, haveOffset = false, haveSweep = false;
    bool txDryRun = false, txConfirm = false;
    TxLimits cliLimits;  // over WVT_TX_MAX_GAIN / WVT_TX_ALLOW
    std::string playFile, chainFile;
    std::vector<std::string> chainAdds;
    int chainLoops = -1;
    double playRate = 0;
    double genSeconds = 0;
    AudioParams ap;
    bool haveAudio = false;
    std::string audioOut, audioDevice;
    double audioVol = -6;
    // The configuration file (issue #88) comes first: it sets the defaults that
    // the environment and the options then change, the log folder among them.
    std::vector<char*> args(argv, argv + argc);
    WvtConfig cfg;
    std::vector<std::string> cfgWarnings;
    std::string cfgOption, cfgFile, cfgErr;
    bool noConfig = false;
    if (configTakeArgs(args, cfgOption, noConfig, cfgErr) && !noConfig) {
        bool required = false;
        cfgFile = configPath(cfgOption, required);
        if (required || access(cfgFile.c_str(), F_OK) == 0) {
            if (!loadConfigFile(cfgFile, cfg, cfgWarnings, cfgErr) && !required) {
                cfgWarnings.push_back(cfgErr);  // the default file exists but cannot be read
                cfgErr.clear();
            }
        } else {
            cfgFile.clear();  // no file: the built-in defaults
        }
    }
    rc.sigmf = cfg.recordingSigmf;
    rc.cf32 = cfg.recordingCf32;
    rc.maxSeconds = cfg.recordingMaxSeconds;
    gc.fmt = cfg.siggenSample;
    extractCi16 = cfg.exportBandCi16;
    bool haveRecordFormat = false;  // --record-format also sets the generator file format

    float scale = (float)cfg.uiScale;
    int winW = cfg.uiWidth, winH = cfg.uiHeight;
    if (const char* e = envVar("WVT_SCALE"); e && *e) {
        double x;
        if (parseSI(e, x) && x >= 0.5 && x <= 4) scale = (float)x;
        else LOG_WARN("main", "WVT_SCALE=%s is not a number from 0.5 to 4; using 1", e);
    }
    GpuMode gpuMode = GpuMode::Auto;
    parseGpuMode(cfg.gpu.c_str(), gpuMode);  // the key table checked the value
    int gpuDevice = cfg.gpuDevice;

    // Logging options come out of argv first, so the rest of the parse is logged.
    LogConfig lc;
    for (int i = 0; i < argc; ++i) lc.commandLine += (i ? " " : "") + std::string(argv[i]);
    lc.build = std::string(buildGitHash()) + (*buildVersionTag() ? std::string(" (") + buildVersionTag() + ")" : "");
    std::string logErr;
    if (!logParseArgs(args, lc, logErr)) {
        LOG_ERROR("main", "%s", logErr.c_str());
        return 2;
    }
    // The file's log folder is below --log-dir and $WVT_LOG_DIR.
    if (const char* e = envVar("WVT_LOG_DIR"); lc.dir.empty() && !cfg.logFolder.empty() && !(e && *e))
        lc.dir = expandHome(cfg.logFolder);
    logInit(lc);
    if (!cfgErr.empty()) {
        LOG_ERROR("config", "%s", cfgErr.c_str());
        return 2;
    }
    for (const auto& w : cfgWarnings) LOG_WARN("config", "%s", w.c_str());
    if (noConfig) LOG_INFO("config", "no configuration file (--no-config)");
    else if (cfgFile.empty()) LOG_DEBUG("config", "no configuration file; built-in defaults");
    else LOG_INFO("config", "read %s: %zu settings", cfgFile.c_str(), cfg.origin.size());
    if (const char* e = envVar("WVT_GPU"); e && *e && !parseGpuMode(e, gpuMode))
        LOG_WARN("main", "WVT_GPU=%s is not auto, on or off; using auto", e);
    argc = (int)args.size();
    args.push_back(nullptr);
    argv = args.data();

    // The options (issue #54): one table for parsing and for --help.
    int exitNow = -1;  // set by the options that print and exit
    auto bad = [](const char* fmt, const char* what = "") {
        LOG_ERROR("main", fmt, what);
        return false;
    };
    // A number with SI suffixes into `dst`; `then` runs after a good value.
    auto si = [&](const char* name, double& dst, const std::function<void()>& then = {}) -> CliParser::Handler {
        return [&dst, name, then, &bad](const char* v) {
            if (!parseSI(v, dst)) return bad("bad value for %s", name);
            if (then) then();
            return true;
        };
    };
    auto pair = [&](const char* msg, double& a, double& b, const std::function<bool()>& then = {}) -> CliParser::Handler {
        return [&a, &b, msg, then, &parsePair, &bad](const char* v) {
            if (!parsePair(v, a, b)) return bad("%s", msg);
            return then ? then() : true;
        };
    };
    auto flag = [](bool& b) -> CliParser::Handler { return [&b](const char*) { return b = true; }; };
    // A whole number in [lo, hi]. atoi() took "abc" as 0, and for some options
    // 0 means "forever" (issue #54).
    auto whole = [](const char* name, const char* v, long long lo, long long hi, long long& out) {
        char* end = nullptr;
        errno = 0;
        out = strtoll(v, &end, 10);
        if (end == v || *end || errno == ERANGE || out < lo || out > hi) {
            LOG_ERROR("main", "%s wants a whole number from %lld to %lld (got '%s')", name, lo, hi, v);
            return false;
        }
        return true;
    };
    auto text = [](std::string& s) -> CliParser::Handler { return [&s](const char* v) { s = v; return true; }; };

    CliParser cli;
    cli.section("options:");
    cli.add("-f", "--format", "FMT", "cf32 | cf64 | ci16 | ci32 | ci8 | cu8  (suffix _be for big-endian)", [&](const char* v) {
        if (!parseFormatName(v, ov.fmt, ov.bigEndian)) return bad("unknown format '%s'", v);
        return ov.haveFmt = true;
    });
    cli.add("-r", "--rate", "HZ", "sample rate, SI suffixes allowed (e.g. 2.4M)", [&](const char* v) {
        if (!parseSI(v, ov.rate) || ov.rate <= 0) return bad("bad rate");
        return ov.haveRate = true;
    });
    cli.add("-c", "--fc", "HZ", "center frequency (e.g. 915M)", [&](const char* v) {
        if (!parseSI(v, ov.fc)) return bad("bad center frequency");
        return ov.haveFc = true;
    });
    cli.add("-H", "--header", "BYTES", "bytes to skip at the start of the file", [&](const char* v) {
        long long n;
        if (!whole("--header", v, 0, LLONG_MAX, n)) return false;
        ov.header = n;
        return true;
    });
    cli.add(nullptr, "--swap-iq", nullptr, "swap I and Q", flag(ov.swapIQ));
    cli.add(nullptr, "--chan", "OFF:BW", "enable the channel filter, e.g. --chan 300k:150k",
            pair("--chan wants OFFSET:BW", chanOff, chanBw, [&] { return haveChan = true; }));
    cli.add(nullptr, "--segment", "T0:T1",
            "load only this part of the file, in seconds; -1 = file\nstart / end, e.g. --segment 40:50 or --segment 40:-1",
            pair("--segment wants T0:T1 (-1 = file start / end)", seg0, seg1, [&] {
                if (seg0 >= 0 && seg1 >= 0 && seg1 <= seg0) return bad("--segment end must be after its start");
                return haveSeg = true;
            }));
    cli.add(nullptr, "--view", "T0:T1", "initial time range in seconds, e.g. --view 0.5:0.52",
            pair("--view wants T0:T1", view0, view1, [&] { return haveView = true; }));
    cli.add(nullptr, "--software", nullptr, "force the software renderer (no GLX)", flag(software));
    cli.add(nullptr, "--scale", "S", "UI scale factor (default 1, or $WVT_SCALE)", [&](const char* v) {
        double x;
        if (!parseSI(v, x) || x < 0.5 || x > 4) return bad("--scale wants a number from 0.5 to 4");
        scale = (float)x;
        return true;
    });
    cli.add(nullptr, "--geometry", "WxH", "initial window size (default 1600x950)", [&](const char* v) {
        const std::string s = v;
        const size_t x = s.find('x');
        long long w, h;
        if (x == std::string::npos) return bad("--geometry wants WxH, e.g. 1600x950");
        if (!whole("--geometry width", s.substr(0, x).c_str(), 100, 16384, w) ||
            !whole("--geometry height", s.substr(x + 1).c_str(), 100, 16384, h))
            return false;
        winW = (int)w;
        winH = (int)h;
        return true;
    });
    cli.add(nullptr, "--screenshot", "F", "render, write a PPM screenshot to F and exit", text(shotPath));
    cli.add(nullptr, "--session-out", "F", "open the file and options, write a file session to F and exit", text(sessionOut));
    cli.add(nullptr, "--gpu", "auto|on|off", "GPU processing, CUDA or Metal (default auto, or $WVT_GPU);\non = stop if no GPU can be used",
            [&](const char* v) {
                if (!parseGpuMode(v, gpuMode)) return bad("--gpu wants auto, on or off");
                return true;
            });
    cli.add(nullptr, "--gpu-device", "N", "use GPU N (see --list-devices)", [&](const char* v) {
        long long n;
        if (!whole("--gpu-device", v, 0, 63, n)) return false;
        gpuDevice = (int)n;
        return true;
    });
    cli.note("-v", "--verbose", "[LEVEL]",
             "print log messages to the console: debug | info | warn | error\n(default info; without -v only warnings and errors print)");
    cli.note(nullptr, "--log-dir", "DIR", "log file folder (default $WVT_LOG_DIR, then $TMPDIR, then /tmp)");
    cli.note(nullptr, "--no-log-file", nullptr, "don't write wvt_log_<date>_<time>.log");
    cli.note(nullptr, "--config", "FILE", "configuration file (default $WVT_CONFIG, then ~/.config/wvt/wvt.conf)");
    cli.note(nullptr, "--no-config", nullptr, "read no configuration file: the built-in defaults");
    cli.action("-h", "--help", "show this help", [&](const char*) {
        exitNow = 0;
        showHelp = true;
        return true;
    });

    cli.section("live devices (-c and -r set the tuning):");
    cli.add(nullptr, "--device", "DEV",
            "rtlsdr[:INDEX|SERIAL] | uhd[:ARGS] | bb[:SERIAL] | skiq[:SERIAL|#N] | sim\n"
            "e.g. --device uhd  --device uhd:addr=192.168.10.2  --device rtlsdr:0  --device bb  --device skiq:#0",
            [&](const char* v) {
                const std::string s = v;
                const size_t c = s.find(':');
                dev.driver = s.substr(0, c);
                dev.args = c == std::string::npos ? "" : s.substr(c + 1);
                return haveDevice = true;
            });
    cli.add(nullptr, "--gain", "DB|auto", "receiver gain (default 30 dB)", [&](const char* v) {
        if (std::string(v) == "auto") dev.agc = true;
        else if (!parseSI(v, dev.gain)) return bad("bad gain");
        return haveGain = true;
    });
    cli.add(nullptr, "--ref-level", "DBM", "BB60: strongest expected input (default -20 dBm)", [&](const char* v) {
        if (!parseSI(v, dev.gain)) return bad("bad reference level");
        return haveGain = true;
    });
    cli.add(nullptr, "--atten", "auto|0|10|20|30", "BB60 attenuator (default auto)", [&](const char* v) {
        const std::string s = v;
        if (s == "auto") dev.bbAtten = -1;
        else if (s == "0" || s == "10" || s == "20" || s == "30") dev.bbAtten = s[0] - '0';
        else return bad("--atten wants auto, 0, 10, 20 or 30");
        return true;
    });
    cli.add(nullptr, "--bb-amp", "auto|0-3", "BB60 amplifier setting (default auto)", [&](const char* v) {
        const std::string s = v;
        if (s == "auto") dev.bbAmp = -1;
        else if (s.size() == 1 && s[0] >= '0' && s[0] <= '3') dev.bbAmp = s[0] - '0';
        else return bad("--bb-amp wants auto or 0-3");
        return true;
    });
    cli.add(nullptr, "--ext-ref", nullptr, "BB60: lock to a 10 MHz reference on port 1; Sidekiq: use the external reference clock",
            flag(dev.extRef));
    cli.add(nullptr, "--antenna", "NAME", "USRP receive antenna (e.g. RX2, TX/RX) or Sidekiq RF port (e.g. \"J3 (RX A2)\" or J1; default: the device's)",
            text(dev.antenna));
    cli.add(nullptr, "--usrp-channel", "N", "USRP receive / transmit channel (X310, N310, X410 have several; default 0)",
            [&](const char* v) {
                long long c;
                if (!whole("--usrp-channel", v, 0, 63, c)) return false;
                dev.channel = txc.channel = (int)c;
                return true;
            });
    cli.add(nullptr, "--ref", "SRC", "USRP clock and time reference: internal | external | gpsdo", [&](const char* v) {
        dev.ref = txc.ref = v;
        return true;
    });
    cli.add(nullptr, "--bandwidth", "HZ", "USRP analog / BB60 I/Q / Sidekiq RF bandwidth (default: automatic)", [&](const char* v) {
        if (!parseSI(v, dev.bandwidth)) return bad("bad bandwidth");
        return true;
    });
    cli.add(nullptr, "--bias-tee", nullptr, "RTL-SDR: power an LNA / active antenna over the coax", flag(dev.biasTee));
    cli.add(nullptr, "--direct-sampling", "I|Q", "RTL-SDR: HF direct sampling (v3: Q)", [&](const char* v) {
        const std::string s = v;
        dev.directSampling = (s == "I" || s == "i" || s == "1") ? 1 : (s == "Q" || s == "q" || s == "2") ? 2 : 0;
        return true;
    });
    cli.add(nullptr, "--ppm", "N", "RTL-SDR frequency correction", [&](const char* v) {
        long long n;
        if (!whole("--ppm", v, -1000, 1000, n)) return false;
        dev.ppm = (int)n;
        return true;
    });
    cli.action(nullptr, "--list-devices", "print attached devices and GPUs, and exit", [&](const char*) {
        for (const auto& d : enumerateDevices()) printf("%-8s %-28s %s\n", d.driver.c_str(), d.args.c_str(), d.label.c_str());
        printf("\nGPUs (--gpu-device N):\n");
        for (const auto& g : gpuListDevices()) printf("  %s\n", g.c_str());
        exitNow = 0;
        return true;
    });

    cli.section("recording (needs --device):");
    cli.add(nullptr, "--record", "PREFIX", "record to PREFIX_<date>_<time>_<fc>_<rate>.<ext>", [&](const char* v) {
        rc.prefix = v;
        return haveRecord = true;
    });
    cli.add(nullptr, "--record-format", "sigmf|raw", "(default sigmf)", [&](const char* v) {
        const std::string s = v;
        if (s != "sigmf" && s != "raw") return bad("--record-format wants sigmf or raw");
        rc.sigmf = s == "sigmf";
        return haveRecordFormat = true;
    });
    cli.add(nullptr, "--record-sample", "native|cf32", "(default native: cu8 for RTL-SDR, ci16 for USRP and BB60)",
            [&](const char* v) {
                const std::string s = v;
                if (s != "native" && s != "cf32") return bad("--record-sample wants native or cf32");
                rc.cf32 = s == "cf32";
                return true;
            });
    cli.add(nullptr, "--record-duration", "T", "stop after T seconds, e.g. 30", [&](const char* v) {
        if (!parseSI(v, rc.maxSeconds) || rc.maxSeconds <= 0) return bad("bad --record-duration");
        return true;
    });
    cli.add(nullptr, "--no-gui", nullptr, "record without opening a window (Ctrl+C stops)", flag(noGui));

    cli.section("signal generator (-c sets the center frequency, -r the sample rate):");
    cli.add(nullptr, "--siggen", nullptr, "open the signal generator window", flag(openGen));
    cli.add(nullptr, "--gen", "cw|pulsed|playback", "waveform mode (also opens the window)", [&](const char* v) {
        const std::string s = v;
        if (s != "cw" && s != "pulsed" && s != "playback") return bad("--gen wants cw, pulsed or playback");
        gp.mode = s == "cw" ? GenMode::CW : s == "pulsed" ? GenMode::Pulsed : GenMode::Playback;
        return haveGen = true;
    });
    // These set a generator value, so they also open the generator.
    const auto gen = [&](const char* name, double& dst, const std::function<void()>& then = {}) {
        return si(name, dst, [&, then] {
            if (then) then();
            haveGen = true;
        });
    };
    cli.add(nullptr, "--offset", "HZ", "carrier offset from the center (default 100k)",
            gen("--offset", gp.offset, [&] { haveOffset = true; }));
    cli.add(nullptr, "--amp", "DBFS", "peak amplitude (default -6)", gen("--amp", gp.ampDb));
    cli.add(nullptr, "--drift", "HZ", "CW frequency drift per second", gen("--drift", gp.driftHzPerSec));
    cli.add(nullptr, "--sweep", "FROM:TO",
            "frequency sweep, offsets from the center (e.g. -400k:400k); CW or,\nwith --gen pulsed, pulse to pulse",
            pair("--sweep wants A:B", gp.fFrom, gp.fTo, [&] {
                if (gp.fSweep == SweepShape::Off) gp.fSweep = SweepShape::RampUp;
                return haveGen = haveSweep = true;
            }));
    const auto shape = [&](const char* name, SweepShape& dst) -> CliParser::Handler {
        return [&, name](const char* v) {
            const std::string s = v;
            const SweepShape sh = s == "up" ? SweepShape::RampUp : s == "down" ? SweepShape::RampDown
                                : s == "tri" ? SweepShape::Triangle : SweepShape::Off;
            if (sh == SweepShape::Off) return bad("%s wants up, down or tri", name);
            dst = sh;
            return haveGen = haveSweep = true;
        };
    };
    cli.add(nullptr, "--sweep-shape", "up|down|tri", "frequency sweep shape", shape("--sweep-shape", gp.fSweep));
    cli.add(nullptr, "--sweep-step", "HZ:DWELL", "stepped frequency sweep",
            pair("--sweep-step wants A:B", gp.fStep, gp.fDwell, [&] {
                gp.fSweep = SweepShape::Stepped;
                return haveGen = haveSweep = true;
            }));
    cli.add(nullptr, "--sweep-time", "S", "sweep period", [&](const char* v) {
        if (!parseSI(v, gp.sweepPeriod)) return bad("bad --sweep-time");
        return haveGen = haveSweep = true;
    });
    cli.add(nullptr, "--level-sweep", "FROM:TO", "level sweep in dBFS over the same period (CW or pulsed)",
            pair("--level-sweep wants A:B", gp.aFrom, gp.aTo, [&] {
                if (gp.aSweep == SweepShape::Off) gp.aSweep = SweepShape::RampUp;
                return haveGen = haveSweep = true;
            }));
    cli.add(nullptr, "--level-shape", "up|down|tri", "level sweep shape (sweeps repeat until stopped)",
            shape("--level-shape", gp.aSweep));
    cli.add(nullptr, "--pri", "S", "pulse repetition interval", gen("--pri", gp.pri));
    cli.add(nullptr, "--prf", "HZ", "pulse repetition frequency (sets --pri to 1/HZ)", [&](const char* v) {
        double f;
        if (!parseSI(v, f)) return bad("bad value for %s", "--prf");
        gp.pri = f > 0 ? 1 / f : 0;
        return haveGen = true;
    });
    cli.add(nullptr, "--pw", "S", "pulse width, edges included", gen("--pw", gp.pw));
    cli.add(nullptr, "--edge", "rect|linear|cos|gauss", "pulse edge shape", [&](const char* v) {
        const std::string s = v;
        if (s == "rect") gp.edge = EdgeShape::Rect;
        else if (s == "linear") gp.edge = EdgeShape::Linear;
        else if (s == "cos") gp.edge = EdgeShape::RaisedCos;
        else if (s == "gauss") gp.edge = EdgeShape::Gaussian;
        else return bad("--edge wants rect, linear, cos or gauss");
        return haveGen = true;
    });
    cli.add(nullptr, "--rise", "S", "rise time of the pulse edge", gen("--rise", gp.rise));
    cli.add(nullptr, "--fall", "S", "fall time of the pulse edge", gen("--fall", gp.fall));
    cli.add(nullptr, "--stagger", "S,S,..", "PRIs used in turn", [&](const char* v) {
        const std::string s = v;
        std::string tok;
        gp.stagger.clear();
        for (size_t k = 0; k <= s.size(); ++k) {
            if (k == s.size() || s[k] == ',') {
                double x;
                if (!parseSI(tok.c_str(), x)) return bad("bad --stagger list");
                gp.stagger.push_back(x);
                tok.clear();
            } else {
                tok += s[k];
            }
        }
        gp.priPattern = PriPattern::Stagger;
        return haveGen = true;
    });
    cli.add(nullptr, "--jitter", "PCT", "random PRI jitter in percent",
            gen("--jitter", gp.jitterPct, [&] { gp.priPattern = PriPattern::Jitter; }));
    cli.add(nullptr, "--seed", "N", "random seed for the jitter and the noise", [&](const char* v) {
        long long n;
        if (!whole("--seed", v, 0, UINT32_MAX, n)) return false;
        gp.seed = (uint32_t)n;
        return true;
    });
    cli.add(nullptr, "--burst", "N[:GAP]", "N pulses per burst, GAP seconds of extra silence after each", [&](const char* v) {
        const std::string s = v;
        const size_t c = s.find(':');
        long long n;
        if (!whole("--burst", s.substr(0, c).c_str(), 0, INT_MAX, n)) return false;
        gp.burstPulses = (int)n;
        if (c != std::string::npos && !parseSI(s.substr(c + 1).c_str(), gp.burstGap)) return bad("--burst wants N or N:GAP");
        return haveGen = true;
    });
    cli.add(nullptr, "--delay", "S", "silence before the first pulse", gen("--delay", gp.startDelay));
    cli.add(nullptr, "--lfm", "up|down|tri:BW", "chirp each pulse, e.g. --lfm up:2M", [&](const char* v) {
        const std::string s = v;
        const size_t c = s.find(':');
        const std::string dir = s.substr(0, c);
        gp.intra = dir == "up" ? IntraMod::LfmUp : dir == "down" ? IntraMod::LfmDown : dir == "tri" ? IntraMod::LfmTriangle : IntraMod::None;
        if (gp.intra == IntraMod::None || c == std::string::npos || !parseSI(s.substr(c + 1).c_str(), gp.chirpBw))
            return bad("--lfm wants up|down|tri:BW, e.g. up:2M");
        return haveGen = true;
    });
    cli.add(nullptr, "--barker", "N", "Barker phase code (2 3 4 5 7 11 13)", [&](const char* v) {
        long long n;
        if (!whole("--barker", v, 0, INT_MAX, n)) return false;
        gp.intra = IntraMod::Barker;
        gp.barkerLen = (int)n;
        return haveGen = true;
    });
    cli.add(nullptr, "--play", "FILE",
            "play back a SigMF or raw IQ recording (-f overrides its format;\nits rate and frequency become -r and -c unless given)",
            [&](const char* v) {
                playFile = v;
                gp.mode = GenMode::Playback;
                return haveGen = true;
            });
    cli.add(nullptr, "--play-rate", "HZ", "the recording's sample rate (resampled to -r if different)",
            gen("--play-rate", playRate));
    cli.add(nullptr, "--play-segment", "START:LEN", "play part of it (LEN 0 = to the end)",
            pair("--play-segment wants START:LEN, e.g. 0.5:0.1", gp.playStart, gp.playLen));
    cli.add(nullptr, "--play-gap", "S", "silence between repeats", gen("--play-gap", gp.playGap));
    cli.add(nullptr, "--play-once", nullptr, "don't repeat", [&](const char*) {
        gp.playLoop = false;
        return true;
    });
    cli.add(nullptr, "--chain", "FILE", "play a chain saved from the window (.wvtchain.json)", [&](const char* v) {
        chainFile = v;
        return haveGen = true;
    });
    cli.add(nullptr, "--chain-add", "PATH[@OFFSET][:N]",
            "add a recording to the chain (repeatable), played N times,\n"
            "e.g. b13.sigmf-meta@250k:3; the first sets -r and -c unless given",
            [&](const char* v) {
                chainAdds.push_back(v);
                return haveGen = true;
            });
    cli.add(nullptr, "--chain-loops", "N", "passes through the chain (default 0 = until stopped)", [&](const char* v) {
        long long n;
        if (!whole("--chain-loops", v, 0, INT_MAX, n)) return false;
        chainLoops = (int)n;
        return true;
    });
    cli.add(nullptr, "--snr", "DB", "add noise at this signal-to-noise ratio",
            gen("--snr", gp.snrDb, [&] { gp.noise = true; }));
    cli.add(nullptr, "--dc", "I:Q", "DC offset", pair("--dc wants A:B", gp.dcI, gp.dcQ, [&] { return haveGen = true; }));
    cli.add(nullptr, "--iq", "GAIN_DB:PHASE_DEG", "I/Q imbalance",
            pair("--iq wants A:B", gp.iqGainDb, gp.iqPhaseDeg, [&] { return haveGen = true; }));
    cli.add(nullptr, "--linewidth", "HZ", "phase noise linewidth", gen("--linewidth", gp.linewidth));
    cli.add(nullptr, "--gen-out", "PREFIX", "write to a file and exit (format via --record-format)", [&](const char* v) {
        gc.rec.prefix = v;
        return haveGenOut = true;
    });
    cli.add(nullptr, "--gen-sample", "cf32|ci16|ci8|cu8", "file sample format (default cf32)", [&](const char* v) {
        bool be;
        if (!parseFormatName(v, gc.fmt, be) || be ||
            !(gc.fmt == SampleFormat::CF32 || gc.fmt == SampleFormat::CI16 || gc.fmt == SampleFormat::CI8 || gc.fmt == SampleFormat::CU8))
            return bad("--gen-sample wants cf32, ci16, ci8 or cu8");
        return true;
    });
    cli.add(nullptr, "--gen-duration", "T", "file length (default 1 s), or how long --tx transmits", si("--gen-duration", genSeconds));
    cli.add(nullptr, "--gen-pulses", "N", "file length in pulses, plays, sweeps or chain passes", [&](const char* v) {
        long long n;
        if (!whole("--gen-pulses", v, 0, LLONG_MAX, n)) return false;
        gc.pulses = n;
        return true;
    });
    cli.add(nullptr, "--gen-annotate", nullptr, "SigMF: one annotation per pulse, play, sweep or chain item",
            flag(gc.annotatePulses));
    cli.add(nullptr, "--tx", nullptr, "TRANSMIT with a USRP until Ctrl+C (or --gen-duration)", flag(haveTx));
    cli.add(nullptr, "--tx-gain", "DB", "transmit gain (default 0)", si("--tx-gain", txc.gain));
    cli.add(nullptr, "--tx-args", "ARGS", "the USRP to transmit with (default: the first USRP)", text(txc.args));
    cli.add(nullptr, "--tx-antenna", "NAME", "transmit port (default: the channel's first, e.g. TX/RX)", text(txc.antenna));
    cli.add(nullptr, "--tx-dry-run", nullptr, "show what --tx would transmit, check the limits, and exit", [&](const char*) {
        return haveTx = txDryRun = true;
    });
    cli.add(nullptr, "--tx-confirm", nullptr, "transmit without asking (needed when stdin is not a terminal)", flag(txConfirm));
    cli.add(nullptr, "--tx-max-gain", "DB", "refuse a higher transmit gain, also in the GUI (env WVT_TX_MAX_GAIN)",
            si("--tx-max-gain", cliLimits.maxGain));
    cli.add(nullptr, "--tx-allow", "LO:HI,..", "transmit only inside these bands, also in the GUI (env WVT_TX_ALLOW)",
            [&](const char* v) {
                std::string err;
                if (!parseTxBands(v, cliLimits.allow, err)) return bad("--tx-allow: %s", err.c_str());
                return true;
            });

    cli.section("export part of a file (the --segment limits it; --record-format sigmf|raw):");
    cli.add(nullptr, "--extract-out", "PATH", "write it to PATH (.sigmf-data / .sigmf-meta, or raw) and exit",
            text(extractOut));
    cli.add(nullptr, "--extract", "T0:T1", "time range in seconds (default: the whole segment)",
            pair("--extract wants T0:T1", extract0, extract1, [&] { return haveExtract = true; }));
    cli.add(nullptr, "--extract-band", "LO:HI",
            "only this band (absolute Hz), mixed to 0 Hz, filtered and\ndecimated; without it the samples are copied unchanged",
            pair("--extract-band wants LO:HI", extractLo, extractHi, [&] { return haveExtractBand = true; }));
    cli.add(nullptr, "--extract-format", "cf32|ci16", "band output format (default cf32)", [&](const char* v) {
        const std::string s = v;
        if (s != "cf32" && s != "ci16") return bad("--extract-format wants cf32 or ci16");
        extractCi16 = s == "ci16";
        return true;
    });
    cli.add(nullptr, "--extract-overwrite", nullptr, "replace existing output files", flag(extractOverwrite));
    cli.add(nullptr, "--export-window", nullptr, "open the export window (Ctrl+E): the --chan band, or the view",
            flag(openExportWin));

    cli.section("audio (the channel comes from --chan OFFSET:BW; without it: 0 Hz, the mode's width):");
    cli.add(nullptr, "--audio", "am|nfm|wfm|usb|lsb|cw", "open the audio window and play", [&](const char* v) {
        std::string s = v;
        for (auto& c : s) c = (char)tolower(c);
        if (!parseAudioMode(s, ap.mode)) return bad("--audio wants am, nfm, wfm, usb, lsb or cw");
        return haveAudio = true;
    });
    cli.add(nullptr, "--audio-out", "FILE.wav",
            "demodulate to WAV and exit: the file (or --view range),\nor with --device the live stream until Ctrl+C / --record-duration",
            text(audioOut));
    cli.add(nullptr, "--audio-device", "NAME", "sound output (default: system default)", text(audioDevice));
    cli.action(nullptr, "--list-audio-devices", "print sound output devices and exit", [&](const char*) {
        std::string err;
        auto devs = listAudioDevices(err);
        exitNow = 1;
        if (!err.empty()) return bad("%s", err.c_str());
        for (const auto& d : devs) printf("%s%s\n", d.name.c_str(), d.isDefault ? "  (default)" : "");
        exitNow = 0;
        return true;
    });
    cli.add(nullptr, "--volume", "DB", "playback volume (default -6)", [&](const char* v) {
        if (!parseSI(v, audioVol)) return bad("bad --volume");
        return true;
    });
    cli.add(nullptr, "--squelch", "DBFS", "mute while the channel power is below this", [&](const char* v) {
        if (!parseSI(v, ap.squelchDb)) return bad("bad --squelch");
        return ap.squelch = true;
    });
    cli.epilogue(
        "Format, rate and center frequency are auto-detected from SigMF metadata,\n"
        "gqrx file names and extensions (.cu8 .cs8 .cs16 .cf32 ...) when possible.\n"
        "\n"
        "mouse:  drag = pan, wheel = zoom (over an axis: zoom that axis only),\n"
        "        right-drag = box zoom, double-click = fit, Ctrl+click spectrum/waterfall = tune channel\n"
        "        right-click the waterfall = export a region (view, channel or a box you drag)\n"
        "        Shift+click a time plot or the waterfall = play audio from there\n"
        "keys:   Left/Right pan, PgUp/PgDn page, +/- zoom, Home/End, A all, T/F markers,\n"
        "        Z zoom to time markers, C channel on/off, B bookmark,\n"
        "        Ctrl+G signal generator, Ctrl+D audio window, Ctrl+E export window\n");

    std::vector<std::string> positional;
    const bool parsed = cli.parse(argc, argv, positional);
    if (showHelp) {
        printf("%s", cli.help("usage: wvt [options] [file]\n\nInteractive waterfall / AM / FM / PM viewer for raw IQ recordings.\n").c_str());
        return 0;
    }
    if (!parsed) return exitNow > 0 ? exitNow : 2;
    if (exitNow >= 0) return exitNow;
    if (std::string err; !txLimitsFromEnv(txc.limits, err)) {
        LOG_ERROR("main", "%s", err.c_str());
        return 2;
    }
    if (!std::isnan(cliLimits.maxGain)) txc.limits.maxGain = cliLimits.maxGain;
    if (!cliLimits.allow.empty()) txc.limits.allow = cliLimits.allow;
    if (!positional.empty()) path = positional.back();
    scale = std::clamp(scale, 0.5f, 4.f);
    if (ov.haveFc) gp.fc = txc.fc = ov.fc;
    if (ov.haveRate) gp.rate = ov.rate;
    if (haveSweep && gp.mode != GenMode::CW && gp.mode != GenMode::Pulsed) {
        LOG_ERROR("main", "the sweep options need --gen cw or --gen pulsed");
        return 2;
    }
    if (!chainFile.empty() || !chainAdds.empty()) {
        if (haveSweep || !playFile.empty()) {
            LOG_ERROR("main", "--chain and --chain-add can't be combined with --play or a sweep");
            return 2;
        }
        gp.mode = GenMode::Chain;
        if (!chainFile.empty()) {
            std::string err;
            const WaveformParams before = gp;
            if (!loadChain(chainFile, gp, err)) {
                LOG_ERROR("main", "%s", err.c_str());
                return 2;
            }
            if (ov.haveRate) gp.rate = before.rate;
            if (ov.haveFc) gp.fc = before.fc;
        }
        for (const std::string& spec : chainAdds) {
            // PATH[@OFFSET][:N]
            std::string path = spec;
            int reps = 1;
            const size_t x = path.find_last_of(':');
            if (x != std::string::npos && x + 1 < path.size() &&
                path.find_first_not_of("0123456789", x + 1) == std::string::npos) {
                char* end = nullptr;
                errno = 0;
                const long n = strtol(path.c_str() + x + 1, &end, 10);
                if (errno == ERANGE || n < 1 || n > INT_MAX) {
                    LOG_ERROR("main", "--chain-add: the repeat count N in PATH:N must be a whole number from 1 to %d (got '%s')",
                              INT_MAX, path.c_str() + x + 1);
                    return 2;
                }
                reps = (int)n;
                path.erase(x);
            }
            const size_t at = path.find_last_of('@');
            double off = 0;
            const bool haveOff = at != std::string::npos;
            if (haveOff) {
                if (!parseSI(path.substr(at + 1).c_str(), off)) {
                    LOG_ERROR("main", "--chain-add wants PATH[@OFFSET][:N], e.g. b13.sigmf-meta@250k:3");
                    return 2;
                }
                path.erase(at);
            }
            if (gp.chain.empty()) {  // the first recording sets the rate and center unless -r / -c do
                const FileMetaGuess g = guessMeta(expandHome(path));
                if (!ov.haveRate && g.haveRate) gp.rate = g.rate;
                if (!ov.haveFc && g.haveFc) gp.fc = g.fc;
            }
            if (std::string err; !metaReadable(guessMeta(expandHome(path)), err)) {
                LOG_ERROR("main", "--chain-add: %s", err.c_str());
                return 2;
            }
            std::string note;
            ChainItem c = makeChainItem(path, gp, &note);
            if (haveOff) c.offset = off;
            c.repeats = reps;
            LOG_INFO("main", "chain item %zu: %s (%s)", gp.chain.size() + 1, c.path.c_str(), note.c_str());
            gp.chain.push_back(c);
        }
        if (chainLoops >= 0) gp.chainLoops = chainLoops;
        txc.fc = gp.fc;
    }
    if (!playFile.empty()) {
        // Plays as recorded unless -r / -c / --offset say otherwise.
        const FileMetaGuess g = guessMeta(expandHome(playFile));
        if (std::string err; !ov.haveFmt && !metaReadable(g, err)) {
            LOG_ERROR("main", "--play: %s; -f FMT reads it anyway", err.c_str());
            return 2;
        }
        gp.playPath = g.dataPath;
        if (g.haveFmt) { gp.playFp.fmt = g.fmt; gp.playFp.bigEndian = g.bigEndian; }
        if (ov.haveFmt) { gp.playFp.fmt = ov.fmt; gp.playFp.bigEndian = ov.bigEndian; }
        if (ov.header > 0) gp.playFp.headerBytes = ov.header;
        gp.playFp.swapIQ = ov.swapIQ;
        gp.playRate = playRate > 0 ? playRate : g.haveRate ? g.rate : 0;
        if (!ov.haveRate && gp.playRate > 0) gp.rate = gp.playRate;
        if (!ov.haveFc && g.haveFc) gp.fc = txc.fc = g.fc;
        if (!haveOffset) gp.offset = 0;
    }
    if (haveGenOut || haveTx) {
        if (haveGenOut && haveTx) {
            LOG_ERROR("main", "use either --gen-out or --tx");
            return 2;
        }
        ParamCheck chk = checkParams(gp);
        for (const auto& w : chk.warnings) LOG_WARN("main", "%s", w.c_str());
        if (!chk.ok()) {
            for (const auto& e : chk.errors) LOG_ERROR("main", "%s", e.c_str());
            return 2;
        }
        if (haveGenOut) {
            gc.rec.sigmf = haveRecordFormat ? rc.sigmf : cfg.siggenSigmf;
            gc.seconds = genSeconds > 0 ? genSeconds : 1;
            if (gc.pulses > 0 && gp.mode == GenMode::CW && !gp.sweeping()) {
                LOG_ERROR("main", "--gen-pulses needs --gen pulsed, --play, a chain or a sweep");
                return 2;
            }
            return runGenFile(gp, gc);
        }
        txc.rate = gp.rate;
        txc.fc = gp.fc;
        if (const int rc = confirmTx(gp, txc, genSeconds, txDryRun, txConfirm); rc != 0) return std::max(rc, 0);
        return runTx(gp, txc, genSeconds);
    }
    if (haveDevice) {
        if (ov.haveFc) dev.fc = ov.fc;
        else if (dev.driver == "siggen" && haveGen) dev.fc = gp.fc;  // the loopback listens where the generator plays
        if (ov.haveRate) dev.rate = ov.rate;
        else if (dev.driver == "uhd" || dev.driver == "bb" || dev.driver == "skiq") dev.rate = 10e6;
        if (dev.driver == "bb") {
            // The BB60 has a reference level, not a gain, and no AGC.
            if (dev.agc) {
                LOG_ERROR("main", "the BB60 has no automatic gain: use --ref-level DBM");
                return 2;
            }
            if (!haveGain) dev.gain = -20;
            const double snapped = snapSampleRate("bb", dev.rate);
            if (std::fabs(snapped - dev.rate) > 1)
                LOG_WARN("main", "BB60 rate %s: using %s (40 MS/s divided by a power of 2)",
                         fmtSI(dev.rate, "S/s", 6).c_str(), fmtSI(snapped, "S/s", 6).c_str());
            dev.rate = snapped;
        }
        else if (dev.driver == "siggen" && haveGen) dev.rate = gp.rate;
    }
    // The audio channel: --chan, or the mode's default width at 0 Hz.
    ap.bw = haveChan ? chanBw : audioModeDefaultBw((AudioMode)ap.mode);
    ap.offset = haveChan ? chanOff : 0;
    if (ap.mode == (int)AudioMode::WFM) ap.deemph = (int)Deemphasis::Us75;
    if (!extractOut.empty()) {
        if (path.empty()) {
            LOG_ERROR("main", "--extract-out needs a file");
            return 2;
        }
        return runExtract(path, ov, seg0, seg1, haveExtract, extract0, extract1, haveExtractBand, extractLo, extractHi,
                          extractCi16, rc.sigmf, extractOverwrite, extractOut);
    }
    if (haveExtract || haveExtractBand) {
        LOG_ERROR("main", "--extract and --extract-band need --extract-out");
        return 2;
    }
    if (!audioOut.empty()) {
        if (haveDevice) return runAudioLive(dev, ap, audioOut, rc.maxSeconds);
        if (path.empty()) {
            LOG_ERROR("main", "--audio-out needs a file or --device");
            return 2;
        }
        return runAudioExport(path, ov, haveView, view0, view1, seg0, seg1, ap, audioOut);
    }
    if ((haveRecord || noGui) && !haveDevice) {
        LOG_ERROR("main", "recording needs --device");
        return 2;
    }
    if (noGui) {
        if (!haveRecord) {
            LOG_ERROR("main", "--no-gui needs --record");
            return 2;
        }
        return runHeadless(dev, rc);
    }

    {
        std::string gpuErr;
        if (!gpuInit(gpuMode, gpuDevice, gpuErr)) {
            LOG_ERROR("gpu", "%s", gpuErr.c_str());
            return 1;
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 2;
    io.Fonts->AddFontFromMemoryTTF((void*)g_embedded_font, (int)g_embedded_font_size, std::round(15.f * scale), &fc);
    SetupStyle(scale);
    ImPlot::GetStyle().Use24HourClock = true;

    X11Window win;
    std::string err;
    if (!win.create("wvt - IQ waveform viewer", winW, winH, software, err)) {
        LOG_ERROR("main", "%s", err.c_str());
        return 1;
    }
    LOG_INFO("main", "window %dx%d, scale %.2f, %s renderer", winW, winH, scale, win.usingGL() ? "GLX" : "software");

    int exitCode = 0;
    {
        App app(win, cfg);
        app.setTxLimits(txc.limits);
        if (haveGen || openGen) app.openSigGen(haveGen ? &gp : nullptr);
        if (haveDevice) {
            LOG_INFO("main", "starting device %s%s%s", dev.driver.c_str(), dev.args.empty() ? "" : ":", dev.args.c_str());
            // App logs the reason when these fail.
            if (app.startDevice(dev) && haveRecord) app.startRecording(rc);
        } else if (!path.empty()) {
            LOG_INFO("main", "opening %s", path.c_str());
            if (haveSeg) app.setSegment(seg0, seg1);
            app.openFile(path, ov);  // logs the reason on failure
            if (haveView) app.setView(view0, view1);
            if (haveChan) app.setChannel(chanOff, chanBw);
            if (openExportWin) app.openExport();
        }
        if (haveAudio) {
            if (haveDevice && haveChan) app.setChannel(chanOff, chanBw);
            app.setAudioOutput(audioDevice, audioVol);
            app.openAudio(&ap, haveDevice || !path.empty());
        }

        auto lastActivity = std::chrono::steady_clock::now();
        int frame = 0, idleFrames = 0;
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            bool active = app.busy() || app.audioBusy() || now - lastActivity < std::chrono::milliseconds(600) ||
                          !shotPath.empty() || !sessionOut.empty();
            if (!win.pumpEvents(active ? 1.0 / 60.0 : 0.5) || app.quitRequested()) break;
            // Any input (or worker wake-up) keeps the loop running at frame rate for a moment.
            if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || ImGui::IsAnyItemActive() ||
                io.InputQueueCharacters.Size > 0 || ImGui::IsAnyMouseDown())
                lastActivity = now;
            if (app.busy() || app.audioBusy()) lastActivity = now;
            win.newFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            win.render(ImGui::GetDrawData(), ImVec4(0.06f, 0.06f, 0.07f, 1.f));
            app.afterRender();
            ++frame;
            if (!shotPath.empty() || !sessionOut.empty()) {
                idleFrames = app.busy() ? 0 : idleFrames + 1;
                // A live stream is never idle: wait until the waterfall has some history.
                bool ready = app.isLive() ? app.liveRowsReceived() >= 90 : (frame > 20 && idleFrames > 10);
                if ((haveGen || openGen) && !app.sigGenPreviewReady()) ready = false;
                if (ready || frame > 3000) {
                    if (!shotPath.empty()) {
                        bool ok = win.screenshot(shotPath);
                        if (ok) logStatus("main", "screenshot %s written", shotPath.c_str());
                        else LOG_ERROR("main", "screenshot %s FAILED", shotPath.c_str());
                    }
                    if (!sessionOut.empty()) {
                        std::string err;
                        if (app.saveSession(sessionOut, false, err)) {
                            logStatus("main", "session %s written", sessionOut.c_str());
                        } else {
                            LOG_ERROR("main", "session not written: %s", err.c_str());
                            exitCode = 1;
                        }
                    }
                    break;
                }
            }
        }
    }
    LOG_INFO("main", "window closed, exiting");
    ImPlot::DestroyContext();
    // Window owns the GL backend; destroy it before the ImGui context.
    return exitCode;
}
