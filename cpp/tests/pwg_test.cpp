#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "print/pwg.h"

namespace {

bool unitsEqual(const std::vector<uint8_t>& line, int a, int b, int unit) {
    for (int i = 0; i < unit; ++i)
        if (line[(size_t)a * unit + i] != line[(size_t)b * unit + i]) return false;
    return true;
}

void referenceEncode(const std::vector<uint8_t>& data, int w, int h, int unit, std::vector<uint8_t>* out) {
    const size_t rowBytes = (size_t)w * unit;
    int y = 0;
    while (y < h) {
        std::vector<uint8_t> current(data.begin() + (size_t)y * rowBytes, data.begin() + (size_t)(y + 1) * rowBytes);
        int repeatCount = 1;
        while (repeatCount < 256 && y + repeatCount < h) {
            std::vector<uint8_t> next(data.begin() + (size_t)(y + repeatCount) * rowBytes,
                                      data.begin() + (size_t)(y + repeatCount + 1) * rowBytes);
            if (next != current) break;
            repeatCount++;
        }
        out->push_back((uint8_t)(repeatCount - 1));
        int pos = 0;
        while (pos < w) {
            int runLen = 1;
            while (pos + runLen < w && runLen < 128 && unitsEqual(current, pos + runLen, pos, unit)) runLen++;
            if (runLen >= 2) {
                out->push_back((uint8_t)(runLen - 1));
                for (int i = 0; i < unit; ++i) out->push_back(current[(size_t)pos * unit + i]);
                pos += runLen;
            } else {
                const int start = pos;
                int litLen = 1;
                pos++;
                while (pos < w && litLen < 128) {
                    if (pos + 1 < w && unitsEqual(current, pos, pos + 1, unit)) break;
                    litLen++;
                    pos++;
                }
                out->push_back((uint8_t)(257 - litLen));
                for (int i = 0; i < litLen * unit; ++i) out->push_back(current[(size_t)start * unit + i]);
            }
        }
        y += repeatCount;
    }
}

}

int main() {
    std::mt19937 rng(7);
    int cases = 0, bad = 0;
    const int widths[] = {1, 2, 3, 7, 8, 127, 128, 129, 255, 256, 257, 300, 1000, 4960};
    const int heights[] = {1, 2, 3, 17, 255, 256, 257, 300};
    for (int unit : {1, 3})
        for (int w : widths)
            for (int h : heights) {
                if ((long)w * h > 2'000'000L) continue;
                for (int mode = 0; mode < 4; ++mode) {
                    std::vector<uint8_t> px((size_t)w * h * unit);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w * unit; ++x) {
                            uint8_t v;
                            switch (mode) {
                                case 0:  v = (uint8_t)rng(); break;
                                case 1:  v = (rng() % 23 == 0) ? (uint8_t)rng() : 255; break;
                                case 2:  v = (y % 7 < 5) ? 255 : (uint8_t)((x / 9) & 0xFF); break;
                                default: v = (uint8_t)(((x / (1 + (int)(rng() % 3))) ^ y) & 0xFF);
                            }
                            px[(size_t)y * w * unit + x] = v;
                        }
                    std::vector<uint8_t> ref, fast, back;
                    referenceEncode(px, w, h, unit, &ref);
                    hardcopy::pwg::encodeRows(px.data(), w, h, unit, &fast);
                    size_t consumed = 0;
                    const bool decoded = hardcopy::pwg::decodeRows(fast.data(), fast.size(), w, h, unit, &back, &consumed);
                    ++cases;
                    if (ref != fast || !decoded || back != px || consumed != fast.size()) {
                        ++bad;
                        std::printf("MISMATCH unit=%d %dx%d mode=%d ref=%zu fast=%zu decoded=%d roundtrip=%d consumed=%zu\n",
                                    unit, w, h, mode, ref.size(), fast.size(), (int)decoded, (int)(back == px), consumed);
                    }
                }
            }
    {
        const int w = 1234, h = 3;
        const size_t stride = (size_t)w * 4 + 16;
        std::vector<uint8_t> rgba(stride * h);
        for (auto& v : rgba) v = (uint8_t)rng();
        std::vector<uint8_t> gray, rgb;
        hardcopy::pwg::convertRgba8888(rgba.data(), stride, w, h, false, &gray);
        hardcopy::pwg::convertRgba8888(rgba.data(), stride, w, h, true, &rgb);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = &rgba[(size_t)y * stride + (size_t)x * 4];
                const uint8_t g = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
                ++cases;
                if (gray[(size_t)y * w + x] != g || rgb[((size_t)y * w + x) * 3] != p[0] ||
                    rgb[((size_t)y * w + x) * 3 + 1] != p[1] || rgb[((size_t)y * w + x) * 3 + 2] != p[2]) {
                    ++bad;
                }
            }
    }
    std::printf("pwg: %d cases, %d mismatches\n", cases, bad);
    if (bad) return 1;
    std::printf("OK\n");
    return 0;
}
