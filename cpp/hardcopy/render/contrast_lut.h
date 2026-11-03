#ifndef HARDCOPY_RENDER_CONTRAST_LUT_H_
#define HARDCOPY_RENDER_CONTRAST_LUT_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hardcopy {

namespace night_invert_detail {

struct DarkAcc {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1, n = 0;
};

inline int ufFind(std::vector<int>& parent, int i) {
    int r = i;
    while (parent[r] != r) r = parent[r];
    while (parent[i] != r) {
        const int next = parent[i];
        parent[i] = r;
        i = next;
    }
    return r;
}

inline void ufUnion(std::vector<int>& parent, int a, int b) {
    a = ufFind(parent, a);
    b = ufFind(parent, b);
    if (a != b) parent[b] = a;
}

inline void markKeepDarkPlates(const uint8_t* px, int w, int h,
                               std::vector<uint8_t>* keep) {
    const size_t n = (size_t)w * (size_t)h;
    keep->assign(n, 0);
    if (w <= 0 || h <= 0 || n == 0) return;

    constexpr uint8_t kDark = 96;
    constexpr int kMinArea = 32 * 32;
    constexpr int kMinSide = 28;
    constexpr float kMinCoverage = 0.50f;

    std::vector<int> parent(n, -1);
    auto at = [w](int x, int y) { return y * w + x; };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = at(x, y);
            if (px[i] > kDark) continue;
            parent[i] = i;
            if (x > 0 && parent[i - 1] >= 0) ufUnion(parent, i, i - 1);
            if (y > 0 && parent[i - w] >= 0) ufUnion(parent, i, i - w);
        }
    }

    std::unordered_map<int, DarkAcc> acc;
    acc.reserve(256);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = at(x, y);
            if (parent[i] < 0) continue;
            DarkAcc& a = acc[ufFind(parent, i)];
            if (a.n == 0) {
                a.x0 = a.x1 = x;
                a.y0 = a.y1 = y;
            } else {
                a.x0 = std::min(a.x0, x);
                a.y0 = std::min(a.y0, y);
                a.x1 = std::max(a.x1, x);
                a.y1 = std::max(a.y1, y);
            }
            a.n += 1;
        }
    }

    for (const auto& kv : acc) {
        const DarkAcc& a = kv.second;
        const int bw = a.x1 - a.x0 + 1;
        const int bh = a.y1 - a.y0 + 1;
        if (a.n < kMinArea || std::min(bw, bh) < kMinSide) continue;
        if ((float)a.n / (float)(bw * bh) < kMinCoverage) continue;
        for (int y = a.y0; y <= a.y1; ++y)
            std::fill(keep->begin() + (size_t)at(a.x0, y),
                      keep->begin() + (size_t)at(a.x1, y) + 1, 1);
    }
}

inline void snapThinHorizontalInk(uint8_t* px, int w, int h,
                                  const std::vector<uint8_t>& keep) {
    if (w <= 0 || h <= 0) return;
    auto at = [w](int x, int y) { return y * w + x; };
    auto isWhite = [&](int x, int y) -> bool {
        if (x < 0 || x >= w || y < 0 || y >= h) return true;
        const int i = at(x, y);
        return keep[i] || px[i] > 236;
    };

    for (int y = 0; y < h; ++y) {
        int x = 0;
        while (x < w) {
            const int i = at(x, y);
            if (keep[i] || px[i] > 236) {
                ++x;
                continue;
            }
            const int x0 = x;
            while (x < w && !keep[at(x, y)] && px[at(x, y)] <= 236) ++x;
            const int x1 = x;
            if (x1 - x0 < 8) continue;
            int extra = 0;
            for (int xx = x0; xx < x1; ++xx) {
                for (int dy = -2; dy <= 2; ++dy) {
                    if (dy == 0) continue;
                    if (!isWhite(xx, y + dy)) ++extra;
                }
            }
            if ((float)extra / (float)(x1 - x0) > 2.5f) continue;
            for (int xx = x0; xx < x1; ++xx)
                px[at(xx, y)] = 0;
        }
    }
}

}

