#pragma once
// Leveled logging to the console and a per-run log file. The console shows
// WARN and up unless -v lowers it; the file, when open, gets everything.
// Safe to call from any thread, but not from real-time callbacks (audio,
// SDR sample callbacks): a line write takes a mutex and may do file I/O.
// Before logInit() the logger prints WARN and up to stderr and has no file,
// so the self-test tools work without setting it up.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

// The text of an errno value. Unlike strerror(), safe on any thread (the
// recorder, export and audio threads report file errors).
inline std::string errnoText(int err) { return std::generic_category().message(err); }

// getenv(). It is not thread safe only with a concurrent setenv(), and wvt
// never calls setenv(); the tests do, from their only thread.
inline const char* envVar(const char* name) { return getenv(name); }  // NOLINT(concurrency-mt-unsafe)

enum class LogLevel { Debug, Info, Warn, Error, Off };

struct LogConfig {
    LogLevel console = LogLevel::Warn;
    bool verbose = false;  // -v given: timestamped console lines instead of "wvt: ..."
    bool file = true;
    std::string dir;  // empty: $WVT_LOG_DIR, then $TMPDIR, then /tmp
    std::string commandLine;  // written to the file header
    std::string build;        // git commit and version tag, for the file header; empty = "unknown"
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

// A message the user always sees, as wvt printed before the logger existed
// ("wvt: wrote x.sigmf-data"): printed whatever the console level, INFO in
// the file.
void logStatus(const char* component, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// A one-line live display, rewritten in place with "\r" ("wvt: 12 s  3 MB ...").
// Always printed; written to the file at DEBUG at most every 10 s. Log lines
// that arrive while it is shown start on a new line. logProgressEnd() ends it.
void logProgress(const char* component, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logProgressEnd();

// The macros skip formatting when no sink takes the level.
#define WVT_LOG(lvl, comp, ...)                                     \
    do {                                                            \
        if (logEnabled(lvl)) logWrite(lvl, comp, __VA_ARGS__);      \
    } while (0)
#define LOG_DEBUG(comp, ...) WVT_LOG(LogLevel::Debug, comp, __VA_ARGS__)
#define LOG_INFO(comp, ...) WVT_LOG(LogLevel::Info, comp, __VA_ARGS__)
#define LOG_WARN(comp, ...) WVT_LOG(LogLevel::Warn, comp, __VA_ARGS__)
#define LOG_ERROR(comp, ...) WVT_LOG(LogLevel::Error, comp, __VA_ARGS__)
