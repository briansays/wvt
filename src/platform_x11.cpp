#include "platform_x11.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui_impl_opengl2.h"
#include "sw_renderer.h"

namespace {
int g_xerror = 0;
int xErrorHandler(Display*, XErrorEvent* e) {
    g_xerror = e->error_code;
    return 0;
}

ImGuiKey mapKey(KeySym k) {
    if (k >= XK_a && k <= XK_z) return (ImGuiKey)(ImGuiKey_A + (k - XK_a));
    if (k >= XK_A && k <= XK_Z) return (ImGuiKey)(ImGuiKey_A + (k - XK_A));
    if (k >= XK_0 && k <= XK_9) return (ImGuiKey)(ImGuiKey_0 + (k - XK_0));
    if (k >= XK_F1 && k <= XK_F12) return (ImGuiKey)(ImGuiKey_F1 + (k - XK_F1));
    if (k >= XK_KP_0 && k <= XK_KP_9) return (ImGuiKey)(ImGuiKey_Keypad0 + (k - XK_KP_0));
    switch (k) {
        case XK_Tab: case XK_ISO_Left_Tab: return ImGuiKey_Tab;
        case XK_Left: return ImGuiKey_LeftArrow;
        case XK_Right: return ImGuiKey_RightArrow;
        case XK_Up: return ImGuiKey_UpArrow;
        case XK_Down: return ImGuiKey_DownArrow;
        case XK_Page_Up: return ImGuiKey_PageUp;
        case XK_Page_Down: return ImGuiKey_PageDown;
        case XK_Home: return ImGuiKey_Home;
        case XK_End: return ImGuiKey_End;
        case XK_Insert: return ImGuiKey_Insert;
        case XK_Delete: return ImGuiKey_Delete;
        case XK_BackSpace: return ImGuiKey_Backspace;
        case XK_space: return ImGuiKey_Space;
        case XK_Return: return ImGuiKey_Enter;
        case XK_KP_Enter: return ImGuiKey_KeypadEnter;
        case XK_Escape: return ImGuiKey_Escape;
        case XK_apostrophe: return ImGuiKey_Apostrophe;
        case XK_comma: return ImGuiKey_Comma;
        case XK_minus: return ImGuiKey_Minus;
        case XK_period: return ImGuiKey_Period;
        case XK_slash: return ImGuiKey_Slash;
        case XK_semicolon: return ImGuiKey_Semicolon;
        case XK_equal: return ImGuiKey_Equal;
        case XK_plus: return ImGuiKey_Equal;
        case XK_bracketleft: return ImGuiKey_LeftBracket;
        case XK_backslash: return ImGuiKey_Backslash;
        case XK_bracketright: return ImGuiKey_RightBracket;
        case XK_grave: return ImGuiKey_GraveAccent;
        case XK_KP_Add: return ImGuiKey_KeypadAdd;
        case XK_KP_Subtract: return ImGuiKey_KeypadSubtract;
        case XK_KP_Decimal: return ImGuiKey_KeypadDecimal;
        case XK_Shift_L: return ImGuiKey_LeftShift;
        case XK_Shift_R: return ImGuiKey_RightShift;
        case XK_Control_L: return ImGuiKey_LeftCtrl;
        case XK_Control_R: return ImGuiKey_RightCtrl;
        case XK_Alt_L: case XK_Meta_L: return ImGuiKey_LeftAlt;
        case XK_Alt_R: case XK_Meta_R: return ImGuiKey_RightAlt;
        case XK_Super_L: return ImGuiKey_LeftSuper;
        case XK_Super_R: return ImGuiKey_RightSuper;
        default: return ImGuiKey_None;
    }
}
}  // namespace

struct X11Window::Impl {
    Display* dpy = nullptr;
    Window win = 0;
    Visual* visual = nullptr;
    int depth = 24;
    GLXContext glc = nullptr;
    bool gl = false;
    GC gc = 0;
    XImage* ximg = nullptr;
    std::vector<uint32_t> xbuf;
    SwRenderer sw;
    SwTexture fontTex;
    Atom wmDelete = 0, clipboard = 0, utf8 = 0, targets = 0, wvtProp = 0;
    int w = 0, h = 0;
    int wakeFd[2] = {-1, -1};
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    std::string clipOut, clipIn;
    int rshift = 16, gshift = 8, bshift = 0;