inline void buildContrastLut(double setting, uint8_t (&lut)[256]) {
    const double level = std::clamp(setting, 0.0, 1.0) * 20.0;
    const double gamma = 1.0 + std::min(level, 2.0) * 0.5;
    const double extra = std::max(0.0, level - 2.0) / 18.0;

    for (int i = 0; i < 256; ++i) {
        double v = std::pow(i / 255.0, gamma);
        if (extra > 0.0) {
            const double gain = 1.0 + extra * 2.0;
            const double mid = 0.5 - extra * 0.1;
            const double s = 1.0 / (1.0 + std::exp(-gain * (v - mid)));
            const double s0 = 1.0 / (1.0 + std::exp(-gain * (0.0 - mid)));
            const double s1 = 1.0 / (1.0 + std::exp(-gain * (1.0 - mid)));
            v = (s - s0) / (s1 - s0);
        }
        lut[i] = (uint8_t)std::clamp((int)(v * 255.0 + 0.5), 0, 255);
    }
}

struct ImageRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline void applyContrastWithImageMask(uint8_t* pixels, int width, int height,
                                       const uint8_t (&textLut)[256],
                                       const uint8_t (&imageLut)[256],
                                       const std::vector<ImageRect>& images,
                                       int grow = 2) {
    if (!pixels || width <= 0 || height <= 0) return;
    if (images.empty()) {
        const size_t n = (size_t)width * (size_t)height;
        for (size_t i = 0; i < n; ++i) pixels[i] = textLut[pixels[i]];
        return;
    }
    std::vector<ImageRect> rects;
    rects.reserve(images.size());
    for (const ImageRect& r : images) {
        ImageRect c;
        c.x0 = std::clamp(r.x0 - grow, 0, width);
        c.y0 = std::clamp(r.y0 - grow, 0, height);
        c.x1 = std::clamp(r.x1 + grow, 0, width);
        c.y1 = std::clamp(r.y1 + grow, 0, height);
        if (c.x1 > c.x0 && c.y1 > c.y0) rects.push_back(c);
    }
    if (rects.empty()) {
        const size_t n = (size_t)width * (size_t)height;
        for (size_t i = 0; i < n; ++i) pixels[i] = textLut[pixels[i]];
        return;
    }
    std::vector<uint8_t> isImage((size_t)width, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = pixels + (size_t)y * width;
        bool anySpan = false;
        for (const ImageRect& r : rects) {
            if (y < r.y0 || y >= r.y1) continue;
            anySpan = true;
            std::fill(isImage.begin() + r.x0, isImage.begin() + r.x1, uint8_t(1));
        }
        if (!anySpan) {
            for (int x = 0; x < width; ++x) row[x] = textLut[row[x]];
            continue;
        }
        for (int x = 0; x < width; ++x)
            row[x] = isImage[x] ? imageLut[row[x]] : textLut[row[x]];
        for (const ImageRect& r : rects) {
            if (y < r.y0 || y >= r.y1) continue;
            std::fill(isImage.begin() + r.x0, isImage.begin() + r.x1, uint8_t(0));
        }
    }
}

inline void applyNightInvert(uint8_t* pixels, int width, int height,
                             bool enabled) {
    if (!enabled || !pixels || width <= 0 || height <= 0) return;
    const size_t n = (size_t)width * (size_t)height;
    std::vector<uint8_t> keep;
    night_invert_detail::markKeepDarkPlates(pixels, width, height, &keep);
    night_invert_detail::snapThinHorizontalInk(pixels, width, height, keep);
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i])
            pixels[i] = static_cast<uint8_t>(255u - pixels[i]);
    }
}

inline void applyNightInvert(uint8_t* pixels, size_t count, bool enabled) {
    if (!enabled || !pixels || count == 0) return;
    applyNightInvert(pixels, (int)count, 1, enabled);
}

}

#endif
