// libFuzzer target: guessMeta (issue #53). The first byte picks the input:
// even = a file name (gqrx and "2.4MSps_915MHz" regexes, extension), odd = the
// text of a SigMF .sigmf-meta file.
#include <cstdint>
#include <string>

#include "fuzz_util.h"
#include "io/iq_file.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const bool asName = (data[0] & 1) == 0;
    ++data;
    --size;
    if (asName) {
        // A real file name: at most 255 bytes, no '/' or NUL.
        std::string name;
        for (size_t i = 0; i < size && name.size() < 255; ++i)
            if (data[i] != '/' && data[i] != 0) name += (char)data[i];
        (void)guessMeta("/nonexistent-wvt-fuzz/" + name);
    } else {
        static const std::string path = fuzzFile(".sigmf-meta");
        if (writeFuzzFile(path, data, size)) (void)guessMeta(path);
    }
    return 0;
}
