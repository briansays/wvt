// Logger self-test: level names, -v / --log-dir argument parsing, console and
// file filtering, the log file name and header, name clashes, a missing log
// folder (made), and lines from many threads at once. Writes into a fresh temp folder. Exit status is
// nonzero on failure.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string slurp(FILE* f) {
    fflush(f);
    rewind(f);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    return s;
}

int count(const std::string& s, const std::string& what) {
    int n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}

// Runs logParseArgs on a space-separated command line.
struct Parsed {
    bool ok;
    LogConfig cfg;
    std::string rest, err;
};
Parsed parse(const std::string& line) {
    std::vector<std::string> words;
    std::istringstream is("wvt " + line);
    for (std::string w; is >> w;) words.push_back(w);
    std::vector<char*> args;
    for (auto& w : words) args.push_back(w.data());
    Parsed p;
    p.ok = logParseArgs(args, p.cfg, p.err);
    for (size_t i = 1; i < args.size(); ++i) p.rest += (i > 1 ? " " : "") + std::string(args[i]);
    return p;
}

void testLevels() {
    printf("levels\n");
    LogLevel l;
    check(parseLogLevel("DEBUG", l) && l == LogLevel::Debug, "DEBUG");
    check(parseLogLevel("info", l) && l == LogLevel::Info, "info");
    check(parseLogLevel("Warning", l) && l == LogLevel::Warn, "Warning is an alias of WARN");
    check(parseLogLevel("warn", l) && l == LogLevel::Warn, "warn");
    check(parseLogLevel("ERROR", l) && l == LogLevel::Error, "ERROR");
    check(!parseLogLevel("loud", l), "unknown level rejected");
}

void testArgs() {
    printf("arguments\n");
    auto p = parse("rec.cu8");
    check(p.ok && !p.cfg.verbose && p.cfg.console == LogLevel::Warn && p.rest == "rec.cu8", "no -v: WARN");
    p = parse("-v rec.cu8");
    check(p.ok && p.cfg.verbose && p.cfg.console == LogLevel::Info && p.rest == "rec.cu8", "-v FILE: INFO, file kept");
    p = parse("-v debug rec.cu8");
    check(p.ok && p.cfg.console == LogLevel::Debug && p.rest == "rec.cu8", "-v debug");
    p = parse("--verbose ERROR -r 2.4M");
    check(p.ok && p.cfg.console == LogLevel::Error && p.rest == "-r 2.4M", "--verbose ERROR");
    p = parse("--verbose=warning");
    check(p.ok && p.cfg.console == LogLevel::Warn && p.cfg.verbose, "--verbose=warning");
    p = parse("-vDEBUG");
    check(p.ok && p.cfg.console == LogLevel::Debug, "-vDEBUG");
    p = parse("--verbose=loud");
    check(!p.ok && !p.err.empty(), "--verbose=loud is an error");
    p = parse("--log-dir /x --no-log-file --view 0:1");
    check(p.ok && p.cfg.dir == "/x" && !p.cfg.file && p.rest == "--view 0:1", "--log-dir, --no-log-file");
    p = parse("--log-dir");
    check(!p.ok, "--log-dir without a value is an error");
    p = parse("-v ./debug");
    check(p.ok && p.cfg.console == LogLevel::Info && p.rest == "./debug", "-v ./debug keeps the file");
}

void testFile(const std::string& dir) {
    printf("log file\n");
    FILE* con = tmpfile();
    LogConfig cfg;
    cfg.dir = dir + "/";  // trailing slash is trimmed
    cfg.commandLine = "wvt --test";
    cfg.consoleStream = con;
    logInit(cfg);
    std::string path = logFilePath();
    std::string name = path.substr(path.rfind('/') + 1);
    check(path.rfind(dir + "/wvt_log_", 0) == 0, "file in --log-dir: " + path);
    check(name.size() == strlen("wvt_log_YYYYMMDD_HHMMSS.log") && name.substr(name.size() - 4) == ".log",
          "name wvt_log_<date>_<time>.log");

    LOG_DEBUG("test", "debug line %d", 1);
    LOG_INFO("test", "info line");
    LOG_WARN("test", "warn line\n");
    LOG_ERROR("test", "error line");
    check(!logEnabled(LogLevel::Off), "Off is never enabled");
    std::string c = slurp(con);
    check(count(c, "debug line") == 0 && count(c, "info line") == 0, "console default hides DEBUG and INFO");
    check(count(c, "wvt: warning: warn line\n") == 1 && count(c, "wvt: error line\n") == 1,
          "console default keeps the wvt: format");

    // A second run in the same second must not overwrite the first.
    logInit(cfg);
    std::string path2 = logFilePath();
    check(!path2.empty() && path2 != path, "name clash gets a new name: " + path2.substr(path2.rfind('/') + 1));

    std::string f = slurp(path);
    check(count(f, "# command: wvt --test") == 1 && count(f, "# build: git") == 1 && count(f, "# os: ") == 1,
          "header has command, build and os");
    check(count(f, "[DEBUG] [test] debug line 1") == 1 && count(f, "[INFO ] [test] info line") == 1 &&
              count(f, "[WARN ] [test] warn line\n") == 1 && count(f, "[ERROR] [test] error line") == 1,
          "file gets all levels, trailing newline trimmed");

    // Long messages go through the heap path.
    std::string longMsg(2000, 'x');
    LOG_INFO("test", "%s", longMsg.c_str());
    logFlush();
    check(count(slurp(path2), longMsg) == 1, "2000-char message kept whole");
    fclose(con);
}

