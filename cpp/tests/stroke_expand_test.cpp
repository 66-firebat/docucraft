#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "render/page_raster.h"

namespace {

void referenceStrokeExpand(hardcopy::Bitmap8& bm, double strokeExpand, bool lightInk) {
    if (strokeExpand <= 0.0) return;
    const double amount = std::clamp(strokeExpand / 0.1, 0.0, 1.0);
    const int W = bm.width, H = bm.height;
    std::vector<uint8_t> src = bm.pixels;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int ext = src[(size_t)y * W + x];
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= H || xx < 0 || xx >= W) continue;
                    const int v = src[(size_t)yy * W + xx];
                    ext = lightInk ? std::max(ext, v) : std::min(ext, v);
                }
            const int s = src[(size_t)y * W + x];
            const int blended = (int)std::lround(s + (ext - s) * amount);
            bm.pixels[(size_t)y * W + x] = (uint8_t)std::clamp(blended, 0, 255);
        }
}

}

int main() {
    std::mt19937 rng(20261023);
    const int sizes[][2] = {{1, 1}, {1, 5}, {5, 1}, {2, 2}, {3, 3}, {7, 3}, {8, 8}, {9, 9},
                            {16, 2}, {17, 5}, {23, 7}, {64, 64}, {301, 77}, {1727, 3}};
    int cases = 0, bad = 0;
    for (const auto& sz : sizes)
        for (int light = 0; light < 2; ++light)
            for (double s : {0.01, 0.02, 0.05, 0.07, 0.1})
                for (int mode = 0; mode < 3; ++mode) {
                    hardcopy::Bitmap8 a;
                    a.width = sz[0];
                    a.height = sz[1];
                    a.pixels.resize((size_t)sz[0] * sz[1]);
                    const uint8_t paper = light ? 0 : 255;
                    for (auto& p : a.pixels) {
                        if (mode == 0) p = (uint8_t)rng();
                        else if (mode == 1) p = (rng() % 7 == 0) ? (uint8_t)rng() : paper;
                        else p = (rng() % 50 == 0) ? (uint8_t)rng() : paper;
                    }
                    hardcopy::Bitmap8 b = a;
                    referenceStrokeExpand(a, s, light != 0);
                    hardcopy::applyStrokeExpand(b, s, light != 0);
                    ++cases;
                    if (a.pixels != b.pixels) {
                        ++bad;
                        std::printf("MISMATCH %dx%d light=%d stroke=%g mode=%d\n",
                                    sz[0], sz[1], light, s, mode);
                    }
                }
    std::printf("stroke expand: %d cases, %d mismatches\n", cases, bad);
    if (bad) return 1;
    std::printf("OK\n");
    return 0;
}