    void createXImage() {
        if (gl) return;
        if (ximg) {
            ximg->data = nullptr;
            XDestroyImage(ximg);
            ximg = nullptr;
        }
        xbuf.assign((size_t)w * h, 0);
        ximg = XCreateImage(dpy, visual, depth, ZPixmap, 0, (char*)xbuf.data(), w, h, 32, 0);
        sw.resize(w, h);
    }

    void handleSelectionRequest(const XSelectionRequestEvent& req) {
        XSelectionEvent ev{};
        ev.type = SelectionNotify;
        ev.display = req.display;
        ev.requestor = req.requestor;
        ev.selection = req.selection;
        ev.target = req.target;
        ev.time = req.time;
        ev.property = None;
        if (req.target == targets) {
            Atom list[] = {targets, utf8, XA_STRING};
            XChangeProperty(dpy, req.requestor, req.property, XA_ATOM, 32, PropModeReplace, (unsigned char*)list, 3);
            ev.property = req.property;
        } else if (req.target == utf8 || req.target == XA_STRING) {
            XChangeProperty(dpy, req.requestor, req.property, req.target, 8, PropModeReplace,
                            (const unsigned char*)clipOut.data(), (int)clipOut.size());
            ev.property = req.property;
        }
        XSendEvent(dpy, req.requestor, False, 0, (XEvent*)&ev);
        XFlush(dpy);
    }

    const char* getClipboard() {
        Window owner = XGetSelectionOwner(dpy, clipboard);
        if (owner == win) return clipOut.c_str();
        clipIn.clear();
        if (owner == None) return clipIn.c_str();
        XConvertSelection(dpy, clipboard, utf8, wvtProp, win, CurrentTime);
        XFlush(dpy);
        auto start = std::chrono::steady_clock::now();
        XEvent ev;
        for (;;) {
            if (XCheckTypedWindowEvent(dpy, win, SelectionNotify, &ev)) {
                if (ev.xselection.property != None) {
                    Atom type;
                    int fmt;
                    unsigned long n, rest;
                    unsigned char* data = nullptr;
                    XGetWindowProperty(dpy, win, wvtProp, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &rest,
                                       &data);
                    if (data) {
                        clipIn.assign((const char*)data, n);
                        XFree(data);
                    }
                }
                break;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500)) break;
            usleep(2000);
        }
        return clipIn.c_str();
    }

    void handle(XEvent& ev) {
        ImGuiIO& io = ImGui::GetIO();
        auto mods = [&](unsigned state) {
            io.AddKeyEvent(ImGuiMod_Ctrl, (state & ControlMask) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (state & ShiftMask) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (state & Mod1Mask) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (state & Mod4Mask) != 0);
        };
        switch (ev.type) {
            case ConfigureNotify:
                if (ev.xconfigure.width != w || ev.xconfigure.height != h) {
                    w = ev.xconfigure.width;
                    h = ev.xconfigure.height;
                    createXImage();
                }
                break;
            case MotionNotify:
                mods(ev.xmotion.state);
                io.AddMousePosEvent((float)ev.xmotion.x, (float)ev.xmotion.y);
                break;
            case LeaveNotify:
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                break;
            case ButtonPress:
            case ButtonRelease: {
                bool down = ev.type == ButtonPress;
                mods(ev.xbutton.state);
                io.AddMousePosEvent((float)ev.xbutton.x, (float)ev.xbutton.y);
                switch (ev.xbutton.button) {
                    case 1: io.AddMouseButtonEvent(0, down); break;
                    case 2: io.AddMouseButtonEvent(2, down); break;
                    case 3: io.AddMouseButtonEvent(1, down); break;
                    case 4: if (down) io.AddMouseWheelEvent(0, 1); break;
                    case 5: if (down) io.AddMouseWheelEvent(0, -1); break;
                    case 6: if (down) io.AddMouseWheelEvent(1, 0); break;
                    case 7: if (down) io.AddMouseWheelEvent(-1, 0); break;
                    default: break;
                }
                break;
            }
            case KeyPress:
            case KeyRelease: {
                bool down = ev.type == KeyPress;
                // Suppress auto-repeat release/press pairs being treated as releases.
                if (!down && XEventsQueued(dpy, QueuedAfterReading)) {
                    XEvent nev;
                    XPeekEvent(dpy, &nev);
                    if (nev.type == KeyPress && nev.xkey.time == ev.xkey.time && nev.xkey.keycode == ev.xkey.keycode)
                        break;
                }
                char buf[32];
                KeySym ks = 0;
                int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, nullptr);
                KeySym base = XLookupKeysym(&ev.xkey, 0);
                mods(ev.xkey.state);
                ImGuiKey k = mapKey(base);
                if (k == ImGuiKey_None) k = mapKey(ks);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
                if (down && len > 0 && !(ev.xkey.state & ControlMask)) {
                    buf[len] = 0;
                    for (int i = 0; i < len; ++i)
                        if ((unsigned char)buf[i] >= 32 && buf[i] != 127) io.AddInputCharacter((unsigned char)buf[i]);
                }
                break;
            }
            case FocusIn: io.AddFocusEvent(true); break;
            case FocusOut: io.AddFocusEvent(false); break;
            case SelectionRequest: handleSelectionRequest(ev.xselectionrequest); break;
            default: break;
        }
    }
};

