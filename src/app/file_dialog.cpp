// Native file dialogs: see file_dialog.h.
#include "app/file_dialog.h"
#include "common/log.h"  // envVar

#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "common/ui_util.h"  // tooltipAlways
#include "imgui.h"
#include "io/recorder.h"  // expandHome

#ifdef WVT_HAVE_NFD
#include "nfd.h"
#endif

#if defined(WVT_HAVE_NFD) && defined(__APPLE__)
// file_dialog_mac.m: wvt is an X11 (XQuartz) client, not a Cocoa app, so the
// panel would open behind the window without an activation first.
extern "C" void* wvt_dialog_begin(void);
extern "C" void wvt_dialog_end(void* previous);
#endif

namespace filedlg {
namespace {

std::string home() {
    const char* h = envVar("HOME");
    return h && *h ? h : "/";
}

std::string stripSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

#ifdef WVT_HAVE_NFD
bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// "/a/b/c" -> "/a/b", "/a" -> "/"
std::string parentOf(const std::string& path) {
    const std::string p = stripSlashes(path);
    const size_t s = p.find_last_of('/');
    if (s == std::string::npos) return "";
    return s == 0 ? "/" : p.substr(0, s);
}

// An absolute path with "~" expanded; a relative path is taken from the
// current folder, an empty one is the home folder.
std::string absolute(const std::string& path) {
    std::string p = expandHome(path);
    if (p.empty()) return home();
    if (p[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) p = std::string(cwd) + "/" + p;
    }
    return p;
}

// The nearest folder that exists, with a trailing '/' (zenity opens the
// folder itself, not its parent with the folder selected).
std::string existingFolder(const std::string& path) {
    std::string p = stripSlashes(absolute(path));
    while (!p.empty() && p != "/" && !isDir(p)) p = parentOf(p);
    if (p.empty() || p == "/") return "/";
    return p + "/";
}

std::string saveStart(const std::string& path) {
    const std::string a = absolute(path);
    if (isDir(a) || a.back() == '/') return existingFolder(a);
    const std::string name = a.substr(a.find_last_of('/') + 1);
    return existingFolder(parentOf(a)) + name;
}

// Makes the dialog usable from wvt's window, and afterwards drops the input
// state the window still holds: the dialog took the key and button releases.
struct Scope {
#ifdef __APPLE__
    void* prev = wvt_dialog_begin();
#endif
    ~Scope() {
#ifdef __APPLE__
        wvt_dialog_end(prev);
#endif
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
    }
};

Result finish(nfdresult_t r, std::string& err) {
    if (r == NFD_OKAY) return Result::Ok;
    if (r == NFD_CANCEL) return Result::Cancel;
    const char* e = NFD_GetError();
    err = e && *e ? e : "the file dialog failed";
    return Result::Error;
}

Result single(nfdresult_t r, nfdchar_t* path, std::string& out, std::string& err) {
    const Result res = finish(r, err);
    if (res == Result::Ok) out = path ? path : "";
    free(path);  // NFD allocates with malloc
    if (res == Result::Ok && out.empty()) return Result::Cancel;
    return res;
}
#endif

Result unavailable(std::string& err) {
    available(&err);
    return Result::Error;
}

}  // namespace

bool available(std::string* why) {
#ifndef WVT_HAVE_NFD
    if (why) *why = "wvt was built without file dialogs (WVT_WITH_NFD=OFF). Type the path instead.";
    return false;
#elif defined(__APPLE__)
    return true;
#else
    // zenity runs as a program: look for it once.
    static const bool found = [] {
        const char* path = envVar("PATH");
        std::string dirs = path ? path : "/usr/bin:/bin";
        size_t a = 0;
        while (a <= dirs.size()) {
            size_t b = dirs.find(':', a);
            if (b == std::string::npos) b = dirs.size();
            const std::string dir = b > a ? dirs.substr(a, b - a) : ".";
            if (access((dir + "/zenity").c_str(), X_OK) == 0) return true;
            a = b + 1;
        }
        return false;
    }();
    if (!found && why) *why = "The file browser needs zenity (e.g. sudo apt install zenity). Type the path instead.";
    return found;
#endif
}

Result openFile(const std::string& filters, const std::string& start, std::string& out, std::string& err) {
    if (!available()) return unavailable(err);
#ifdef WVT_HAVE_NFD
    const std::string dir = existingFolder(start);
    Scope scope;
    nfdchar_t* path = nullptr;
    const nfdresult_t r = NFD_OpenDialog(filters.empty() ? nullptr : filters.c_str(), dir.c_str(), &path);
    return single(r, path, out, err);
#else
    return Result::Error;
#endif
}

Result openFiles(const std::string& filters, const std::string& start, std::vector<std::string>& out,
                 std::string& err) {
    if (!available()) return unavailable(err);
#ifdef WVT_HAVE_NFD
    const std::string dir = existingFolder(start);
    Scope scope;
    nfdpathset_t set{};
    const Result res =
        finish(NFD_OpenDialogMultiple(filters.empty() ? nullptr : filters.c_str(), dir.c_str(), &set), err);
    if (res != Result::Ok) return res;
    out.clear();
    for (size_t i = 0; i < NFD_PathSet_GetCount(&set); ++i) out.emplace_back(NFD_PathSet_GetPath(&set, i));
    NFD_PathSet_Free(&set);
    return out.empty() ? Result::Cancel : Result::Ok;
#else
    return Result::Error;
#endif
}

Result saveFile(const std::string& filters, const std::string& start, std::string& out, std::string& err) {
    if (!available()) return unavailable(err);
#ifdef WVT_HAVE_NFD
    const std::string from = saveStart(start);
    Scope scope;
    nfdchar_t* path = nullptr;
    const nfdresult_t r = NFD_SaveDialog(filters.empty() ? nullptr : filters.c_str(), from.c_str(), &path);
    return single(r, path, out, err);
#else
    return Result::Error;
#endif
}

Result pickFolder(const std::string& start, std::string& out, std::string& err) {
    if (!available()) return unavailable(err);
#ifdef WVT_HAVE_NFD
    const std::string dir = existingFolder(start);
    Scope scope;
    nfdchar_t* path = nullptr;
    const nfdresult_t r = NFD_PickFolder(dir.c_str(), &path);
    const Result res = single(r, path, out, err);
    if (res == Result::Ok) out = stripSlashes(out);
    return res;
#else
    return Result::Error;
#endif
}

bool button(const char* label, const char* tip) {
    std::string why;
    const bool ok = available(&why);
    ImGui::BeginDisabled(!ok);
    const bool hit = ImGui::Button(label);
    ImGui::EndDisabled();
    tooltipAlways(ok ? tip : why.c_str());
    return hit && ok;
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

std::string prefixFolder(const std::string& prefix) {
    const size_t s = prefix.find_last_of('/');
    if (s == std::string::npos) return "";
    return s == 0 ? "/" : prefix.substr(0, s);
}

std::string withFolder(const std::string& prefix, const std::string& folder) {
    const size_t s = prefix.find_last_of('/');
    const std::string name = s == std::string::npos ? prefix : prefix.substr(s + 1);
    // Show the home folder as "~", as the defaults do.
    std::string dir = stripSlashes(folder);
    const std::string h = stripSlashes(home());
    if (h != "/" && (dir == h || dir.rfind(h + "/", 0) == 0)) dir = "~" + dir.substr(h.size());
    if (dir.empty() || dir.back() != '/') dir += '/';
    return dir + name;
}

}  // namespace filedlg
