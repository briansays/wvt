#include "common/paths.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

std::string canonicalPath(const std::string& path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf)) return buf;
    return path;
}

std::string executableDir() {
    std::string exe;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // gets the size
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) exe = buf.data();
#else
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) exe.assign(buf, (size_t)n);
#endif
    if (exe.empty()) return {};
    exe = canonicalPath(exe);
    const size_t slash = exe.rfind('/');
    if (slash == std::string::npos) return {};
    return slash == 0 ? "/" : exe.substr(0, slash);
}

bool isDirectory(const std::string& path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isRegularFile(const std::string& path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> listFiles(const std::string& dir, const std::string& prefix, const std::string& suffix) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    // readdir is safe here: this DIR is not shared with another thread (POSIX
    // deprecates readdir_r).
    while (const dirent* e = readdir(d)) {  // NOLINT(concurrency-mt-unsafe)
        const std::string name = e->d_name;
        if (name.size() < prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        std::string path = dir;
        path += '/';
        path += name;
        if (isRegularFile(path)) out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}
