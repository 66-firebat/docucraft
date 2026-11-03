#include "horizontal_layout.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>

#include <hb.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "../cancel.h"
#include "../render/font_face.h"
#include "../hb_glue.h"
#include "geometry.h"
#include "line_break_rules.h"
#include "../parser/font_alias.h"
#include "../util/text_join.h"

namespace hardcopy {

namespace {

bool isCjk(uint32_t cp) {
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    if (cp >= 0x3040 && cp <= 0x30FF) return true;
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;
    if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
    if (cp >= 0x20000 && cp <= 0x2FFFF) return true;
    return false;
}

uint32_t utf8At(const std::string& s, int byteOffset) {
    if (byteOffset < 0 || byteOffset >= (int)s.size()) return 0;
    uint8_t b = (uint8_t)s[byteOffset];
    if (b < 0x80) return b;
    int extra;
    uint32_t cp;
    if      ((b & 0xE0) == 0xC0) { extra = 1; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; }
    else                          { return 0; }
    int p = byteOffset + 1;
    while (extra-- > 0) {
        if (p >= (int)s.size() || ((uint8_t)s[p] & 0xC0) != 0x80) return cp;
        cp = (cp << 6) | ((uint8_t)s[p] & 0x3F);
        ++p;
    }
    return cp;
}

struct GlyphMeta {
    PlacedGlyph g;
    bool isWhitespace = false;
    bool breakAfter   = false;
    float xShift = 0.0f;
};

bool allowsBreakAfter(uint32_t cp) {
    if (isCjk(cp)) return true;
    if (textjoin::isWideScript(cp)) return true;
    if (cp >= 0x2010 && cp <= 0x2027) return true;
    if (cp == 0x200B) return true;
    if (cp == '-' || cp == '/') return true;
    return false;
}

bool isBreakableSpace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x3000;
}

uint32_t glyphCodepoint(const GlyphMeta& gm) {
    return utf8At(gm.g.sourceUtf8, 0);
}

bool canBreakBetween(const GlyphMeta& left, const GlyphMeta& right) {
    const uint32_t l = glyphCodepoint(left);
    const uint32_t r = glyphCodepoint(right);
    bool allow = left.breakAfter || (isCjk(r) && !right.isWhitespace);
    if ((l == '-' || l == '/') && !left.isWhitespace) {
        allow = right.g.sourceUtf8.size() == 1 && std::isalnum((unsigned char)right.g.sourceUtf8[0]);
    }
    if (l == 0x00A0 || r == 0x00A0) allow = false;
    if ((l == 0x2018 || l == 0x2019 || l == 0x201C || l == 0x201D)
            && !right.isWhitespace && right.g.sourceUtf8.size() == 1
            && std::isalnum((unsigned char)right.g.sourceUtf8[0])) {
        allow = false;
    }
    return linebreak::canBreakBetween(allow, l, r);
}

void shapeSingle(const std::string& text, FontFace* face, FontSlot slot,
                 double px, std::vector<GlyphMeta>& out) {
    if (!face || !face->isOpen() || text.empty()) return;

    HbGuard hbGuard;
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.c_str(), (int)text.size(), 0, (int)text.size());
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buf);

    hb_font_t* hbf = (hb_font_t*)face->harfbuzzFont(px);
    hb_shape(hbf, buf, nullptr, 0);

    unsigned int n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);

    for (unsigned i = 0; i < n; ++i) {
        GlyphMeta gm{};
        gm.g.glyphIndex = info[i].codepoint;
        gm.g.x = 0;
        gm.xShift = pos[i].x_offset / 64.0f;
        gm.g.y = pos[i].y_offset / 64.0f;
        gm.g.advance = pos[i].x_advance / 64.0f;
        gm.g.fontSlot = slot;
        gm.g.fontPx = (float)px;

        int byteStart = (int)info[i].cluster;
        int byteEnd   = (i + 1 < n) ? (int)info[i + 1].cluster : (int)text.size();
        if (byteStart >= 0 && byteEnd >= byteStart && byteEnd <= (int)text.size()) {
            gm.g.sourceUtf8 = text.substr(byteStart, byteEnd - byteStart);
        }

        uint32_t cp = utf8At(text, byteStart);
        gm.isWhitespace = (cp == ' ' || cp == '\t' || cp == 0x00A0);
        gm.breakAfter   = isBreakableSpace(cp) || allowsBreakAfter(cp);

        out.push_back(gm);
    }
    hb_buffer_destroy(buf);
}

int utf8CharLen(const std::string& s, int off) {
    if (off >= (int)s.size()) return 0;
    uint8_t b = (uint8_t)s[off];
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

bool fontHasGlyph(FontFace* face, uint32_t cp) {
    if (!face || !face->isOpen()) return false;
    FT_Face ft = (FT_Face)face->freetypeFace();
    return FT_Get_Char_Index(ft, cp) != 0;
}

void shapeRunInto(const std::string& text, FontSlot preferredSlot,
                  const FontSet& fonts, double px, std::vector<GlyphMeta>& out) {
    if (text.empty()) return;

    struct Candidate { FontFace* face; FontSlot slot; };
    Candidate candidates[5];
    int candidateCount = 0;
    auto add = [&](FontSlot slot) {
        FontFace* face = fonts.faceFor(slot);
        if (!face || !face->isOpen()) return;
        for (int i = 0; i < candidateCount; ++i)
            if (candidates[i].face == face) return;
        candidates[candidateCount++] = Candidate{face, slot};
    };

    add(preferredSlot);
    add(FontSlot::Regular);
    add(FontSlot::Sans);
    add(FontSlot::Kai);
    add(FontSlot::Fallback);
    if (candidateCount == 0) return;

    auto candidateFor = [&](uint32_t cp) {
        for (int i = 0; i < candidateCount; ++i)
            if (fontHasGlyph(candidates[i].face, cp)) return i;
        return 0;
    };

    auto isCombining = [](uint32_t cp) {
        return (cp >= 0x0300 && cp <= 0x036F)
            || (cp >= 0x1AB0 && cp <= 0x1AFF)
            || (cp >= 0x1DC0 && cp <= 0x1DFF)
            || (cp >= 0x20D0 && cp <= 0x20FF)
            || (cp >= 0xFE20 && cp <= 0xFE2F);
    };
    auto clusterEnd = [&](int start) {
        int end = start + utf8CharLen(text, start);
        while (end < (int)text.size() && isCombining(utf8At(text, end)))
            end += utf8CharLen(text, end);
        return end;
    };
    auto candidateForCluster = [&](int start, int end) {
        for (int ci = 0; ci < candidateCount; ++ci) {
            bool complete = true;
            for (int p = start; p < end; p += utf8CharLen(text, p)) {
                if (!fontHasGlyph(candidates[ci].face, utf8At(text, p))) {
                    complete = false;
                    break;
                }
            }
            if (complete) return ci;
        }
        return candidateFor(utf8At(text, start));
    };

    int off = 0;
    const int len = (int)text.size();
    while (off < len) {
        int end = clusterEnd(off);
        const int selected = candidateForCluster(off, end);
        const int runStart = off;
        off = end;
        while (off < len) {
            end = clusterEnd(off);
            if (candidateForCluster(off, end) != selected) break;
            off = end;
        }
        const Candidate& c = candidates[selected];
        shapeSingle(text.substr(runStart, off - runStart),
                    c.face, c.slot, px, out);
    }
}

double slotPx(const Options& opts, const Block& b, FontSlot s,
              float runFontSizeEm = -1.f) {
    const float sizeEm = runFontSizeEm >= 0 ? runFontSizeEm : b.cssFontSizeEm;
    double base = opts.fontSize;
    if (sizeEm >= 0) {
        base = opts.fontSize * (double)sizeEm;
    } else if (b.kind == BlockKind::Heading && b.level >= 1 && b.level <= 6) {
        base = opts.fontSize * opts.headingScale[b.level - 1];
    } else if (b.kind == BlockKind::MathBlock) {
        base = opts.fontSize * 1.12;
    }
    if (b.kind == BlockKind::CodeBlock || s == FontSlot::Mono) {
        base = std::max(base * 0.95, 12.0);
    }
    return base;
}

FontSlot slotForRun(const Block& b, const InlineRun& r, const FontSet& fonts) {
    if (b.kind == BlockKind::CodeBlock || r.kind == InlineKind::Code)
        return FontSlot::Mono;
    const bool strong = r.kind == InlineKind::Strong
                     || b.kind == BlockKind::Heading;
    FontSlot embedded;
    if (fonts.findFamilySlot(r.cssFamilyKey, strong, &embedded)) return embedded;
    if (fonts.findFamilySlot(b.cssFamilyKey, strong, &embedded)) return embedded;
    FontRole role = r.cssFontRole >= 0 ? (FontRole)r.cssFontRole
                                       : (FontRole)b.cssFontRole;
    switch (role) {
        case FontRole::Sans: return FontSlot::Sans;
        case FontRole::Kai:  return FontSlot::Kai;
        case FontRole::Mono: return FontSlot::Mono;
        case FontRole::Serif:return strong ? FontSlot::Bold : FontSlot::Regular;
        default:             return strong ? FontSlot::Bold : FontSlot::Regular;
    }
}

bool isCjkPunct(uint32_t cp) {
    return (cp >= 0x3001 && cp <= 0x3002) || (cp >= 0x3008 && cp <= 0x3011)
        || (cp >= 0x3014 && cp <= 0x301B)
        || cp == 0xFF01 || cp == 0xFF08 || cp == 0xFF09 || cp == 0xFF0C
        || cp == 0xFF0E || cp == 0xFF1A || cp == 0xFF1B || cp == 0xFF1F
        || cp == 0xFF3B || cp == 0xFF3D || cp == 0xFF5B || cp == 0xFF5D
        || cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D;
}

bool isStretchableGap(const GlyphMeta& a, const GlyphMeta& b) {
    if (a.isWhitespace) return glyphCodepoint(a) != 0x00A0;
    if (b.isWhitespace) return false;
    const uint32_t ca = glyphCodepoint(a), cb = glyphCodepoint(b);
    return isCjk(ca) || isCjk(cb);
}

enum class LineAlign { Start, Center, End };

}

