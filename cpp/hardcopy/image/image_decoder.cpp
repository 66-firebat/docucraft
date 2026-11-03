#include "image_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "stb_image.h"

#if HARDCOPY_HAS_WEBP
#include <webp/decode.h>
#endif

namespace hardcopy {

namespace {

bool isWebp(const uint8_t* data, size_t len) {
    return len >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
           std::memcmp(data + 8, "WEBP", 4) == 0;
}

#if HARDCOPY_HAS_WEBP
bool decodeWebpOnWhite(const uint8_t* data, size_t len, Bitmap8* out) {
    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA(data, len, &w, &h);
    if (!rgba) return false;
    out->width = w;
    out->height = h;
    const size_t count = (size_t)w * h;
    out->pixels.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = rgba + i * 4;
        const unsigned gray = ((unsigned)p[0] * 77 + (unsigned)p[1] * 150 + (unsigned)p[2] * 29) >> 8;
        const unsigned alpha = p[3];
        out->pixels[i] = (uint8_t)((gray * alpha + 255u * (255u - alpha) + 127u) / 255u);
    }
    WebPFree(rgba);
    return true;
}
#endif

}

bool decodeToGray(const uint8_t* data, size_t len, Bitmap8* out) {
    if (!data || len == 0 || !out) return false;
#if HARDCOPY_HAS_WEBP
    if (isWebp(data, len)) return decodeWebpOnWhite(data, len, out);
#endif

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, (int)len, &w, &h, &channels, 1);
    if (!pixels) return false;

    out->width  = w;
    out->height = h;
    out->pixels.assign(pixels, pixels + (size_t)w * h);
    stbi_image_free(pixels);
    return true;
}

bool decodeToGrayOnWhite(const uint8_t* data, size_t len, Bitmap8* out) {
    if (!data || len == 0 || !out) return false;
#if HARDCOPY_HAS_WEBP
    if (isWebp(data, len)) return decodeWebpOnWhite(data, len, out);
#endif

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, (int)len, &w, &h, &channels, 2);
    if (!pixels) return false;

    out->width = w;
    out->height = h;
    const size_t count = (size_t)w * h;
    out->pixels.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned gray = pixels[i * 2];
        const unsigned alpha = pixels[i * 2 + 1];
        out->pixels[i] = (uint8_t)((gray * alpha + 255u * (255u - alpha) + 127u) / 255u);
    }
    stbi_image_free(pixels);
    return true;
}

bool isWebpImage(const uint8_t* data, size_t len) { return data && isWebp(data, len); }

bool imageInfo(const uint8_t* data, size_t len, int* width, int* height) {
    if (!data || len == 0 || !width || !height) return false;
#if HARDCOPY_HAS_WEBP
    if (isWebp(data, len)) return WebPGetInfo(data, len, width, height) != 0;
#endif
    int w = 0, h = 0, ch = 0;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &ch)) return false;
    *width = w;
    *height = h;
    return true;
}

void scaleToFit(const Bitmap8& src, int targetW, int targetH, Bitmap8* out) {
    out->width  = targetW;
    out->height = targetH;
    out->pixels.resize((size_t)targetW * targetH);
    out->clearWhite();

    if (src.width <= 0 || src.height <= 0) return;

    float scaleX = (float)targetW  / src.width;
    float scaleY = (float)targetH / src.height;
    float scale  = std::min(scaleX, scaleY);

    int dstW = std::max(1, (int)std::round(src.width  * scale));
    int dstH = std::max(1, (int)std::round(src.height * scale));
    int offX = (targetW - dstW) / 2;
    int offY = (targetH - dstH) / 2;

    for (int y = 0; y < dstH; ++y) {
        float srcY = (y + 0.5f) * src.height / dstH - 0.5f;
        int   sy0  = std::max(0, (int)std::floor(srcY));
        int   sy1  = std::min(src.height - 1, sy0 + 1);
        float fy   = srcY - sy0;

        for (int x = 0; x < dstW; ++x) {
            float srcX = (x + 0.5f) * src.width / dstW - 0.5f;
            int   sx0  = std::max(0, (int)std::floor(srcX));
            int   sx1  = std::min(src.width - 1, sx0 + 1);
            float fx   = srcX - sx0;

            float p00 = src.pixels[sy0 * src.width + sx0];
            float p10 = src.pixels[sy0 * src.width + sx1];
            float p01 = src.pixels[sy1 * src.width + sx0];
            float p11 = src.pixels[sy1 * src.width + sx1];

            float val = p00 * (1 - fx) * (1 - fy)
                      + p10 * fx       * (1 - fy)
                      + p01 * (1 - fx) * fy
                      + p11 * fx       * fy;

            int dstIdx = (offY + y) * targetW + (offX + x);
            out->pixels[dstIdx] = (uint8_t)std::clamp((int)std::round(val), 0, 255);
        }
    }
}

}
