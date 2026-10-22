#include "app/cli.h"

#include <map>

#include "common/log.h"

namespace {
constexpr size_t kHelpColumn = 23;  // where the help text of each option starts
}  // namespace

void CliParser::section(const std::string& title) { sections_.push_back({title, {}}); }

void CliParser::add(const char* shortName, const char* longName, const char* arg, const char* help, Handler handler) {
    if (sections_.empty()) section("options:");
    sections_.back().options.push_back(
        {shortName ? shortName : "", longName ? longName : "", arg ? arg : "", help ? help : "", std::move(handler)});
}

void CliParser::action(const char* shortName, const char* longName, const char* help, Handler handler) {
    add(shortName, longName, nullptr, help, std::move(handler));
    sections_.back().options.back().stops = true;
}

void CliParser::note(const char* shortName, const char* longName, const char* arg, const char* help) {
    add(shortName, longName, arg, help, nullptr);
}

void CliParser::epilogue(const std::string& text) { epilogue_ += text; }

const CliParser::Option* CliParser::find(const std::string& name) const {
    for (const auto& s : sections_)
        for (const auto& o : s.options)
            if (name == o.longName || (!o.shortName.empty() && name == o.shortName)) return &o;
    return nullptr;
}

std::vector<std::string> CliParser::duplicates() const {
    std::map<std::string, int> n;
    for (const auto& s : sections_)
        for (const auto& o : s.options) {
            if (!o.shortName.empty()) ++n[o.shortName];
            ++n[o.longName];
        }
    std::vector<std::string> d;
    for (const auto& [name, count] : n)
        if (count > 1) d.push_back(name);
    return d;
}

bool CliParser::parse(int argc, char** argv, std::vector<std::string>& positional) const {
    const auto dup = duplicates();
    for (const auto& d : dup) LOG_ERROR("main", "internal error: option %s is defined twice", d.c_str());
    if (!dup.empty()) return false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.empty() || a[0] != '-') {
            positional.push_back(a);
            continue;
        }
        const Option* o = find(a);
        if (!o) {
            LOG_ERROR("main", "unknown option %s (try --help)", a.c_str());
            return false;
        }
        const char* value = nullptr;
        if (!o->arg.empty()) {
            if (i + 1 >= argc) {
                LOG_ERROR("main", "%s needs a value", o->longName.c_str());
                return false;
            }
            value = argv[++i];
        }
        if (o->handler && !o->handler(value)) return false;
        if (o->stops) return true;
    }
    return true;
}

std::string CliParser::help(const std::string& header) const {
    std::string h = header;
    for (const auto& s : sections_) {
        h += "\n" + s.title + "\n";
        for (const auto& o : s.options) {
            std::string left = o.shortName.empty() ? "      " : "  " + o.shortName + ", ";
            left += o.longName + (o.arg.empty() ? "" : " " + o.arg);
            left += left.size() < kHelpColumn - 1 ? std::string(kHelpColumn - left.size(), ' ') : "  ";
            std::string text = o.help;
            for (size_t k = text.find('\n'); k != std::string::npos; k = text.find('\n', k + 1 + kHelpColumn))
                text.insert(k + 1, kHelpColumn, ' ');
            h += left + text + "\n";
        }
    }
    return h + (epilogue_.empty() ? "" : "\n" + epilogue_);
}
