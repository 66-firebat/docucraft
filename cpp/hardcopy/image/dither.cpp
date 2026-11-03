#include "dither.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hardcopy {

namespace {

static const int kBayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

void buildPalette(int levels, std::vector<uint8_t>& pal) {
    pal.resize(levels);
    for (int i = 0; i < levels; ++i)
        pal[i] = (uint8_t)std::round(255.0 * i / (levels - 1));
}

inline uint8_t nearestLevel(int val, const std::vector<uint8_t>& pal) {
    int n = (int)pal.size();
    int idx = (val * (n - 1) + 127) / 255;
    idx = std::clamp(idx, 0, n - 1);
    return pal[idx];
}

}

void ditherBayer(Bitmap8* bm, int levels) {
    if (!bm || levels < 2) return;

    std::vector<uint8_t> pal;
    buildPalette(levels, pal);

    const int w = bm->width;
    const int h = bm->height;
    const double step = 255.0 / (levels - 1);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            double old_val = bm->pixels[idx];

            double threshold = (kBayer8[y & 7][x & 7] / 64.0 - 0.5) * step;
            double biased = old_val + threshold;

            int level = (int)std::round(biased / step);
            level = std::clamp(level, 0, levels - 1);
            bm->pixels[idx] = pal[level];
        }
    }
}

void ditherFloydSteinberg(Bitmap8* bm, int levels) {
    if (!bm || levels < 2) return;

    std::vector<uint8_t> pal;
    buildPalette(levels, pal);

    const int w = bm->width;
    const int h = bm->height;

    std::vector<int16_t> buf(bm->pixels.begin(), bm->pixels.end());

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            int old_val = std::clamp((int)buf[idx], 0, 255);
            uint8_t new_val = nearestLevel(old_val, pal);
            buf[idx] = new_val;
            int err = old_val - (int)new_val;

            if (x + 1 < w)
                buf[idx + 1]     += err * 7 / 16;
            if (y + 1 < h) {
                if (x > 0)
                    buf[(y + 1) * w + x - 1] += err * 3 / 16;
                buf[(y + 1) * w + x]          += err * 5 / 16;
                if (x + 1 < w)
                    buf[(y + 1) * w + x + 1] += err * 1 / 16;
            }
        }
    }

    for (int i = 0; i < w * h; ++i)
        bm->pixels[i] = (uint8_t)std::clamp((int)buf[i], 0, 255);
}

}