X11Window::X11Window() : d(new Impl) {}

X11Window::~X11Window() {
    if (!d->dpy) return;
    if (d->gl) {
        ImGui_ImplOpenGL2_Shutdown();
        glXMakeCurrent(d->dpy, None, nullptr);
        glXDestroyContext(d->dpy, d->glc);
    }
    if (d->ximg) {
        d->ximg->data = nullptr;
        XDestroyImage(d->ximg);
    }
    if (d->gc) XFreeGC(d->dpy, d->gc);
    XDestroyWindow(d->dpy, d->win);
    XCloseDisplay(d->dpy);
    if (d->wakeFd[0] >= 0) { close(d->wakeFd[0]); close(d->wakeFd[1]); }
}

bool X11Window::create(const char* title, int w, int h, bool forceSoftware, std::string& err) {
    XInitThreads();
    d->dpy = XOpenDisplay(nullptr);
    if (!d->dpy) {
        const char* disp = getenv("DISPLAY");
        err = std::string("cannot open X display '") + (disp ? disp : "(DISPLAY not set)") +
              "'. Start an X server (e.g. XQuartz on macOS) and set DISPLAY.";
        return false;
    }
    XSetErrorHandler(xErrorHandler);
    int screen = DefaultScreen(d->dpy);
    Window root = RootWindow(d->dpy, screen);

    XVisualInfo* vi = nullptr;
    if (!forceSoftware) {
        int eb, evb;
        if (glXQueryExtension(d->dpy, &eb, &evb)) {
            int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
            vi = glXChooseVisual(d->dpy, screen, attrs);
        }
    }
    if (vi) {
        d->visual = vi->visual;
        d->depth = vi->depth;
    } else {
        d->visual = DefaultVisual(d->dpy, screen);
        d->depth = DefaultDepth(d->dpy, screen);
        if (d->depth < 24) {
            err = "software renderer needs a 24/32-bit TrueColor visual";
            return false;
        }
        auto shiftOf = [](unsigned long m) { int s = 0; while (m && !(m & 1)) { m >>= 1; ++s; } return s; };
        d->rshift = shiftOf(d->visual->red_mask);
        d->gshift = shiftOf(d->visual->green_mask);
        d->bshift = shiftOf(d->visual->blue_mask);
    }

    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(d->dpy, root, d->visual, AllocNone);
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask | LeaveWindowMask;
    d->w = w;
    d->h = h;
    d->win = XCreateWindow(d->dpy, root, 0, 0, w, h, 0, d->depth, InputOutput, d->visual,
                           CWColormap | CWEventMask | CWBorderPixel | CWBackPixel, &swa);
    d->wmDelete = XInternAtom(d->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d->dpy, d->win, &d->wmDelete, 1);
    d->clipboard = XInternAtom(d->dpy, "CLIPBOARD", False);
    d->utf8 = XInternAtom(d->dpy, "UTF8_STRING", False);
    d->targets = XInternAtom(d->dpy, "TARGETS", False);
    d->wvtProp = XInternAtom(d->dpy, "WVT_CLIP", False);
    XClassHint ch{(char*)"wvt", (char*)"Wvt"};
    XSetClassHint(d->dpy, d->win, &ch);
    setTitle(title);
    XMapWindow(d->dpy, d->win);

    if (vi) {
        g_xerror = 0;
        d->glc = glXCreateContext(d->dpy, vi, nullptr, True);
        if (!d->glc) d->glc = glXCreateContext(d->dpy, vi, nullptr, False);
        XSync(d->dpy, False);
        if (d->glc && !g_xerror && glXMakeCurrent(d->dpy, d->win, d->glc)) {
            d->gl = true;
        } else if (d->glc) {
            glXDestroyContext(d->dpy, d->glc);
            d->glc = nullptr;
        }
        XFree(vi);
    }
    if (!d->gl) {
        if (d->visual != DefaultVisual(d->dpy, screen)) {
            // GLX visual chosen but context failed: recreate with default visual
            XDestroyWindow(d->dpy, d->win);
            return create(title, w, h, true, err);
        }
        d->gc = XCreateGC(d->dpy, d->win, 0, nullptr);
        d->createXImage();
    }

    if (pipe(d->wakeFd) == 0) {
        fcntl(d->wakeFd[0], F_SETFL, O_NONBLOCK);
        fcntl(d->wakeFd[1], F_SETFL, O_NONBLOCK);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "wvt_x11";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_ClipboardUserData = d.get();
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        return ((Impl*)ImGui::GetPlatformIO().Platform_ClipboardUserData)->getClipboard();
    };
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        Impl* im = (Impl*)ImGui::GetPlatformIO().Platform_ClipboardUserData;
        im->clipOut = text ? text : "";
        XSetSelectionOwner(im->dpy, im->clipboard, im->win, CurrentTime);
    };
    if (d->gl) {
        ImGui_ImplOpenGL2_Init();
    } else {
        io.BackendRendererName = "wvt_software";
    }
    rebuildFonts();

    // Wait for the window to be mapped so the first frame has a real size.
    XEvent ev;
    for (int i = 0; i < 200; ++i) {
        if (XCheckTypedWindowEvent(d->dpy, d->win, MapNotify, &ev)) break;
        usleep(5000);
    }
    return true;
}