struct HorizontalLayout::Impl {
    struct QueuedLine {
        std::vector<GlyphMeta> glyphs;
        float xStart = 0.0f;
        float avail = 0.0f;
        bool justify = false;
        bool blank = false;
    };

    Options       opts;
    PageGeometry  geo;
    FontSet       fonts;
    Logger*       log;

    LayoutResult  result;
    PageGlyphs    currentPage;
    float         penY;
    int           pageIdx0;
    double        curLineHeightMul;
    bool          codeBlockActive;
    int           codeBoxRectIndex;
    LineAlign     curAlign = LineAlign::Start;
    bool          curJustify = false;
    bool          nextBlockIsText = false;
    std::map<uint64_t, int8_t> punctInkCache;

    float         colX = 0.0f;
    float         colW = 0.0f;
    float         colHang = 0.0f;
    int8_t        colIndex = -1;
    bool          skipHeadingAnchor = false;
    float contentLeft() const { return colW > 0.0f ? colX : (float)opts.marginLeft; }
    float contentWidthF() const { return colW > 0.0f ? colW : (float)geo.contentWidth(); }
    float contentRight() const {
        return colW > 0.0f ? colX + colW : (float)(geo.width - opts.marginRight);
    }
    float hangRoom() const { return colW > 0.0f ? colHang : (float)opts.marginRight * 0.8f; }
    void setColumn(int8_t index, float x, float w, float hang) {
        colIndex = index;
        colX = x;
        colW = w;
        colHang = hang;
    }
    void clearColumn() { setColumn(-1, 0.0f, 0.0f, 0.0f); }

    uint32_t      boxId = 0;
    uint8_t       boxGray = 0;
    float         boxTop = 0.0f;
    size_t        boxRectAt = 0, boxGlyphsAt = 0, boxImagesAt = 0;
    int           boxSavedLeft = 0, boxSavedRight = 0, boxSavedBottom = 0;
    float boxPadX() const { return (float)(opts.fontSize * 0.8); }
    float boxPadY() const { return (float)(opts.fontSize * 0.5); }

    void flushBoxRect(float bottom) {
        if (currentPage.glyphs.size() == boxGlyphsAt
            && currentPage.images.size() == boxImagesAt
            && currentPage.rects.size() == boxRectAt) return;
        PageGlyphs::Rect r;
        r.x = (float)boxSavedLeft;
        r.y = boxTop;
        r.w = (float)(geo.width - boxSavedRight - boxSavedLeft);
        r.h = std::max(0.0f, bottom - boxTop);
        r.fillGray = boxGray;
        r.borderWidth = 0;
        r.radius = (float)(opts.fontSize * 0.25);
        const size_t at = std::min(boxRectAt, currentPage.rects.size());
        currentPage.rects.insert(currentPage.rects.begin() + (ptrdiff_t)at, r);
    }
    void openBox(const Block& b) {
        const float bottom = (float)(geo.height - geo.marginBottom);
        if (penY > (float)opts.marginTop) penY += (float)(opts.fontSize * 0.3);
        if (penY + boxPadY() * 2.0f + bodyLineHeight(opts.lineHeightMul) > bottom) newPage();
        boxId = b.boxId;
        boxGray = b.boxGray;
        boxSavedLeft = opts.marginLeft;
        boxSavedRight = opts.marginRight;
        boxTop = penY;
        boxRectAt = currentPage.rects.size();
        boxGlyphsAt = currentPage.glyphs.size();
        boxImagesAt = currentPage.images.size();
        penY += boxPadY();
        const int pad = (int)std::lround(boxPadX());
        opts.marginLeft += pad;
        opts.marginRight += pad;
        geo.marginLeft += pad;
        geo.marginRight += pad;
        boxSavedBottom = geo.marginBottom;
        geo.marginBottom += (int)std::lround(boxPadY());
    }
    void closeBox() {
        if (boxId == 0) return;
        penY += boxPadY();
        flushBoxRect(penY);
        opts.marginLeft = boxSavedLeft;
        opts.marginRight = boxSavedRight;
        geo.marginLeft = boxSavedLeft;
        geo.marginRight = boxSavedRight;
        geo.marginBottom = boxSavedBottom;
        boxId = 0;
        penY += (float)(opts.fontSize * 0.6);
    }

    Impl(const Options& o, const FontSet& fs, Logger* l)
        : opts(o), geo(geometryFromOptions(o)), fonts(fs), log(l),
          penY((float)o.marginTop), pageIdx0(0), curLineHeightMul(o.lineHeightMul),
          codeBlockActive(false), codeBoxRectIndex(-1) {
        geo.marginBottom = footerContentBottomInsetPx(o);
        currentPage.pageNumber = 1;
    }

    FontFace* faceFor(FontSlot s) { return fonts.faceFor(s); }

    FontFace* metricFace(FontSlot s) {
        if (s == FontSlot::Fallback || s == FontSlot::Sans || s == FontSlot::Kai
            || s == FontSlot::Bold) {
            FontFace* reg = faceFor(FontSlot::Regular);
            if (reg && reg->isOpen()) return reg;
        }
        return faceFor(s);
    }

    double maxAscender(const std::vector<GlyphMeta>& line) {
        double a = 0;
        for (auto& gm : line) {
            FontFace* f = metricFace(gm.g.fontSlot);
            if (f) a = std::max(a, f->ascender(gm.g.fontPx));
        }
        return a;
    }
    double maxDescent(const std::vector<GlyphMeta>& line) {
        double d = 0;
        for (auto& gm : line) {
            FontFace* f = metricFace(gm.g.fontSlot);
            if (f) d = std::max(d, -f->descender(gm.g.fontPx));
        }
        return d;
    }
    static constexpr double kMinEmLeading = 1.05;
    double maxLineHeight(const std::vector<GlyphMeta>& line) {
        double h = 0;
        for (auto& gm : line) h = std::max(h, (double)gm.g.fontPx);
        return h * std::max(kMinEmLeading, curLineHeightMul);
    }
    double bodyLineHeight(double mul) {
        return opts.fontSize * std::max(kMinEmLeading, mul);
    }
    double baselineOffset(const std::vector<GlyphMeta>& line, double lineH,
                          double asc) {
        return asc + (lineH - (asc + maxDescent(line))) * 0.5;
    }

