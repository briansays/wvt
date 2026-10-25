// libFuzzer target: parseConfig, the configuration file parser (issue #88).
#include <cstdint>
#include <string>
#include <vector>

#include "app/config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    WvtConfig cfg;
    std::vector<std::string> warnings;
    parseConfig(std::string((const char*)data, size), "fuzz.conf", cfg, warnings);
    return 0;
}