void X11Window::setTitle(const std::string& title) {
    if (!d->dpy) return;
    XStoreName(d->dpy, d->win, title.c_str());
    Atom netName = XInternAtom(d->dpy, "_NET_WM_NAME", False);
    XChangeProperty(d->dpy, d->win, netName, d->utf8, 8, PropModeReplace, (const unsigned char*)title.data(),
                    (int)title.size());
}

void X11Window::rebuildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (d->gl) {
        ImGui_ImplOpenGL2_DestroyFontsTexture();
        ImGui_ImplOpenGL2_CreateFontsTexture();
        return;
    }
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    d->fontTex.w = w;
    d->fontTex.h = h;
    d->fontTex.px.assign((const uint32_t*)px, (const uint32_t*)px + (size_t)w * h);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)&d->fontTex);
}

bool X11Window::pumpEvents(double timeoutSec) {
    if (!XPending(d->dpy) && timeoutSec > 0) {
        int xfd = ConnectionNumber(d->dpy);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int maxfd = xfd;
        if (d->wakeFd[0] >= 0) {
            FD_SET(d->wakeFd[0], &fds);
            maxfd = std::max(maxfd, d->wakeFd[0]);
        }
        timeval tv{(time_t)timeoutSec, (suseconds_t)((timeoutSec - (time_t)timeoutSec) * 1e6)};
        select(maxfd + 1, &fds, nullptr, nullptr, &tv);
    }
    if (d->wakeFd[0] >= 0) {
        char tmp[64];
        while (read(d->wakeFd[0], tmp, sizeof(tmp)) > 0) {}
    }
    while (XPending(d->dpy)) {
        XEvent ev;
        XNextEvent(d->dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == d->wmDelete) return false;
        d->handle(ev);
    }
    return true;
}

