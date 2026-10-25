#pragma once
// The run-time configuration file (issue #88, docs/design/config-file-plan.md):
// one "key = value" per line, "#" comments. A table of keys drives the parse;
// each key has a type, a help line and the WvtConfig field it sets. The file
// changes the built-in defaults below; the environment, the options and the
// GUI change them after it.
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dsp/dsp.h"
#include "io/iq_file.h"

// The settings of one SDR (sdr.N.* in the file). Fields that are not set keep
// the device's default.
struct SdrProfile {
    int index = -1;    // N in sdr.N.*
    int line = 0;      // the first line of the profile, for messages
    std::string type;  // "b205", "x310", "bb60", "x4", "rtlsdr", ... (configSdrTypes)
    std::string serial, args;
    std::optional<double> fc, rate, gainDb, refLevelDbm;
    std::optional<double> bandwidth;  // 0 = automatic
    std::optional<bool> agc, extRef, biasTee;
    std::optional<std::string> port, ref;
    std::optional<int> channel, ppm;
    std::optional<int> atten;           // BB60: -1 automatic, 0-3 = 0, 10, 20, 30 dB
    std::optional<int> amp;             // BB60: -1 automatic, 0-3
    std::optional<int> directSampling;  // RTL-SDR: 0 off, 1 I branch, 2 Q branch
};

struct WvtConfig {
    // Folders: "~" is the home folder. File names start with the *Name values.
    std::string recordingFolder = "~/wvt-recordings/", recordingName = "wvt";
    std::string snapshotFolder = "~/wvt-recordings/";
    std::string siggenFolder = "~/wvt-recordings/", siggenName = "siggen";
    std::string audioFolder = "~/wvt-recordings/";

    // File formats
    bool recordingSigmf = true;
    bool recordingCf32 = false;  // false: the device's native samples
    double recordingMaxSeconds = 0;  // 0 = no limit
    bool siggenSigmf = true;
    SampleFormat siggenSample = SampleFormat::CF32;
    bool exportBandCi16 = false;  // region export with a band filter: ci16, else cf32

    // File waterfall
    int fftSize = 1024;
    WindowType fftWindow = WindowType::BlackmanHarris;
    double fftOverlapPct = 50;
    std::string colormap = "viridis";
    double dbMin = NAN, dbMax = NAN;  // NaN = automatic

    // Live spectrum (LiveParams)
    int liveFftSize = 2048;
    WindowType liveWindow = WindowType::BlackmanHarris;
    int liveAvg = 0;  // 0 = automatic
    double liveRowsPerSec = 30;
    int liveMaxColumns = 2048;
    int liveHistoryRows = 600;

    // General
    std::string gpu = "auto";  // auto | on | off
    int gpuDevice = -1;        // -1 = the best GPU
    double uiScale = 1;
    int uiWidth = 1600, uiHeight = 950;
    std::string logFolder;  // empty: $WVT_LOG_DIR, then $TMPDIR, then /tmp

    // Transmit limits; they only make the other limits stricter
    double txMaxGainDb = NAN;                         // NaN = no limit from the file
    std::vector<std::pair<double, double>> txAllow;  // bands in Hz; empty = no limit from the file

    // SDRs
    std::string sdrDefault;  // a type (configSdrTypes) or a profile index; empty = the first device found
    std::vector<SdrProfile> sdrs;  // sorted by index

    // Where each key that the file set came from: key -> "wvt.conf:12".
    std::map<std::string, std::string> origin;
};

// The SDR types of sdr.N.type, and the driver of each ("uhd", "bb", "skiq",
// "rtlsdr", "sim").
const std::vector<std::pair<const char*, const char*>>& configSdrTypes();
// The driver of a type, or nullptr for an unknown type. Any case.
const char* configSdrDriver(const std::string& type);

// Parses the text of a configuration file into cfg. Keys that the text does
// not set keep their values. `name` goes in the messages ("wvt.conf:12: ...").
// Problems go to warnings, one line each; the rest of the text still applies.
void parseConfig(const std::string& text, const std::string& name, WvtConfig& cfg, std::vector<std::string>& warnings);

// Reads and parses a file. False (err set) when it cannot be read.
bool loadConfigFile(const std::string& path, WvtConfig& cfg, std::vector<std::string>& warnings, std::string& err);

// The file to read: `option` (--config) if given, else $WVT_CONFIG, else
// $XDG_CONFIG_HOME/wvt/wvt.conf, else ~/.config/wvt/wvt.conf. `required` is
// true when the user named the file (option or $WVT_CONFIG): then a missing
// file is an error, else it is skipped.
std::string configPath(const std::string& option, bool& required);

// A number in base units, plain or in scientific notation ("61.44e6", "-20").
// No SI suffixes: "61.44M" fails, and hint gets "use 61.44e6".
bool parseConfigNumber(const std::string& s, double& out, std::string& hint);

// All key names, for help and "did you mean" (sdr.N.* as "sdr.N.<field>").
std::vector<std::string> configKeyNames();
