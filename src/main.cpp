// wvt - IQ waveform visualization tool (X11)
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "app.h"
#include "imgui.h"
#include "implot.h"
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
        fprintf(stderr, "wvt: %s\n", err.c_str());
        return 1;
    }
    auto st = live.stats();
    fprintf(stderr, "wvt: streaming from %s, %s at %s\n", st.description.c_str(), fmtSI(st.rate, "S/s", 6).c_str(),
            fmtSI(st.fc, "Hz", 9).c_str());
    if (!st.notes.empty()) fprintf(stderr, "wvt: note: %s\n", st.notes.c_str());
    if (!live.startRecording(rc, err)) {
        fprintf(stderr, "wvt: %s\n", err.c_str());
        return 1;
    }
    signal(SIGINT, [](int) { g_interrupted = 1; });
    signal(SIGTERM, [](int) { g_interrupted = 1; });
    fprintf(stderr, "wvt: recording to %s (Ctrl+C to stop)\n", live.recorder().status().dataPath.c_str());
    auto next = std::chrono::steady_clock::now();
    while (!g_interrupted && live.recorder().active()) {
        next += std::chrono::milliseconds(200);
        std::this_thread::sleep_until(next);
        if (!live.stats().error.empty()) break;
        static int tick = 0;
        if (++tick % 5 == 0) {
            auto rs = live.recorder().status();
            fprintf(stderr, "\rwvt: %s  %s  buffer %.0f%%  gaps %d   ", fmtSI(rs.seconds, "s", 4).c_str(),
                    fmtSI((double)rs.bytes, "B", 4).c_str(), rs.ringFill * 100, rs.gaps);
        }
    }
    std::string devErr = live.stats().error;
    live.stopRecording();
    auto rs = live.recorder().status();
    live.stop();
    fprintf(stderr, "\nwvt: wrote %s (%s, %s%s)\n", rs.dataPath.c_str(), fmtSI(rs.seconds, "s", 5).c_str(),
            fmtSI((double)rs.bytes, "B", 4).c_str(), rs.gaps ? (", " + std::to_string(rs.gaps) + " gaps").c_str() : "");
    if (!rs.metaPath.empty()) fprintf(stderr, "wvt: metadata %s\n", rs.metaPath.c_str());
    if (!devErr.empty()) fprintf(stderr, "wvt: device error: %s\n", devErr.c_str());
    if (rs.failed) fprintf(stderr, "wvt: recording failed: %s\n", rs.stopReason.c_str());
    return rs.failed || !devErr.empty() ? 1 : 0;
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
        "      --view T0:T1     initial time range in seconds, e.g. --view 0.5:0.52\n"
        "      --software       force the software renderer (no GLX)\n"
        "      --scale S        UI scale factor (default 1, or $WVT_SCALE)\n"
        "      --geometry WxH   initial window size (default 1600x950)\n"
        "      --screenshot F   render, write a PPM screenshot to F and exit\n"
        "  -h, --help           show this help\n"
        "\n"
        "live devices (-c and -r set the tuning):\n"
        "      --device DEV     rtlsdr[:INDEX|SERIAL] | uhd[:ARGS] | sim\n"
        "                       e.g. --device uhd:type=b200  --device rtlsdr:0\n"
        "      --gain DB|auto   receiver gain (default 30 dB)\n"
        "      --antenna NAME   USRP antenna (RX2 or TX/RX)\n"
        "      --bandwidth HZ   USRP analog bandwidth (default: automatic)\n"
        "      --bias-tee       RTL-SDR: power an LNA / active antenna over the coax\n"
        "      --direct-sampling I|Q  RTL-SDR: HF direct sampling (v3: Q)\n"
        "      --ppm N          RTL-SDR frequency correction\n"
        "      --list-devices   print attached devices and exit\n"
        "\n"
        "recording (needs --device):\n"
        "      --record PREFIX  record to PREFIX_<date>_<time>_<fc>_<rate>.<ext>\n"
        "      --record-format sigmf|raw   (default sigmf)\n"
        "      --record-sample native|cf32 (default native: cu8 for RTL-SDR, ci16 for USRP)\n"
        "      --record-duration T         stop after T seconds, e.g. 30\n"
        "      --no-gui         record without opening a window (Ctrl+C stops)\n"
        "\n"
        "Format, rate and center frequency are auto-detected from SigMF metadata,\n"
        "gqrx file names and extensions (.cu8 .cs8 .cs16 .cf32 ...) when possible.\n"
        "\n"
        "mouse:  drag = pan, wheel = zoom (over an axis: zoom that axis only),\n"
        "        right-drag = box zoom, double-click = fit, Ctrl+click spectrum/waterfall = tune channel\n"
        "keys:   Left/Right pan, PgUp/PgDn page, +/- zoom, Home/End, A all, T/F markers,\n"
        "        Z zoom to time markers, C channel on/off, B bookmark\n");
}

