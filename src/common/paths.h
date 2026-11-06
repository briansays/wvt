#pragma once
// File system helpers for the config folder (issue #101).
#include <string>
#include <vector>

// The folder of the running program, with links resolved
// (_NSGetExecutablePath on macOS, /proc/self/exe on Linux). Empty if it cannot
// be found.
std::string executableDir();

// True when `path` is a folder (links followed).
bool isDirectory(const std::string& path);

// True when `path` exists and is a regular file (links followed).
bool isRegularFile(const std::string& path);

// The names of the regular files in `dir` that start with `prefix` and end
// with `suffix`, sorted. Empty if the folder cannot be read.
std::vector<std::string> listFiles(const std::string& dir, const std::string& prefix, const std::string& suffix);

// The absolute path with links, "." and ".." resolved (realpath), or `path`
// unchanged if it does not exist.
std::string canonicalPath(const std::string& path);
