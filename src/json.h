#pragma once
// A small JSON reader (issue #27, chain files): objects, arrays, strings,
// numbers, true / false / null. Enough for wvt's own files; not a validator
// for every corner of RFC 8259 (it accepts what it can read).
#include <string>
#include <utility>
#include <vector>

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;  // in file order

    bool isNull() const { return type == Type::Null; }
    // Object member, or null if it's missing (or this isn't an object).
    const JsonValue* get(const std::string& key) const;
    // Typed members with a default when missing or of another type.
    double number(const std::string& key, double def = 0) const;
    std::string string(const std::string& key, const std::string& def = {}) const;
    bool boolean(const std::string& key, bool def = false) const;
};

// Parses the whole text. On failure, err says what and where (line:column).
bool parseJson(const std::string& text, JsonValue& out, std::string& err);

// A JSON string literal, quotes included, with the needed escapes.
std::string jsonQuote(const std::string& s);
