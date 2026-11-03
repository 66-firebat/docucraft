#ifndef HARDCOPY_RENDER_RULING_PATTERN_H_
#define HARDCOPY_RENDER_RULING_PATTERN_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hardcopy {

inline void paintRulingPattern(uint8_t* pixels, int width, int height,
                              int left, int right, int y, int dot, int gap,
                              uint8_t gray, bool roundDots, bool dashed) {
    if (!pixels || width <= 0 || height <= 0 || dot <= 0 || gap < 0) return;
    left = std::max(0, left);
    right = std::min(width, right);
    const int markW = dashed ? dot * 3 + 1 : dot;
    const int markH = dashed ? std::max(1, (int)std::lround(dot * 0.55f)) : dot;
    if (right - left < markW || y < 0 || y + markH > height) return;
    const int count = (right - left + gap) / (markW + gap);
    const int used = count * markW + (count - 1) * gap;
    const int startX = left + (right - left - used) / 2;
    const float radius = markH * 0.5f;
    const float cy = (markH - 1) * 0.5f;
    for (int i = 0; i < count; ++i) {
        const int x = startX + i * (markW + gap);
        for (int dy = 0; dy < markH; ++dy) {
            for (int dx = 0; dx < markW; ++dx) {
                if (roundDots) {
                    const float cx = std::clamp((float)dx, cy, markW - 1 - cy);
                    const float ox = dx - cx, oy = dy - cy;
                    if (ox * ox + oy * oy > radius * radius) continue;
                }
                auto& pixel = pixels[(std::size_t)(y + dy) * width + x + dx];
                pixel = std::min(pixel, gray);
            }
        }
    }
}

}
#endif
