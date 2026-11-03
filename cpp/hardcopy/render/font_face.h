#ifndef HARDCOPY_RENDER_FONT_FACE_H_
#define HARDCOPY_RENDER_FONT_FACE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hardcopy {

class FontFace {
public:
    FontFace();
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    bool open(const std::string& path);
    bool openFromMemory(const uint8_t* data, size_t size);
    bool openFromMemory(std::shared_ptr<const std::vector<uint8_t>> data);
    bool isOpen() const;

    bool setPixelSize(double px);

    void* freetypeFace() const;
    void* harfbuzzFont(double px);

    struct Glyph {
        int width = 0;
        int height = 0;
        int bearingLeft = 0;
        int bearingTop  = 0;
        int advance     = 0;
        std::vector<uint8_t> pixels;
    };
    bool renderGlyph(uint32_t glyphIndex, double px, Glyph* out,
                     bool embolden = false, bool oblique = false,
                     bool rotateCw = false);

    const Glyph* cachedGlyph(uint32_t glyphIndex, double px,
                             bool embolden = false, bool oblique = false,
                             bool rotateCw = false);
    void glyphCacheStats(size_t* entries, size_t* bytes,
                         uint64_t* renders = nullptr, uint64_t* renderUs = nullptr) const;

    struct HMetrics {
        float bearingX = 0;
        float bearingY = 0;
        float width = 0;
        float height = 0;
        float advance = 0;
    };
    bool glyphHMetrics(uint32_t glyphIndex, double px, HMetrics* out);

    uint32_t glyphIndexForChar(uint32_t codepoint) const;

    double ascender(double px) const;
    double descender(double px) const;
    double lineGap(double px) const;
    double lineHeight(double px) const { return ascender(px) - descender(px) + lineGap(px); }

    const std::vector<uint8_t>& fontData() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