    int punctInk(const GlyphMeta& gm) {
        const uint32_t cp = glyphCodepoint(gm);
        if (!isCjkPunct(cp) || gm.g.advance < gm.g.fontPx * 0.8f) return 0;
        const uint64_t key = ((uint64_t)(uint8_t)gm.g.fontSlot << 56)
                           | ((uint64_t)gm.g.glyphIndex << 20)
                           | (uint64_t)(uint32_t)(gm.g.fontPx * 16.0f);
        auto it = punctInkCache.find(key);
        if (it != punctInkCache.end()) return it->second;
        int ink = 0;
        FontFace* f = faceFor(gm.g.fontSlot);
        FontFace::HMetrics m;
        if (f && f->glyphHMetrics(gm.g.glyphIndex, gm.g.fontPx, &m) && m.width > 0) {
            const float adv = gm.g.advance;
            if (m.bearingX + m.width <= adv * 0.56f) ink = -1;
            else if (m.bearingX >= adv * 0.44f) ink = 1;
        }
        punctInkCache.emplace(key, (int8_t)ink);
        return ink;
    }

    void compressPunctuation(std::vector<GlyphMeta>& all) {
        for (size_t i = 0; i + 1 < all.size(); ++i) {
            const uint32_t a = glyphCodepoint(all[i]);
            const uint32_t b = glyphCodepoint(all[i + 1]);
            if (!isCjkPunct(a) || !isCjkPunct(b)) continue;
            if (punctInk(all[i]) < 0) {
                all[i].g.advance *= 0.5f;
            } else if (punctInk(all[i + 1]) > 0) {
                const float half = all[i + 1].g.advance * 0.5f;
                all[i + 1].g.advance -= half;
                all[i + 1].xShift -= half;
            }
        }
    }

    bool pageDirty() const {
        return !currentPage.glyphs.empty()
            || !currentPage.images.empty()
            || !currentPage.hRules.empty()
            || !currentPage.textSpans.empty()
            || !currentPage.rects.empty();
    }

    void newPage() {
        if (!pageDirty()) {
            penY = (float)opts.marginTop;
            codeBoxRectIndex = -1;
            return;
        }
        if (boxId != 0) flushBoxRect(penY + boxPadY());
        result.pages.push_back(std::move(currentPage));
        ++pageIdx0;
        currentPage = PageGlyphs{};
        currentPage.pageNumber = pageIdx0 + 1;
        penY = (float)opts.marginTop;
        codeBoxRectIndex = -1;
        if (boxId != 0) {
            boxTop = penY;
            penY += boxPadY();
            boxRectAt = boxGlyphsAt = boxImagesAt = 0;
        }
    }

    void emitLine(std::vector<GlyphMeta>& line, float xStart, float avail = 0.0f,
                  bool justify = false) {
        if (line.empty()) return;
        double asc = maxAscender(line);
        double lineH = maxLineHeight(line);

        std::vector<float> gapExtra(line.size(), 0.0f);
        float natural = 0.0f;
        for (const auto& gm : line) natural += gm.g.advance;
        if (avail > 0.0f && justify && natural < avail) {
            GlyphMeta& last = line.back();
            if (punctInk(last) < 0) {
                natural -= last.g.advance * 0.5f;
                last.g.advance *= 0.5f;
            }
            float extra = avail - natural;
            int opportunities = 0;
            for (size_t k = 0; k + 1 < line.size(); ++k)
                if (isStretchableGap(line[k], line[k + 1])) ++opportunities;
            const float fontPx = line.front().g.fontPx;
            if (opportunities > 0 && extra / opportunities <= fontPx * 0.9f) {
                const float each = extra / opportunities;
                for (size_t k = 0; k + 1 < line.size(); ++k)
                    if (isStretchableGap(line[k], line[k + 1])) gapExtra[k] = each;
            }
        } else if (avail > 0.0f && natural < avail) {
            if (curAlign == LineAlign::Center) xStart += (avail - natural) * 0.5f;
            else if (curAlign == LineAlign::End) xStart += avail - natural;
        }

        if (penY + lineH > geo.height - geo.marginBottom) {
            newPage();
        }
        float baseline = penY + (float)baselineOffset(line, lineH, asc);

        if (codeBlockActive) {
            const float outerLeft = std::max(12.0f, (float)opts.marginLeft * 0.55f);
            const float outerRight = std::max(12.0f, (float)opts.marginRight * 0.55f);
            const float verticalPad = (float)(opts.fontSize * 0.42);
            if (codeBoxRectIndex < 0) {
                PageGlyphs::Rect bg;
                bg.x = outerLeft;
                bg.y = penY - verticalPad;
                bg.w = (float)geo.width - outerLeft - outerRight;
                bg.h = (float)lineH + verticalPad * 2.0f;
                bg.fillGray = 247;
                bg.borderGray = 187;
                bg.borderWidth = 3;
                bg.radius = std::max(14.0f, (float)opts.fontSize * 0.55f);
                currentPage.rects.push_back(bg);
                codeBoxRectIndex = (int)currentPage.rects.size() - 1;
            } else {
                auto& bg = currentPage.rects[(size_t)codeBoxRectIndex];
                bg.h = penY + (float)lineH + verticalPad - bg.y;
            }
        }

        float x = xStart;
        for (size_t k = 0; k < line.size(); ++k) {
            auto& gm = line[k];
            gm.g.x = x + gm.xShift;
            gm.g.y = baseline + gm.g.y;
            gm.g.advance += gapExtra[k];
            currentPage.glyphs.push_back(gm.g);
            x += gm.g.advance;
            gm.g.x = x - gm.g.advance;
        }

        size_t i = 0;
        while (i < line.size()) {
            FontSlot curSlot = line[i].g.fontSlot;
            float spanX = line[i].g.x;
            std::string spanUtf8;
            float spanEnd = spanX;
            size_t j = i;
            while (j < line.size() && line[j].g.fontSlot == curSlot) {
                spanUtf8 += line[j].g.sourceUtf8;
                spanEnd = line[j].g.x + line[j].g.advance;
                ++j;
            }
            PageGlyphs::TextSpan sp;
            sp.utf8 = std::move(spanUtf8);
            sp.x = spanX;
            sp.y = baseline - (float)asc;
            sp.w = spanEnd - spanX;
            sp.h = (float)lineH;
            sp.fontSize = line[i].g.fontPx;
            sp.baseline = baseline;
            sp.fontSlot = curSlot;
            currentPage.textSpans.push_back(std::move(sp));
            i = j;
        }

        if (!codeBlockActive) {
            PageGlyphs::LineInfo li;
            li.y = baseline - (float)asc;
            li.baseline = baseline;
            li.height = (float)lineH;
            li.xStart = contentLeft();
            li.xEnd = contentRight();
            li.column = colIndex;
            int slotCounts[256]{};
            for (const auto& gm : line) {
                const unsigned slot = (unsigned)(uint8_t)gm.g.fontSlot;
                ++slotCounts[slot];
                li.fontPx = std::max(li.fontPx, gm.g.fontPx);
            }
            int dominantSlot = 0;
            for (int slot = 1; slot < 256; ++slot) {
                if (slotCounts[slot] > slotCounts[dominantSlot])
                    dominantSlot = slot;
            }
            li.fontSlot = (FontSlot)(uint8_t)dominantSlot;
            currentPage.lines.push_back(li);
        }

        penY += (float)lineH;
        line.clear();
    }

    float queuedHeight(const QueuedLine& q) {
        return q.blank ? (float)bodyLineHeight(curLineHeightMul)
                       : (float)maxLineHeight(q.glyphs);
    }

    void emitParagraphLines(std::vector<QueuedLine>& lines,
                            int* firstPage = nullptr, float* firstY = nullptr,
                            float keepBelow = 0.0f) {
        size_t first = 0;
        bool started = false;
        const float pageBottom = (float)(geo.height - geo.marginBottom);
        auto pageHasContent = [&]() {
            return !currentPage.glyphs.empty() || !currentPage.images.empty()
                || !currentPage.rects.empty() || !currentPage.hRules.empty();
        };

        if (keepBelow > 0.0f && pageHasContent()) {
            float total = 0.0f;
            for (const auto& q : lines) total += queuedHeight(q);
            const float pageH = pageBottom - (float)opts.marginTop;
            if (penY + total + keepBelow > pageBottom && total + keepBelow < pageH)
                newPage();
        }

        while (first < lines.size()) {
            if (lines[first].blank && !pageHasContent()) {
                ++first;
                continue;
            }
            float y = penY;
            size_t fit = 0;
            while (first + fit < lines.size()) {
                const float h = queuedHeight(lines[first + fit]);
                if (y + h > pageBottom) break;
                y += h;
                ++fit;
            }

            if (fit == 0) {
                if (!pageHasContent()) fit = 1;
                else {
                    newPage();
                    continue;
                }
            }

            const size_t remaining = lines.size() - first;
            if (fit < remaining) {
                if (fit == 1 && remaining == 2 && pageHasContent()) {
                    newPage();
                    continue;
                }
                if (remaining - fit == 1 && fit > 1) --fit;
            }

            for (size_t i = 0; i < fit; ++i) {
                auto& queued = lines[first + i];
                if (!started) {
                    if (firstPage) *firstPage = pageIdx0;
                    if (firstY) *firstY = penY;
                    started = true;
                }
                if (queued.blank) {
                    if (pageHasContent()) penY += queuedHeight(queued);
                    continue;
                }
                emitLine(queued.glyphs, queued.xStart, queued.avail, queued.justify);
            }
            first += fit;
        }
    }

