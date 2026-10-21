// libFuzzer target: parseSI, the "2.4M" / "915 MHz" number parser of the
// command line and the input fields (issue #53).
#include <cstdint>
#include <string>

#include "ui_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string s((const char*)data, size);  // parseSI wants a NUL-terminated string
    double v = 0;
    (void)parseSI(s.c_str(), v);
    return 0;
}
