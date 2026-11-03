#include "pdf_bitmap_processor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hardcopy {
namespace {

void trimWhiteBorder(Bitmap8* bm) {
    if (!bm || bm->width < 4 || bm->height < 4) return;
    const int w = bm->width, h = bm->height;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        if (bm->pixels[(size_t)y * w + x] < 245) {
            x0 = std::min(x0, x); y0 = std::min(y0, y);
            x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
    }
    if (x1 < x0 || y1 < y0) return;
    if (x0 < 4 && y0 < 4 && x1 >= w - 4 && y1 >= h - 4) return;
    const int pad = std::max(2, std::min(w, h) / 400);
    x0 = std::max(0, x0 - pad); y0 = std::max(0, y0 - pad);
    x1 = std::min(w - 1, x1 + pad); y1 = std::min(h - 1, y1 + pad);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<uint8_t> src = bm->pixels;
    std::fill(bm->pixels.begin(), bm->pixels.end(), uint8_t(255));
    const double scale = std::min((double)w / cw, (double)h / ch);
    const int dw = std::max(1, (int)std::lround(cw * scale));
    const int dh = std::max(1, (int)std::lround(ch * scale));
    const int ox = (w - dw) / 2, oy = (h - dh) / 2;
    for (int y = 0; y < dh; ++y) {
        const int sy = std::clamp((int)std::floor(y / scale), 0, ch - 1);
        for (int x = 0; x < dw; ++x) {
            const int sx = std::clamp((int)std::floor(x / scale), 0, cw - 1);
            bm->pixels[(size_t)(oy + y) * w + (ox + x)] =
                src[(size_t)(y0 + sy) * w + (x0 + sx)];
        }
    }
}

}

void buildFillToneLut(double fillTone, uint8_t (&lut)[256]) {
    fillTone = std::clamp(fillTone, -1.0, 1.0);
    const double gamma = 1.0 + 2.0 * std::max(0.0, fillTone);
    const double keep = 1.0 + std::min(0.0, fillTone);
    for (int i = 0; i < 256; ++i) {
        const double v = 1.0 - (1.0 - std::pow(i / 255.0, gamma)) * keep;
        lut[i] = (uint8_t)std::clamp((int)std::lround(v * 255.0), 0, 255);
    }
}

namespace {

void buildToneLut(double gamma, double contrast, uint8_t (&lut)[256]) {
    gamma = std::clamp(gamma, 0.25, 4.0);
    contrast = std::clamp(contrast, 0.25, 4.0);
    for (int i = 0; i < 256; ++i) {
        const double g = std::pow(i / 255.0, gamma);
        const double v = contrast >= 1.0 ? (g - 0.5) * contrast + 0.5
                                         : 1.0 - (1.0 - g) * contrast;
        lut[i] = (uint8_t)std::clamp((int)std::lround(v * 255.0), 0, 255);
    }
}

template <class Emit>
void scanWatermark(Bitmap8* bm, int threshold, Emit&& emit) {
    threshold = std::clamp(threshold, 128, 254);
    const int w = bm->width, h = bm->height;
    std::vector<uint8_t> rows((size_t)w * 3);
    for (int y = 0; y < std::min(2, h); ++y)
        std::copy_n(bm->pixels.data() + (size_t)y*w, w, rows.data() + (y%3)*w);
    for (int y = 0; y < h; ++y) {
        if (y+1 < h)
            std::copy_n(bm->pixels.data() + (size_t)(y+1)*w, w, rows.data() + ((y+1)%3)*w);
        for (int x = 0; x < w; ++x) {
            int v = rows[(y%3)*w+x];
            if (v >= threshold && y > 0 && y+1 < h && x > 0 && x+1 < w) {
                int lo = 255, hi = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    int n = rows[((y+dy)%3)*w+x+dx];
                    lo = std::min(lo, n); hi = std::max(hi, n);
                }
                if (hi-lo <= 18) v = 255;
            }
            emit((size_t)y*w+x, v);
        }
    }
}

void applyTone(Bitmap8* bm, double gamma, double contrast, int threshold) {
    gamma = std::clamp(gamma, 0.25, 4.0);
    contrast = std::clamp(contrast, 0.25, 4.0);
    if (threshold <= 0 && gamma == 1.0 && contrast == 1.0) return;
    uint8_t lut[256];
    buildToneLut(gamma, contrast, lut);
    if (threshold <= 0) {
        for (auto& p : bm->pixels) p = lut[p];
        return;
    }
    uint8_t* px = bm->pixels.data();
    scanWatermark(bm, threshold, [&](size_t i, int v) { px[i] = lut[v]; });
}