int main(int argc, char** argv) {
    CliOverrides ov;
    std::string path, shotPath;
    bool software = false;
    bool haveChan = false, haveView = false;
    double chanOff = 0, chanBw = 0, view0 = 0, view1 = 0;
    auto parsePair = [](const char* s, double& a, double& b) {
        std::string str(s);
        size_t c = str.find(':');
        return c != std::string::npos && parseSI(str.substr(0, c).c_str(), a) && parseSI(str.substr(c + 1).c_str(), b);
    };
    StreamConfig dev;
    bool haveDevice = false, haveRecord = false, noGui = false;
    RecordConfig rc;
    float scale = 1.f;
    int winW = 1600, winH = 950;
    if (const char* e = getenv("WVT_SCALE")) scale = (float)atof(e);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "wvt: %s needs a value\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-f" || a == "--format") {
            const char* v = next("--format");
            if (!parseFormatName(v, ov.fmt, ov.bigEndian)) { fprintf(stderr, "wvt: unknown format '%s'\n", v); return 2; }
            ov.haveFmt = true;
        } else if (a == "-r" || a == "--rate") {
            if (!parseSI(next("--rate"), ov.rate) || ov.rate <= 0) { fprintf(stderr, "wvt: bad rate\n"); return 2; }
            ov.haveRate = true;
        } else if (a == "-c" || a == "--fc") {
            if (!parseSI(next("--fc"), ov.fc)) { fprintf(stderr, "wvt: bad center frequency\n"); return 2; }
            ov.haveFc = true;
        } else if (a == "-H" || a == "--header") ov.header = atoll(next("--header"));
        else if (a == "--swap-iq") ov.swapIQ = true;
        else if (a == "--chan") {
            if (!parsePair(next("--chan"), chanOff, chanBw)) { fprintf(stderr, "wvt: --chan wants OFFSET:BW\n"); return 2; }
            haveChan = true;
        } else if (a == "--view") {
            if (!parsePair(next("--view"), view0, view1)) { fprintf(stderr, "wvt: --view wants T0:T1\n"); return 2; }
            haveView = true;
        }
        else if (a == "--software") software = true;
        else if (a == "--scale") scale = (float)atof(next("--scale"));
        else if (a == "--geometry") {
            if (sscanf(next("--geometry"), "%dx%d", &winW, &winH) != 2) { fprintf(stderr, "wvt: bad geometry\n"); return 2; }
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
            else if (!parseSI(v.c_str(), dev.gain)) { fprintf(stderr, "wvt: bad gain\n"); return 2; }
        } else if (a == "--antenna") dev.antenna = next("--antenna");
        else if (a == "--bandwidth") {
            if (!parseSI(next("--bandwidth"), dev.bandwidth)) { fprintf(stderr, "wvt: bad bandwidth\n"); return 2; }
        } else if (a == "--bias-tee") dev.biasTee = true;
        else if (a == "--direct-sampling") {
            std::string v = next("--direct-sampling");
            dev.directSampling = (v == "I" || v == "i" || v == "1") ? 1 : (v == "Q" || v == "q" || v == "2") ? 2 : 0;
        } else if (a == "--ppm") dev.ppm = atoi(next("--ppm"));
        else if (a == "--list-devices") {
            for (const auto& d : enumerateDevices()) printf("%-8s %-28s %s\n", d.driver.c_str(), d.args.c_str(), d.label.c_str());
            return 0;
        } else if (a == "--record") {
            rc.prefix = next("--record");
            haveRecord = true;
        } else if (a == "--record-format") {
            std::string v = next("--record-format");
            if (v != "sigmf" && v != "raw") { fprintf(stderr, "wvt: --record-format wants sigmf or raw\n"); return 2; }
            rc.sigmf = v == "sigmf";
        } else if (a == "--record-sample") {
            std::string v = next("--record-sample");
            if (v != "native" && v != "cf32") { fprintf(stderr, "wvt: --record-sample wants native or cf32\n"); return 2; }
            rc.cf32 = v == "cf32";
        } else if (a == "--record-duration") {
            if (!parseSI(next("--record-duration"), rc.maxSeconds) || rc.maxSeconds <= 0) {
                fprintf(stderr, "wvt: bad --record-duration\n");
                return 2;
            }
        } else if (a == "--no-gui") noGui = true;
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "wvt: unknown option %s (try --help)\n", a.c_str()); return 2; }
        else path = a;
    }
    scale = std::clamp(scale, 0.5f, 4.f);
    if (haveDevice) {
        if (ov.haveFc) dev.fc = ov.fc;
        if (ov.haveRate) dev.rate = ov.rate;
        else if (dev.driver == "uhd") dev.rate = 10e6;
    }
    if ((haveRecord || noGui) && !haveDevice) {
        fprintf(stderr, "wvt: recording needs --device\n");
        return 2;
    }
    if (noGui) {
        if (!haveRecord) {
            fprintf(stderr, "wvt: --no-gui needs --record\n");
            return 2;
        }
        return runHeadless(dev, rc);
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
        fprintf(stderr, "wvt: %s\n", err.c_str());
        return 1;
    }

    {
        App app(win);
        if (haveDevice) {
            if (!app.startDevice(dev)) fprintf(stderr, "wvt: device failed to start (see the status bar)\n");
            else if (haveRecord && !app.startRecording(rc)) fprintf(stderr, "wvt: recording failed to start\n");
        } else if (!path.empty()) {
            if (!app.openFile(path, ov)) fprintf(stderr, "wvt: failed to open %s\n", path.c_str());
            if (haveView) app.setView(view0, view1);
            if (haveChan) app.setChannel(chanOff, chanBw);
        }

        auto lastActivity = std::chrono::steady_clock::now();
        int frame = 0, idleFrames = 0;
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            bool active = app.busy() || now - lastActivity < std::chrono::milliseconds(600) || !shotPath.empty();
            if (!win.pumpEvents(active ? 1.0 / 60.0 : 0.5)) break;
            // Any input (or worker wake-up) keeps the loop running at frame rate for a moment.
            if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || ImGui::IsAnyItemActive() ||
                io.InputQueueCharacters.Size > 0 || ImGui::IsAnyMouseDown())
                lastActivity = now;
            if (app.busy()) lastActivity = now;
            win.newFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            win.render(ImGui::GetDrawData(), ImVec4(0.06f, 0.06f, 0.07f, 1.f));
            ++frame;
            if (!shotPath.empty()) {
                idleFrames = app.busy() ? 0 : idleFrames + 1;
                // A live stream is never idle: wait until the waterfall has some history.
                bool ready = app.isLive() ? app.liveRowsReceived() >= 90 : (frame > 20 && idleFrames > 10);
                if (ready || frame > 3000) {
                    bool ok = win.screenshot(shotPath);
                    fprintf(stderr, "wvt: screenshot %s %s\n", shotPath.c_str(), ok ? "written" : "FAILED");
                    break;
                }
            }
        }
    }
    ImPlot::DestroyContext();
    // Window owns the GL backend; destroy it before the ImGui context.
    return 0;
}
