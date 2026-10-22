// Clipboard self-test (issue #72): wvt's X11 window and a second X client
// (in its own thread, as another program would be) exchange the CLIPBOARD
// selection both ways: UTF-8 text, a program that offers only STRING
// (Latin-1), a large text that comes in parts (INCR), and copy from wvt to
// the other program. It needs an X server: in CI, Xvfb. Without $DISPLAY it
// exits with 77 (skipped).
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/platform_x11.h"
#include "imgui.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

using Clock = std::chrono::steady_clock;

// The other program: a window on its own connection.
struct Peer {
    Display* dpy = nullptr;
    Window win = 0;
    Atom clipboard = 0, utf8 = 0, targets = 0, incr = 0, prop = 0;

    Peer() {
        dpy = XOpenDisplay(nullptr);
        win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 10, 10, 0, 0, 0);
        XSelectInput(dpy, win, PropertyChangeMask);
        clipboard = XInternAtom(dpy, "CLIPBOARD", False);
        utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        targets = XInternAtom(dpy, "TARGETS", False);
        incr = XInternAtom(dpy, "INCR", False);
        prop = XInternAtom(dpy, "PEER_PROP", False);
    }
    ~Peer() {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }
    Time now() {  // a server timestamp, from a property change on our own window
        XChangeProperty(dpy, win, prop, XA_STRING, 8, PropModeAppend, nullptr, 0);
        XEvent ev;
        XWindowEvent(dpy, win, PropertyChangeMask, &ev);
        return ev.xproperty.time;
    }

    // Owns CLIPBOARD and serves `text` for `stopAt` seconds or until `done`.
    // utf8Ok = false: offers only STRING (an older program). chunk > 0:
    // sends it in parts of that size (INCR).
    void serve(const std::string& text, bool utf8Ok, size_t chunk, const std::atomic<bool>& done) {
        XSetSelectionOwner(dpy, clipboard, win, now());
        XFlush(dpy);
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (!done && Clock::now() < until) {
            if (!XPending(dpy)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type != SelectionRequest) continue;
            const XSelectionRequestEvent& r = ev.xselectionrequest;
            XSelectionEvent n{};
            n.type = SelectionNotify;
            n.requestor = r.requestor;
            n.selection = r.selection;
            n.target = r.target;
            n.time = r.time;
            n.property = None;
            if (r.target == targets) {
                Atom list[] = {targets, XA_STRING, utf8};
                XChangeProperty(dpy, r.requestor, r.property, XA_ATOM, 32, PropModeReplace, (unsigned char*)list,
                                utf8Ok ? 3 : 2);
                n.property = r.property;
            } else if (r.target == XA_STRING || (utf8Ok && r.target == utf8)) {
                if (chunk == 0 || text.size() <= chunk) {
                    XChangeProperty(dpy, r.requestor, r.property, r.target, 8, PropModeReplace,
                                    (const unsigned char*)text.data(), (int)text.size());
                    n.property = r.property;
                } else {
                    sendIncr(r, text, chunk);
                    continue;
                }
            }
            XSendEvent(dpy, r.requestor, False, 0, (XEvent*)&n);
            XFlush(dpy);
        }
    }

    // ICCCM INCR: announce the size, then one part each time the requestor
    // deletes the property, then an empty part.
    void sendIncr(const XSelectionRequestEvent& r, const std::string& text, size_t chunk) {
        XSelectInput(dpy, r.requestor, PropertyChangeMask);
        long size = (long)text.size();
        XChangeProperty(dpy, r.requestor, r.property, incr, 32, PropModeReplace, (unsigned char*)&size, 1);
        XSelectionEvent n{};
        n.type = SelectionNotify;
        n.requestor = r.requestor;
        n.selection = r.selection;
        n.target = r.target;
        n.time = r.time;
        n.property = r.property;
        XSendEvent(dpy, r.requestor, False, 0, (XEvent*)&n);
        XFlush(dpy);
        size_t pos = 0;
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < until) {
            XEvent ev;
            if (!XCheckTypedWindowEvent(dpy, r.requestor, PropertyNotify, &ev)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ev.xproperty.atom != r.property || ev.xproperty.state != PropertyDelete) continue;
            const size_t m = std::min(chunk, text.size() - pos);
            XChangeProperty(dpy, r.requestor, r.property, r.target, 8, PropModeReplace,
                            (const unsigned char*)text.data() + pos, (int)m);
            XFlush(dpy);
            pos += m;
            if (m == 0) break;
        }
        XSelectInput(dpy, r.requestor, NoEventMask);
    }

    // Asks the CLIPBOARD owner for `target`, as a paste in another program.
    bool request(Atom target, std::string& out, Atom& type) {
        XConvertSelection(dpy, clipboard, target, prop, win, now());
        XFlush(dpy);
        const auto until = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < until) {
            XEvent ev;
            if (!XCheckTypedWindowEvent(dpy, win, SelectionNotify, &ev)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ev.xselection.property == None) return false;
            int fmt;
            unsigned long n, rest;
            unsigned char* data = nullptr;
            XGetWindowProperty(dpy, win, prop, 0, 1 << 24, True, AnyPropertyType, &type, &fmt, &n, &rest, &data);
            out.assign((const char*)data, data ? n * (fmt == 32 ? sizeof(long) : fmt / 8) : 0);
            if (data) XFree(data);
            return true;
        }
        return false;
    }
};

