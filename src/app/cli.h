#pragma once
// A table-driven command-line parser (issue #54). Each option has its names,
// the name of its value, its help text and a handler, in one place; --help is
// made from the same table, so the two cannot disagree.
#include <functional>
#include <string>
#include <vector>

class CliParser {
public:
    // Gets the option's value (nullptr for a flag). Returns false on a bad
    // value, after it logs why.
    using Handler = std::function<bool(const char* value)>;

    // Starts a new group of options in the help text, e.g. "live devices:".
    void section(const std::string& title);
    // An option. shortName may be nullptr; arg is the value's name in the help
    // ("HZ"), or nullptr for a flag. A '\n' in help starts an indented line.
    void add(const char* shortName, const char* longName, const char* arg, const char* help, Handler handler);
    // A flag that does its work and ends the parse (--help, --list-devices):
    // the options after it are not read.
    void action(const char* shortName, const char* longName, const char* help, Handler handler);
    // An option that another parser takes out of argv first (the log options):
    // help text only.
    void note(const char* shortName, const char* longName, const char* arg, const char* help);
    // Lines printed after the options.
    void epilogue(const std::string& text);

    // Parses argv[1..]: options run their handlers, other words go to
    // positional. It stops after an action. False on an unknown option, a
    // missing value, a handler error or an option name used twice in the
    // table (logged).
    bool parse(int argc, char** argv, std::vector<std::string>& positional) const;
    // The help text: the usage line, then the sections, then the epilogue.
    std::string help(const std::string& header) const;
    // Option names that are in the table more than once (should be none).
    std::vector<std::string> duplicates() const;

private:
    struct Option {
        std::string shortName, longName, arg, help;
        Handler handler;  // empty for note()
        bool stops = false;  // action()
    };
    struct Section {
        std::string title;
        std::vector<Option> options;
    };
    const Option* find(const std::string& name) const;
    std::vector<Section> sections_;
    std::string epilogue_;
};