void X11Window::wake() {
    if (d->wakeFd[1] >= 0) {
        char c = 1;
        (void)!write(d->wakeFd[1], &c, 1);
    }
}

void X11Window::newFrame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)d->w, (float)d->h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - d->last).count();
    io.DeltaTime = dt > 0 ? dt : 1.f / 60.f;
    d->last = now;
    if (d->gl) ImGui_ImplOpenGL2_NewFrame();
}

void X11Window::render(ImDrawData* dd, const ImVec4& c) {
    if (d->gl) {
        glViewport(0, 0, d->w, d->h);
        glClearColor(c.x, c.y, c.z, c.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(dd);
        glXSwapBuffers(d->dpy, d->win);
        return;
    }
    uint32_t cc = (uint32_t)(c.x * 255) | ((uint32_t)(c.y * 255) << 8) | ((uint32_t)(c.z * 255) << 16) | 0xff000000u;
    d->sw.clear(cc);
    d->sw.render(dd);
    const uint32_t* src = d->sw.pixels();
    const size_t n = (size_t)d->w * d->h;
    for (size_t i = 0; i < n; ++i) {
        uint32_t p = src[i];
        d->xbuf[i] = ((p & 0xff) << d->rshift) | (((p >> 8) & 0xff) << d->gshift) | (((p >> 16) & 0xff) << d->bshift);
    }
    XPutImage(d->dpy, d->win, d->gc, d->ximg, 0, 0, 0, 0, d->w, d->h);
    XFlush(d->dpy);
}

ImTextureID X11Window::createTexture(int w, int h, const uint32_t* rgba) {
    if (d->gl) {
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return (ImTextureID)(intptr_t)tex;
    }
    auto* t = new SwTexture;
    t->w = w;
    t->h = h;
    t->px.assign(rgba, rgba + (size_t)w * h);
    return (ImTextureID)(intptr_t)t;
}

void X11Window::updateTexture(ImTextureID& id, int w, int h, const uint32_t* rgba) {
    if (!id) {
        id = createTexture(w, h, rgba);
        return;
    }
    if (d->gl) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return;
    }
    auto* t = (SwTexture*)(intptr_t)id;
    t->w = w;
    t->h = h;
    t->px.assign(rgba, rgba + (size_t)w * h);
}

void X11Window::destroyTexture(ImTextureID id) {
    if (!id) return;
    if (d->gl) {
        GLuint tex = (GLuint)(intptr_t)id;
        glDeleteTextures(1, &tex);
    } else {
        delete (SwTexture*)(intptr_t)id;
    }
}

bool X11Window::usingGL() const { return d->gl; }
int X11Window::width() const { return d->w; }
int X11Window::height() const { return d->h; }

bool X11Window::screenshot(const std::string& path) {
    std::vector<uint8_t> rgb((size_t)d->w * d->h * 3);
    if (d->gl) {
        std::vector<uint8_t> px((size_t)d->w * d->h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_FRONT);
        glReadPixels(0, 0, d->w, d->h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < d->h; ++y)
            for (int x = 0; x < d->w; ++x)
                for (int c = 0; c < 3; ++c)
                    rgb[((size_t)y * d->w + x) * 3 + c] = px[((size_t)(d->h - 1 - y) * d->w + x) * 4 + c];
    } else {
        const uint32_t* p = d->sw.pixels();
        for (size_t i = 0; i < (size_t)d->w * d->h; ++i) {
            rgb[i * 3 + 0] = p[i] & 0xff;
            rgb[i * 3 + 1] = (p[i] >> 8) & 0xff;
            rgb[i * 3 + 2] = (p[i] >> 16) & 0xff;
        }
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", d->w, d->h);
    fwrite(rgb.data(), 1, rgb.size(), f);
    fclose(f);
    return true;
}
