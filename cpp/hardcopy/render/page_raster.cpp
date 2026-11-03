#include "page_raster.h"
#include "ruling_pattern.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "contrast_lut.h"
#include "font_face.h"
#include "../image/image_decoder.h"
#include "../include/hardcopy/presets.h"

namespace hardcopy {

namespace {

void blit(Bitmap8& dst, const FontFace::Glyph& g, int penX, int penY,
          uint8_t inkGray, bool lightInk = false) {
    const int x0 = penX + g.bearingLeft;
    const int y0 = penY - g.bearingTop;
    const int i0 = std::max(0, -x0), i1 = std::min(g.width, dst.width - x0);
    const int j0 = std::max(0, -y0), j1 = std::min(g.height, dst.height - y0);
    if (i0 >= i1 || j0 >= j1) return;
    for (int j = j0; j < j1; ++j) {
        const uint8_t* src = &g.pixels[(size_t)j * g.width];
        uint8_t* row = &dst.pixels[(size_t)(y0 + j) * dst.width + x0];
        for (int i = i0; i < i1; ++i) {
            const uint8_t cov = src[i];
            if (cov == 0) continue;
            uint8_t* px = row + i;
            const uint16_t bg = *px;
            const uint16_t blended = (bg * (255 - cov) + inkGray * cov) / 255;
            *px = lightInk
                ? (uint8_t)std::max<uint16_t>(*px, blended)
                : (uint8_t)std::min<uint16_t>(*px, blended);
        }
    }
}

}

PageRaster::PageRaster(const Options& opts, const FontSet& fonts, Logger* log)
    : opts_(opts), fonts_(fonts), log_(log) {}

const Bitmap8* PageRaster::background() {
    if (!bgTried_) {
        bgTried_ = true;
        bgReady_ = !opts_.backgroundPath.empty()
                && loadPageBackground(opts_, log_, &bg_);
    }
    return bgReady_ ? &bg_ : nullptr;
}

bool PageRaster::render(const PageGlyphs& page, Bitmap8* out,
                        const std::vector<ImageData>* images, int totalPages) {
    if (!out) return false;
    using Clock = std::chrono::steady_clock;
    Clock::time_point stageStart = stats_ ? Clock::now() : Clock::time_point{};
    auto mark = [&](uint64_t RasterStageUs::*field) {
        if (!stats_) return;
        const auto now = Clock::now();
        (*stats_).*field += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            now - stageStart).count();
        stageStart = now;
    };
    out->width = opts_.pageWidth;
    out->height = opts_.pageHeight;
    out->pixels.assign((size_t)out->width * out->height, uint8_t(255));

    if (const Bitmap8* bg = background()) {
        out->pixels = bg->pixels;
    }
    mark(&RasterStageUs::clear);

    if (opts_.preset == "note") {
        const int thick = std::max(2, std::min(out->width, out->height) / 720);
        if (out->height >= out->width) {
            const int y0 = noteDividerPosition(out->height);
            for (int y = y0; y < std::min(out->height, y0 + thick); ++y)
                std::fill_n(&out->pixels[(size_t)y * out->width], out->width, uint8_t(72));
        } else {
            const int x0 = noteDividerPosition(out->width);
            for (int y = 0; y < out->height; ++y) {
                for (int x = x0; x < std::min(out->width, x0 + thick); ++x)
                    out->pixels[(size_t)y * out->width + x] = 72;
            }
        }
    }

