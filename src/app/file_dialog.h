#pragma once
// Native open, save and folder dialogs through Native File Dialog
// (third_party/nativefiledialog): the Cocoa panels on macOS, and zenity on
// Linux. Each call blocks until the dialog closes; call it from the UI thread
// (Cocoa requires the main thread). The engines keep running on their own
// threads meanwhile; only the window stops redrawing.
//
// The text boxes stay: a dialog only fills them in. Without a back end (wvt
// built with WVT_WITH_NFD=OFF, or no zenity on Linux) available() is false,
// and the Browse buttons are disabled and say why.
#include <string>
#include <vector>

namespace filedlg {

enum class Result { Ok, Cancel, Error };

// True when a dialog can be shown. When false, why says what is missing.
bool available(std::string* why = nullptr);

// Call once per frame from the main loop. On macOS it reads the Cocoa events
// that a dialog leaves queued: an app that never reads them blocks clicks in
// every app. Elsewhere it does nothing.
void pump();

// filters: extensions without the dot, separated by commas; a ';' starts
// another filter group on Linux (macOS joins the groups). Empty: all files.
// start: the folder (or, for a save dialog, the path with a file name) the
// dialog starts at; "~" is expanded, a missing folder falls back to the
// nearest one that exists, and an empty start is the home folder. err is set
// on Result::Error.
Result openFile(const std::string& filters, const std::string& start, std::string& out, std::string& err);
Result openFiles(const std::string& filters, const std::string& start, std::vector<std::string>& out,
                 std::string& err);
Result saveFile(const std::string& filters, const std::string& start, std::string& out, std::string& err);
Result pickFolder(const std::string& start, std::string& out, std::string& err);

// A button next to a path box; true when it is clicked and a dialog can be
// shown. Without a back end it is disabled, and its tooltip says why.
// buttonWidth() is its width, to size the box in front of it.
bool button(const char* label, const char* tip);
float buttonWidth(const char* label);

// The folder part of a path prefix such as "~/wvt-recordings/wvt" ("~/wvt-recordings"),
// and the prefix with its folder replaced (the file name part is kept).
std::string prefixFolder(const std::string& prefix);
std::string withFolder(const std::string& prefix, const std::string& folder);

}  // namespace filedlg
