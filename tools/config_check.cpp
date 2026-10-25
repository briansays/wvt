// Configuration file self-test (issue #88): comments, quotes, "#" in values,
// numbers in base units and the refused SI suffixes, "~", unknown keys and
// bad values with their line numbers, duplicates, SDR profiles, sdr.default,
// and the file search order.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/config.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Parsed {
    WvtConfig cfg;
    std::vector<std::string> warnings;
};

Parsed parse(const std::string& text) {
    Parsed p;
    parseConfig(text, "t.conf", p.cfg, p.warnings);
    return p;
}

// True when one warning contains all of the parts.
bool warned(const Parsed& p, const std::vector<std::string>& parts) {
    for (const auto& w : p.warnings) {
        bool all = true;
        for (const auto& s : parts) all = all && w.find(s) != std::string::npos;
        if (all) return true;
    }
    return false;
}

std::string list(const Parsed& p) {
    std::string s;
    for (const auto& w : p.warnings) s += "\n         " + w;
    return s;
}

// The example of docs/design/config-file-plan.md §5.
const char* kExample = R"(# wvt configuration file (issue #88)
recording.folder  = ~/wvt/recordings/
recording.name    = wvt                  # file names: <name>_<date>_<time>_<fc>_<rate>
snapshot.folder   = ~/wvt/snapshots/
siggen.folder     = ~/wvt/siggen/
siggen.name       = siggen
audio.folder      = ~/wvt/audio/
recording.format  = sigmf                # sigmf | raw
recording.sample  = native
recording.max_seconds = 0
siggen.format     = sigmf
siggen.sample     = cf32
export.band_sample = cf32
fft.size          = 1024
fft.window        = blackman-harris
fft.overlap_pct   = 50
display.colormap  = viridis
# display.db_min  = auto
live.fft_size     = 2048
live.window       = blackman-harris
live.avg          = auto
live.rows_per_sec = 30
live.max_columns  = 2048
live.history_rows = 600
gpu               = auto
ui.scale          = 1
ui.geometry       = 1600x950
# tx.allow        = 902e6:928e6, 2.4e9:2.5e9
sdr.default = b205
sdr.0.type          = b205
sdr.0.fc            = 100e6
sdr.0.rate          = 61.44e6
sdr.0.gain_db       = 0
sdr.0.agc           = off
sdr.0.port          = TX/RX              # USRP antenna: TX/RX | RX2
sdr.0.bandwidth     = auto
sdr.0.ref           = internal
sdr.1.type          = x310
sdr.1.serial        = 31F4A2C
sdr.1.args          = "addr=192.168.40.2"
sdr.1.channel       = 1
sdr.1.fc            = 2.45e9
sdr.1.rate          = 50e6
sdr.1.gain_db       = 20
sdr.1.port          = RX2
sdr.1.ref           = external
sdr.2.type          = bb60
sdr.2.fc            = 1.09e9
sdr.2.rate          = 40e6
sdr.2.ref_level_dbm = -20
sdr.2.atten         = auto
sdr.2.amp           = auto
sdr.2.bandwidth     = 27e6
sdr.2.ext_ref       = off
sdr.3.type          = x4
sdr.3.args          = "#0"               # "#" in quotes is not a comment
sdr.3.fc            = 3.5e9
sdr.3.rate          = 61.44e6
sdr.3.gain_db       = 10
sdr.3.port          = J1
sdr.4.type            = rtlsdr
sdr.4.fc              = 7.1e6
sdr.4.rate            = 2.4e6
sdr.4.agc             = on
sdr.4.direct_sampling = q
sdr.4.bias_tee        = off
sdr.4.ppm             = 0
)";
}  // namespace

