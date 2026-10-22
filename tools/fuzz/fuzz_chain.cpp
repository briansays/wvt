// libFuzzer target: loadChain (chain files) and checkParams on what it loads (issue #53).
#include <cstdint>
#include <string>

#include "fuzz_util.h"
#include "siggen/siggen.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const std::string path = fuzzFile(".json");
    if (!writeFuzzFile(path, data, size)) return 0;
    WaveformParams p;
    std::string err;
    if (loadChain(path, p, err)) (void)checkParams(p);
    return 0;
}