    if (opts_.drawRuling) {
        struct Row {
            float centreY;
            float cellHeight;
            float fontPx;
            FontSlot fontSlot;
            float xStart;
            float xEnd;
            int column;
        };
        struct Rule {
            float y;
            float cellHeight;
            float xStart;
            float xEnd;
            int column;
        };
        std::vector<Row> rows;
        rows.reserve(page.lines.size());
        for (const auto& li : page.lines) {
            const float ascender = li.baseline - li.y;
            const float centreY = li.baseline - ascender * 0.45f;
            rows.push_back(Row{centreY, li.height, li.fontPx, li.fontSlot,
                               li.xStart, li.xEnd, (int)li.column});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.column != b.column) return a.column < b.column;
            return a.centreY < b.centreY;
        });

        auto isRegularBodyRow = [&](const Row& row) {
            const bool bodySlot = row.fontSlot == FontSlot::Regular
                               || row.fontSlot == FontSlot::Fallback;
            const float sizeTolerance = std::max(1.0f, (float)opts_.fontSize * 0.08f);
            return bodySlot
                && std::abs(row.fontPx - (float)opts_.fontSize) <= sizeTolerance;
        };
        auto sharesGridStep = [&](const Row& upper, const Row& lower) {
            if (upper.column != lower.column) return false;
            if (!isRegularBodyRow(upper) || !isRegularBodyRow(lower)) return false;
            const float expected = (upper.cellHeight + lower.cellHeight) * 0.5f;
            const float actual = lower.centreY - upper.centreY;
            const float tolerance = std::max(3.0f, expected * 0.16f);
            return std::abs(actual - expected) <= tolerance;
        };

        std::vector<Rule> rules;
        rules.reserve(rows.size() + 1);
        for (size_t start = 0; start < rows.size();) {
            if (!isRegularBodyRow(rows[start])) {
                ++start;
                continue;
            }
            size_t end = start;
            while (end + 1 < rows.size() && sharesGridStep(rows[end], rows[end + 1]))
                ++end;
            if (end > start) {
                float xStart = rows[start].xStart;
                float xEnd = rows[start].xEnd;
                for (size_t i = start + 1; i <= end; ++i) {
                    xStart = std::min(xStart, rows[i].xStart);
                    xEnd = std::max(xEnd, rows[i].xEnd);
                }
                const int column = rows[start].column;
                const bool firstOfColumn = start == 0 || rows[start - 1].column != column;
                const bool lastOfColumn = end + 1 == rows.size() || rows[end + 1].column != column;
                if (firstOfColumn) {
                    const float step = rows[start + 1].centreY - rows[start].centreY;
                    rules.push_back(Rule{rows[start].centreY - step * 0.5f,
                                         step, xStart, xEnd, column});
                }
                for (size_t i = start; i < end; ++i) {
                    const float step = rows[i + 1].centreY - rows[i].centreY;
                    rules.push_back(Rule{(rows[i].centreY + rows[i + 1].centreY) * 0.5f,
                                         step, xStart, xEnd, column});
                }
                if (lastOfColumn) {
                    const float step = rows[end].centreY - rows[end - 1].centreY;
                    rules.push_back(Rule{rows[end].centreY + step * 0.5f,
                                         step, xStart, xEnd, column});
                }
            }
            start = end + 1;
        }

        int lastRuleY = -1000;
        int lastRuleColumn = -1;
        auto drawRule = [&](const Rule& rule) {
            const int lineY = (int)std::lround(rule.y);
            if (lineY < 0 || lineY >= out->height ||
                (std::abs(lineY - lastRuleY) <= 1 && rule.column == lastRuleColumn))
                return;
            lastRuleY = lineY;
            lastRuleColumn = rule.column;

            const int inset = std::max(8, (int)std::lround(rule.cellHeight * 0.08f));
            const int x0 = std::max(0, (int)std::lround(rule.xStart) + inset);
            const int x1 = std::min(out->width, (int)std::lround(rule.xEnd) - inset);
            const int dot = std::max(4, (int)std::lround(rule.cellHeight * 0.065f));
            const int gap = std::max(6, (int)std::lround(rule.cellHeight * 0.12f));
            paintRulingPattern(out->pixels.data(), out->width, out->height,
                              x0, x1, lineY, dot, gap, 175, true,
                              opts_.rulingStyle == "dashed");
        };

        for (const auto& rule : rules) drawRule(rule);
    }

    for (const auto& r : page.rects) {
        int x0 = std::max(0, (int)std::floor(r.x));
        int y0 = std::max(0, (int)std::floor(r.y));
        int x1 = std::min(out->width, (int)std::ceil(r.x + r.w));
        int y1 = std::min(out->height, (int)std::ceil(r.y + r.h));
        const float radius = std::max(0.0f,
            std::min(r.radius, std::min(r.w, r.h) * 0.5f));
        auto insideRounded = [](float px, float py, float left, float top,
                                float right, float bottom, float rad) {
            if (px < left || px >= right || py < top || py >= bottom) return false;
            if (rad <= 0) return true;
            float cx = std::clamp(px, left + rad, right - rad);
            float cy = std::clamp(py, top + rad, bottom - rad);
            float dx = px - cx;
            float dy = py - cy;
            return dx * dx + dy * dy <= rad * rad;
        };
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                float px = x + 0.5f;
                float py = y + 0.5f;
                if (!insideRounded(px, py, r.x, r.y,
                                   r.x + r.w, r.y + r.h, radius)) continue;
                bool border = false;
                if (r.borderWidth > 0) {
                    const float bw = (float)r.borderWidth;
                    float innerRadius = std::max(0.0f, radius - bw);
                    border = !insideRounded(px, py, r.x + bw, r.y + bw,
                        r.x + r.w - bw, r.y + r.h - bw, innerRadius);
                }
                uint8_t gray = border ? r.borderGray : r.fillGray;
                out->pixels[y * out->width + x] =
                    std::min(out->pixels[y * out->width + x], gray);
            }
        }
    }

    for (const auto& hr : page.hRules) {
        int ry = (int)(hr.y + 0.5f);
        int x0 = std::max(0, (int)(hr.xStart + 0.5f));
        int x1 = std::min(out->width, (int)(hr.xEnd + 0.5f));
        for (int dy = 0; dy < hr.thickness; ++dy) {
            int y = ry + dy;
            if (y < 0 || y >= out->height) continue;
            for (int x = x0; x < x1; ++x) {
                uint8_t& px = out->pixels[y * out->width + x];
                px = std::min(px, hr.gray);
            }
        }
    }
    mark(&RasterStageUs::decor);

    for (const auto& g : page.glyphs) {
        FontFace* face = fonts_.faceFor(g.fontSlot);
        if (!face) continue;

        const bool embolden = g.syntheticBold;
        const FontFace::Glyph* rg = face->cachedGlyph(g.glyphIndex, g.fontPx,
                                                      embolden, g.oblique, g.rotated);
        if (!rg) continue;
        blit(*out, *rg, (int)(g.x + 0.5f), (int)(g.y + 0.5f), g.inkGray);
    }
    mark(&RasterStageUs::glyphs);

    std::vector<ImageRect> imageRects;
    if (images) {
        for (const auto& pi : page.images) {
            if (pi.imageIndex < 0 || pi.imageIndex >= (int)images->size()) continue;
            const auto& imgData = (*images)[pi.imageIndex];
            if (imgData.rawBytes.empty()) continue;

            Bitmap8 decoded;
            if (!decodeToGray(imgData.rawBytes.data(), imgData.rawBytes.size(), &decoded))
                continue;

            int dstW = std::max(1, (int)(pi.w + 0.5f));
            int dstH = std::max(1, (int)(pi.h + 0.5f));
            int dstX = (int)(pi.x + 0.5f);
            int dstY = (int)(pi.y + 0.5f);
            imageRects.push_back(ImageRect{dstX, dstY, dstX + dstW, dstY + dstH});

            for (int y = 0; y < dstH; ++y) {
                int dy = dstY + y;
                if (dy < 0 || dy >= out->height) continue;
                float srcY = (y + 0.5f) * decoded.height / dstH - 0.5f;
                int sy0 = std::max(0, (int)std::floor(srcY));
                int sy1 = std::min(decoded.height - 1, sy0 + 1);
                float fy = srcY - sy0;

                for (int x = 0; x < dstW; ++x) {
                    int dx = dstX + x;
                    if (dx < 0 || dx >= out->width) continue;
                    float srcX = (x + 0.5f) * decoded.width / dstW - 0.5f;
                    int sx0 = std::max(0, (int)std::floor(srcX));
                    int sx1 = std::min(decoded.width - 1, sx0 + 1);
                    float fx = srcX - sx0;

                    float p00 = decoded.pixels[sy0 * decoded.width + sx0];
                    float p10 = decoded.pixels[sy0 * decoded.width + sx1];
                    float p01 = decoded.pixels[sy1 * decoded.width + sx0];
                    float p11 = decoded.pixels[sy1 * decoded.width + sx1];
                    float val = p00 * (1-fx)*(1-fy) + p10 * fx*(1-fy)
                              + p01 * (1-fx)*fy     + p11 * fx*fy;
                    out->pixels[dy * out->width + dx] =
                        (uint8_t)std::clamp((int)(val + 0.5f), 0, 255);
                }
            }
        }
    }

    mark(&RasterStageUs::images);

    if (opts_.footerProgress && totalPages > 0) {
        drawFooterProgress(*out, opts_, fonts_.faceFor(FontSlot::Regular),
                           fonts_.fallback, page.pageNumber, totalPages);
    }

    uint8_t lut[256];
    buildContrastLut(opts_.contrastBoost, lut);
    uint8_t imgLut[256];
    buildContrastLut(opts_.imageContrastBoost, imgLut);
    if (log_) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "contrast level=%.0f/20 lut[128]=%d lut[200]=%d lut[50]=%d imgLevel=%.0f/20",
            std::clamp(opts_.contrastBoost, 0.0, 1.0) * 20.0,
            lut[128], lut[200], lut[50],
            std::clamp(opts_.imageContrastBoost, 0.0, 1.0) * 20.0);
        log_->log(LogLevel::Info, "page_raster", msg);
    }
    applyContrastWithImageMask(out->pixels.data(), out->width, out->height,
                               lut, imgLut, imageRects);
    mark(&RasterStageUs::tone);

    if (opts_.strokeExpand > 0.0)
        applyStrokeExpand(*out, opts_.strokeExpand, false);
    applyNightInvert(out->pixels.data(), out->width, out->height, opts_.whiteText);
    mark(&RasterStageUs::stroke);

    return true;
}

