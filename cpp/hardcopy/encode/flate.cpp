#include "flate.h"

#include <cstring>
#include <limits>

#if HARDCOPY_HAS_LIBDEFLATE
#include <libdeflate.h>
#elif HARDCOPY_HAS_PDFIN
extern "C" {
#include <mupdf/fitz.h>
}
#endif

namespace hardcopy {

namespace {

void packGray4(const Bitmap8& src, std::vector<uint8_t>* packed) {
    const int W = src.width, H = src.height;
    const size_t rowBytes = ((size_t)W + 1) / 2;
    packed->assign(rowBytes * (size_t)H, 0);
    static uint8_t level[256];
    static bool levelReady = false;
    if (!levelReady) {
        for (int v = 0; v < 256; ++v) level[v] = (uint8_t)((v * 15 + 127) / 255);
        levelReady = true;
    }
    for (int y = 0; y < H; ++y) {
        const uint8_t* in = src.pixels.data() + (size_t)y * W;
        uint8_t* out = packed->data() + (size_t)y * rowBytes;
        int x = 0;
        for (; x + 1 < W; x += 2)
            out[x >> 1] = (uint8_t)((level[in[x]] << 4) | level[in[x + 1]]);
        if (x < W) out[x >> 1] = (uint8_t)(level[in[x]] << 4);
    }
}

#if HARDCOPY_HAS_LIBDEFLATE
constexpr int kLibdeflateLevel = 1;

libdeflate_compressor* flateCompressor() {
    struct Holder {
        libdeflate_compressor* c = libdeflate_alloc_compressor(kLibdeflateLevel);
        ~Holder() { libdeflate_free_compressor(c); }
    };
    thread_local Holder holder;
    return holder.c;
}
#elif HARDCOPY_HAS_PDFIN
fz_context* flateContext() {
    struct Holder {
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        ~Holder() { if (ctx) fz_drop_context(ctx); }
    };
    thread_local Holder holder;
    return holder.ctx;
}
#endif

}

const char* flateBackendName() {
#if HARDCOPY_HAS_LIBDEFLATE
    return "libdeflate";
#elif HARDCOPY_HAS_PDFIN
    return "mupdf-zlib";
#else
    return "none";
#endif
}

bool encodeFlateGray4(const Bitmap8& src, std::vector<uint8_t>* out) {
    if (!out || src.pixels.empty() || src.width <= 0 || src.height <= 0) return false;
    const size_t width = (size_t)src.width, height = (size_t)src.height;
    if (width > std::numeric_limits<size_t>::max() / height ||
        src.pixels.size() < width * height) return false;
    out->clear();
#if HARDCOPY_HAS_LIBDEFLATE
    libdeflate_compressor* c = flateCompressor();
    if (!c) return false;
    std::vector<uint8_t> packed;
    packGray4(src, &packed);
    const size_t bound = libdeflate_zlib_compress_bound(c, packed.size());
    out->resize(bound);
    const size_t used = libdeflate_zlib_compress(c, packed.data(), packed.size(),
                                                 out->data(), bound);
    if (used == 0) { out->clear(); return false; }
    out->resize(used);
    return true;
#elif HARDCOPY_HAS_PDFIN
    fz_context* ctx = flateContext();
    if (!ctx) return false;
    std::vector<uint8_t> packed;
    packGray4(src, &packed);
    bool ok = false;
    fz_try(ctx) {
        size_t bound = fz_deflate_bound(ctx, packed.size());
        out->resize(bound);
        size_t used = bound;
        fz_deflate(ctx, out->data(), &used, packed.data(), packed.size(),
                   FZ_DEFLATE_BEST_SPEED);
        out->resize(used);
        ok = used > 0;
    }
    fz_catch(ctx) {
        out->clear();
        ok = false;
    }
    return ok;
#else
    return false;
#endif
}

}
