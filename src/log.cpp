#include "log.h"

#include <sys/utsname.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <mutex>

#ifndef WVT_GIT_HASH
#define WVT_GIT_HASH "unknown"
#endif

namespace {

std::mutex g_mutex;  // guards everything below except the two atomics
std::atomic<int> g_consoleMin{(int)LogLevel::Warn};
std::atomic<int> g_fileMin{(int)LogLevel::Off};
bool g_verbose = false;
FILE* g_console = nullptr;
FILE* g_file = nullptr;
std::string g_path;
bool g_progressOpen = false;  // a "\r" progress line is on the console without its newline
std::chrono::steady_clock::time_point g_lastProgressFile;

const char* kNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};

// "HH:MM:SS.mmm" (date = false) or "YYYY-MM-DD HH:MM:SS.mmm", local time.
void stamp(char* buf, size_t n, bool date) {
    auto now = std::chrono::system_clock::now();
    time_t s = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    struct tm tm;
    localtime_r(&s, &tm);
    size_t len = strftime(buf, n, date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &tm);
    snprintf(buf + len, n - len, ".%03d", ms);
}

std::string logDir(const LogConfig& cfg) {
    std::string d = cfg.dir;
    if (d.empty())
        if (const char* e = envVar("WVT_LOG_DIR"); e && *e) d = e;
    if (d.empty())
        if (const char* e = envVar("TMPDIR"); e && *e) d = e;
    if (d.empty()) d = "/tmp";
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d;
}

// Opens a new file exclusively; on a name clash (two runs in one second)
// appends _<pid>, then _<pid>_<n>.
FILE* openUnique(const std::string& dir, std::string& path, std::string& err) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
    std::string base = dir + "/wvt_log_" + ts;
    for (int n = 0; n < 100; ++n) {
        path = base;
        if (n > 0) path += "_" + std::to_string(getpid());
        if (n > 1) path += "_" + std::to_string(n);
        path += ".log";
        if (FILE* f = fopen(path.c_str(), "wx")) return f;
        if (errno != EEXIST) break;
    }
    err = errnoText(errno);
    path.clear();
    return nullptr;
}

void writeHeader(const LogConfig& cfg) {
    char ts[40];
    stamp(ts, sizeof ts, true);
    fprintf(g_file, "# wvt log, started %s, pid %d\n", ts, (int)getpid());
    fprintf(g_file, "# build: git %s, RTL-SDR %s, UHD %s, audio %s\n", WVT_GIT_HASH,
#ifdef WVT_HAVE_RTLSDR
            "on",
#else
            "off",
#endif
#ifdef WVT_HAVE_UHD
            "on",
#else
            "off",
#endif
#ifdef WVT_HAVE_AUDIO
            "on"
#else
            "off"
#endif
    );
    struct utsname u;
    if (uname(&u) == 0) fprintf(g_file, "# os: %s %s %s\n", u.sysname, u.release, u.machine);
    if (!cfg.commandLine.empty()) fprintf(g_file, "# command: %s\n", cfg.commandLine.c_str());
    fflush(g_file);
}

void closeFile() {
    if (g_file) fclose(g_file);
    g_file = nullptr;
    g_path.clear();
    g_fileMin = (int)LogLevel::Off;
}

}  // namespace

bool parseLogLevel(const char* s, LogLevel& out) {
    std::string v;
    for (const char* p = s; *p; ++p) v += (char)tolower((unsigned char)*p);
    if (v == "debug") out = LogLevel::Debug;
    else if (v == "info") out = LogLevel::Info;
    else if (v == "warn" || v == "warning") out = LogLevel::Warn;
    else if (v == "error") out = LogLevel::Error;
    else return false;
    return true;
}

const char* logLevelName(LogLevel l) { return l < LogLevel::Off ? kNames[(int)l] : "OFF"; }

bool logParseArgs(std::vector<char*>& args, LogConfig& cfg, std::string& err) {
    std::vector<char*> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        if (i == 0) {
            rest.push_back(args[i]);
            continue;
        }
        if (a == "--") {  // everything after "--" is not ours
            rest.insert(rest.end(), args.begin() + i, args.end());
            break;
        }
        if (a == "-v" || a == "--verbose") {
            cfg.verbose = true;
            cfg.console = LogLevel::Info;
            LogLevel l;
            if (i + 1 < args.size() && parseLogLevel(args[i + 1], l)) {
                cfg.console = l;
                ++i;
            }
        } else if (a.rfind("--verbose=", 0) == 0 || (a.size() > 2 && a.rfind("-v", 0) == 0 && a[2] != '-')) {
            std::string v = a[1] == '-' ? a.substr(10) : a.substr(2);
            if (!parseLogLevel(v.c_str(), cfg.console)) {
                err = "unknown log level '" + v + "' (debug, info, warn, error)";
                return false;
            }
            cfg.verbose = true;
        } else if (a == "--log-dir") {
            if (i + 1 >= args.size()) {
                err = "--log-dir needs a value";
                return false;
            }
            cfg.dir = args[++i];
        } else if (a == "--no-log-file") {
            cfg.file = false;
        } else {
            rest.push_back(args[i]);
        }
    }
    args = std::move(rest);
    return true;
}

