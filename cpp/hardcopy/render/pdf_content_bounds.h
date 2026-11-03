#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hardcopy {
struct PdfContentBounds { int left, top, right, bottom; };

inline PdfContentBounds detectPdfContentBounds(const uint8_t* pixels, int w, int h,
                                                int top, int bottom) {
    if (!pixels || w <= 0 || h <= 0) return {0, 0, 0, 0};
    top = std::clamp(top, 0, h - 1);
    bottom = std::clamp(bottom, top + 1, h);
    const PdfContentBounds body{0, top, w, bottom};
    int histogram[256]{};
    int samples = 0;
    for (int y = top; y < bottom; ++y) for (int x = 0; x < w; ++x) {
        if (x < 2 || x >= w - 2 || y < top + 2 || y >= bottom - 2) {
            ++histogram[pixels[(size_t)y * w + x]];
            ++samples;
        }
    }
    int paper = 255, cumulative = 0;
    for (int i = 0; i < 256; ++i) {
        cumulative += histogram[i];
        if (cumulative >= (samples * 9 + 9) / 10) { paper = i; break; }
    }
    if (paper < 160) return body;
    const int threshold = paper - 12;
    std::vector<uint8_t> seen((size_t)w * h, 0);
    std::vector<size_t> queue;
    PdfContentBounds result{w, bottom, 0, top};
    for (int y = top; y < bottom; ++y) for (int x = 0; x < w; ++x) {
        const size_t start = (size_t)y * w + x;
        if (seen[start] || pixels[start] >= threshold) continue;
        queue.clear(); queue.push_back(start); seen[start] = 1;
        PdfContentBounds component{x, y, x + 1, y + 1};
        for (size_t n = 0; n < queue.size(); ++n) {
            const int cy = (int)(queue[n] / w), cx = (int)(queue[n] % w);
            component.left = std::min(component.left, cx);
            component.right = std::max(component.right, cx + 1);
            component.top = std::min(component.top, cy);
            component.bottom = std::max(component.bottom, cy + 1);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || nx >= w || ny < top || ny >= bottom) continue;
                const size_t next = (size_t)ny * w + nx;
                if (!seen[next] && pixels[next] < threshold) {
                    seen[next] = 1; queue.push_back(next);
                }
            }
        }
        if (queue.size() < 3) continue;
        result.left = std::min(result.left, component.left);
        result.right = std::max(result.right, component.right);
        result.top = std::min(result.top, component.top);
        result.bottom = std::max(result.bottom, component.bottom);
    }
    if (result.right <= result.left || result.bottom <= result.top) return body;
    return {std::max(0, result.left - 1), std::max(top, result.top - 1),
            std::min(w, result.right + 1), std::min(bottom, result.bottom + 1)};
}
}