namespace {

uint32_t nextCodepoint(const std::string& s, size_t* i) {
    const auto c = (uint8_t)s[*i];
    if (c < 0x80) { ++*i; return c; }
    int len = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0)      { len = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 3; cp = c & 0x07; }
    else { ++*i; return 0xFFFD; }
    for (int k = 0; k < len; ++k) {
        if (*i + 1 >= s.size()) { *i = s.size(); return 0xFFFD; }
        const auto cc = (uint8_t)s[++*i];
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    ++*i;
    return cp;
}

}

bool loadPageBackground(const Options& opts, Logger* log, Bitmap8* out) {
    if (!out) return false;
    std::ifstream ifs(opts.backgroundPath, std::ios::binary);
    std::vector<uint8_t> bytes;
    if (ifs) {
        bytes.assign(std::istreambuf_iterator<char>(ifs),
                     std::istreambuf_iterator<char>());
    }
    Bitmap8 decoded;
    if (bytes.empty() || !decodeToGrayOnWhite(bytes.data(), bytes.size(), &decoded)
        || decoded.width <= 0 || decoded.height <= 0) {
        if (log) {
            log->log(LogLevel::Warn, "page_raster",
                     ("background decode failed: " + opts.backgroundPath).c_str());
        }
        return false;
    }

    const int pw = opts.pageWidth;
    const int ph = opts.pageHeight;
    if (pw <= 0 || ph <= 0) return false;
    int rw = pw, rh = ph;
    if (opts.preset == "note") {
        if (ph >= pw) rh = noteDividerPosition(ph);
        else          rw = noteDividerPosition(pw);
    }
    const double scale = std::max((double)rw / decoded.width,
                                  (double)rh / decoded.height);
    const int cropX = (std::max(rw, (int)std::ceil(decoded.width  * scale)) - rw) / 2;
    const int cropY = (std::max(rh, (int)std::ceil(decoded.height * scale)) - rh) / 2;
    out->width  = pw;
    out->height = ph;
    out->pixels.assign((size_t)pw * ph, uint8_t(255));
    for (int y = 0; y < rh; ++y) {
        const double srcY = (y + cropY + 0.5) / scale - 0.5;
        const int sy0 = std::clamp((int)std::floor(srcY), 0, decoded.height - 1);
        const int sy1 = std::min(sy0 + 1, decoded.height - 1);
        const double fy = srcY - std::floor(srcY);
        for (int x = 0; x < rw; ++x) {
            const double srcX = (x + cropX + 0.5) / scale - 0.5;
            const int sx0 = std::clamp((int)std::floor(srcX), 0, decoded.width - 1);
            const int sx1 = std::min(sx0 + 1, decoded.width - 1);
            const double fx = srcX - std::floor(srcX);
            const double p00 = decoded.pixels[sy0 * decoded.width + sx0];
            const double p10 = decoded.pixels[sy0 * decoded.width + sx1];
            const double p01 = decoded.pixels[sy1 * decoded.width + sx0];
            const double p11 = decoded.pixels[sy1 * decoded.width + sx1];
            const double val = p00 * (1 - fx) * (1 - fy) + p10 * fx * (1 - fy)
                             + p01 * (1 - fx) * fy       + p11 * fx * fy;
            out->pixels[(size_t)y * pw + x] =
                (uint8_t)std::clamp((int)(val + 0.5), 0, 255);
        }
    }
    if (log) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
            "background loaded: %s %dx%d -> cover %dx%d of page %dx%d",
            opts.backgroundPath.c_str(), decoded.width, decoded.height,
            rw, rh, pw, ph);
        log->log(LogLevel::Info, "page_raster", msg);
    }
    return true;
}

