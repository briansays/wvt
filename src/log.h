#pragma once
// Leveled logging to the console and a per-run log file. The console shows
// WARN and up unless -v lowers it; the file, when open, gets everything.
// Safe to call from any thread, but not from real-time callbacks (audio,
// SDR sample callbacks): a line write takes a mutex and may do file I/O.
// Before logInit() the logger prints WARN and up to stderr and has no file,
// so the self-test tools work without setting it up.
#include <cstdio>
#include <string>
#include <vector>

enum class LogLevel { Debug, Info, Warn, Error, Off };

struct LogConfig {
    LogLevel console = LogLevel::Warn;
    bool verbose = false;  // -v given: timestamped console lines instead of "wvt: ..."
    bool file = true;
    std::string dir;  // empty: $WVT_LOG_DIR, then $TMPDIR, then /tmp
    std::string commandLine;  // written to the file header
    FILE* consoleStream = nullptr;  // nullptr = stderr (tests redirect it)
};

// "debug", "info", "warn", "warning", "error", any case.
bool parseLogLevel(const char* s, LogLevel& out);
const char* logLevelName(LogLevel l);

// Takes -v / --verbose[=LEVEL] / -vLEVEL / -v LEVEL, --log-dir DIR and
// --no-log-file out of args (args[0] is the program name) and fills cfg.
// "-v LEVEL" only takes the next word if it is a level name.
bool logParseArgs(std::vector<char*>& args, LogConfig& cfg, std::string& err);

// Applies cfg and opens wvt_log_<YYYYMMDD>_<HHMMSS>.log (local time). It can
// be called again; the previous file is closed. A file that cannot be opened
// is reported once as a warning and logging goes on without it.
void logInit(const LogConfig& cfg);
void logShutdown();
std::string logFilePath();  // empty when there is no file

bool logEnabled(LogLevel l);
void logWrite(LogLevel l, const char* component, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void logFlush();

// The macros skip formatting when no sink takes the level.
#define WVT_LOG(lvl, comp, ...)                                     \
    do {                                                            \
        if (logEnabled(lvl)) logWrite(lvl, comp, __VA_ARGS__);      \
    } while (0)
#define LOG_DEBUG(comp, ...) WVT_LOG(LogLevel::Debug, comp, __VA_ARGS__)
#define LOG_INFO(comp, ...) WVT_LOG(LogLevel::Info, comp, __VA_ARGS__)
#define LOG_WARN(comp, ...) WVT_LOG(LogLevel::Warn, comp, __VA_ARGS__)
#define LOG_ERROR(comp, ...) WVT_LOG(LogLevel::Error, comp, __VA_ARGS__)
