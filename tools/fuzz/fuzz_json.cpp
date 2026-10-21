// libFuzzer target: parseJson and the JsonValue accessors (issue #53).
#include <cstdint>
#include <string>

#include "json.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    JsonValue v;
    std::string err;
    if (!parseJson(std::string((const char*)data, size), v, err)) return 0;
    // Exercise the typed getters on whatever came out.
    int64_t n = 0;
    (void)v.number("a");
    (void)v.string("b");
    (void)v.boolean("c");
    (void)v.integer("d", -1000, 1000, 0, n);
    for (const auto& m : v.obj) (void)m.second.integer("x", INT64_MIN / 2, INT64_MAX / 2, 0, n);
    return 0;
}