    void layoutTable(const Block& b) {
        if (b.rows.empty()) return;
        struct Placed {
            const TableCell* cell = nullptr;
            int r = 0, c = 0, cs = 1, rs = 1;
            std::vector<GlyphMeta> glyphs;
            float naturalWidth = 0;
            float longest = 0;
        };
        const int numRows = (int)b.rows.size();
        std::vector<Placed> placed;
        std::vector<std::vector<int>> occ(numRows);
        int numCols = 0;
        for (int r = 0; r < numRows; ++r) {
            int c = 0;
            for (const auto& cell : b.rows[r].cells) {
                while (c < (int)occ[r].size() && occ[r][c] >= 0) ++c;
                Placed p;
                p.cell = &cell;
                p.r = r;
                p.c = c;
                p.cs = std::clamp(cell.colspan, 1, 64);
                p.rs = std::clamp(cell.rowspan, 1, numRows - r);
                for (int dr = 0; dr < p.rs; ++dr) {
                    auto& row = occ[r + dr];
                    if ((int)row.size() < c + p.cs) row.resize(c + p.cs, -1);
                    for (int dc = 0; dc < p.cs; ++dc) row[c + dc] = (int)placed.size();
                }
                c += p.cs;
                numCols = std::max(numCols, c);
                placed.push_back(std::move(p));
            }
        }
        if (numCols == 0) return;
        for (auto& row : occ) row.resize(numCols, -1);

        const float contentW = (float)geo.contentWidth();
        const float padX = (float)(opts.fontSize * 0.55);
        const float padY = (float)(opts.fontSize * 0.28);
        const float minColW = (float)(opts.fontSize * 4.5);
        const float maxColW = numCols == 1 ? contentW : contentW * 0.48f;

        for (auto& p : placed) {
            const TableCell& cell = *p.cell;
            auto& glyphs = p.glyphs;
            const bool header = cell.isHeader;
            const double px = header ? opts.fontSize * 1.05 : opts.fontSize;
            for (const auto& r : cell.runs) {
                if (r.kind == InlineKind::LineBreak) {
                    GlyphMeta gm{};
                    gm.g.advance = 0.0f;
                    gm.g.fontSlot = FontSlot::Regular;
                    gm.g.fontPx = (float)px;
                    gm.g.sourceUtf8 = "\n";
                    gm.isWhitespace = true;
                    gm.breakAfter = true;
                    glyphs.push_back(gm);
                    continue;
                }
                FontSlot slot = header || r.kind == InlineKind::Strong
                    ? FontSlot::Bold : FontSlot::Regular;
                if (r.kind == InlineKind::Code) slot = FontSlot::Mono;
                double rpx = slot == FontSlot::Mono
                    ? std::max(px * 0.95, 12.0) : px;
                if (r.kind == InlineKind::Superscript || r.kind == InlineKind::Subscript)
                    rpx = std::max(8.0, px * 0.72);
                size_t before = glyphs.size();
                shapeRunInto(r.text, slot, fonts, rpx, glyphs);
                const bool nativeBold = slot == FontSlot::Bold && fonts.bold;
                for (size_t gi = before; gi < glyphs.size(); ++gi) {
                    glyphs[gi].g.syntheticBold =
                        (header || r.kind == InlineKind::Strong)
                        && !(nativeBold && glyphs[gi].g.fontSlot == FontSlot::Bold);
                    glyphs[gi].g.oblique = r.kind == InlineKind::Emphasis;
                    glyphs[gi].g.linkTarget = r.linkTarget;
                    if (r.kind == InlineKind::Superscript)
                        glyphs[gi].g.y -= (float)(px * 0.34);
                    else if (r.kind == InlineKind::Subscript)
                        glyphs[gi].g.y += (float)(px * 0.18);
                    if (r.kind == InlineKind::Code)
                        glyphs[gi].g.inkGray = 132;
                }
            }
            for (const auto& gm : glyphs) p.naturalWidth += gm.g.advance;
            float stretch = 0;
            for (size_t gi = 0; gi < glyphs.size(); ++gi) {
                const auto& gm = glyphs[gi];
                if (gm.isWhitespace) { stretch = 0; continue; }
                stretch += gm.g.advance;
                p.longest = std::max(p.longest, stretch);
                if (gi + 1 < glyphs.size() && canBreakBetween(gm, glyphs[gi + 1]))
                    stretch = 0;
            }
        }

        std::vector<float> naturalColW(numCols, 0.0f);
        std::vector<float> minWordColW(numCols, 0.0f);
        for (const auto& p : placed) {
            if (p.cs != 1) continue;
            naturalColW[p.c] = std::max(naturalColW[p.c],
                std::clamp(p.naturalWidth + padX * 2.0f, minColW, contentW));
            minWordColW[p.c] = std::max(minWordColW[p.c],
                std::min(p.longest + padX * 2.0f, maxColW));
        }
        std::vector<float> minColWs(numCols);
        const float minWrapColW = (float)(opts.fontSize * 3.0);
        for (int ci = 0; ci < numCols; ++ci) {
            if (naturalColW[ci] <= 0) naturalColW[ci] = minColW;
            minColWs[ci] = std::max(minWordColW[ci], std::min(minWrapColW, naturalColW[ci]));
            naturalColW[ci] = std::max(naturalColW[ci], minColWs[ci]);
        }
        for (const auto& p : placed) {
            if (p.cs == 1) continue;
            const int c1 = std::min(numCols, p.c + p.cs);
            auto grow = [&](std::vector<float>& w, float need) {
                float have = 0;
                for (int k = p.c; k < c1; ++k) have += w[k];
                if (need <= have) return;
                for (int k = p.c; k < c1; ++k)
                    w[k] += have > 0 ? (need - have) * w[k] / have : (need - have) / (float)(c1 - p.c);
            };
            grow(minColWs, std::min(p.longest + padX * 2.0f, contentW));
            grow(naturalColW, std::clamp(p.naturalWidth + padX * 2.0f, minColW, contentW));
            for (int k = p.c; k < c1; ++k) naturalColW[k] = std::max(naturalColW[k], minColWs[k]);
        }
        std::vector<char> cutOk(numCols + 1, 1);
        for (const auto& p : placed)
            for (int k = p.c + 1; k < p.c + p.cs && k < numCols; ++k) cutOk[k] = 0;
        std::vector<std::vector<int>> groups;
        {
            int start = 0;
            float groupW = 0;
            for (int ci = 0; ci < numCols; ++ci) {
                if (ci > start && groupW + minColWs[ci] > contentW) {
                    int cut = ci;
                    if (!cutOk[ci])
                        for (int k = ci - 1; k > start; --k)
                            if (cutOk[k]) { cut = k; break; }
                    std::vector<int> g;
                    for (int k = start; k < cut; ++k) g.push_back(k);
                    groups.push_back(std::move(g));
                    start = cut;
                    groupW = 0;
                    for (int k = start; k < ci; ++k) groupW += minColWs[k];
                }
                groupW += minColWs[ci];
            }
            std::vector<int> g;
            for (int k = start; k < numCols; ++k) g.push_back(k);
            groups.push_back(std::move(g));
        }
        while (groups.size() >= 2) {
            auto& prev = groups[groups.size() - 2];
            auto& last = groups.back();
            if (last.size() * 2 >= prev.size() || prev.size() < 2) break;
            if (!cutOk[prev.back()]) break;
            float lastW = 0;
            for (int ci : last) lastW += minColWs[ci];
            if (lastW + minColWs[prev.back()] > contentW) break;
            last.insert(last.begin(), prev.back());
            prev.pop_back();
        }

        auto wrapCell = [](const std::vector<GlyphMeta>& glyphs, float maxW) {
            std::vector<std::vector<GlyphMeta>> lines;
            std::vector<GlyphMeta> line;
            float width = 0;
            int lastBreak = -1;
            auto flushAt = [&](int at) {
                std::vector<GlyphMeta> head(line.begin(), line.begin() + at + 1);
                while (!head.empty() && head.back().isWhitespace) head.pop_back();
                lines.push_back(std::move(head));
                std::vector<GlyphMeta> tail(line.begin() + at + 1, line.end());
                while (!tail.empty() && tail.front().isWhitespace) tail.erase(tail.begin());
                line = std::move(tail);
                width = 0;
                lastBreak = -1;
                for (int i = 0; i < (int)line.size(); ++i) {
                    width += line[i].g.advance;
                    if (i + 1 < (int)line.size()
                        && canBreakBetween(line[i], line[i + 1])) {
                        lastBreak = i;
                    }
                }
            };
            for (const auto& gm : glyphs) {
                if (gm.g.sourceUtf8 == "\n" && gm.g.advance == 0.0f) {
                    lines.push_back(std::move(line));
                    line.clear();
                    width = 0;
                    lastBreak = -1;
                    continue;
                }
                line.push_back(gm);
                width += gm.g.advance;
                if (line.size() >= 2
                    && canBreakBetween(line[line.size() - 2], line.back())) {
                    lastBreak = (int)line.size() - 2;
                }
                if (width > maxW && line.size() > 1 && lastBreak >= 0) {
                    flushAt(lastBreak);
                } else if (width > maxW && line.size() > 1) {
                    flushAt((int)line.size() - 2);
                }
            }
            if (!line.empty()) lines.push_back(std::move(line));
            if (lines.empty()) lines.push_back({});
            return lines;
        };

        int headerRows = 0;
        if (!b.rows[0].cells.empty() && b.rows[0].cells[0].isHeader) {
            headerRows = 1;
            for (const auto& p : placed)
                if (p.r == 0) headerRows = std::max(headerRows, p.rs);
        }

        curLineHeightMul = std::max(1.25, opts.lineHeightMul);
        if (penY > (float)opts.marginTop) penY += (float)(opts.fontSize * 0.45);

        using CellLines = std::vector<std::vector<GlyphMeta>>;
        for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            if (groupIndex > 0) penY += (float)(opts.fontSize * 0.8);
            const auto& cols = groups[groupIndex];
            const int g0 = cols.front(), g1 = cols.back() + 1;
            std::vector<float> widths;
            float sumNatural = 0, sumMin = 0;
            for (int ci : cols) { sumNatural += naturalColW[ci]; sumMin += minColWs[ci]; }
            const float shortColW = (float)(opts.fontSize * 8.0);
            float shortNat = 0, longMin = 0, longNat = 0;
            for (int ci : cols) {
                if (naturalColW[ci] <= shortColW) shortNat += naturalColW[ci];
                else { longMin += minColWs[ci]; longNat += naturalColW[ci]; }
            }
            const bool shortFirst = sumNatural > contentW && longNat > 0
                                 && shortNat + longMin <= contentW;
            for (int ci : cols) {
                float w = naturalColW[ci];
                if (shortFirst) {
                    if (w > shortColW)
                        w = minColWs[ci] + (naturalColW[ci] - minColWs[ci])
                              * (contentW - shortNat - longMin) / std::max(1e-3f, longNat - longMin);
                } else if (sumNatural > contentW) {
                    w = sumMin >= contentW
                        ? minColWs[ci] * contentW / sumMin
                        : minColWs[ci] + (naturalColW[ci] - minColWs[ci])
                              * (contentW - sumMin) / std::max(1e-3f, sumNatural - sumMin);
                }
                widths.push_back(w);
            }
            const float xBase = (float)opts.marginLeft;
            std::vector<float> colX(cols.size() + 1, xBase);
            for (size_t k = 0; k < cols.size(); ++k) colX[k + 1] = colX[k] + widths[k];

            std::vector<CellLines> lines(placed.size());
            std::vector<float> cellH(placed.size(), 0.0f);
            std::vector<std::vector<int>> startsAt(numRows);
            std::vector<float> rowH(numRows, 0.0f);
            const float emptyH = padY * 2.0f + (float)opts.fontSize;
            for (size_t pi = 0; pi < placed.size(); ++pi) {
                const Placed& p = placed[pi];
                const int a = std::max(p.c, g0), z = std::min(p.c + p.cs, g1);
                if (a >= z) continue;
                const float w = colX[z - g0] - colX[a - g0];
                lines[pi] = wrapCell(p.glyphs, std::max(1.0f, w - padX * 2.0f));
                float h = padY * 2.0f;
                for (const auto& line : lines[pi])
                    h += line.empty() ? (float)opts.fontSize : (float)maxLineHeight(line);
                cellH[pi] = h;
                startsAt[p.r].push_back((int)pi);
                if (p.rs == 1) rowH[p.r] = std::max(rowH[p.r], h);
            }
            for (int r = 0; r < numRows; ++r)
                for (int ci = g0; ci < g1; ++ci)
                    if (occ[r][ci] < 0) rowH[r] = std::max(rowH[r], emptyH);
            std::vector<size_t> spanning;
            for (size_t pi = 0; pi < placed.size(); ++pi)
                if (placed[pi].rs > 1 && cellH[pi] > 0) spanning.push_back(pi);
            std::stable_sort(spanning.begin(), spanning.end(),
                             [&](size_t x, size_t y) { return placed[x].rs < placed[y].rs; });
            for (size_t pi : spanning) {
                const Placed& p = placed[pi];
                float sum = 0;
                for (int k = p.r; k < p.r + p.rs; ++k) sum += rowH[k];
                if (cellH[pi] > sum) rowH[p.r + p.rs - 1] += cellH[pi] - sum;
            }

            auto drawRows = [&](int from, int to) {
                std::vector<float> rowY(to - from + 1, penY);
                for (int k = from; k < to; ++k) rowY[k - from + 1] = rowY[k - from] + rowH[k];
                for (int k = from; k < to; ++k) {
                    for (int ci = g0; ci < g1; ++ci) {
                        if (occ[k][ci] >= 0) continue;
                        PageGlyphs::Rect hole;
                        hole.x = colX[ci - g0];
                        hole.y = rowY[k - from];
                        hole.w = widths[ci - g0];
                        hole.h = rowH[k];
                        hole.fillGray = k < headerRows ? 232 : 255;
                        hole.borderGray = 175;
                        hole.borderWidth = 1;
                        currentPage.rects.push_back(hole);
                    }
                    for (int pi : startsAt[k]) {
                        const Placed& p = placed[pi];
                        const int a = std::max(p.c, g0), z = std::min(p.c + p.cs, g1);
                        const int end = std::min(p.r + p.rs, to);
                        const float x = colX[a - g0], w = colX[z - g0] - colX[a - g0];
                        const float y = rowY[k - from], h = rowY[end - from] - y;
                        PageGlyphs::Rect cellBg;
                        cellBg.x = x;
                        cellBg.y = y;
                        cellBg.w = w;
                        cellBg.h = h;
                        cellBg.fillGray = k < headerRows ? 232 : 255;
                        cellBg.borderGray = 175;
                        cellBg.borderWidth = 1;
                        currentPage.rects.push_back(cellBg);
                        float lineY = y + padY;
                        if (p.rs > 1) lineY = y + std::max(padY, (h - cellH[pi]) * 0.5f + padY);
                        for (auto& line : lines[pi]) {
                            const float lineH = line.empty() ? (float)opts.fontSize
                                                            : (float)maxLineHeight(line);
                            const float asc = line.empty() ? (float)(opts.fontSize * 0.8)
                                                          : (float)maxAscender(line);
                            const float lead = line.empty() ? 0.0f
                                : (float)baselineOffset(line, lineH, asc) - asc;
                            const float baseline = lineY + lead + asc;
                            float gx = x + padX;
                            for (auto& gm : line) {
                                GlyphMeta placedGlyph = gm;
                                placedGlyph.g.x = gx;
                                placedGlyph.g.y = baseline + gm.g.y;
                                currentPage.glyphs.push_back(placedGlyph.g);
                                gx += gm.g.advance;
                            }
                            if (!line.empty()) {
                                PageGlyphs::TextSpan sp;
                                for (const auto& gm : line) sp.utf8 += gm.g.sourceUtf8;
                                sp.x = x + padX;
                                sp.y = lineY;
                                sp.w = w - padX * 2.0f;
                                sp.h = lineH;
                                sp.fontSize = line.front().g.fontPx;
                                sp.baseline = baseline;
                                sp.fontSlot = line.front().g.fontSlot;
                                currentPage.textSpans.push_back(std::move(sp));
                            }
                            lineY += lineH;
                        }
                    }
                }
                penY = rowY.back();
            };
            auto unitEnd = [&](int from) {
                int end = from + 1;
                for (int k = from; k < end; ++k)
                    for (int pi : startsAt[k]) end = std::max(end, k + placed[pi].rs);
                return std::min(end, numRows);
            };
            auto rowsHeight = [&](int from, int to) {
                float h = 0;
                for (int k = from; k < to; ++k) h += rowH[k];
                return h;
            };
            const float bottom = (float)(geo.height - geo.marginBottom);
            const float pageH = (float)geo.contentHeight();
            for (int r = 0; r < numRows;) {
                const int end = unitEnd(r);
                const float unitH = rowsHeight(r, end);
                if (penY + unitH > bottom) {
                    newPage();
                    if (r >= headerRows && headerRows > 0) {
                        const float headH = rowsHeight(0, headerRows);
                        if (headH < pageH / 4.0f) drawRows(0, headerRows);
                    }
                }
                if (unitH > pageH) {
                    for (int k = r; k < end; ++k) {
                        if (penY + rowH[k] > bottom && penY > (float)opts.marginTop + 1.0f) newPage();
                        drawRows(k, k + 1);
                    }
                } else {
                    drawRows(r, end);
                }
                r = end;
            }
        }
        penY += (float)opts.paragraphSpacing;
    }

    struct BuiltBlock {
        std::vector<QueuedLine> lines;
        int headingIndex = -1;
        bool headingLike = false;
    };

    bool buildBlock(const Block& b, BuiltBlock* built) {
        codeBlockActive = b.kind == BlockKind::CodeBlock;
        codeBoxRectIndex = -1;
        std::vector<GlyphMeta> all;
        std::vector<bool> isHardBreak;
        for (auto& r : b.runs) {
            if (r.kind == InlineKind::LineBreak) {
                GlyphMeta gm{};
                gm.g.advance = 0.0f;
                gm.g.fontSlot = FontSlot::Regular;
                gm.g.fontPx = (float)opts.fontSize;
                gm.isWhitespace = true;
                gm.breakAfter   = true;
                all.push_back(gm);
                isHardBreak.push_back(true);
                continue;
            }
            FontSlot slot = slotForRun(b, r, fonts);
            const double basePx = slotPx(opts, b, slot, r.cssFontSizeEm);
            double px = basePx;
            if (r.kind == InlineKind::Ruby) px = std::max(8.0, px * 0.55);
            if (r.kind == InlineKind::Superscript || r.kind == InlineKind::Subscript)
                px = std::max(8.0, px * 0.72);
            size_t before = all.size();
            shapeRunInto(r.text, slot, fonts, px, all);
            const bool strong = r.kind == InlineKind::Strong
                             || b.kind == BlockKind::Heading;
            const bool nativeBold = slot == FontSlot::Bold && fonts.bold;
            for (size_t gi = before; gi < all.size(); ++gi) {
                all[gi].g.syntheticBold = strong
                    && !(nativeBold && all[gi].g.fontSlot == FontSlot::Bold);
                all[gi].g.oblique = r.kind == InlineKind::Emphasis;
                all[gi].g.linkTarget = r.linkTarget;
                if (r.kind == InlineKind::Code && b.kind != BlockKind::CodeBlock)
                    all[gi].g.inkGray = 132;
                if (r.kind == InlineKind::Superscript)
                    all[gi].g.y -= (float)(basePx * 0.34);
                else if (r.kind == InlineKind::Subscript)
                    all[gi].g.y += (float)(basePx * 0.18);
            }
            isHardBreak.resize(all.size(), false);
        }
        if (all.empty()) {
            codeBlockActive = false;
            return false;
        }
        if (!codeBlockActive) compressPunctuation(all);
        float markerW = 0.0f;
        if ((b.kind == BlockKind::OrderedItem || b.kind == BlockKind::UnorderedItem)
                && !b.runs.empty() && b.runs[0].kind != InlineKind::LineBreak) {
            const std::string& m = b.runs[0].text;
            const bool isMarker = m.size() <= 8 && !m.empty() && m.back() == ' ';
            if (isMarker) {
                size_t n = 0;
                for (const auto& gm : all) {
                    if (n >= m.size()) break;
                    n += gm.g.sourceUtf8.size();
                    markerW += gm.g.advance;
                }
            }
        }
        curAlign = b.cssTextAlign == 1 ? LineAlign::Center
                 : b.cssTextAlign == 2 ? LineAlign::End : LineAlign::Start;
        curJustify = curAlign == LineAlign::Start && !codeBlockActive
                  && (b.kind == BlockKind::Paragraph || b.kind == BlockKind::OrderedItem
                      || b.kind == BlockKind::UnorderedItem);

        if (b.kind == BlockKind::Heading && penY > (float)opts.marginTop) {
            penY += (float)(opts.fontSize * (b.level <= 2 ? 0.8 : 0.45));
        } else if (codeBlockActive && penY > (float)opts.marginTop) {
            penY += (float)(opts.fontSize * 0.35);
        } else if (b.kind == BlockKind::MathBlock
                   && penY > (float)opts.marginTop) {
            penY += (float)(opts.fontSize * 0.4);
        }

        const bool regularRuledParagraph = opts.drawRuling
                                         && b.kind == BlockKind::Paragraph;
        curLineHeightMul = (!regularRuledParagraph && b.cssLineHeightMul >= 0)
                               ? (double)b.cssLineHeightMul
                               : opts.lineHeightMul;
        if ((!opts.drawRuling || b.kind != BlockKind::Paragraph)
            && b.cssMarginTopEm > 0
            && penY > (float)opts.marginTop) {
            penY += (float)(b.cssMarginTopEm * opts.fontSize);
        }

        float xStart = contentLeft();
        if (b.quoteDepth > 0) {
            xStart += (float)(b.quoteDepth * opts.fontSize * 1.6);
        }
        if (b.kind == BlockKind::OrderedItem || b.kind == BlockKind::UnorderedItem) {
            xStart += (float)((b.listDepth * 1.5 + 0.8) * opts.fontSize) + markerW;
        } else if (codeBlockActive) {
            const float outerLeft = std::max(12.0f, (float)opts.marginLeft * 0.55f);
            xStart = std::max(xStart, outerLeft + (float)(opts.fontSize * 0.9));
        } else if (b.kind == BlockKind::MathBlock) {
            float formulaW = 0.0f;
            for (const auto& gm : all) formulaW += gm.g.advance;
            if (formulaW < contentWidthF())
                xStart += (contentWidthF() - formulaW) * 0.5f;
        }
        float maxW = contentWidthF() - (xStart - contentLeft());
        if (codeBlockActive) {
            const float outerRight = std::max(12.0f, (float)opts.marginRight * 0.55f);
            maxW = (float)geo.width - outerRight
                 - (float)(opts.fontSize * 0.9) - xStart;
        }

        float indentPx = 0.0f;
        const double indentChars = (b.cssTextIndentEm >= 0)
                                       ? (double)b.cssTextIndentEm
                                       : opts.firstLineIndentChars;
        if (b.kind == BlockKind::Paragraph && indentChars != 0.0
                && curAlign == LineAlign::Start) {
            const double em = b.cssFontSizeEm > 0 ? opts.fontSize * b.cssFontSizeEm
                                                  : opts.fontSize;
            indentPx = (float)(indentChars * em);
        }
        if (markerW > 0.0f) indentPx = -markerW;
        bool firstLine = true;
        auto curXStart = [&]() { return xStart + (firstLine ? indentPx : 0.0f); };
        auto curMaxW   = [&]() { return maxW  - (firstLine ? indentPx : 0.0f); };

        int headingIndex = -1;
        if (b.kind == BlockKind::Heading && !skipHeadingAnchor) {
            std::string flat;
            for (auto& r : b.runs) flat += r.text;
            HeadingAnchor a{};
            a.title = flat;
            a.level = b.level;
            a.pageIndex0 = pageIdx0;
            a.yOnPage = penY;
            result.headings.push_back(std::move(a));
            headingIndex = (int)result.headings.size() - 1;
        }

        std::vector<QueuedLine> paragraphLines;
        std::vector<GlyphMeta> line;
        line.reserve(all.size());
        float lineW = 0.0f;
        int   lastBreak = -1;

        auto queue = [&](std::vector<GlyphMeta>&& glyphs, bool justify) {
            QueuedLine q;
            q.glyphs = std::move(glyphs);
            q.xStart = curXStart();
            q.avail = curMaxW();
            q.justify = justify && curJustify;
            paragraphLines.push_back(std::move(q));
            firstLine = false;
        };
        auto flush = [&](int upToIncl) {
            std::vector<GlyphMeta> head(line.begin(), line.begin() + upToIncl + 1);
            while (!head.empty() && head.back().isWhitespace) head.pop_back();
            queue(std::move(head), true);
            std::vector<GlyphMeta> tail(line.begin() + upToIncl + 1, line.end());
            while (!tail.empty() && tail.front().isWhitespace) tail.erase(tail.begin());
            line = std::move(tail);
            lineW = 0.0f;
            for (auto& gm : line) lineW += gm.g.advance;
            lastBreak = -1;
            for (int i = 0; i + 1 < (int)line.size(); ++i) {
                if (canBreakBetween(line[i], line[i + 1])) lastBreak = i;
            }
        };
        auto visibleW = [&]() {
            float w = lineW;
            for (int k = (int)line.size() - 1; k >= 0 && line[k].isWhitespace; --k)
                w -= line[k].g.advance;
            return w;
        };

        bool prevHardBreak = true;
        for (size_t i = 0; i < all.size(); ++i) {
            if (isHardBreak[i]) {
                if (!line.empty()) {
                    queue(std::move(line), false);
                    line.clear();
                } else if (prevHardBreak || paragraphLines.empty()) {
                    QueuedLine q;
                    q.blank = true;
                    paragraphLines.push_back(std::move(q));
                }
                prevHardBreak = true;
                lineW = 0.0f;
                lastBreak = -1;
                continue;
            }
            prevHardBreak = false;
            line.push_back(all[i]);
            lineW += all[i].g.advance;

            if (line.size() >= 2
                && canBreakBetween(line[line.size() - 2], line.back())) {
                lastBreak = (int)line.size() - 2;
            }

            while (visibleW() > curMaxW()) {
                const uint32_t cp = glyphCodepoint(line.back());
                const float marginRoom = hangRoom();
                const float hangingLimit = codeBlockActive ? 0.0f
                    : std::min(line.back().g.fontPx * 1.05f, marginRoom);
                if (linebreak::isHangingPunctuation(cp)
                    && visibleW() <= curMaxW() + hangingLimit) {
                    break;
                }
                if (lastBreak >= 0) {
                    flush(lastBreak);
                    continue;
                }
                if (line.size() == 1) {
                    std::vector<GlyphMeta> tmp = std::move(line);
                    queue(std::move(tmp), false);
                    line.clear();
                    lineW = 0.0f;
                    break;
                }
                int at = (int)line.size() - 2;
                while (at > 0 && linebreak::isLineStartForbidden(glyphCodepoint(line[at + 1])))
                    --at;
                flush(at);
            }
        }
        if (!line.empty()) {
            queue(std::move(line), false);
        }
        bool headingLike = b.kind == BlockKind::Heading;
        if (!headingLike && b.kind == BlockKind::Paragraph && paragraphLines.size() == 1
                && b.quoteDepth == 0) {
            bool allStrong = true, anyText = false;
            std::string flat;
            for (const auto& r : b.runs) {
                if (r.kind == InlineKind::LineBreak) continue;
                bool blank = true;
                for (char c : r.text) if (c != ' ') { blank = false; break; }
                if (blank) continue;
                anyText = true;
                if (r.kind != InlineKind::Strong) allStrong = false;
                flat += r.text;
            }
            const uint32_t last = flat.empty() ? 0 : textjoin::lastCodepoint(flat);
            const bool sentenceEnd = last == 0x3002 || last == '.' || last == 0xFF0C
                                  || last == ',' || last == 0x3001;
            headingLike = anyText && !sentenceEnd && flat.size() <= 90
                && (allStrong || b.cssFontSizeEm >= 1.1f);
        }
        built->lines = std::move(paragraphLines);
        built->headingIndex = headingIndex;
        built->headingLike = headingLike;
        return true;
    }

    float keepBelowFor(const BuiltBlock& built) {
        return (built.headingLike && nextBlockIsText)
            ? (float)(bodyLineHeight(opts.lineHeightMul) * 2.0 + opts.paragraphSpacing)
            : 0.0f;
    }

    void layoutBlock(const Block& b) {
        BuiltBlock built;
        if (!buildBlock(b, &built)) return;
        int paragraphStartPage = pageIdx0;
        float paragraphStartY = penY;
        emitParagraphLines(built.lines, &paragraphStartPage, &paragraphStartY,
                           keepBelowFor(built));
        if (built.headingIndex >= 0) {
            result.headings[(size_t)built.headingIndex].pageIndex0 = paragraphStartPage;
            result.headings[(size_t)built.headingIndex].yOnPage = paragraphStartY;
        }
        finishBlock(b);
    }

    void finishBlock(const Block& b) {
        if (!opts.drawRuling || b.kind != BlockKind::Paragraph) {
            penY += (b.cssMarginBottomEm >= 0)
                        ? (float)(b.cssMarginBottomEm * opts.fontSize)
                        : (float)opts.paragraphSpacing;
        }
        if (codeBlockActive) penY += (float)(opts.fontSize * 1.15);
        codeBlockActive = false;
        codeBoxRectIndex = -1;
        curAlign = LineAlign::Start;
        curJustify = false;
    }

    void layoutPair(const Block& src, const Block& dst) {
        const float gutter = (float)(opts.fontSize * 1.2);
        const float half = std::max(1.0f, ((float)geo.contentWidth() - gutter) * 0.5f);
        const float leftX = (float)opts.marginLeft;
        const float rightX = leftX + half + gutter;
        const float leftHang = gutter * 0.5f;
        const float rightHang = (float)opts.marginRight * 0.8f;
        auto useColumn = [&](int side) {
            if (side == 0) setColumn(0, leftX, half, leftHang);
            else setColumn(1, rightX, half, rightHang);
        };

        const float y0 = penY;
        BuiltBlock L, R;
        useColumn(0);
        const bool hasL = buildBlock(src, &L);
        const float yText = penY;
        penY = y0;
        useColumn(1);
        skipHeadingAnchor = true;
        const bool hasR = buildBlock(dst, &R);
        skipHeadingAnchor = false;
        if (hasL) penY = yText;
        if (!hasL && !hasR) {
            clearColumn();
            return;
        }

        const size_t rows = std::max(L.lines.size(), R.lines.size());
        auto rowH = [&](size_t i) {
            float h = 0.0f;
            if (i < L.lines.size()) h = std::max(h, queuedHeight(L.lines[i]));
            if (i < R.lines.size()) h = std::max(h, queuedHeight(R.lines[i]));
            return h;
        };
        auto rowBlank = [&](size_t i) {
            return (i >= L.lines.size() || L.lines[i].blank)
                && (i >= R.lines.size() || R.lines[i].blank);
        };
        const float pageBottom = (float)(geo.height - geo.marginBottom);
        auto pageHasContent = [&]() {
            return !currentPage.glyphs.empty() || !currentPage.images.empty()
                || !currentPage.rects.empty() || !currentPage.hRules.empty();
        };

        const float keepBelow = keepBelowFor(hasL ? L : R);
        if (keepBelow > 0.0f && pageHasContent()) {
            float total = 0.0f;
            for (size_t i = 0; i < rows; ++i) total += rowH(i);
            const float pageH = pageBottom - (float)opts.marginTop;
            if (penY + total + keepBelow > pageBottom && total + keepBelow < pageH)
                newPage();
        }

        int firstPage = pageIdx0;
        float firstY = penY;
        bool started = false;
        size_t first = 0;
        std::vector<float> rowY;
        while (first < rows) {
            if (rowBlank(first) && !pageHasContent()) {
                ++first;
                continue;
            }
            float y = penY;
            size_t fit = 0;
            while (first + fit < rows) {
                const float h = rowH(first + fit);
                if (y + h > pageBottom) break;
                y += h;
                ++fit;
            }
            if (fit == 0) {
                if (!pageHasContent()) fit = 1;
                else {
                    newPage();
                    continue;
                }
            }
            const size_t remaining = rows - first;
            if (fit < remaining) {
                if (fit == 1 && remaining == 2 && pageHasContent()) {
                    newPage();
                    continue;
                }
                if (remaining - fit == 1 && fit > 1) --fit;
            }
            if (!started) {
                firstPage = pageIdx0;
                firstY = penY;
                started = true;
            }
            rowY.assign(fit, 0.0f);
            float yy = penY;
            for (size_t k = 0; k < fit; ++k) {
                rowY[k] = yy;
                yy += rowH(first + k);
            }
            for (int side = 0; side < 2; ++side) {
                std::vector<QueuedLine>& lines = side == 0 ? L.lines : R.lines;
                useColumn(side);
                for (size_t k = 0; k < fit; ++k) {
                    const size_t i = first + k;
                    if (i >= lines.size() || lines[i].blank) continue;
                    penY = rowY[k];
                    emitLine(lines[i].glyphs, lines[i].xStart, lines[i].avail, lines[i].justify);
                }
            }
            penY = yy;
            first += fit;
        }
        clearColumn();
        if (L.headingIndex >= 0) {
            result.headings[(size_t)L.headingIndex].pageIndex0 = firstPage;
            result.headings[(size_t)L.headingIndex].yOnPage = firstY;
        }
        finishBlock(src);
    }

    void layoutTableTranslation(const Block& b) {
        Block t = b;
        t.rows = b.rowsDst;
        t.rowsDst.clear();
        const float bottom = (float)(geo.height - geo.marginBottom);
        penY += (float)(opts.fontSize * 0.5);
        if (penY + (float)bodyLineHeight(opts.lineHeightMul) * 3.0f > bottom) newPage();
        layoutTable(t);
    }
};

