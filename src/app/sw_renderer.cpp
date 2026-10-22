#include "app/sw_renderer.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

void SwRenderer::resize(int w, int h) {
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    fb_.assign((size_t)w_ * h_, 0);
}

void SwRenderer::clear(uint32_t c) { std::fill(fb_.begin(), fb_.end(), c); }

namespace {

inline uint32_t modulate(uint32_t a, uint32_t b) {
    uint32_t r = ((a & 0xff) * (b & 0xff) + 127) / 255;
    uint32_t g = (((a >> 8) & 0xff) * ((b >> 8) & 0xff) + 127) / 255;
    uint32_t bl = (((a >> 16) & 0xff) * ((b >> 16) & 0xff) + 127) / 255;
    uint32_t al = (((a >> 24) & 0xff) * ((b >> 24) & 0xff) + 127) / 255;
    return r | (g << 8) | (bl << 16) | (al << 24);
}

inline void blend(uint32_t& dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 0) return;
    if (a == 255) {
        dst = src;
        return;
    }
    uint32_t ia = 255 - a;
    uint32_t rb = ((src & 0x00ff00ff) * a + (dst & 0x00ff00ff) * ia) >> 8;
    uint32_t g = ((src & 0x0000ff00) * a + (dst & 0x0000ff00) * ia) >> 8;
    dst = (rb & 0x00ff00ff) | (g & 0x0000ff00) | 0xff000000;
}

inline uint32_t sample(const SwTexture* t, float u, float v) {
    if (!t || t->w == 0) return 0xffffffff;
    int x = (int)(u * t->w), y = (int)(v * t->h);
    x = std::clamp(x, 0, t->w - 1);
    y = std::clamp(y, 0, t->h - 1);
    return t->px[(size_t)y * t->w + x];
}

struct Clip {
    int x0, y0, x1, y1;
};

}  // namespace

void SwRenderer::fillRect(int x0, int y0, int x1, int y1, uint32_t col, const SwTexture* tex, float u0, float v0,
                          float u1, float v1, float rx0, float ry0, float rx1, float ry1) {
    // (rx0,ry0)-(rx1,ry1) is the unclipped geometric rect that uv maps onto.
    const bool solid = (u0 == u1 && v0 == v1);
    const uint32_t solidCol = solid ? modulate(col, sample(tex, u0, v0)) : 0;
    const float du = (u1 - u0) / (rx1 - rx0), dv = (v1 - v0) / (ry1 - ry0);
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = &fb_[(size_t)y * w_];
        if (solid) {
            for (int x = x0; x < x1; ++x) blend(row[x], solidCol);
        } else {
            float v = v0 + (y + 0.5f - ry0) * dv;
            for (int x = x0; x < x1; ++x) {
                float u = u0 + (x + 0.5f - rx0) * du;
                blend(row[x], modulate(col, sample(tex, u, v)));
            }
        }
    }
}

