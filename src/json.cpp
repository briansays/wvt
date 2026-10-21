#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
struct Parser {
    explicit Parser(const std::string& text) : t(text) {}
    const std::string& t;
    size_t i = 0;
    std::string err;

    bool fail(const char* what) {
        if (!err.empty()) return false;
        int line = 1, col = 1;
        for (size_t k = 0; k < i && k < t.size(); ++k) {
            if (t[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        char b[160];
        snprintf(b, sizeof b, "%s at line %d:%d", what, line, col);
        err = b;
        return false;
    }
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    }
    bool word(const char* w) {
        size_t k = 0;
        while (w[k] && i + k < t.size() && t[i + k] == w[k]) ++k;
        if (w[k]) return false;
        i += k;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xc0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            o += (char)(0xe0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3f));
            o += (char)(0x80 | (cp & 0x3f));
        } else {
            o += (char)(0xf0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3f));
            o += (char)(0x80 | ((cp >> 6) & 0x3f));
            o += (char)(0x80 | (cp & 0x3f));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        ++i;  // opening quote
        while (i < t.size() && t[i] != '"') {
            const char c = t[i++];
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i >= t.size()) break;
            const char e = t[i++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return fail("bad \\u escape");
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (i >= t.size()) return fail("unterminated string");
        ++i;  // closing quote
        return true;
    }
    bool value(JsonValue& v, int depth) {
        if (depth > 64) return fail("nested too deeply");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        const char c = t[i];
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                ws();
                if (i >= t.size() || t[i] != '"') return fail("expected a key");
                std::string k;
                if (!string(k)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                ++i;
                JsonValue m;
                if (!value(m, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(m));
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                JsonValue m;
                if (!value(m, depth + 1)) return false;
                v.arr.push_back(std::move(m));
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return string(v.str);
        }
        if (word("true")) {
            v.type = JsonValue::Type::Bool;
            v.b = true;
            return true;
        }
        if (word("false")) {
            v.type = JsonValue::Type::Bool;
            return true;
        }
        if (word("null")) return true;
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* s = t.c_str() + i;
            char* e = nullptr;
            v.num = strtod(s, &e);
            if (e == s) return fail("bad number");
            v.type = JsonValue::Type::Number;
            i += (size_t)(e - s);
            return true;
        }
        return fail("unexpected character");
    }
};
}  // namespace

const JsonValue* JsonValue::get(const std::string& key) const {
    for (const auto& m : obj)
        if (m.first == key) return &m.second;
    return nullptr;
}

double JsonValue::number(const std::string& key, double def) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::Number ? v->num : def;
}

std::string JsonValue::string(const std::string& key, const std::string& def) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::String ? v->str : def;
}

bool JsonValue::boolean(const std::string& key, bool def) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::Bool ? v->b : def;
}

bool JsonValue::integer(const std::string& key, int64_t lo, int64_t hi, int64_t def, int64_t& out) const {
    const JsonValue* v = get(key);
    if (!v) {
        out = def;
        return true;
    }
    // NaN fails the floor test, infinities the range test.
    if (v->type != Type::Number || v->num != std::floor(v->num) || v->num < (double)lo || v->num > (double)hi) return false;
    out = (int64_t)v->num;
    return true;
}

bool parseJson(const std::string& text, JsonValue& out, std::string& err) {
    Parser p(text);
    out = {};
    if (!p.value(out, 0)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        p.fail("extra text after the value");
        err = p.err;
        return false;
    }
    return true;
}

std::string jsonQuote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c == '\n') {
            o += "\\n";
        } else if (c == '\t') {
            o += "\\t";
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += (char)c;
        }
    }
    return o + "\"";
}
