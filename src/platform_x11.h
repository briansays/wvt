#pragma once
// Native X11 window + Dear ImGui platform glue. Renders with GLX/OpenGL when
// available, otherwise falls back to a software rasterizer + XPutImage.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

class X11Window {
public:
    X11Window();
    ~X11Window();

    bool create(const char* title, int w, int h, bool forceSoftware, std::string& err);
    void setTitle(const std::string& title);
    // Blocks up to timeout seconds for X events or a wake(); then feeds all
    // pending events to ImGui. Returns false when the window was closed.
    bool pumpEvents(double timeoutSec);
    void newFrame();
    void render(ImDrawData* dd, const ImVec4& clear);
    // Thread-safe: makes pumpEvents() return promptly (e.g. a worker finished).
    void wake();

    ImTextureID createTexture(int w, int h, const uint32_t* rgba);
    void updateTexture(ImTextureID& id, int w, int h, const uint32_t* rgba);
    void destroyTexture(ImTextureID id);
    void rebuildFonts();  // after changing the font atlas

    bool usingGL() const;
    bool screenshot(const std::string& path);  // writes a binary PPM
    // The last rendered frame as RGB, top row first (3 bytes per pixel).
    void readFrame(int& w, int& h, std::vector<uint8_t>& rgb);
    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
