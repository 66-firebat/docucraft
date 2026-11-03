#ifndef HARDCOPY_LAYOUT_LINE_BOX_H_
#define HARDCOPY_LAYOUT_LINE_BOX_H_

#include <cstdint>
#include <string>
#include <vector>

namespace hardcopy {

enum class FontSlot : uint8_t {
    Regular  = 0,
    Bold     = 1,
    Mono     = 2,
    Fallback = 3,
    Sans     = 4,
    Kai      = 5,
    Embedded0 = 8,
};

constexpr int kMaxEmbeddedFonts = 8;

inline bool isEmbeddedSlot(FontSlot s) {
    return (uint8_t)s >= (uint8_t)FontSlot::Embedded0
        && (uint8_t)s <  (uint8_t)FontSlot::Embedded0 + kMaxEmbeddedFonts;
}
inline int embeddedIndex(FontSlot s) {
    return (int)((uint8_t)s - (uint8_t)FontSlot::Embedded0);
}
inline FontSlot embeddedSlot(int idx) {
    return (FontSlot)((uint8_t)FontSlot::Embedded0 + idx);
}

struct PlacedGlyph {
    uint32_t    glyphIndex;
    float       x;
    float       y;
    float       advance;
    FontSlot    fontSlot;
    float       fontPx;
    bool        syntheticBold = false;
    bool        oblique = false;
    bool        rotated = false;
    bool        hasTextOrigin = false;
    float       textX = 0;
    float       textY = 0;
    uint8_t     inkGray = 0;
    std::string sourceUtf8;
    std::string linkTarget;

    uint32_t    textBlockId = 0xFFFFFFFFu;
    float       blockX0 = 0;
    float       blockY0 = 0;
    float       blockX1 = 0;
    float       blockY1 = 0;
};

struct PlacedImage {
    int   imageIndex;
    float x, y;
    float w, h;
};

struct PageGlyphs {
    int                       pageNumber = 0;
    std::vector<PlacedGlyph>  glyphs;

    struct LineInfo {
        float y;
        float baseline;
        float height;
        float xStart;
        float xEnd;
        float fontPx = 0;
        FontSlot fontSlot = FontSlot::Regular;
        int8_t column = -1;
    };
    std::vector<LineInfo>     lines;

    struct TextSpan {
        std::string utf8;
        float x, y, w, h;
        float fontSize = 0;
        float baseline = 0;
        FontSlot fontSlot = FontSlot::Regular;
    };
    std::vector<TextSpan>     textSpans;
    std::vector<PlacedImage>  images;

    struct Rect {
        float x, y, w, h;
        uint8_t fillGray = 255;
        uint8_t borderGray = 180;
        int borderWidth = 0;
        float radius = 0;
    };
    std::vector<Rect> rects;

    struct HRule {
        float y;
        float xStart;
        float xEnd;
        uint8_t gray = 180;
        int   thickness = 2;
    };
    std::vector<HRule>        hRules;
};

struct HeadingAnchor {
    std::string title;
    int         level;
    int         pageIndex0;
    float       yOnPage;
};

}

#endif
