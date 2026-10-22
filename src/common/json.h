#pragma once
// wvt's one JSON module (issue #54). The reader (issue #27, chain files)
// handles objects, arrays, strings, numbers, true / false / null: enough for
// wvt's own files and SigMF metadata, not a validator for every corner of
// RFC 8259 (it accepts what it can read). The writer is two functions,
// jsonQuote and jsonNumber; the files that write JSON keep their own layout.
#include <cstdint>
#include <string>
#include <vector>

struct JsonMember;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<JsonMember> obj;  // in file order

    bool isNull() const { return type == Type::Null; }
    // Object member, or null if it's missing (or this isn't an object).
    const JsonValue* get(const std::string& key) const;
    // Typed members with a default when missing or of another type.
    double number(const std::string& key, double def = 0) const;
    std::string string(const std::string& key, const std::string& def = {}) const;
    bool boolean(const std::string& key, bool def = false) const;
    // Whole-number member in [lo, hi], or `def` when missing. False if the
    // member is there but isn't such a number (casting it would be undefined).
    bool integer(const std::string& key, int64_t lo, int64_t hi, int64_t def, int64_t& out) const;
};

// An object member. Not std::pair<std::string, JsonValue>: JsonValue is not
// complete inside its own definition, and libstdc++ 14 refuses a pair of an
// incomplete type (a vector of an incomplete type is allowed).
struct JsonMember {
    std::string first;
    JsonValue second;
};

// Parses the whole text. On failure, err says what and where (line:column).
bool parseJson(const std::string& text, JsonValue& out, std::string& err);

// A JSON string literal, quotes included, with the needed escapes.
std::string jsonQuote(const std::string& s);

// A JSON number: the fewest digits (%g, 15 to 17) that read back as the same
// double ("0.1", "915000000", "1e+21"). NaN and infinity are not JSON: "null".
std::string jsonNumber(double v);
