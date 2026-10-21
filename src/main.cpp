// wvt - IQ waveform visualization tool (X11)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "extract.h"
#include "gpu.h"
#include "imgui.h"
#include "implot.h"
#include "log.h"
#include "platform_x11.h"
#include "ui_util.h"

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

static void usage() {
    printf(
        "usage: wvt [options] [file]\n"
        "\n"
        "Interactive waterfall / AM / FM / PM viewer for raw IQ recordings.\n"
        "\n"
        "options:\n"
        "  -f, --format FMT     cf32 | cf64 | ci16 | ci32 | ci8 | cu8  (suffix _be for big-endian)\n"
        "  -r, --rate HZ        sample rate, SI suffixes allowed (e.g. 2.4M)\n"
        "  -c, --fc HZ          center frequency (e.g. 915M)\n"
        "  -H, --header BYTES   bytes to skip at the start of the file\n"
        "      --swap-iq        swap I and Q\n"
        "      --chan OFF:BW    enable the channel filter, e.g. --chan 300k:150k\n"
        "      --segment T0:T1  load only this part of the file, in seconds; -1 = file\n"
        "                       start / end, e.g. --segment 40:50 or --segment 40:-1\n"
        "      --view T0:T1     initial time range in seconds, e.g. --view 0.5:0.52\n"
        "      --software       force the software renderer (no GLX)\n"
        "      --scale S        UI scale factor (default 1, or $WVT_SCALE)\n"
        "      --geometry WxH   initial window size (default 1600x950)\n"
        "      --screenshot F   render, write a PPM screenshot to F and exit\n"
        "      --gpu auto|on|off  GPU processing, CUDA or Metal (default auto, or $WVT_GPU); on = stop\n"
        "                       if no GPU can be used\n"
        "      --gpu-device N   use GPU N (see --list-devices)\n"
        "  -v, --verbose [LEVEL]  print log messages to the console: debug | info | warn | error\n"
        "                       (default info; without -v only warnings and errors print)\n"
        "      --log-dir DIR    log file folder (default $WVT_LOG_DIR, then $TMPDIR, then /tmp)\n"
        "      --no-log-file    don't write wvt_log_<date>_<time>.log\n"
        "  -h, --help           show this help\n"
        "\n"
        "live devices (-c and -r set the tuning):\n"
        "      --device DEV     rtlsdr[:INDEX|SERIAL] | uhd[:ARGS] | bb[:SERIAL] | sim\n"
        "                       e.g. --device uhd:type=b200  --device rtlsdr:0  --device bb\n"
        "      --gain DB|auto   receiver gain (default 30 dB)\n"
        "      --ref-level DBM  BB60: strongest expected input (default -20 dBm)\n"
        "      --atten auto|0|10|20|30   BB60 attenuator (default auto)\n"
        "      --amp auto|0-3   BB60 amplifier setting (default auto)\n"
        "      --ext-ref        BB60: lock to a 10 MHz reference on port 1\n"
        "      --antenna NAME   USRP antenna (RX2 or TX/RX)\n"
        "      --bandwidth HZ   USRP analog / BB60 I/Q bandwidth (default: automatic)\n"
        "      --bias-tee       RTL-SDR: power an LNA / active antenna over the coax\n"
        "      --direct-sampling I|Q  RTL-SDR: HF direct sampling (v3: Q)\n"
        "      --ppm N          RTL-SDR frequency correction\n"
        "      --list-devices   print attached devices and GPUs, and exit\n"
        "\n"
        "recording (needs --device):\n"
        "      --record PREFIX  record to PREFIX_<date>_<time>_<fc>_<rate>.<ext>\n"
        "      --record-format sigmf|raw   (default sigmf)\n"
        "      --record-sample native|cf32 (default native: cu8 for RTL-SDR, ci16 for USRP and BB60)\n"
        "      --record-duration T         stop after T seconds, e.g. 30\n"
        "      --no-gui         record without opening a window (Ctrl+C stops)\n"
        "\n"
        "signal generator (-c sets the center frequency, -r the sample rate):\n"
        "      --siggen         open the signal generator window\n"
        "      --gen cw|pulsed|playback   waveform mode (also opens the window)\n"
        "      --offset HZ      carrier offset from the center (default 100k)\n"
        "      --amp DBFS       peak amplitude (default -6)\n"
        "      --drift HZ       CW frequency drift per second\n"
        "      --sweep FROM:TO  frequency sweep, offsets from the center (e.g. -400k:400k); CW or,\n"
        "                       with --gen pulsed, pulse to pulse\n"
        "      --sweep-shape up|down|tri   --sweep-step HZ:DWELL (stepped)   --sweep-time S (period)\n"
        "      --level-sweep FROM:TO   level sweep in dBFS over the same period (CW or pulsed)\n"
        "      --level-shape up|down|tri   (sweeps repeat until stopped)\n"
        "      --pri S | --prf HZ   pulse repetition interval / frequency\n"
        "      --pw S           pulse width, edges included\n"
        "      --edge rect|linear|cos|gauss   --rise S  --fall S\n"
        "      --stagger S,S,.. PRIs used in turn     --jitter PCT  --seed N\n"
        "      --burst N[:GAP]  N pulses per burst, GAP seconds of extra silence after each\n"
        "      --delay S        silence before the first pulse\n"
        "      --lfm up|down|tri:BW  chirp each pulse, e.g. --lfm up:2M\n"
        "      --barker N       Barker phase code (2 3 4 5 7 11 13)\n"
        "      --play FILE      play back a SigMF or raw IQ recording (-f overrides its format;\n"
        "                       its rate and frequency become -r and -c unless given)\n"
        "      --play-rate HZ   the recording's sample rate (resampled to -r if different)\n"
        "      --play-segment START:LEN   play part of it (LEN 0 = to the end)\n"
        "      --play-gap S     silence between repeats    --play-once  don't repeat\n"
        "      --chain FILE     play a chain saved from the window (.wvtchain.json)\n"
        "      --chain-add PATH[@OFFSET][:N]   add a recording to the chain (repeatable), played N\n"
        "                       times, e.g. b13.sigmf-meta@250k:3; the first sets -r and -c unless given\n"
        "      --chain-loops N  passes through the chain (default 0 = until stopped)\n"
        "      --snr DB  --dc I:Q  --iq GAIN_DB:PHASE_DEG  --linewidth HZ   impairments\n"
        "      --gen-out PREFIX write to a file and exit (format via --record-format)\n"
        "      --gen-sample cf32|ci16|ci8|cu8   file sample format (default cf32)\n"
        "      --gen-duration T | --gen-pulses N   file length (default 1 s; N pulses, plays, sweeps\n"
        "                       or chain passes)\n"
        "      --gen-annotate   SigMF: one annotation per pulse, play, sweep or chain item\n"
        "      --tx             TRANSMIT with a USRP until Ctrl+C (or --gen-duration)\n"
        "      --tx-gain DB     transmit gain (default 0)   --tx-args ARGS  (default type=b200)\n"
        "\n"
        "export part of a file (the --segment limits it; --record-format sigmf|raw):\n"
        "      --extract-out PATH    write it to PATH (.sigmf-data / .sigmf-meta, or raw) and exit\n"
        "      --extract T0:T1       time range in seconds (default: the whole segment)\n"
        "      --extract-band LO:HI  only this band (absolute Hz), mixed to 0 Hz, filtered and\n"
        "                            decimated; without it the samples are copied unchanged\n"
        "      --extract-format cf32|ci16   band output format (default cf32)\n"
        "      --extract-overwrite   replace existing output files\n"
        "      --export-window       open the export window (Ctrl+E): the --chan band, or the view\n"
        "\n"
        "audio (the channel comes from --chan OFFSET:BW; without it: 0 Hz, the mode's width):\n"
        "      --audio am|nfm|wfm|usb|lsb|cw   open the audio window and play\n"
        "      --audio-out FILE.wav  demodulate to WAV and exit: the file (or --view range),\n"
        "                       or with --device the live stream until Ctrl+C / --record-duration\n"
        "      --audio-device NAME   sound output (default: system default)\n"
        "      --list-audio-devices  print sound output devices and exit\n"
        "      --volume DB      playback volume (default -6)\n"
        "      --squelch DBFS   mute while the channel power is below this\n"
        "\n"
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
}

int main(int argc, char** argv) {
    CliOverrides ov;
    std::string path, shotPath;
    bool software = false;
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
    std::string playFile, chainFile;
    std::vector<std::string> chainAdds;
    int chainLoops = -1;
    double playRate = 0;
    double genSeconds = 0;
    AudioParams ap;
    bool haveAudio = false;
    std::string audioOut, audioDevice;
    double audioVol = -6;
    float scale = 1.f;
    int winW = 1600, winH = 950;
    if (const char* e = getenv("WVT_SCALE")) scale = (float)atof(e);
    GpuMode gpuMode = GpuMode::Auto;
    int gpuDevice = -1;

    // Logging options come out of argv first, so the rest of the parse is logged.
    LogConfig lc;
    for (int i = 0; i < argc; ++i) lc.commandLine += (i ? " " : "") + std::string(argv[i]);
    std::vector<char*> args(argv, argv + argc);
    std::string logErr;
    if (!logParseArgs(args, lc, logErr)) {
        LOG_ERROR("main", "%s", logErr.c_str());
        return 2;
    }
    logInit(lc);
    if (const char* e = getenv("WVT_GPU"); e && *e && !parseGpuMode(e, gpuMode))
        LOG_WARN("main", "WVT_GPU=%s is not auto, on or off; using auto", e);
    argc = (int)args.size();
    args.push_back(nullptr);
    argv = args.data();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                LOG_ERROR("main", "%s needs a value", what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-f" || a == "--format") {
            const char* v = next("--format");
            if (!parseFormatName(v, ov.fmt, ov.bigEndian)) { LOG_ERROR("main", "unknown format '%s'", v); return 2; }
            ov.haveFmt = true;
        } else if (a == "-r" || a == "--rate") {
            if (!parseSI(next("--rate"), ov.rate) || ov.rate <= 0) { LOG_ERROR("main", "bad rate"); return 2; }
            ov.haveRate = true;
        } else if (a == "-c" || a == "--fc") {
            if (!parseSI(next("--fc"), ov.fc)) { LOG_ERROR("main", "bad center frequency"); return 2; }
            ov.haveFc = true;
        } else if (a == "-H" || a == "--header") ov.header = atoll(next("--header"));
        else if (a == "--swap-iq") ov.swapIQ = true;
        else if (a == "--chan") {
            if (!parsePair(next("--chan"), chanOff, chanBw)) { LOG_ERROR("main", "--chan wants OFFSET:BW"); return 2; }
            haveChan = true;
        } else if (a == "--view") {
            if (!parsePair(next("--view"), view0, view1)) { LOG_ERROR("main", "--view wants T0:T1"); return 2; }
            haveView = true;
        } else if (a == "--segment") {
            if (!parsePair(next("--segment"), seg0, seg1)) { LOG_ERROR("main", "--segment wants T0:T1 (-1 = file start / end)"); return 2; }
            if (seg0 >= 0 && seg1 >= 0 && seg1 <= seg0) { LOG_ERROR("main", "--segment end must be after its start"); return 2; }
            haveSeg = true;
        }
        else if (a == "--extract-out") extractOut = next("--extract-out");
        else if (a == "--extract") {
            if (!parsePair(next("--extract"), extract0, extract1)) { LOG_ERROR("main", "--extract wants T0:T1"); return 2; }
            haveExtract = true;
        } else if (a == "--extract-band") {
            if (!parsePair(next("--extract-band"), extractLo, extractHi)) { LOG_ERROR("main", "--extract-band wants LO:HI"); return 2; }
            haveExtractBand = true;
        } else if (a == "--extract-format") {
            std::string v = next("--extract-format");
            if (v != "cf32" && v != "ci16") { LOG_ERROR("main", "--extract-format wants cf32 or ci16"); return 2; }
            extractCi16 = v == "ci16";
        } else if (a == "--extract-overwrite") extractOverwrite = true;
        else if (a == "--export-window") openExportWin = true;
        else if (a == "--software") software = true;
        else if (a == "--gpu") {
            const char* v = next("--gpu");
            if (!parseGpuMode(v, gpuMode)) { LOG_ERROR("main", "--gpu wants auto, on or off"); return 2; }
        } else if (a == "--gpu-device") {
            const char* v = next("--gpu-device");
            char* end = nullptr;
            gpuDevice = (int)strtol(v, &end, 10);
            if (end == v || *end || gpuDevice < 0) { LOG_ERROR("main", "--gpu-device wants a device number"); return 2; }
        }
        else if (a == "--scale") scale = (float)atof(next("--scale"));
        else if (a == "--geometry") {
            if (sscanf(next("--geometry"), "%dx%d", &winW, &winH) != 2) { LOG_ERROR("main", "bad geometry"); return 2; }
        } else if (a == "--screenshot") shotPath = next("--screenshot");
        else if (a == "--device") {
            std::string v = next("--device");
            size_t c = v.find(':');
            dev.driver = v.substr(0, c);
            dev.args = c == std::string::npos ? "" : v.substr(c + 1);
            haveDevice = true;
        } else if (a == "--gain") {
            std::string v = next("--gain");
            if (v == "auto") dev.agc = true;
            else if (!parseSI(v.c_str(), dev.gain)) { LOG_ERROR("main", "bad gain"); return 2; }
            haveGain = true;
        } else if (a == "--ref-level") {
            if (!parseSI(next("--ref-level"), dev.gain)) { LOG_ERROR("main", "bad reference level"); return 2; }
            haveGain = true;
        } else if (a == "--atten") {
            std::string v = next("--atten");
            if (v == "auto") dev.bbAtten = -1;
            else if (v == "0" || v == "10" || v == "20" || v == "30") dev.bbAtten = atoi(v.c_str()) / 10;
            else { LOG_ERROR("main", "--atten wants auto, 0, 10, 20 or 30"); return 2; }
        } else if (a == "--amp") {
            std::string v = next("--amp");
            if (v == "auto") dev.bbAmp = -1;
            else if (v.size() == 1 && v[0] >= '0' && v[0] <= '3') dev.bbAmp = v[0] - '0';
            else { LOG_ERROR("main", "--amp wants auto or 0-3"); return 2; }
        } else if (a == "--ext-ref") {
            dev.extRef = true;
        } else if (a == "--antenna") dev.antenna = next("--antenna");
        else if (a == "--bandwidth") {
            if (!parseSI(next("--bandwidth"), dev.bandwidth)) { LOG_ERROR("main", "bad bandwidth"); return 2; }
        } else if (a == "--bias-tee") dev.biasTee = true;
        else if (a == "--direct-sampling") {
            std::string v = next("--direct-sampling");
            dev.directSampling = (v == "I" || v == "i" || v == "1") ? 1 : (v == "Q" || v == "q" || v == "2") ? 2 : 0;
        } else if (a == "--ppm") dev.ppm = atoi(next("--ppm"));
        else if (a == "--list-devices") {
            for (const auto& d : enumerateDevices()) printf("%-8s %-28s %s\n", d.driver.c_str(), d.args.c_str(), d.label.c_str());
            printf("\nGPUs (--gpu-device N):\n");
            for (const auto& g : gpuListDevices()) printf("  %s\n", g.c_str());
            return 0;
        } else if (a == "--record") {
            rc.prefix = next("--record");
            haveRecord = true;
        } else if (a == "--record-format") {
            std::string v = next("--record-format");
            if (v != "sigmf" && v != "raw") { LOG_ERROR("main", "--record-format wants sigmf or raw"); return 2; }
            rc.sigmf = v == "sigmf";
        } else if (a == "--record-sample") {
            std::string v = next("--record-sample");
            if (v != "native" && v != "cf32") { LOG_ERROR("main", "--record-sample wants native or cf32"); return 2; }
            rc.cf32 = v == "cf32";
        } else if (a == "--record-duration") {
            if (!parseSI(next("--record-duration"), rc.maxSeconds) || rc.maxSeconds <= 0) {
                LOG_ERROR("main", "bad --record-duration");
                return 2;
            }
        } else if (a == "--no-gui") noGui = true;
        else if (a == "--siggen") openGen = true;
        else if (a == "--gen") {
            std::string v = next("--gen");
            if (v != "cw" && v != "pulsed" && v != "playback") { LOG_ERROR("main", "--gen wants cw, pulsed or playback"); return 2; }
            gp.mode = v == "cw" ? GenMode::CW : v == "pulsed" ? GenMode::Pulsed : GenMode::Playback;
            haveGen = true;
        } else if (a == "--offset" || a == "--amp" || a == "--drift" || a == "--pri" || a == "--prf" || a == "--pw" ||
                   a == "--rise" || a == "--fall" || a == "--jitter" || a == "--delay" || a == "--snr" ||
                   a == "--linewidth" || a == "--tx-gain" || a == "--gen-duration" || a == "--play-rate" || a == "--play-gap") {
            double v;
            if (!parseSI(next(a.c_str()), v)) { LOG_ERROR("main", "bad value for %s", a.c_str()); return 2; }
            if (a == "--offset") { gp.offset = v; haveOffset = true; }
            else if (a == "--play-rate") playRate = v;
            else if (a == "--play-gap") gp.playGap = v;
            else if (a == "--amp") gp.ampDb = v;
            else if (a == "--drift") gp.driftHzPerSec = v;
            else if (a == "--pri") gp.pri = v;
            else if (a == "--prf") gp.pri = v > 0 ? 1 / v : 0;
            else if (a == "--pw") gp.pw = v;
            else if (a == "--rise") gp.rise = v;
            else if (a == "--fall") gp.fall = v;
            else if (a == "--jitter") { gp.jitterPct = v; gp.priPattern = PriPattern::Jitter; }
            else if (a == "--delay") gp.startDelay = v;
            else if (a == "--snr") { gp.snrDb = v; gp.noise = true; }
            else if (a == "--linewidth") gp.linewidth = v;
            else if (a == "--tx-gain") txc.gain = v;
            else genSeconds = v;
            haveGen |= a != "--tx-gain" && a != "--gen-duration";
        } else if (a == "--edge") {
            std::string v = next("--edge");
            if (v == "rect") gp.edge = EdgeShape::Rect;
            else if (v == "linear") gp.edge = EdgeShape::Linear;
            else if (v == "cos") gp.edge = EdgeShape::RaisedCos;
            else if (v == "gauss") gp.edge = EdgeShape::Gaussian;
            else { LOG_ERROR("main", "--edge wants rect, linear, cos or gauss"); return 2; }
            haveGen = true;
        } else if (a == "--stagger") {
            std::string v = next("--stagger"), tok;
            gp.stagger.clear();
            for (size_t k = 0; k <= v.size(); ++k) {
                if (k == v.size() || v[k] == ',') {
                    double x;
                    if (!parseSI(tok.c_str(), x)) { LOG_ERROR("main", "bad --stagger list"); return 2; }
                    gp.stagger.push_back(x);
                    tok.clear();
                } else {
                    tok += v[k];
                }
            }
            gp.priPattern = PriPattern::Stagger;
            haveGen = true;
        } else if (a == "--seed") gp.seed = (uint32_t)atol(next("--seed"));
        else if (a == "--burst") {
            std::string v = next("--burst");
            size_t c = v.find(':');
            gp.burstPulses = atoi(v.substr(0, c).c_str());
            if (c != std::string::npos && !parseSI(v.substr(c + 1).c_str(), gp.burstGap)) {
                LOG_ERROR("main", "--burst wants N or N:GAP");
                return 2;
            }
            haveGen = true;
        } else if (a == "--lfm") {
            std::string v = next("--lfm");
            size_t c = v.find(':');
            std::string dir = v.substr(0, c);
            gp.intra = dir == "up" ? IntraMod::LfmUp : dir == "down" ? IntraMod::LfmDown : dir == "tri" ? IntraMod::LfmTriangle : IntraMod::None;
            if (gp.intra == IntraMod::None || c == std::string::npos || !parseSI(v.substr(c + 1).c_str(), gp.chirpBw)) {
                LOG_ERROR("main", "--lfm wants up|down|tri:BW, e.g. up:2M");
                return 2;
            }
            haveGen = true;
        } else if (a == "--barker") {
            gp.intra = IntraMod::Barker;
            gp.barkerLen = atoi(next("--barker"));
            haveGen = true;
        } else if (a == "--dc" || a == "--iq") {
            double x, y;
            if (!parsePair(next(a.c_str()), x, y)) { LOG_ERROR("main", "%s wants A:B", a.c_str()); return 2; }
            if (a == "--dc") { gp.dcI = x; gp.dcQ = y; }
            else { gp.iqGainDb = x; gp.iqPhaseDeg = y; }
            haveGen = true;
        } else if (a == "--sweep" || a == "--level-sweep" || a == "--sweep-step") {
            double x, y;
            if (!parsePair(next(a.c_str()), x, y)) { LOG_ERROR("main", "%s wants A:B", a.c_str()); return 2; }
            if (a == "--sweep") {
                gp.fFrom = x;
                gp.fTo = y;
                if (gp.fSweep == SweepShape::Off) gp.fSweep = SweepShape::RampUp;
            } else if (a == "--sweep-step") {
                gp.fStep = x;
                gp.fDwell = y;
                gp.fSweep = SweepShape::Stepped;
            } else {
                gp.aFrom = x;
                gp.aTo = y;
                if (gp.aSweep == SweepShape::Off) gp.aSweep = SweepShape::RampUp;
            }
            haveGen = haveSweep = true;
        } else if (a == "--sweep-shape" || a == "--level-shape") {
            std::string v = next(a.c_str());
            const SweepShape sh = v == "up" ? SweepShape::RampUp : v == "down" ? SweepShape::RampDown
                                : v == "tri" ? SweepShape::Triangle : SweepShape::Off;
            if (sh == SweepShape::Off) { LOG_ERROR("main", "%s wants up, down or tri", a.c_str()); return 2; }
            (a == "--sweep-shape" ? gp.fSweep : gp.aSweep) = sh;
            haveGen = haveSweep = true;
        } else if (a == "--sweep-time") {
            if (!parseSI(next("--sweep-time"), gp.sweepPeriod)) { LOG_ERROR("main", "bad --sweep-time"); return 2; }
            haveGen = haveSweep = true;
        } else if (a == "--chain") {
            chainFile = next("--chain");
            haveGen = true;
        } else if (a == "--chain-add") {
            chainAdds.push_back(next("--chain-add"));
            haveGen = true;
        } else if (a == "--chain-loops") {
            chainLoops = atoi(next("--chain-loops"));
            if (chainLoops < 0) { LOG_ERROR("main", "--chain-loops wants 0 (forever) or more"); return 2; }
        } else if (a == "--play") {
            playFile = next("--play");
            gp.mode = GenMode::Playback;
            haveGen = true;
        } else if (a == "--play-segment") {
            if (!parsePair(next("--play-segment"), gp.playStart, gp.playLen)) {
                LOG_ERROR("main", "--play-segment wants START:LEN, e.g. 0.5:0.1");
                return 2;
            }
        } else if (a == "--play-once") gp.playLoop = false;
        else if (a == "--gen-out") {
            gc.rec.prefix = next("--gen-out");
            haveGenOut = true;
        } else if (a == "--gen-sample") {
            std::string v = next("--gen-sample");
            bool be;
            if (!parseFormatName(v, gc.fmt, be) || be ||
                !(gc.fmt == SampleFormat::CF32 || gc.fmt == SampleFormat::CI16 || gc.fmt == SampleFormat::CI8 || gc.fmt == SampleFormat::CU8)) {
                LOG_ERROR("main", "--gen-sample wants cf32, ci16, ci8 or cu8");
                return 2;
            }
        } else if (a == "--gen-pulses") gc.pulses = atoll(next("--gen-pulses"));
        else if (a == "--gen-annotate") gc.annotatePulses = true;
        else if (a == "--audio") {
            std::string v = next("--audio");
            for (auto& c : v) c = (char)tolower(c);
            if (!parseAudioMode(v, ap.mode)) { LOG_ERROR("main", "--audio wants am, nfm, wfm, usb, lsb or cw"); return 2; }
            haveAudio = true;
        } else if (a == "--audio-out") audioOut = next("--audio-out");
        else if (a == "--audio-device") audioDevice = next("--audio-device");
        else if (a == "--volume") {
            if (!parseSI(next("--volume"), audioVol)) { LOG_ERROR("main", "bad --volume"); return 2; }
        } else if (a == "--squelch") {
            if (!parseSI(next("--squelch"), ap.squelchDb)) { LOG_ERROR("main", "bad --squelch"); return 2; }
            ap.squelch = true;
        } else if (a == "--list-audio-devices") {
            std::string err;
            auto devs = listAudioDevices(err);
            if (!err.empty()) { LOG_ERROR("main", "%s", err.c_str()); return 1; }
            for (const auto& d : devs) printf("%s%s\n", d.name.c_str(), d.isDefault ? "  (default)" : "");
            return 0;
        }
        else if (a == "--tx") haveTx = true;
        else if (a == "--tx-args") txc.args = next("--tx-args");
        else if (!a.empty() && a[0] == '-') { LOG_ERROR("main", "unknown option %s (try --help)", a.c_str()); return 2; }
        else path = a;
    }
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
                reps = atoi(path.c_str() + x + 1);
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
            std::string note;
            ChainItem c = makeChainItem(path, gp, &note);
            if (haveOff) c.offset = off;
            c.repeats = std::max(1, reps);
            LOG_INFO("main", "chain item %zu: %s (%s)", gp.chain.size() + 1, c.path.c_str(), note.c_str());
            gp.chain.push_back(c);
        }
        if (chainLoops >= 0) gp.chainLoops = chainLoops;
        txc.fc = gp.fc;
    }
    if (!playFile.empty()) {
        // Plays as recorded unless -r / -c / --offset say otherwise.
        const FileMetaGuess g = guessMeta(expandHome(playFile));
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
            gc.rec.sigmf = rc.sigmf;
            gc.seconds = genSeconds > 0 ? genSeconds : 1;
            if (gc.pulses > 0 && gp.mode == GenMode::CW && !gp.sweeping()) {
                LOG_ERROR("main", "--gen-pulses needs --gen pulsed, --play, a chain or a sweep");
                return 2;
            }
            return runGenFile(gp, gc);
        }
        txc.rate = gp.rate;
        txc.fc = gp.fc;
        return runTx(gp, txc, genSeconds);
    }
    if (haveDevice) {
        if (ov.haveFc) dev.fc = ov.fc;
        else if (dev.driver == "siggen" && haveGen) dev.fc = gp.fc;  // the loopback listens where the generator plays
        if (ov.haveRate) dev.rate = ov.rate;
        else if (dev.driver == "uhd" || dev.driver == "bb") dev.rate = 10e6;
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

    {
        App app(win);
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
                          !shotPath.empty();
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
            if (!shotPath.empty()) {
                idleFrames = app.busy() ? 0 : idleFrames + 1;
                // A live stream is never idle: wait until the waterfall has some history.
                bool ready = app.isLive() ? app.liveRowsReceived() >= 90 : (frame > 20 && idleFrames > 10);
                if ((haveGen || openGen) && !app.sigGenPreviewReady()) ready = false;
                if (ready || frame > 3000) {
                    bool ok = win.screenshot(shotPath);
                    if (ok) logStatus("main", "screenshot %s written", shotPath.c_str());
                    else LOG_ERROR("main", "screenshot %s FAILED", shotPath.c_str());
                    break;
                }
            }
        }
    }
    LOG_INFO("main", "window closed, exiting");
    ImPlot::DestroyContext();
    // Window owns the GL backend; destroy it before the ImGui context.
    return 0;
}