void SwRenderer::render(ImDrawData* dd) {
    if (!dd) return;
    const ImVec2 off = dd->DisplayPos;
    for (int li = 0; li < dd->CmdListsCount; ++li) {
        const ImDrawList* dl = dd->CmdLists[li];
        const ImDrawVert* vtx = dl->VtxBuffer.Data;
        const ImDrawIdx* idxAll = dl->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(dl, &cmd);
                continue;
            }
            Clip clip{std::max(0, (int)std::floor(cmd.ClipRect.x - off.x)),
                      std::max(0, (int)std::floor(cmd.ClipRect.y - off.y)),
                      std::min(w_, (int)std::ceil(cmd.ClipRect.z - off.x)),
                      std::min(h_, (int)std::ceil(cmd.ClipRect.w - off.y))};
            if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) continue;
            const SwTexture* tex = (const SwTexture*)(intptr_t)cmd.GetTexID();
            const ImDrawIdx* idx = idxAll + cmd.IdxOffset;
            const ImDrawVert* vb = vtx + cmd.VtxOffset;
            const unsigned n = cmd.ElemCount;
            for (unsigned i = 0; i + 2 < n; i += 3) {
                // Fast path: axis-aligned quad emitted as (0,1,2)(0,2,3) with uniform color.
                if (i + 5 < n && idx[i + 3] == idx[i] && idx[i + 4] == idx[i + 2]) {
                    const ImDrawVert &a = vb[idx[i]], &b = vb[idx[i + 1]], &c = vb[idx[i + 2]], &d = vb[idx[i + 5]];
                    if (a.col == b.col && a.col == c.col && a.col == d.col && a.pos.y == b.pos.y &&
                        b.pos.x == c.pos.x && c.pos.y == d.pos.y && d.pos.x == a.pos.x && a.uv.y == b.uv.y &&
                        b.uv.x == c.uv.x && c.uv.y == d.uv.y && d.uv.x == a.uv.x) {
                        float rx0 = a.pos.x - off.x, ry0 = a.pos.y - off.y, rx1 = c.pos.x - off.x,
                              ry1 = c.pos.y - off.y;
                        float u0 = a.uv.x, v0 = a.uv.y, u1 = c.uv.x, v1 = c.uv.y;
                        if (rx0 > rx1) { std::swap(rx0, rx1); std::swap(u0, u1); }
                        if (ry0 > ry1) { std::swap(ry0, ry1); std::swap(v0, v1); }
                        int x0 = std::max(clip.x0, (int)std::lround(rx0)), x1 = std::min(clip.x1, (int)std::lround(rx1));
                        int y0 = std::max(clip.y0, (int)std::lround(ry0)), y1 = std::min(clip.y1, (int)std::lround(ry1));
                        if (x0 < x1 && y0 < y1 && rx1 > rx0 && ry1 > ry0)
                            fillRect(x0, y0, x1, y1, a.col, tex, u0, v0, u1, v1, rx0, ry0, rx1, ry1);
                        i += 3;
                        continue;
                    }
                }
                const ImDrawVert& A = vb[idx[i]];
                const ImDrawVert& B = vb[idx[i + 1]];
                const ImDrawVert& C = vb[idx[i + 2]];
                float ax = A.pos.x - off.x, ay = A.pos.y - off.y;
                float bx = B.pos.x - off.x, by = B.pos.y - off.y;
                float cx = C.pos.x - off.x, cy = C.pos.y - off.y;
                float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                if (std::fabs(area) < 1e-6f) continue;
                int minx = std::max(clip.x0, (int)std::floor(std::min({ax, bx, cx})));
                int maxx = std::min(clip.x1, (int)std::ceil(std::max({ax, bx, cx})));
                int miny = std::max(clip.y0, (int)std::floor(std::min({ay, by, cy})));
                int maxy = std::min(clip.y1, (int)std::ceil(std::max({ay, by, cy})));
                if (minx >= maxx || miny >= maxy) continue;
                const float inv = 1.f / area;
                const bool flatCol = (A.col == B.col && A.col == C.col);
                const bool flatUv = (A.uv.x == B.uv.x && A.uv.x == C.uv.x && A.uv.y == B.uv.y && A.uv.y == C.uv.y);
                const uint32_t flatTex = flatUv ? sample(tex, A.uv.x, A.uv.y) : 0;
                const uint32_t flatAll = (flatCol && flatUv) ? modulate(A.col, flatTex) : 0;
                auto ch = [](uint32_t c, int s) { return (float)((c >> s) & 0xff); };
                for (int y = miny; y < maxy; ++y) {
                    float py = y + 0.5f;
                    uint32_t* row = &fb_[(size_t)y * w_];
                    for (int x = minx; x < maxx; ++x) {
                        float px = x + 0.5f;
                        float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * inv;
                        float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * inv;
                        float w2 = 1.f - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        uint32_t out;
                        if (flatCol && flatUv) {
                            out = flatAll;
                        } else {
                            uint32_t col = A.col;
                            if (!flatCol) {
                                uint32_t r = (uint32_t)(ch(A.col, 0) * w0 + ch(B.col, 0) * w1 + ch(C.col, 0) * w2 + 0.5f);
                                uint32_t g = (uint32_t)(ch(A.col, 8) * w0 + ch(B.col, 8) * w1 + ch(C.col, 8) * w2 + 0.5f);
                                uint32_t b = (uint32_t)(ch(A.col, 16) * w0 + ch(B.col, 16) * w1 + ch(C.col, 16) * w2 + 0.5f);
                                uint32_t al = (uint32_t)(ch(A.col, 24) * w0 + ch(B.col, 24) * w1 + ch(C.col, 24) * w2 + 0.5f);
                                col = std::min(r, 255u) | (std::min(g, 255u) << 8) | (std::min(b, 255u) << 16) |
                                      (std::min(al, 255u) << 24);
                            }
                            uint32_t t = flatUv ? flatTex
                                                : sample(tex, A.uv.x * w0 + B.uv.x * w1 + C.uv.x * w2,
                                                         A.uv.y * w0 + B.uv.y * w1 + C.uv.y * w2);
                            out = modulate(col, t);
                        }
                        blend(row[x], out);
                    }
                }
            }
        }
    }
}