void applyToneOverBase(Bitmap8* bm, const PdfBaseLayer& base,
                       const PdfBitmapOptions& o) {
    const int w = bm->width, h = bm->height;
    const size_t n = (size_t)w * h;
    uint8_t textLut[256];
    buildToneLut(o.gamma, o.contrast, textLut);
    uint8_t imageLut[256];
    buildContrastLut(o.imageContrast, imageLut);
    uint8_t fillLut[256];
    buildFillToneLut(o.fillTone, fillLut);
    const uint8_t* b = base.pixels->pixels.data();
    std::vector<uint8_t> plain((size_t)h);
    {
        const std::vector<uint8_t> white((size_t)w, 255);
        for (int y = 0; y < h; ++y)
            plain[y] = std::memcmp(b + (size_t)y * w, white.data(), (size_t)w) == 0;
    }

    uint32_t recip[256];
    recip[0] = 0;
    for (int v = 1; v < 256; ++v) recip[v] = ((255u << 16) + v / 2) / v;
    Bitmap8 ink;
    ink.width = w; ink.height = h;
    ink.pixels.resize(n);
    uint8_t* c = bm->pixels.data();
    uint8_t* r = ink.pixels.data();
    for (int y = 0; y < h; ++y) {
        const size_t row = (size_t)y * w;
        if (plain[y]) { std::memcpy(r + row, c + row, (size_t)w); continue; }
        for (int x = 0; x < w; ++x) {
            const int cv = c[row + x], bv = b[row + x];
            r[row + x] = (cv > bv || bv == 0) ? 255
                : (uint8_t)std::min<uint32_t>((cv * recip[bv] + 32768u) >> 16, 255);
        }
    }
    if (o.watermarkThreshold > 0)
        scanWatermark(&ink, o.watermarkThreshold, [&](size_t i, int v) { r[i] = textLut[v]; });
    else
        for (size_t i = 0; i < n; ++i) r[i] = textLut[r[i]];
    if (o.strokeExpand > 0.0) applyStrokeExpand(ink, o.strokeExpand, false);
    std::vector<ImageRect> pictures;
    for (const ImageRect& p : base.images) {
        ImageRect g;
        g.x0 = std::clamp(p.x0 - 2, 0, w); g.x1 = std::clamp(p.x1 + 2, 0, w);
        g.y0 = std::clamp(p.y0 - 2, 0, h); g.y1 = std::clamp(p.y1 + 2, 0, h);
        if (g.x1 > g.x0 && g.y1 > g.y0) pictures.push_back(g);
    }
    std::vector<uint8_t> isPicture((size_t)w, 0);
    for (int y = 0; y < h; ++y) {
        const size_t row = (size_t)y * w;
        if (plain[y]) { std::memcpy(c + row, r + row, (size_t)w); continue; }
        for (const ImageRect& p : pictures)
            if (y >= p.y0 && y < p.y1) std::fill(isPicture.begin() + p.x0, isPicture.begin() + p.x1, uint8_t(1));
        for (int x = 0; x < w; ++x) {
            const int cv = c[row + x], bv = b[row + x];
            const int t = isPicture[x] ? imageLut[bv] : fillLut[bv];
            if (cv <= bv)
                c[row + x] = (uint8_t)((t * r[row + x] + 127) / 255);
            else
                c[row + x] = (uint8_t)(255 - ((255 - t) * (255 - cv) + (255 - bv) / 2) / (255 - bv));
        }
        for (const ImageRect& p : pictures)
            if (y >= p.y0 && y < p.y1) std::fill(isPicture.begin() + p.x0, isPicture.begin() + p.x1, uint8_t(0));
    }
}

}

void processPdfBitmap(Bitmap8* bitmap, const PdfBitmapOptions& options,
                      Logger* log, const PdfBaseLayer* base) {
    if (!bitmap || bitmap->pixels.empty()) return;
    if (base && (!base->pixels || base->pixels->width != bitmap->width ||
                 base->pixels->height != bitmap->height || options.trim))
        base = nullptr;
    if (options.trim) trimWhiteBorder(bitmap);
    auto avg=[](const std::vector<uint8_t>& v){ double s=0; for(auto p:v)s+=p; return v.empty()?0:s/v.size(); };
    const double before = log ? avg(bitmap->pixels) : 0;
    if (base) {
        applyToneOverBase(bitmap, *base, options);
    } else {
        applyTone(bitmap, options.gamma, options.contrast, options.watermarkThreshold);
        if (options.strokeExpand > 0.0)
            applyStrokeExpand(*bitmap, options.strokeExpand, false);
    }
    if (log) {
        char msg[320];
        std::snprintf(msg,sizeof(msg),"tone pixels=%zu avg=%.1f->%.1f gamma=%.2f contrast=%.2f watermark=%d stroke=%.3f base=%s images=%d imageContrast=%.2f fillTone=%.2f",
            bitmap->pixels.size(),before,avg(bitmap->pixels),options.gamma,options.contrast,options.watermarkThreshold,
            options.strokeExpand,base?"yes":"no",base?(int)base->images.size():0,options.imageContrast,options.fillTone);
        log->log(LogLevel::Info,"pdf-filter",msg);
    }
}

}
