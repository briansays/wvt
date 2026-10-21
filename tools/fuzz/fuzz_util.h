// Helpers for the libFuzzer targets (issue #53).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

// A temp file for this process. The parsers under test read files, so each
// run writes the fuzz input to it first.
inline std::string fuzzFile(const char* suffix) {
    const char* tmp = getenv("TMPDIR");
    return std::string(tmp && *tmp ? tmp : "/tmp") + "/wvt_fuzz_" + std::to_string(getpid()) + suffix;
}

inline bool writeFuzzFile(const std::string& path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
