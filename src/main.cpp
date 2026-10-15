// wvt - IQ waveform visualization tool (X11)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"
#include "imgui.h"
#include "implot.h"
#include "platform_x11.h"
#include "ui_util.h"

extern const unsigned char g_embedded_font[];
extern const size_t g_embedded_font_size;

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
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "wvt: unknown option %s (try --help)\n", a.c_str()); return 2; }
        else path = a;
    }
    scale = std::clamp(scale, 0.5f, 4.f);

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
        if (!path.empty()) {
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
                if ((frame > 20 && idleFrames > 10) || frame > 3000) {
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
