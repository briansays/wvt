// Clipboard self-test (issue #72): wvt's X11 window and a second X client
// (in its own thread, as another program would be) exchange the CLIPBOARD
// selection both ways: UTF-8 text, a program that offers only STRING
// (Latin-1), a large text that comes in parts (INCR), and copy from wvt to
// the other program. Then key presses sent to wvt's window copy from one
// text field and paste into another (Ctrl, and Command on macOS). It needs
// an X server: in CI, Xvfb. Without one it exits with 77 (skipped).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

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
// wvt's window: the top-level window with our title (Xvfb has no window manager).
Window findWindow(Display* d, const char* title) {
    Window root, parent, *kids = nullptr;
    unsigned n = 0;
    Window found = 0;
    if (XQueryTree(d, DefaultRootWindow(d), &root, &parent, &kids, &n)) {
        for (unsigned i = 0; i < n && !found; ++i) {
            char* name = nullptr;
            if (XFetchName(d, kids[i], &name) && name) {
                if (strcmp(name, title) == 0) found = kids[i];
                XFree(name);
            }
        }
        if (kids) XFree(kids);
    }
    return found;
}

// A key press and release, as the X server sends them when a person types.
void sendKey(Display* d, Window w, KeySym ks, unsigned state, Time& t) {
    XKeyEvent e{};
    e.display = d;
    e.window = w;
    e.root = DefaultRootWindow(d);
    e.same_screen = True;
    e.keycode = XKeysymToKeycode(d, ks);
    e.state = state;
    for (int type : {KeyPress, KeyRelease}) {
        e.type = type;
        e.time = ++t;
        XSendEvent(d, w, False, type == KeyPress ? KeyPressMask : KeyReleaseMask, (XEvent*)&e);
        XFlush(d);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// The X modifier bit of Super or Meta (the Command key in XQuartz); 0 if none.
unsigned commandMask(Display* d) {
    unsigned mask = 0;
    XModifierKeymap* mm = XGetModifierMapping(d);
    const KeyCode cmd[] = {XKeysymToKeycode(d, XK_Super_L), XKeysymToKeycode(d, XK_Meta_L)};
    for (int m = Mod1MapIndex; m <= Mod5MapIndex && !mask; ++m)
        for (int k = 0; k < mm->max_keypermod; ++k)
            for (KeyCode c : cmd)
                if (c && mm->modifiermap[m * mm->max_keypermod + k] == c) mask = 1u << m;
    XFreeModifiermap(mm);
    return mask;
}

// Two text fields in a frame; `focus` 1 or 2 puts the keyboard in that field.
struct Fields {
    X11Window& win;
    char a[64] = "915.5 MHz";
    char b[64] = "";
    void frame(int focus = 0) {
        win.pumpEvents(0.0);
        win.newFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::Begin("fields");
        if (focus == 1) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##a", a, sizeof a);
        if (focus == 2) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##b", b, sizeof b);
        ImGui::End();
        ImGui::Render();
        win.render(ImGui::GetDrawData(), ImVec4(0, 0, 0, 1));
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) frame();
    }
    // Copy all of field a, paste it into the empty field b, with `mod` held.
    bool copyAcross(Display* d, Window w, unsigned mod, Time& t) {
        b[0] = 0;
        frame(1);
        frames(3);
        for (KeySym k : {XK_a, XK_c}) {
            sendKey(d, w, k, mod, t);
            frames(3);
        }
        frame(2);
        frames(3);
        sendKey(d, w, XK_v, mod, t);
        frames(3);
        return strcmp(a, b) == 0;
    }
};
}  // namespace

int main() {
    if (!getenv("DISPLAY")) {
        printf("no DISPLAY: skipped (run it under Xvfb)\n");
        return 77;
    }
    if (Display* probe = XOpenDisplay(nullptr)) {
        XCloseDisplay(probe);
    } else {  // DISPLAY is set, but no X server answers
        printf("cannot open DISPLAY %s: skipped (run it under Xvfb)\n", getenv("DISPLAY"));
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
    bool copyOk = false;

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

    printf("keys in wvt's window\n");
    const Window w = findWindow(d, "wvt clipboard_check");
    check(w != 0, "found the window");
    Fields f{win};
    Time t = 1;
    copyOk = w && f.copyAcross(d, w, ControlMask, t);
    check(copyOk, std::string("Ctrl+A, Ctrl+C in one field, Ctrl+V in another: '") + f.b + "'");
#ifdef __APPLE__
    const unsigned cmd = commandMask(d);
    check(cmd != 0 && f.copyAcross(d, w, cmd, t), std::string("the same with Command: '") + f.b + "'");
#else
    (void)commandMask;
#endif
    XCloseDisplay(d);

    ImGui::DestroyContext();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