FooterStrip renderFooterStrip(const std::string& utf8, double fontPx,
                              bool embolden, FontFace* regular,
                              FontFace* fallback) {
    FooterStrip strip;
    if (!regular) return strip;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = nextCodepoint(utf8, &i);
        if (cp == 0) continue;
        FontFace* face = regular;
        uint32_t gi = face->glyphIndexForChar(cp);
        if (gi == 0 && fallback) {
            const uint32_t fi = fallback->glyphIndexForChar(cp);
            if (fi != 0) { face = fallback; gi = fi; }
        }
        if (gi == 0) continue;
        FontFace::Glyph glyph;
        if (!face->renderGlyph(gi, fontPx, &glyph, embolden, false))
            continue;
        strip.width += glyph.advance;
        strip.glyphs.push_back(std::move(glyph));
    }
    return strip;
}

void drawFooterStrips(Bitmap8& bm, const Options& opts,
                      const FooterStrip& left, const FooterStrip& right) {
    const double fp = footerFontPx(opts);
    const int reserve = footerReservePx(opts);
    const int bottomInset = footerBottomInsetPx(opts);
    const int horizontalInset = footerHorizontalInsetPx(opts);
    const int baseline = (int)((float)(bm.height - bottomInset)
                         - reserve * 0.5f + (float)(fp * 0.35) + 0.5f);
    int penX = horizontalInset;
    for (const auto& glyph : left.glyphs) {
        blit(bm, glyph, penX, baseline, uint8_t(0));
        penX += glyph.advance;
    }
    penX = bm.width - horizontalInset - right.width;
    for (const auto& glyph : right.glyphs) {
        blit(bm, glyph, penX, baseline, uint8_t(0));
        penX += glyph.advance;
    }
}

