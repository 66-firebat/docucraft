#include "pwg.h"

#include <cstring>

namespace hardcopy {
namespace pwg {

void convertRgba8888(const uint8_t* src, size_t srcStride, int width, int rows,
                     bool color, std::vector<uint8_t>* dst) {
    if (!dst) return;
    if (!src || width <= 0 || rows <= 0) { dst->clear(); return; }
    const int unit = color ? 3 : 1;
    dst->resize((size_t)width * (size_t)rows * (size_t)unit);
    for (int y = 0; y < rows; ++y) {
        const uint8_t* in = src + (size_t)y * srcStride;
        uint8_t* out = dst->data() + (size_t)y * (size_t)width * (size_t)unit;
        if (color) {
            for (int x = 0; x < width; ++x) {
                out[3 * x]     = in[4 * x];
                out[3 * x + 1] = in[4 * x + 1];
                out[3 * x + 2] = in[4 * x + 2];
            }
        } else {
            for (int x = 0; x < width; ++x) {
                const unsigned r = in[4 * x], g = in[4 * x + 1], b = in[4 * x + 2];
                out[x] = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
            }
        }
    }
}

namespace {

template <int Unit>
inline bool samePixel(const uint8_t* a, const uint8_t* b) {
    if (Unit == 1) return a[0] == b[0];
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

template <int Unit>
void encodeLine(const uint8_t* line, int width, std::vector<uint8_t>* out) {
    int pos = 0;
    while (pos < width) {
        int run = 1;
        while (pos + run < width && run < 128 &&
               samePixel<Unit>(line + (size_t)(pos + run) * Unit, line + (size_t)pos * Unit))
            ++run;
        if (run >= 2) {
            out->push_back((uint8_t)(run - 1));
            out->insert(out->end(), line + (size_t)pos * Unit, line + (size_t)pos * Unit + Unit);
            pos += run;
            continue;
        }
        const int start = pos;
        int lit = 1;
        ++pos;
        while (pos < width && lit < 128) {
            if (pos + 1 < width &&
                samePixel<Unit>(line + (size_t)pos * Unit, line + (size_t)(pos + 1) * Unit))
                break;
            ++lit;
            ++pos;
        }
        out->push_back((uint8_t)(257 - lit));
        out->insert(out->end(), line + (size_t)start * Unit, line + (size_t)(start + lit) * Unit);
    }
}

template <int Unit>
void encodeRowsT(const uint8_t* data, int width, int rows, std::vector<uint8_t>* out) {
    const size_t rowBytes = (size_t)width * Unit;
    int y = 0;
    while (y < rows) {
        const uint8_t* line = data + (size_t)y * rowBytes;
        int repeat = 1;
        while (repeat < 256 && y + repeat < rows &&
               std::memcmp(line, line + (size_t)repeat * rowBytes, rowBytes) == 0)
            ++repeat;
        out->push_back((uint8_t)(repeat - 1));
        encodeLine<Unit>(line, width, out);
        y += repeat;
    }
}

}

void encodeRows(const uint8_t* data, int width, int rows, int unit,
                std::vector<uint8_t>* out) {
    if (!out || !data || width <= 0 || rows <= 0) return;
    if (unit == 3) encodeRowsT<3>(data, width, rows, out);
    else           encodeRowsT<1>(data, width, rows, out);
}

bool decodeRows(const uint8_t* data, size_t size, int width, int rows, int unit,
                std::vector<uint8_t>* pixels, size_t* consumed) {
    if (!data || !pixels || width <= 0 || rows <= 0 || (unit != 1 && unit != 3)) return false;
    const size_t rowBytes = (size_t)width * (size_t)unit;
    pixels->clear();
    pixels->reserve(rowBytes * (size_t)rows);
    std::vector<uint8_t> line(rowBytes);
    size_t i = 0;
    int done = 0;
    while (done < rows) {
        if (i >= size) return false;
        const int repeat = (int)data[i++] + 1;
        size_t x = 0;
        while (x < (size_t)width) {
            if (i >= size) return false;
            const uint8_t b = data[i++];
            if (b <= 127) {
                const size_t n = (size_t)b + 1;
                if (x + n > (size_t)width || i + (size_t)unit > size) return false;
                for (size_t k = 0; k < n; ++k) std::memcpy(&line[(x + k) * unit], data + i, (size_t)unit);
                i += (size_t)unit;
                x += n;
            } else {
                const size_t n = 257u - b;
                if (x + n > (size_t)width || i + n * unit > size) return false;
                std::memcpy(&line[x * unit], data + i, n * unit);
                i += n * unit;
                x += n;
            }
        }
        for (int k = 0; k < repeat && done < rows; ++k, ++done)
            pixels->insert(pixels->end(), line.begin(), line.end());
    }
    if (consumed) *consumed = i;
    return true;
}

}
}