const char* paste() { return ImGui::GetPlatformIO().Platform_GetClipboardTextFn(ImGui::GetCurrentContext()); }
void copy(const char* s) { ImGui::GetPlatformIO().Platform_SetClipboardTextFn(ImGui::GetCurrentContext(), s); }

// Pastes in wvt while the peer serves the clipboard in its thread.
std::string pasteFromPeer(const std::string& text, bool utf8Ok, size_t chunk) {
    Peer peer;
    std::atomic<bool> done{false};
    std::thread t([&] { peer.serve(text, utf8Ok, chunk, done); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the peer owns it now
    std::string got = paste();
    done = true;
    t.join();
    return got;
}

// Copies in wvt, then the peer pastes (target) while wvt runs its event loop.
bool peerPastes(X11Window& win, Atom target, std::string& out, Atom& type) {
    Peer peer;
    std::atomic<bool> finished{false};
    bool ok = false;
    std::thread t([&] {
        ok = peer.request(target, out, type);
        finished = true;
    });
    const auto until = Clock::now() + std::chrono::seconds(4);
    while (!finished && Clock::now() < until) win.pumpEvents(0.01);
    t.join();
    return ok;
}
}  // namespace

int main() {
    if (!getenv("DISPLAY")) {
        printf("no DISPLAY: skipped (run it under Xvfb)\n");
        return 77;
    }
    XInitThreads();
    ImGui::CreateContext();
    X11Window win;
    std::string err;
    if (!win.create("wvt clipboard_check", 200, 100, true, err)) {
        printf("cannot open the window: %s\n", err.c_str());
        return 1;
    }
    win.pumpEvents(0.05);

    printf("paste into wvt\n");
    const std::string utf = "f\xc3\xa9 915 MHz \xe2\x9c\x93";  // "fé 915 MHz ✓"
    check(pasteFromPeer(utf, true, 0) == utf, "UTF-8 text");
    // A program that offers only STRING (Latin-1): "fé" arrives as UTF-8.
    check(pasteFromPeer("f\xe9 1.5 GHz", false, 0) == "f\xc3\xa9 1.5 GHz", "a STRING-only owner: Latin-1 converted to UTF-8");
    std::string big(600 * 1024, 'x');
    for (size_t i = 0; i < big.size(); i += 1000) big[i] = '\n';
    const std::string gotBig = pasteFromPeer(big, true, 64 * 1024);
    check(gotBig == big, "600 kB in parts (INCR): got " + std::to_string(gotBig.size()) + " bytes");

    printf("copy from wvt\n");
    Display* d = XOpenDisplay(nullptr);
    const Atom utf8 = XInternAtom(d, "UTF8_STRING", False), targets = XInternAtom(d, "TARGETS", False);
    const Atom text = XInternAtom(d, "TEXT", False);
    copy(utf.c_str());
    std::string out;
    Atom type = None;
    check(peerPastes(win, utf8, out, type) && out == utf && type == utf8, "another program pastes UTF-8");
    check(peerPastes(win, XA_STRING, out, type) && out == "f\xe9 915 MHz ?" && type == XA_STRING,
          "STRING is Latin-1 (characters outside it become ?)");
    check(peerPastes(win, text, out, type) && out == utf, "TEXT gives the text");
    bool listed = peerPastes(win, targets, out, type) && type == XA_ATOM;
    bool hasUtf8 = false;
    for (size_t k = 0; listed && k + sizeof(long) <= out.size(); k += sizeof(long)) {
        long a;
        memcpy(&a, out.data() + k, sizeof a);
        hasUtf8 = hasUtf8 || (Atom)a == utf8;
    }
    check(listed && hasUtf8, "TARGETS lists UTF8_STRING");
    check(std::string(paste()) == utf, "wvt pastes its own copy");
    XCloseDisplay(d);

    ImGui::DestroyContext();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
