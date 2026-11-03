#include "font_face.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H

#include <hb.h>
#include <hb-ft.h>

#include "../hb_glue.h"

namespace hardcopy {

namespace {

class FtLibrary {
public:
    FtLibrary() { FT_Init_FreeType(&lib_); }
    ~FtLibrary() { if (lib_) FT_Done_FreeType(lib_); }
    FT_Library handle() const { return lib_; }
private:
    FT_Library lib_ = nullptr;
};

FtLibrary& ft() {
    static FtLibrary g;
    return g;
}

}

struct FontFace::Impl {
    FT_Face   face = nullptr;
    double    pxCurrent = 0.0;
    std::unordered_map<int, hb_font_t*> hbCache;
    std::shared_ptr<const std::vector<uint8_t>> ownedData;
    std::unordered_map<uint64_t, Glyph> glyphCache;
    size_t glyphCacheBytes = 0;
    uint64_t glyphRenders = 0;
    uint64_t glyphRenderUs = 0;
    static constexpr size_t kGlyphCacheBudget = 24u << 20;

    ~Impl() {
        HbGuard hbGuard;
        for (auto& kv : hbCache) {
            if (kv.second) hb_font_destroy(kv.second);
        }
        if (face) FT_Done_Face(face);
    }
};

FontFace::FontFace() : impl_(std::make_unique<Impl>()) {}
FontFace::~FontFace() = default;

bool FontFace::open(const std::string& path) {
    if (FT_New_Face(ft().handle(), path.c_str(), 0, &impl_->face)) {
        impl_->face = nullptr;
        return false;
    }
    return true;
}

bool FontFace::openFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) return false;
    return openFromMemory(std::make_shared<const std::vector<uint8_t>>(data, data + size));
}

bool FontFace::openFromMemory(std::shared_ptr<const std::vector<uint8_t>> data) {
    if (!data || data->empty()) return false;
    impl_->ownedData = std::move(data);
    if (FT_New_Memory_Face(ft().handle(), impl_->ownedData->data(),
                           (FT_Long)impl_->ownedData->size(), 0, &impl_->face)) {
        impl_->face = nullptr;
        impl_->ownedData.reset();
        return false;
    }
    return true;
}

bool FontFace::isOpen() const { return impl_->face != nullptr; }

const std::vector<uint8_t>& FontFace::fontData() const {
    static const std::vector<uint8_t> empty;
    return impl_->ownedData ? *impl_->ownedData : empty;
}

bool FontFace::setPixelSize(double px) {
    if (!impl_->face) return false;
    if (FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(px + 0.5))) return false;
    impl_->pxCurrent = px;
    return true;
}

void* FontFace::freetypeFace() const { return impl_->face; }

void* FontFace::harfbuzzFont(double px) {
    if (!impl_->face) return nullptr;
    HbGuard hbGuard;
    if (impl_->pxCurrent != px) setPixelSize(px);
    int key = static_cast<int>(px * 64.0 + 0.5);
    auto it = impl_->hbCache.find(key);
    if (it != impl_->hbCache.end()) return it->second;

    hb_font_t* hbf = hb_ft_font_create_referenced(impl_->face);
    hb_ft_font_set_funcs(hbf);
    impl_->hbCache.emplace(key, hbf);
    return hbf;
}

bool FontFace::renderGlyph(uint32_t glyphIndex, double px, Glyph* out,
                           bool embolden, bool oblique, bool rotateCw) {
    if (!impl_->face || !out) return false;
    if (impl_->pxCurrent != px) setPixelSize(px);

    if (rotateCw) {
        FT_Matrix m;
        m.xx = 0;        m.xy = 0x10000L;
        m.yx = -0x10000L; m.yy = 0;
        FT_Set_Transform(impl_->face, &m, nullptr);
        const FT_Error err = FT_Load_Glyph(impl_->face, glyphIndex,
                                           FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);
        FT_Set_Transform(impl_->face, nullptr, nullptr);
        if (err) return false;
    } else if (FT_Load_Glyph(impl_->face, glyphIndex, FT_LOAD_DEFAULT)) {
        return false;
    }
    if (embolden) FT_GlyphSlot_Embolden(impl_->face->glyph);
    if (oblique && !rotateCw) FT_GlyphSlot_Oblique(impl_->face->glyph);
    if (FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL)) return false;

    FT_Bitmap& bm = impl_->face->glyph->bitmap;
    out->width  = bm.width;
    out->height = bm.rows;
    out->bearingLeft = impl_->face->glyph->bitmap_left;
    out->bearingTop  = impl_->face->glyph->bitmap_top;
    out->advance     = impl_->face->glyph->advance.x >> 6;
    out->pixels.assign(bm.buffer, bm.buffer + bm.rows * bm.pitch);
    return true;
}