void logInit(const LogConfig& cfg) {
    std::string warn, path;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        closeFile();
        g_console = cfg.consoleStream ? cfg.consoleStream : stderr;
        g_verbose = cfg.verbose;
        g_consoleMin = (int)cfg.console;
        if (cfg.file) {
            std::string dir = logDir(cfg), e;
            g_file = openUnique(dir, g_path, e);
            if (g_file) {
                writeHeader(cfg);
                g_fileMin = (int)LogLevel::Debug;
                path = g_path;
            } else {
                warn = "cannot create a log file in " + dir + ": " + e;
            }
        }
    }
    static bool registered = false;
    if (!registered) {
        registered = true;
        atexit(logShutdown);
    }
    if (!warn.empty()) LOG_WARN("log", "%s", warn.c_str());
    else if (!path.empty()) LOG_INFO("log", "log file %s", path.c_str());
}

void logShutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_console) fflush(g_console);
    closeFile();
}

std::string logFilePath() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_path;
}

bool logEnabled(LogLevel l) { return l < LogLevel::Off && ((int)l >= g_consoleMin || (int)l >= g_fileMin); }

namespace {

std::string vformat(const char* fmt, va_list ap) {
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap2);
    va_end(ap2);
    std::string msg;
    if (n < 0) return msg;
    if (n < (int)sizeof small) {
        msg.assign(small, n);
    } else {
        msg.resize(n + 1);
        vsnprintf(&msg[0], msg.size(), fmt, ap);
        msg.resize(n);
    }
    while (!msg.empty() && msg.back() == '\n') msg.pop_back();
    return msg;
}

// Ends an open progress line so the next console line starts in column 0.
void endProgressLocked(FILE* c) {
    if (g_progressOpen) fputc('\n', c);
    g_progressOpen = false;
}

// forceConsole: print whatever the console level (status messages).
void emit(LogLevel l, bool forceConsole, const char* component, const std::string& msg) {
    char ts[40];
    stamp(ts, sizeof ts, true);
    const char* name = kNames[(int)l];
    std::lock_guard<std::mutex> lk(g_mutex);
    if (forceConsole || (int)l >= g_consoleMin) {
        FILE* c = g_console ? g_console : stderr;
        endProgressLocked(c);
        if (g_verbose) fprintf(c, "%s [%-5s] [%s] %s\n", ts + 11, name, component, msg.c_str());
        else if (l == LogLevel::Warn) fprintf(c, "wvt: warning: %s\n", msg.c_str());
        else fprintf(c, "wvt: %s\n", msg.c_str());
    }
    if (g_file && (int)l >= g_fileMin) {
        fprintf(g_file, "%s [%-5s] [%s] %s\n", ts, name, component, msg.c_str());
        if (l >= LogLevel::Warn) fflush(g_file);
    }
}

}  // namespace

void logWrite(LogLevel l, const char* component, const char* fmt, ...) {
    if (l >= LogLevel::Off) return;
    va_list ap;
    va_start(ap, fmt);
    std::string msg = vformat(fmt, ap);  // outside the lock
    va_end(ap);
    emit(l, false, component, msg);
}

void logStatus(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = vformat(fmt, ap);
    va_end(ap);
    emit(LogLevel::Info, true, component, msg);
}

void logProgress(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = vformat(fmt, ap);
    va_end(ap);
    auto now = std::chrono::steady_clock::now();
    bool toFile;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        FILE* c = g_console ? g_console : stderr;
        fprintf(c, "\rwvt: %s", msg.c_str());
        fflush(c);
        g_progressOpen = true;
        toFile = g_file && now - g_lastProgressFile >= std::chrono::seconds(10);
        if (toFile) g_lastProgressFile = now;
    }
    if (toFile) {
        // Trailing padding only matters on the console.
        while (!msg.empty() && msg.back() == ' ') msg.pop_back();
        char ts[40];
        stamp(ts, sizeof ts, true);
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_file) fprintf(g_file, "%s [DEBUG] [%s] %s\n", ts, component, msg.c_str());
    }
}

void logProgressEnd() {
    std::lock_guard<std::mutex> lk(g_mutex);
    endProgressLocked(g_console ? g_console : stderr);
    g_lastProgressFile = {};
}

void logFlush() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_console) fflush(g_console);
    if (g_file) fflush(g_file);
}
