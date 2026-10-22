#pragma once
// Minimal software rasterizer for Dear ImGui draw lists. Used when the X server
// has no usable GLX (e.g. ssh -X to a headless box, Xvfb, some VNC servers).
#include <cstdint>
#include <vector>

struct ImDrawData;

struct SwTexture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // RGBA, R in the low byte (ImGui IM_COL32 layout)
};

class SwRenderer {
public:
    void resize(int w, int h);
    void clear(uint32_t rgba);
    void render(ImDrawData* dd);
    const uint32_t* pixels() const { return fb_.data(); }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void fillRect(int x0, int y0, int x1, int y1, uint32_t col, const SwTexture* tex, float u0, float v0, float u1,
                  float v1, float rx0, float ry0, float rx1, float ry1);
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> fb_;
};