void testVerboseConsole(const std::string& dir) {
    printf("verbose console\n");
    FILE* con = tmpfile();
    LogConfig cfg;
    cfg.dir = dir;
    cfg.verbose = true;
    cfg.console = LogLevel::Debug;
    cfg.consoleStream = con;
    logInit(cfg);
    LOG_DEBUG("live", "tuned");
    std::string c = slurp(con);
    // "HH:MM:SS.mmm [DEBUG] [live] tuned"
    size_t p = c.find("[DEBUG] [live] tuned");
    check(p == 13 || (p != std::string::npos && c[p - 1] == ' ' && c[p - 5] == '.'), "timestamped -v format");
    fclose(con);
}

void testStatusProgress(const std::string& dir) {
    printf("status and progress\n");
    FILE* con = tmpfile();
    LogConfig cfg;
    cfg.dir = dir;
    cfg.consoleStream = con;
    logInit(cfg);
    std::string path = logFilePath();
    logStatus("rec", "wrote %s", "x.cf32");
    logProgress("rec", "1 s  2 MB   ");
    logProgress("rec", "2 s  4 MB   ");
    LOG_WARN("rec", "gap");
    logProgress("rec", "3 s  6 MB   ");
    logProgressEnd();
    logProgressEnd();  // no second newline
    logProgress("rec", "again");
    logProgressEnd();
    logStatus("rec", "done");
    std::string c = slurp(con);
    check(c == "wvt: wrote x.cf32\n\rwvt: 1 s  2 MB   \rwvt: 2 s  4 MB   \nwvt: warning: gap\n"
               "\rwvt: 3 s  6 MB   \n\rwvt: again\nwvt: done\n",
          "status always printed; progress rewrites in place; other lines start on a new line");
    logShutdown();
    std::string f = slurp(path);
    check(count(f, "[INFO ] [rec] wrote x.cf32") == 1 && count(f, "[INFO ] [rec] done") == 1, "status is INFO in the file");
    check(count(f, "[DEBUG] [rec] 1 s  2 MB\n") == 1 && count(f, "2 s  4 MB") == 0 && count(f, "3 s  6 MB") == 0 &&
              count(f, "[DEBUG] [rec] again\n") == 1,
          "progress in the file: throttled, restarted after logProgressEnd, padding trimmed");
    fclose(con);
}

void testNoFile(const std::string& dir) {
    printf("no file\n");
    FILE* con = tmpfile();
    LogConfig cfg;
    cfg.file = false;
    cfg.consoleStream = con;
    logInit(cfg);
    check(logFilePath().empty(), "--no-log-file opens nothing");
    check(!logEnabled(LogLevel::Debug), "DEBUG off with no file and console at WARN");
    cfg.file = true;
    std::string blocker = dir + "/a_file";  // a folder below a file cannot be made
    fclose(fopen(blocker.c_str(), "w"));
    cfg.dir = blocker + "/wvt";
    logInit(cfg);
    std::string c = slurp(con);
    check(logFilePath().empty() && count(c, "wvt: warning: cannot create a log file in " + cfg.dir) == 1,
          "folder that cannot be made: one warning, no file");
    fclose(con);
}

void testMakeFolder(const std::string& dir) {
    printf("missing folder\n");
    FILE* con = tmpfile();
    LogConfig cfg;
    cfg.dir = dir + "/new/logs/";
    cfg.consoleStream = con;
    logInit(cfg);
    std::string path = logFilePath();
    check(path.rfind(dir + "/new/logs/wvt_log_", 0) == 0, "missing folders are made: " + path);
    logShutdown();
    check(slurp(path).find("# wvt log, started") == 0, "the file in the new folder has the header");
    fclose(con);
}

void testThreads(const std::string& dir) {
    printf("threads\n");
    FILE* con = tmpfile();
    LogConfig cfg;
    cfg.dir = dir;
    cfg.consoleStream = con;
    logInit(cfg);
    std::string path = logFilePath();
    const int kThreads = 8, kLines = 2000;
    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t)
        th.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) LOG_DEBUG("thr", "thread %d line %d end", t, i);
        });
    for (auto& t : th) t.join();
    logShutdown();
    std::ifstream f(path);
    int good = 0, bad = 0;
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#' || line.find("[thr]") == std::string::npos) continue;
        if (line.find("[DEBUG] [thr] thread ") != std::string::npos && line.size() > 4 &&
            line.compare(line.size() - 4, 4, " end") == 0)
            ++good;
        else
            ++bad;
    }
    check(good == kThreads * kLines && bad == 0,
          std::to_string(good) + " whole lines from " + std::to_string(kThreads) + " threads, " +
              std::to_string(bad) + " broken");
    fclose(con);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/wvt_log_check_XXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    std::string dir = tmpl;
    testLevels();
    testArgs();
    testFile(dir);
    testVerboseConsole(dir);
    testStatusProgress(dir);
    testNoFile(dir);
    testMakeFolder(dir);
    testThreads(dir);
    std::string cmd = "rm -rf '" + dir + "'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", dir.c_str());
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