HorizontalLayout::HorizontalLayout(const Options& opts, const FontSet& fonts,
                                   Logger* log)
    : impl_(std::make_unique<Impl>(opts, fonts, log)) {}

HorizontalLayout::~HorizontalLayout() = default;

LayoutResult HorizontalLayout::run(const Document& doc) {
    impl_->result.blockPageIndex.reserve(doc.blocks.size());
    auto isTextBlock = [](const Block& b) {
        return b.kind == BlockKind::Paragraph || b.kind == BlockKind::OrderedItem
            || b.kind == BlockKind::UnorderedItem || b.kind == BlockKind::Blockquote
            || b.kind == BlockKind::CodeBlock || b.kind == BlockKind::Table
            || b.kind == BlockKind::Heading;
    };
    size_t pendingIndex = (size_t)-1;
    size_t pendingGlyphs = 0, pendingImages = 0, pendingRects = 0;
    auto settlePending = [&]() {
        if (pendingIndex == (size_t)-1) return;
        int& slot = impl_->result.blockPageIndex[pendingIndex];
        if ((size_t)slot < impl_->result.pages.size()) {
            const PageGlyphs& p = impl_->result.pages[(size_t)slot];
            if (p.glyphs.size() == pendingGlyphs && p.images.size() == pendingImages
                && p.rects.size() == pendingRects)
                ++slot;
        }
        pendingIndex = (size_t)-1;
    };
    size_t polled = 0;
    for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
        if ((polled++ & 31) == 0 && currentJobCancelled()) break;
        const Block& b = doc.blocks[bi];
        impl_->result.blockPageIndex.push_back((int)impl_->result.pages.size());
        pendingIndex = bi;
        pendingGlyphs = impl_->currentPage.glyphs.size();
        pendingImages = impl_->currentPage.images.size();
        pendingRects = impl_->currentPage.rects.size();
        impl_->nextBlockIsText = bi + 1 < doc.blocks.size() && isTextBlock(doc.blocks[bi + 1]);
        struct Settle {
            std::function<void()> f;
            ~Settle() { f(); }
        } settle{settlePending};
        if (b.kind != BlockKind::PageBreak && b.boxId != impl_->boxId) {
            impl_->closeBox();
            if (b.boxId != 0) impl_->openBox(b);
        }
        if (b.kind == BlockKind::PageBreak) {
            impl_->newPage();
            continue;
        }
        if (b.kind == BlockKind::ThematicBreak) {
            const float bottom = (float)(impl_->geo.height - impl_->geo.marginBottom);
            if (impl_->penY + (float)(impl_->opts.fontSize * 1.5) > bottom)
                impl_->newPage();
            PageGlyphs::HRule hr;
            hr.y = impl_->penY + (float)(impl_->opts.fontSize * 0.5);
            hr.xStart = (float)impl_->opts.marginLeft;
            hr.xEnd = (float)(impl_->geo.width - impl_->opts.marginRight);
            hr.gray = 200;
            hr.thickness = 2;
            impl_->currentPage.hRules.push_back(hr);
            impl_->penY += (float)(impl_->opts.fontSize * 1.5);
            continue;
        }
        if (b.kind == BlockKind::Table) {
            impl_->layoutTable(b);
            if (impl_->opts.bilingual && !b.rowsDst.empty()) impl_->layoutTableTranslation(b);
            continue;
        }
        if (impl_->opts.bilingual && impl_->opts.bilingualSideBySide() && b.pairSide == 0 && bi + 1 < doc.blocks.size()
                && doc.blocks[bi + 1].pairSide == 1 && doc.blocks[bi + 1].pairId == b.pairId) {
            impl_->nextBlockIsText = bi + 2 < doc.blocks.size() && isTextBlock(doc.blocks[bi + 2]);
            impl_->layoutPair(b, doc.blocks[bi + 1]);
            settlePending();
            impl_->result.blockPageIndex.push_back(impl_->result.blockPageIndex[bi]);
            ++bi;
            continue;
        }
        if (b.kind == BlockKind::Image && b.imageIndex >= 0
                && b.imageIndex < (int)doc.images.size()) {
            const auto& img = doc.images[b.imageIndex];
            if (img.width > 0 && img.height > 0) {
                float cw = (float)impl_->geo.contentWidth();
                if (b.imageFullPage) {
                    float fh = (float)impl_->geo.contentHeight();
                    float s = std::min(cw / img.width, fh / img.height);
                    float w = img.width * s;
                    float h = img.height * s;
                    if (!impl_->currentPage.glyphs.empty()
                            || !impl_->currentPage.images.empty())
                        impl_->newPage();
                    float x = (float)impl_->geo.marginLeft + (cw - w) * 0.5f;
                    float y = (float)impl_->geo.marginTop + (fh - h) * 0.5f;
                    impl_->currentPage.images.push_back(
                        PlacedImage{b.imageIndex, x, y, w, h});
                    impl_->newPage();
                    continue;
                }
                const float ch = (float)impl_->geo.contentHeight();
                const float scale = fitImageScale((float)img.width, (float)img.height,
                                                  cw, ch, imageDeviceScale(impl_->geo));
                if (scale <= 0.0f) continue;
                float w = img.width * scale;
                float h = img.height * scale;
                const float bottom = (float)(impl_->geo.height - impl_->geo.marginBottom);
                const bool pageDirty = !impl_->currentPage.glyphs.empty()
                                    || !impl_->currentPage.images.empty();
                const float gapAbove = pageDirty
                    ? (float)std::max(impl_->opts.paragraphSpacing, impl_->opts.fontSize * 0.5)
                    : 0.0f;
                const float room = bottom - impl_->penY - gapAbove
                    - (float)impl_->opts.fontSize;
                if (pageDirty && h > room && room >= h * 0.6f && room >= ch * 0.35f) {
                    const float k = room / h;
                    w *= k;
                    h *= k;
                }
                if (pageDirty && impl_->penY + gapAbove + h > bottom) impl_->newPage();
                else impl_->penY += gapAbove;
                float x = (float)impl_->opts.marginLeft + (cw - w) * 0.5f;
                PlacedImage pi{b.imageIndex, x, impl_->penY, w, h};
                impl_->currentPage.images.push_back(pi);
                impl_->penY += h + (float)std::max(impl_->opts.paragraphSpacing,
                                                   impl_->opts.fontSize * 0.5);
                if (impl_->penY + (float)impl_->opts.fontSize > bottom) impl_->newPage();
            }
            continue;
        }
        impl_->layoutBlock(b);
    }
    impl_->closeBox();
    if (!impl_->currentPage.glyphs.empty() || !impl_->currentPage.images.empty()
            || !impl_->currentPage.rects.empty() || !impl_->currentPage.hRules.empty()
            || !impl_->result.pages.empty()) {
        impl_->result.pages.push_back(std::move(impl_->currentPage));
    }
    return std::move(impl_->result);
}

}