int main() {
    printf("defaults\n");
    {
        const Parsed p = parse("");
        const WvtConfig d;
        check(p.warnings.empty() && p.cfg.sdrs.empty() && p.cfg.origin.empty(), "an empty file sets nothing");
        check(d.recordingFolder == "~/wvt-recordings/" && d.recordingName == "wvt" && d.fftSize == 1024 &&
                  d.fftWindow == WindowType::BlackmanHarris && d.fftOverlapPct == 50 && d.liveFftSize == 2048 &&
                  d.liveAvg == 0 && d.liveRowsPerSec == 30 && d.liveMaxColumns == 2048 && d.liveHistoryRows == 600 &&
                  d.recordingSigmf && !d.recordingCf32 && d.siggenSample == SampleFormat::CF32 && d.gpu == "auto" &&
                  d.uiWidth == 1600 && d.uiHeight == 950 && std::isnan(d.txMaxGainDb),
              "the defaults are wvt's built-in values");
    }

    printf("the example file\n");
    {
        const Parsed p = parse(kExample);
        check(p.warnings.empty(), "no warnings" + list(p));
        const WvtConfig& c = p.cfg;
        check(c.recordingFolder == "~/wvt/recordings/" && c.snapshotFolder == "~/wvt/snapshots/" &&
                  c.siggenFolder == "~/wvt/siggen/" && c.audioFolder == "~/wvt/audio/",
              "the folders, as written");
        check(c.sdrDefault == "b205" && c.sdrs.size() == 5, "sdr.default and 5 profiles");
        if (c.sdrs.size() == 5) {
            const SdrProfile &b = c.sdrs[0], &x = c.sdrs[1], &bb = c.sdrs[2], &k = c.sdrs[3], &r = c.sdrs[4];
            check(b.type == "b205" && b.fc == 100e6 && b.rate == 61.44e6 && b.gainDb == 0.0 && b.agc == false &&
                      b.port == std::string("TX/RX") && b.bandwidth == 0.0 && b.ref == std::string("internal"),
                  "B205: fc, rate 61.44e6, gain, agc, port TX/RX, bandwidth auto, ref");
            check(x.serial == "31F4A2C" && x.args == "addr=192.168.40.2" && x.channel == 1 && x.fc == 2.45e9,
                  "X310: serial, quoted args, channel, fc");
            check(bb.refLevelDbm == -20.0 && bb.atten == -1 && bb.amp == -1 && bb.bandwidth == 27e6 && bb.extRef == false,
                  "BB60: ref level, atten and amp auto, bandwidth, ext_ref");
            check(k.type == "x4" && k.args == "#0", "Sidekiq X4: \"#0\" in quotes is the value");
            check(r.directSampling == 2 && r.agc == true && r.biasTee == false && r.ppm == 0, "RTL-SDR: direct sampling q");
            check(!b.channel && !b.ppm, "fields that are not set stay unset");
        }
        check(c.origin.count("sdr.0.rate") && c.origin.at("sdr.0.rate") == "t.conf:32", "origin: file and line");
    }

    printf("lines and comments\n");
    {
        const Parsed p = parse("  # comment\n\n   \t\nfft.size = 2048 # trailing\nsdr.0.type = x4\nsdr.0.args = skiq#0\r\n");
        check(p.warnings.empty() && p.cfg.fftSize == 2048, "comment lines, blank lines, a trailing comment" + list(p));
        check(!p.cfg.sdrs.empty() && p.cfg.sdrs[0].args == "skiq#0", "'#' without a space before it is text; CRLF ends");
        const Parsed q = parse("FFT.Size = 512\nsdr.0.type = X4\n");
        check(q.cfg.fftSize == 512 && !q.cfg.sdrs.empty() && q.cfg.sdrs[0].type == "x4", "keys and types in any case");
        const Parsed e = parse("fft.size = \nrecording.name = \"a \\\"b\\\"\"\nsiggen.name = \"open\nfoo\n= 3\n");
        check(warned(e, {"t.conf:1:", "fft.size"}), "line 1: an empty value is a bad value" + list(e));
        check(e.cfg.recordingName == "a \"b\"", "\\\" in quotes");
        check(warned(e, {"t.conf:3:", "quote is not closed"}), "line 3: an open quote");
        check(warned(e, {"t.conf:4:", "expected key = value"}), "line 4: no '='");
        check(warned(e, {"t.conf:5:", "without a key"}), "line 5: no key");
        const Parsed t = parse("recording.name = \"x\" y\n");
        check(warned(t, {"after the closing quote"}) && t.cfg.recordingName == "wvt", "text after a closing quote");
    }

    printf("numbers\n");
    {
        double v = 0;
        std::string hint;
        check(parseConfigNumber("61.44e6", v, hint) && v == 61.44e6, "61.44e6");
        check(parseConfigNumber("-20", v, hint) && v == -20 && parseConfigNumber(" 2.4E6 ", v, hint) && v == 2.4e6,
              "-20, 2.4E6 with spaces");
        check(!parseConfigNumber("61.44M", v, hint) && hint.find("use 61.44e6") != std::string::npos, "61.44M: use 61.44e6");
        check(!parseConfigNumber("915 MHz", v, hint) && hint.find("use 915e6") != std::string::npos, "915 MHz: use 915e6");
        check(!parseConfigNumber("10m", v, hint) && hint.find("use 10e-3") != std::string::npos, "10m: use 10e-3");
        check(!parseConfigNumber("abc", v, hint) && hint.empty(), "abc: no hint");
        check(!parseConfigNumber("0x10", v, hint) && !parseConfigNumber("inf", v, hint) && !parseConfigNumber("nan", v, hint) &&
                  !parseConfigNumber("", v, hint),
              "hex, inf, nan, empty: refused");
        const Parsed p = parse("sdr.0.type = b205\nsdr.0.rate = 61.44M\n");
        check(warned(p, {"t.conf:2:", "sdr.0.rate", "use 61.44e6"}) && !p.cfg.sdrs[0].rate,
              "a suffix in the file: warning with the hint, not used" + list(p));
        const Parsed r = parse("fft.overlap_pct = 99\nlive.rows_per_sec = 0.5\n");
        check(warned(r, {"t.conf:1:", "0 to 95"}) && warned(r, {"t.conf:2:", "1 to 100"}) && r.cfg.fftOverlapPct == 50,
              "out of range: warning, default kept");
    }

    printf("values\n");
    {
        const Parsed p = parse(
            "fft.size = 1000\nlive.fft_size = 4096\nlive.avg = 0\nlive.avg = 16\nfft.window = hann\n"
            "recording.sample = cf32\nrecording.format = raw\nsiggen.sample = ci8\ngpu = maybe\nui.geometry = 800x600\n"
            "ui.geometry = 800\ntx.allow = 902e6:928e6, 2.4e9:2.5e9\ntx.max_gain_db = 30\nrecording.name = a/b\n"
            "display.db_min = auto\ndisplay.db_max = -10\n");
        check(warned(p, {"t.conf:1:", "power of two"}) && p.cfg.fftSize == 1024, "fft.size 1000: not a power of two");
        check(p.cfg.liveFftSize == 4096, "live.fft_size 4096");
        check(warned(p, {"t.conf:3:", "live.avg"}) && p.cfg.liveAvg == 16, "live.avg 0 refused (use auto), 16 taken");
        check(p.cfg.fftWindow == WindowType::Hann && p.cfg.recordingCf32 && !p.cfg.recordingSigmf &&
                  p.cfg.siggenSample == SampleFormat::CI8,
              "choices: window, sample, format");
        check(warned(p, {"t.conf:9:", "auto, on, off"}) && p.cfg.gpu == "auto", "gpu = maybe: the choices are listed");
        check(p.cfg.uiWidth == 800 && p.cfg.uiHeight == 600 && warned(p, {"t.conf:11:", "WxH"}), "ui.geometry");
        check(p.cfg.txAllow.size() == 2 && p.cfg.txAllow[1].first == 2.4e9 && p.cfg.txAllow[1].second == 2.5e9 &&
                  p.cfg.txMaxGainDb == 30,
              "tx.allow bands, tx.max_gain_db");
        check(warned(p, {"t.conf:14:", "without '/'"}), "a name with '/'");
        check(std::isnan(p.cfg.dbMin) && p.cfg.dbMax == -10, "db_min auto, db_max -10");
        const Parsed b = parse("tx.allow = 928e6:902e6\nsdr.0.type=b205\nsdr.0.agc = maybe\n");
        check(warned(b, {"t.conf:1:", "LO:HI"}) && b.cfg.txAllow.empty(), "a band with LO > HI");
        check(warned(b, {"t.conf:3:", "on or off"}), "a bool that is not one" + list(b));
    }

    printf("folders\n");
    {
        const Parsed p = parse("recording.folder = ~/rec/\nsnapshot.folder = /data/snaps\nsiggen.folder = rel/dir\n");
        check(p.cfg.recordingFolder == "~/rec/" && p.cfg.snapshotFolder == "/data/snaps", "~/ and / folders");
        check(warned(p, {"t.conf:3:", "not an absolute folder"}) && p.cfg.siggenFolder == "~/wvt-recordings/",
              "a relative folder: warning, default kept");
    }

    printf("keys\n");
    {
        const Parsed p = parse("fft.sise = 2048\nsdr.0.rat = 1e6\nsdr.0.type = b205\nfoo.bar = 1\nsdr.0.type = x310\nsdr.100.type = b205\n");
        check(warned(p, {"t.conf:1:", "unknown key 'fft.sise'", "did you mean 'fft.size'"}), "fft.sise: did you mean fft.size" + list(p));
        check(warned(p, {"t.conf:2:", "did you mean 'sdr.0.rate'"}), "sdr.0.rat: did you mean sdr.0.rate");
        check(warned(p, {"t.conf:4:", "unknown key 'foo.bar'"}) && !warned(p, {"foo.bar", "did you mean"}),
              "foo.bar: unknown, no guess");
        check(warned(p, {"t.conf:5:", "replaces line 3"}) && p.cfg.sdrs[0].type == "x310", "a key set again: the last wins");
        check(warned(p, {"t.conf:6:", "unknown key 'sdr.100.type'"}), "profile indexes 0 to 99");
        const auto names = configKeyNames();
        check(names.size() > 40 && std::find(names.begin(), names.end(), "sdr.N.rate") != names.end(), "configKeyNames");
    }

    printf("SDR profiles\n");
    {
        const Parsed p = parse(
            "sdr.5.type = rtlsdr\nsdr.2.type = b205\nsdr.2.ppm = 3\nsdr.2.direct_sampling = q\nsdr.7.fc = 1e9\n"
            "sdr.3.type = hackrf\nsdr.default = 9\n");
        check(p.cfg.sdrs.size() == 3 && p.cfg.sdrs[0].index == 2 && p.cfg.sdrs[1].index == 5 && p.cfg.sdrs[2].index == 7,
              "profiles sorted by index; a refused type makes no profile" + list(p));
        bool inOrder = true;
        for (size_t i = 1; i < p.warnings.size(); ++i)
            inOrder = inOrder && std::atoi(p.warnings[i - 1].c_str() + 7) <= std::atoi(p.warnings[i].c_str() + 7);
        check(inOrder, "the warnings are in line order");
        check(warned(p, {"t.conf:3:", "sdr.2.ppm does not apply to a b205"}) &&
                  warned(p, {"t.conf:4:", "sdr.2.direct_sampling does not apply"}),
              "RTL-SDR fields on a USRP");
        check(warned(p, {"t.conf:5:", "sdr.7 has no sdr.7.type"}), "a profile without a type");
        check(warned(p, {"t.conf:6:", "unknown type 'hackrf'", "b205"}), "an unknown type lists the types");
        check(warned(p, {"t.conf:7:", "no profile sdr.9"}), "sdr.default: a missing index");
        const Parsed q = parse("sdr.default = x9\n");
        check(warned(q, {"t.conf:1:", "not a profile index or an SDR type"}), "sdr.default: neither");
        const Parsed r = parse("sdr.default = bb60\n");
        check(r.warnings.empty(), "sdr.default: a type without a profile is fine");
        check(configSdrDriver("B205") && std::string(configSdrDriver("B205")) == "uhd" &&
                  std::string(configSdrDriver("x4")) == "skiq" && std::string(configSdrDriver("bb60")) == "bb" &&
                  !configSdrDriver("hackrf"),
              "type -> driver");
    }

    printf("the file\n");
    {
        WvtConfig c;
        std::vector<std::string> w;
        std::string err;
        check(!loadConfigFile("/nonexistent/wvt.conf", c, w, err) && !err.empty(), "a missing file: false, err");
        const char* tmp = getenv("TMPDIR");
        const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/wvt_config_check_" + std::to_string(getpid()) + ".conf";
        if (FILE* f = fopen(path.c_str(), "w")) {
            fputs("fft.size = 4096\nbad line\n", f);
            fclose(f);
        }
        const bool ok = loadConfigFile(path, c, w, err);
        const std::string base = path.substr(path.rfind('/') + 1);
        check(ok && c.fftSize == 4096 && w.size() == 1 && w[0].rfind(base + ":2:", 0) == 0,
              "a file: read, messages named after the file");
        unlink(path.c_str());

        bool req = false;
        unsetenv("WVT_CONFIG");
        setenv("XDG_CONFIG_HOME", "/xdg", 1);
        check(configPath("/opt/a.conf", req) == "/opt/a.conf" && req, "--config first, required");
        setenv("WVT_CONFIG", "/etc/b.conf", 1);
        check(configPath("", req) == "/etc/b.conf" && req, "then $WVT_CONFIG, required");
        unsetenv("WVT_CONFIG");
        check(configPath("", req) == "/xdg/wvt/wvt.conf" && !req, "then $XDG_CONFIG_HOME/wvt/wvt.conf, optional");
        unsetenv("XDG_CONFIG_HOME");
        setenv("HOME", "/home/u", 1);
        check(configPath("", req) == "/home/u/.config/wvt/wvt.conf" && !req, "then ~/.config/wvt/wvt.conf");
        check(configPath("~/c.conf", req) == "/home/u/c.conf", "~ in --config");
    }

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