void drawFooterProgress(Bitmap8& bm, const Options& opts, FontFace* regular,
                        FontFace* fallback, int pageNumber, int totalPages) {
    char left[64];
    std::snprintf(left, sizeof(left), "第 %d 页", pageNumber);
    char right[16];
    const int pct = (int)std::lround(pageNumber * 100.0 / std::max(1, totalPages));
    std::snprintf(right, sizeof(right), "%d%%", pct);
    const double fp = footerFontPx(opts);
    drawFooterStrips(bm, opts,
                     renderFooterStrip(left, fp, opts.syntheticBold,
                                       regular, fallback),
                     renderFooterStrip(right, fp, opts.syntheticBold,
                                       regular, fallback));
}

namespace {

const uint8_t* strokeBlendTable(double amount, bool lightInk) {
    struct Cache {
        double amount = -1.0;
        bool lightInk = false;
        std::vector<uint8_t> table;
    };
    thread_local Cache cache;
    if (cache.table.empty() || cache.amount != amount || cache.lightInk != lightInk) {
        cache.table.resize(256 * 256);
        for (int source = 0; source < 256; ++source)
            for (int expanded = 0; expanded < 256; ++expanded) {
                const int blended = (int)std::lround(source + (expanded - source) * amount);
                cache.table[source * 256 + expanded] = (uint8_t)std::clamp(blended, 0, 255);
            }
        cache.amount = amount;
        cache.lightInk = lightInk;
    }
    return cache.table.data();
}

inline uint64_t load8(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline void store8(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

template <bool Light>
inline uint8_t strokePick(uint8_t a, uint8_t b) {
    return Light ? std::max(a, b) : std::min(a, b);
}

template <bool Light>
inline bool allPaper(uint64_t a, uint64_t b, uint64_t c) {
    return Light ? ((a | b | c) == 0) : ((a & b & c) == ~0ULL);
}

template <bool Light>
void strokeRowExtreme(const uint8_t* in, uint8_t* out, int W) {
    constexpr uint64_t kPaper = Light ? 0ULL : ~0ULL;
    if (W == 1) { out[0] = in[0]; return; }
    out[0] = strokePick<Light>(in[0], in[1]);
    int x = 1;
    for (; x + 9 <= W; x += 8) {
        if (allPaper<Light>(load8(in + x - 1), load8(in + x), load8(in + x + 1))) {
            store8(out + x, kPaper);
            continue;
        }
        for (int i = 0; i < 8; ++i)
            out[x + i] = strokePick<Light>(
                strokePick<Light>(in[x + i - 1], in[x + i]), in[x + i + 1]);
    }
    for (; x + 1 < W; ++x)
        out[x] = strokePick<Light>(strokePick<Light>(in[x - 1], in[x]), in[x + 1]);
    out[W - 1] = strokePick<Light>(in[W - 2], in[W - 1]);
}

template <bool Light>
void strokeExpandImpl(Bitmap8& bm, const uint8_t* blend) {
    const int W = bm.width, H = bm.height;
    std::vector<uint8_t> ring((size_t)W * 3);
    uint8_t* rows[3] = { ring.data(), ring.data() + W, ring.data() + 2 * (size_t)W };
    strokeRowExtreme<Light>(bm.pixels.data(), rows[1], W);
    std::memcpy(rows[0], rows[1], (size_t)W);
    for (int y = 0; y < H; ++y) {
        if (y + 1 < H)
            strokeRowExtreme<Light>(bm.pixels.data() + (size_t)(y + 1) * W, rows[2], W);
        else
            std::memcpy(rows[2], rows[1], (size_t)W);
        const uint8_t* up = rows[0];
        const uint8_t* mid = rows[1];
        const uint8_t* down = rows[2];
        uint8_t* row = bm.pixels.data() + (size_t)y * W;
        int x = 0;
        for (; x + 8 <= W; x += 8) {
            if (allPaper<Light>(load8(up + x), load8(mid + x), load8(down + x))) continue;
            for (int i = 0; i < 8; ++i) {
                const uint8_t expanded = strokePick<Light>(
                    strokePick<Light>(up[x + i], mid[x + i]), down[x + i]);
                const uint8_t src = row[x + i];
                if (expanded != src) row[x + i] = blend[(size_t)src * 256 + expanded];
            }
        }
        for (; x < W; ++x) {
            const uint8_t expanded = strokePick<Light>(strokePick<Light>(up[x], mid[x]), down[x]);
            const uint8_t src = row[x];
            if (expanded != src) row[x] = blend[(size_t)src * 256 + expanded];
        }
        uint8_t* t = rows[0]; rows[0] = rows[1]; rows[1] = rows[2]; rows[2] = t;
    }
}

}

void applyStrokeExpand(Bitmap8& bm, double strokeExpand, bool lightInk) {
    if (strokeExpand <= 0.0) return;
    const double amount = std::clamp(strokeExpand / 0.1, 0.0, 1.0);
    const int W = bm.width;
    const int H = bm.height;
    if (W <= 0 || H <= 0 || bm.pixels.size() < (size_t)W * H) return;
    const uint8_t* blend = strokeBlendTable(amount, lightInk);
    if (lightInk) strokeExpandImpl<true>(bm, blend);
    else          strokeExpandImpl<false>(bm, blend);
}

}