const FontFace::Glyph* FontFace::cachedGlyph(uint32_t glyphIndex, double px,
                                             bool embolden, bool oblique,
                                             bool rotateCw) {
    if (!impl_->face) return nullptr;
    const uint64_t pxKey = (uint64_t)std::min<long>(0xFFFF, std::lround(px * 16.0));
    const uint64_t key = ((uint64_t)glyphIndex << 32) | (pxKey << 3)
                       | (embolden ? 4u : 0u) | (oblique ? 2u : 0u) | (rotateCw ? 1u : 0u);
    auto it = impl_->glyphCache.find(key);
    if (it != impl_->glyphCache.end()) return &it->second;

    Glyph g;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = renderGlyph(glyphIndex, px, &g, embolden, oblique, rotateCw);
    impl_->glyphRenderUs += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    ++impl_->glyphRenders;
    if (!ok) return nullptr;
    const size_t bytes = g.pixels.size() + sizeof(Glyph);
    if (impl_->glyphCacheBytes + bytes > Impl::kGlyphCacheBudget) {
        impl_->glyphCache.clear();
        impl_->glyphCacheBytes = 0;
    }
    impl_->glyphCacheBytes += bytes;
    auto ins = impl_->glyphCache.emplace(key, std::move(g));
    return &ins.first->second;
}

void FontFace::glyphCacheStats(size_t* entries, size_t* bytes,
                               uint64_t* renders, uint64_t* renderUs) const {
    if (entries) *entries = impl_->glyphCache.size();
    if (bytes) *bytes = impl_->glyphCacheBytes;
    if (renders) *renders = impl_->glyphRenders;
    if (renderUs) *renderUs = impl_->glyphRenderUs;
}

bool FontFace::glyphHMetrics(uint32_t glyphIndex, double px, HMetrics* out) {
    if (!impl_->face || !out) return false;
    if (impl_->pxCurrent != px && !setPixelSize(px)) return false;
    if (FT_Load_Glyph(impl_->face, glyphIndex, FT_LOAD_DEFAULT)) return false;
    const FT_Glyph_Metrics& m = impl_->face->glyph->metrics;
    out->bearingX = m.horiBearingX / 64.0f;
    out->bearingY = m.horiBearingY / 64.0f;
    out->width    = m.width / 64.0f;
    out->height   = m.height / 64.0f;
    out->advance  = m.horiAdvance / 64.0f;
    return true;
}

uint32_t FontFace::glyphIndexForChar(uint32_t codepoint) const {
    if (!impl_->face) return 0;
    return (uint32_t)FT_Get_Char_Index(impl_->face, (FT_ULong)codepoint);
}

double FontFace::ascender(double px) const {
    if (!impl_->face) return 0;
    double upem = impl_->face->units_per_EM;
    return upem ? (impl_->face->ascender * px / upem) : px;
}
double FontFace::descender(double px) const {
    if (!impl_->face) return 0;
    double upem = impl_->face->units_per_EM;
    return upem ? (impl_->face->descender * px / upem) : 0;
}
double FontFace::lineGap(double px) const {
    if (!impl_->face) return 0;
    double upem = impl_->face->units_per_EM;
    double gap  = (double)impl_->face->height - (impl_->face->ascender - impl_->face->descender);
    return upem ? (gap * px / upem) : 0;
}

}
