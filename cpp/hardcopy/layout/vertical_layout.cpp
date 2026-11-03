#include "vertical_layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <hb.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include "../cancel.h"
#include "../render/font_face.h"
#include "../hb_glue.h"
#include "geometry.h"
#include "line_break_rules.h"
#include "../parser/font_alias.h"
#include "../parser/zh_convert.h"
#include "../util/text_join.h"

namespace hardcopy {

namespace {

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

bool isCjk(uint32_t cp) {
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    if (cp >= 0x3040 && cp <= 0x30FF) return true;
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;
    if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
    return false;
}

bool isCjkPunctuation(uint32_t cp) {
    if (cp < 0x80) {
        switch (cp) {
            case '.': case ',': case ':': case ';':
            case '!': case '?': case '\'': case '"':
                return true;
            default:
                return false;
        }
    }
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    if (cp >= 0xFE10 && cp <= 0xFE19) return true;
    if (cp >= 0xFE30 && cp <= 0xFE4F) return true;
    if (cp >= 0xFF00 && cp <= 0xFF20) return true;
    if (cp >= 0xFF3B && cp <= 0xFF40) return true;
    if (cp >= 0xFF5B && cp <= 0xFF65) return true;
    switch (cp) {
        case 0x00B7: case 0x2014: case 0x2015:
        case 0x2018: case 0x2019: case 0x201C: case 0x201D:
        case 0x2026: case 0x2027:
            return true;
        default:
            return false;
    }
}

bool isCenteredPunctuation(uint32_t cp) {
    switch (cp) {
        case 0x3001: case 0x3002:
        case 0xFF0C: case 0xFF0E:
        case 0xFF1B:
        case 0xFF01: case 0xFF1F:
        case 0x00B7: case 0x30FB: case 0x2027:
        case 0xFE50: case 0xFE51: case 0xFE52:
        case 0xFE54: case 0xFE55: case 0xFE56: case 0xFE57:
        case 0xFF61: case 0xFF64:
            return true;
        default:
            return false;
    }
}

bool isAlwaysRotatedPunctuation(uint32_t cp) {
    switch (cp) {
        case 0xFF1A:
        case 0x2026: case 0x2025:
        case 0x22EF:
            return true;
        default:
            return false;
    }
}

bool isRotatedWhenNoVertForm(uint32_t cp) {
    switch (cp) {
        case 0x2013: case 0x2014: case 0x2015:
        case 0x2500: case 0x2501:
        case 0xFF0D: case 0xFF3F: case 0xFF5E:
        case 0x301C: case 0x3030:
        case 0xFF08: case 0xFF09:
        case 0xFF3B: case 0xFF3D:
        case 0xFF5B: case 0xFF5D:
        case 0x3008: case 0x3009:
        case 0x300A: case 0x300B:
        case 0x300C: case 0x300D:
        case 0x300E: case 0x300F:
        case 0x3010: case 0x3011:
        case 0x3014: case 0x3015:
        case 0x3016: case 0x3017:
        case 0x3018: case 0x3019:
        case 0x301A: case 0x301B:
        case 0xFF5F: case 0xFF60:
        case 0xFF1D:
            return true;
        default:
            return false;
    }
}

bool hasVertSubstitution(FontFace* face, uint32_t cp, uint32_t shapedGlyphIndex) {
    if (!face || !face->isOpen()) return false;
    uint32_t defaultGid = face->glyphIndexForChar(cp);
    return defaultGid != 0 && shapedGlyphIndex != defaultGid;
}

bool isDashLike(uint32_t cp) {
    switch (cp) {
        case 0x2013: case 0x2014: case 0x2015: case 0x2500: case 0x2501:
        case 0xFF0D: case 0xFF3F: case 0xFF5E: case 0x301C: case 0x3030:
        case 0x30FC: case 0xFF1D:
            return true;
        default:
            return false;
    }
}

bool shapedGlyphNeedsRotation(FontFace* face, uint32_t cp, uint32_t gid, float px) {
    FontFace::HMetrics hm;
    if (face && face->glyphHMetrics(gid, px, &hm) && hm.width > 0.25f && hm.height > 0.25f) {
        if (isDashLike(cp)) return hm.width > hm.height * 1.5f;
        return hm.height > hm.width * 1.15f;
    }
    return !hasVertSubstitution(face, cp, gid);
}

float emBoxAscent(FontFace* face, float px) {
    if (face && face->isOpen()) {
        FT_Face ft = (FT_Face)face->freetypeFace();
        auto* os2 = (TT_OS2*)FT_Get_Sfnt_Table(ft, FT_SFNT_OS2);
        if (os2 && ft->units_per_EM > 0
            && os2->sTypoAscender - os2->sTypoDescender == (FT_Short)ft->units_per_EM
            && os2->sTypoAscender > 0)
            return px * (float)os2->sTypoAscender / (float)ft->units_per_EM;
    }
    return px * 0.88f;
}

bool isLatinOpener(uint32_t cp) {
    return cp == '(' || cp == '[' || cp == '{' || cp == '"' || cp == '\'' || cp == 0x2018 || cp == 0x201C;
}
bool isLatinCloser(uint32_t cp) {
    switch (cp) {
        case ')': case ']': case '}': case '.': case ',': case ';': case ':':
        case '!': case '?': case '\'': case '"': case '%': case 0x2019: case 0x201D:
            return true;
        default:
            return false;
    }
}

bool isSidewaysLetter(uint32_t cp) {
    if (cp < 0x80) return std::isalnum((int)cp) != 0;
    if (cp >= 0x00C0 && cp <= 0x024F) return cp != 0x00D7 && cp != 0x00F7;
    if (cp >= 0x0370 && cp <= 0x03FF) return true;
    if (cp >= 0x0400 && cp <= 0x04FF) return true;
    return false;
}
bool isSidewaysJoiner(uint32_t cp) {
    if (cp == 0x00A0) return true;
    if (cp == 0x2019) return true;
    if (cp >= 0x20 && cp < 0x7F) return !std::isalnum((int)cp);
    return false;
}

struct VGlyph {
    PlacedGlyph g;
    bool        breakAfter   = false;
    bool        isWhitespace = false;
    uint32_t    cp           = 0;
    float       yoff         = 0.f;
    float       hAdvance     = 0.f;
    int8_t      tcyIndex     = -1;
    bool        synthetic    = false;
    float       emAscent     = 0.f;
};

enum class RunOrientation : uint8_t { Upright, Sideways, Tcy };

struct OrientedSegment {
    int start;
    int end;
    RunOrientation orientation;
};

int utf8CharLen(const std::string& s, int off);

std::vector<OrientedSegment> segmentOrientation(const std::string& text) {
    std::vector<OrientedSegment> segs;
    const int len = (int)text.size();
    int off = 0;
    auto push = [&](int s, int e, RunOrientation o) {
        if (e <= s) return;
        if (!segs.empty() && segs.back().orientation == o
            && segs.back().orientation == RunOrientation::Upright
            && segs.back().end == s) {
            segs.back().end = e;
        } else {
            segs.push_back(OrientedSegment{s, e, o});
        }
    };
    while (off < len) {
        uint32_t cp = utf8At(text, off);
        int runStart = off;
        if (!isSidewaysLetter(cp)) {
            int e = off + utf8CharLen(text, off);
            if (isLatinOpener(cp) && e < len && isSidewaysLetter(utf8At(text, e))) {
                off = e;
            } else {
                push(off, e, RunOrientation::Upright);
                off = e;
                continue;
            }
        }
        int lastLetterEnd = off;
        int digits = 0, letters = 0;
        while (off < len) {
            uint32_t c = utf8At(text, off);
            int e = off + utf8CharLen(text, off);
            if (isSidewaysLetter(c)) {
                lastLetterEnd = e;
                if (c < 0x80 && std::isdigit((int)c)) ++digits; else ++letters;
            } else if (!isSidewaysJoiner(c)) {
                break;
            }
            off = e;
        }
        int runEnd = lastLetterEnd;
        while (runEnd < len && isLatinCloser(utf8At(text, runEnd)))
            runEnd += utf8CharLen(text, runEnd);
        const int runChars = runEnd - runStart;
        RunOrientation o = RunOrientation::Sideways;
        if (letters == 0 && digits == runChars) {
            if (digits == 1) o = RunOrientation::Upright;
            else if (digits == 2) o = RunOrientation::Tcy;
        }
        push(runStart, runEnd, o);
        off = runEnd;
    }
    return segs;
}

void shapeSingleVertical(const std::string& text, FontFace* face, FontSlot slot,
                         double px, std::vector<VGlyph>& out) {
    if (!face || !face->isOpen() || text.empty()) return;

    HbGuard hbGuard;
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.c_str(), (int)text.size(), 0, (int)text.size());
    hb_buffer_set_direction(buf, HB_DIRECTION_TTB);
    hb_buffer_guess_segment_properties(buf);

    hb_font_t* hbf = (hb_font_t*)face->harfbuzzFont(px);
    static hb_feature_t vfeat[3];
    static const bool vfeatInit = [] {
        hb_feature_from_string("vert", -1, &vfeat[0]);
        hb_feature_from_string("vrt2", -1, &vfeat[1]);
        hb_feature_from_string("vkrn", -1, &vfeat[2]);
        return true;
    }();
    (void)vfeatInit;
    hb_shape(hbf, buf, vfeat, 3);

    unsigned int n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    const bool synthetic = true;
    const float emAscent = emBoxAscent(face, (float)px);

    for (unsigned i = 0; i < n; ++i) {
        VGlyph vg{};
        vg.g.glyphIndex = info[i].codepoint;
        vg.g.x = pos[i].x_offset / 64.0f;
        vg.g.y = 0;
        vg.yoff = pos[i].y_offset / 64.0f;
        vg.g.advance = (-pos[i].y_advance) / 64.0f;
        if (vg.g.advance == 0) vg.g.advance = (float)px;
        if (synthetic) {
            vg.synthetic = true;
            vg.emAscent = emAscent;
            vg.hAdvance = hb_font_get_glyph_h_advance(hbf, info[i].codepoint) / 64.0f;
            vg.g.x = -vg.hAdvance * 0.5f;
            vg.yoff = 0.f;
            vg.g.advance = (float)px;
        }
        vg.g.fontSlot = slot;
        vg.g.fontPx = (float)px;

        int byteStart = (int)info[i].cluster;
        int byteEnd   = (i + 1 < n) ? (int)info[i + 1].cluster : (int)text.size();
        if (byteEnd > byteStart && byteEnd <= (int)text.size()) {
            vg.g.sourceUtf8 = text.substr(byteStart, byteEnd - byteStart);
        }

        uint32_t cp = utf8At(text, byteStart);
        vg.cp = cp;
        vg.isWhitespace = (cp == ' ' || cp == '\t' || cp == 0x00A0);
        if (cp == ' ' || cp == 0x00A0) vg.g.advance = (float)px * 0.3f;
        if (cp == 0x2014 || cp == 0x2015) vg.g.advance = (float)px;
        vg.breakAfter = (vg.isWhitespace && cp != 0x00A0) || isCjk(cp)
                     || textjoin::isWideScript(cp) || (cp >= 0x2010 && cp <= 0x2027);

        out.push_back(vg);
    }
    hb_buffer_destroy(buf);
}

void shapeSingleSideways(const std::string& text, FontFace* face, FontSlot slot,
                         double px, bool tcy, std::vector<VGlyph>& out) {
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

    const size_t first = out.size();
    for (unsigned i = 0; i < n; ++i) {
        VGlyph vg{};
        vg.g.glyphIndex = info[i].codepoint;
        vg.g.x = pos[i].x_offset / 64.0f;
        vg.g.y = 0;
        vg.yoff = pos[i].y_offset / 64.0f;
        vg.hAdvance = pos[i].x_advance / 64.0f;
        vg.g.fontSlot = slot;
        vg.g.fontPx = (float)px;
        vg.g.rotated = !tcy;

        int byteStart = (int)info[i].cluster;
        int byteEnd   = (i + 1 < n) ? (int)info[i + 1].cluster : (int)text.size();
        if (byteEnd > byteStart && byteEnd <= (int)text.size()) {
            vg.g.sourceUtf8 = text.substr(byteStart, byteEnd - byteStart);
        }
        uint32_t cp = utf8At(text, byteStart);
        vg.cp = cp;
        vg.isWhitespace = (cp == ' ' || cp == '\t' || cp == 0x00A0);
        if (tcy) {
            vg.tcyIndex = (int8_t)std::min<unsigned>(i, 1);
            vg.g.advance = (i + 1 == n) ? (float)px : 0.0f;
            vg.breakAfter = false;
        } else {
            vg.g.advance = vg.hAdvance;
            vg.breakAfter = vg.isWhitespace || cp == '-' || cp == '/';
        }
        out.push_back(vg);
    }
    if (out.size() > first) out.back().breakAfter = true;
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

void shapeRunVerticalOriented(const std::string& text, FontSlot preferredSlot,
                              const FontSet& fonts, double px,
                              RunOrientation orientation,
                              std::vector<VGlyph>& out);

void shapeRunVertical(const std::string& text, FontSlot preferredSlot,
                      const FontSet& fonts, double px, std::vector<VGlyph>& out) {
    if (text.empty()) return;
    for (const auto& seg : segmentOrientation(text)) {
        shapeRunVerticalOriented(text.substr(seg.start, seg.end - seg.start),
                                 preferredSlot, fonts, px, seg.orientation, out);
    }
}

void shapeRunVerticalOriented(const std::string& text, FontSlot preferredSlot,
                              const FontSet& fonts, double px,
                              RunOrientation orientation,
                              std::vector<VGlyph>& out) {
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
        const std::string piece = text.substr(runStart, off - runStart);
        switch (orientation) {
            case RunOrientation::Upright:
                shapeSingleVertical(piece, c.face, c.slot, px, out);
                break;
            case RunOrientation::Sideways:
                shapeSingleSideways(piece, c.face, c.slot, px, false, out);
                break;
            case RunOrientation::Tcy:
                if (off - runStart == (int)text.size())
                    shapeSingleSideways(piece, c.face, c.slot, px, true, out);
                else
                    shapeSingleVertical(piece, c.face, c.slot, px, out);
                break;
        }
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

std::string cornerBracketQuotes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 2 < s.size()) {
            if (s[i] == '\xE2' && s[i + 1] == '\x80') {
                const char c = s[i + 2];
                const char* rep = nullptr;
                if (c == '\x9C')      rep = "\xE3\x80\x8C";
                else if (c == '\x9D') rep = "\xE3\x80\x8D";
                else if (c == '\x98') rep = "\xE3\x80\x8E";
                else if (c == '\x99') {
                    const bool letterBefore = i > 0 && std::isalpha((unsigned char)s[i - 1]);
                    const bool letterAfter = i + 3 < s.size()
                        && std::isalpha((unsigned char)s[i + 3]);
                    if (!(letterBefore && letterAfter))
                        rep = "\xE3\x80\x8F";
                }
                if (rep) { out += rep; i += 3; continue; }
            }
        }
        out += s[i++];
    }
    return out;
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

}

struct VerticalLayout::Impl {
    Options       opts;
    PageGeometry  geo;
    FontSet       fonts;
    Logger*       log;

    LayoutResult  result;
    PageGlyphs    currentPage;

    float penX;
    float penY;
    float colTop;
    float colWidth;
    float curColWidth;
    float pendingGap = 0.f;
    bool  continueColumn = false;
    float blockTopInset = 0.f;
    bool  centerPunctuation = false;
    int   pageIdx0;

    uint32_t boxId = 0;
    uint8_t  boxGray = 0;
    size_t   boxSpanAt = 0, boxImagesAt = 0, boxRectAt = 0;
    int      boxSavedTop = 0, boxSavedBottom = 0;
    int      boxSavedLeft = 0, boxSavedRight = 0;
    float boxPad() const { return (float)opts.fontSize * 0.6f; }

    void flushBoxRect() {
        float x0 = 1e9f, x1 = -1e9f;
        for (size_t i = boxSpanAt; i < currentPage.textSpans.size(); ++i) {
            const auto& sp = currentPage.textSpans[i];
            x0 = std::min(x0, sp.x);
            x1 = std::max(x1, sp.x + sp.w);
        }
        for (size_t i = boxImagesAt; i < currentPage.images.size(); ++i) {
            const auto& im = currentPage.images[i];
            x0 = std::min(x0, im.x);
            x1 = std::max(x1, im.x + im.w);
        }
        if (x1 <= x0) return;
        PageGlyphs::Rect r;
        r.x = x0 - boxPad();
        r.w = x1 - x0 + boxPad() * 2.0f;
        r.y = (float)boxSavedTop;
        r.h = (float)(geo.height - boxSavedBottom - boxSavedTop);
        r.fillGray = boxGray;
        r.borderWidth = 0;
        r.radius = (float)opts.fontSize * 0.25f;
        const size_t at = std::min(boxRectAt, currentPage.rects.size());
        currentPage.rects.insert(currentPage.rects.begin() + (ptrdiff_t)at, r);
    }
    void openBox(const Block& b) {
        if (columnDirty()) newColumn(std::max(pendingGap, boxPad() * 2.0f));
        pendingGap = 0.f;
        boxId = b.boxId;
        boxGray = b.boxGray;
        boxSavedTop = geo.marginTop;
        boxSavedBottom = geo.marginBottom;
        const int pad = (int)std::lround(boxPad());
        geo.marginTop += pad;
        geo.marginBottom += pad;
        boxSavedLeft = geo.marginLeft;
        boxSavedRight = geo.marginRight;
        geo.marginLeft += pad;
        geo.marginRight += pad;
        if (!columnDirty()) {
            colTop = (float)geo.marginTop;
            penY = colTop + blockTopInset;
            const float maxCenter = (float)(geo.width - geo.marginRight) - curColWidth * 0.5f;
            if (penX > maxCenter) penX = maxCenter;
        }
        boxSpanAt = currentPage.textSpans.size();
        boxImagesAt = currentPage.images.size();
        boxRectAt = currentPage.rects.size();
    }
    void closeBox() {
        if (boxId == 0) return;
        flushBoxRect();
        geo.marginTop = boxSavedTop;
        geo.marginBottom = boxSavedBottom;
        geo.marginLeft = boxSavedLeft;
        geo.marginRight = boxSavedRight;
        boxId = 0;
        pendingGap = std::max(pendingGap, boxPad() * 2.0f);
    }

    Impl(const Options& o, const FontSet& fs, Logger* l)
        : opts(o), geo(geometryFromOptions(o)), fonts(fs),
          log(l), pageIdx0(0) {
        geo.marginBottom = footerContentBottomInsetPx(o);
        colWidth = (float)opts.fontSize * (float)opts.lineHeightMul;
        resetPagePen();
        currentPage.pageNumber = 1;
    }

    FontFace* faceFor(FontSlot s) { return fonts.faceFor(s); }

    void resetPagePen() {
        penX = (float)(geo.width - geo.marginRight) - colWidth * 0.5f;
        curColWidth = colWidth;
        colTop = (float)geo.marginTop;
        penY = colTop;
        pendingGap = 0.f;
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
            resetPagePen();
            return;
        }
        if (boxId != 0) flushBoxRect();
        result.pages.push_back(std::move(currentPage));
        ++pageIdx0;
        currentPage = PageGlyphs{};
        currentPage.pageNumber = pageIdx0 + 1;
        resetPagePen();
        boxSpanAt = boxImagesAt = boxRectAt = 0;
    }

    void newColumn(float extraGap = 0.f) {
        const float leftEdge = penX - curColWidth * 0.5f;
        penX = leftEdge - extraGap - colWidth * 0.5f;
        curColWidth = colWidth;
        if (penX - colWidth * 0.5f < (float)geo.marginLeft - 0.5f) {
            newPage();
            return;
        }
        colTop = (float)geo.marginTop;
        penY = colTop + blockTopInset;
    }

    bool columnDirty() const { return penY > colTop + blockTopInset + 0.5f; }

    float bodyColumnWidth() const {
        return (float)opts.fontSize * (float)opts.lineHeightMul;
    }
    float cssGapPx(float em) const {
        return em >= 0 ? (float)(em * opts.fontSize) : -1.f;
    }
    float headingMinGap() const {
        return std::max((float)opts.paragraphSpacing, bodyColumnWidth());
    }
    float blockGapBefore(const Block& b) const {
        const float css = cssGapPx(b.cssMarginTopEm);
        if (b.kind == BlockKind::Heading) return std::max(css, headingMinGap());
        return std::max(css, 0.f);
    }
    float blockGapAfter(const Block& b) const {
        const float css = cssGapPx(b.cssMarginBottomEm);
        if (b.kind == BlockKind::Heading) return std::max(css, headingMinGap());
        return std::max(css, (float)opts.paragraphSpacing);
    }
    float figureGap() const {
        return std::max((float)opts.paragraphSpacing, bodyColumnWidth() * 0.5f);
    }

    void openBlock(float gapBefore) {
        if (continueColumn) {
            continueColumn = false;
            pendingGap = 0.f;
            return;
        }
        if (columnDirty()) newColumn(std::max(pendingGap, gapBefore));
        pendingGap = 0.f;
    }
    void closeBlock(float gapAfter) {
        pendingGap = std::max(pendingGap, gapAfter);
    }

    void emitColumnSlice(std::vector<VGlyph>& glyphs, size_t from, size_t to,
                         bool centerVertically = false, bool keepLeadingSpace = false) {
        if (from > to || from >= glyphs.size()) return;
        while (!keepLeadingSpace && from < to && glyphs[from].isWhitespace
               && glyphs[from].g.advance > 0) ++from;
        while (to > from && glyphs[to].isWhitespace && glyphs[to].g.advance > 0) --to;
        const float sliceTop = penY;
        const size_t glyphBase = currentPage.glyphs.size();
        const size_t spanBase = currentPage.textSpans.size();
        const size_t rectBase = currentPage.rects.size();
        for (size_t i = from; i <= to; ++i) {
            VGlyph& vg = glyphs[i];
            FontFace* face = faceFor(vg.g.fontSlot);
            const float px = vg.g.fontPx;
            const float asc  = face ? (float)face->ascender(px)  :  px * 0.88f;
            const float desc = face ? (float)face->descender(px) : -px * 0.12f;
            const float centredBaseline = penY + px * 0.5f + (asc + desc) * 0.5f;
            float gx, gy;

            if (vg.g.rotated) {
                gx = penX - (asc + desc) * 0.5f + vg.yoff;
                gy = penY + vg.g.x;
            } else if (vg.tcyIndex >= 0) {
                float w0 = vg.hAdvance, w1 = 0.f;
                if (vg.tcyIndex == 0) {
                    if (i + 1 <= to && glyphs[i + 1].tcyIndex == 1) w1 = glyphs[i + 1].hAdvance;
                } else if (i > from && glyphs[i - 1].tcyIndex == 0) {
                    w1 = w0; w0 = glyphs[i - 1].hAdvance;
                }
                const float pairLeft = penX - (w0 + w1) * 0.5f;
                gx = pairLeft + (vg.tcyIndex == 0 ? 0.f : w0) + vg.g.x;
                gy = centredBaseline - vg.yoff;
            } else {
                gx = penX + vg.g.x;
                if (vg.synthetic) gy = penY + vg.emAscent;
                else gy = (vg.yoff != 0.f) ? penY - vg.yoff : centredBaseline;

                FontFace::HMetrics hm;
                if ((vg.cp == 0x2014 || vg.cp == 0x2015) && face) {
                    const uint32_t baseGid = face->glyphIndexForChar(vg.cp);
                    float t = px * 0.06f;
                    if (baseGid && face->glyphHMetrics(baseGid, px, &hm) && hm.height > 0.25f)
                        t = std::clamp(hm.height, px * 0.04f, px * 0.10f);
                    const bool prevDash = i > from
                        && (glyphs[i - 1].cp == 0x2014 || glyphs[i - 1].cp == 0x2015);
                    const bool nextDash = i < to
                        && (glyphs[i + 1].cp == 0x2014 || glyphs[i + 1].cp == 0x2015);
                    const float inset = px * 0.06f;
                    PageGlyphs::Rect bar;
                    bar.x = penX - t * 0.5f;
                    bar.y = penY + (prevDash ? 0.f : inset);
                    bar.w = t;
                    bar.h = vg.g.advance - (prevDash ? 0.f : inset) - (nextDash ? 0.f : inset);
                    bar.fillGray = 0;
                    bar.borderWidth = 0;
                    currentPage.rects.push_back(bar);
                    const uint32_t spaceGid = face->glyphIndexForChar(' ');
                    vg.g.hasTextOrigin = true;
                    vg.g.textX = gx;
                    vg.g.textY = gy;
                    if (spaceGid) vg.g.glyphIndex = spaceGid;
                    vg.g.x = gx;
                    vg.g.y = gy;
                    currentPage.glyphs.push_back(vg.g);
                    penY += vg.g.advance;
                    continue;
                }
                const bool forceRotate = isAlwaysRotatedPunctuation(vg.cp);
                const bool vertMissing = face
                    && !hasVertSubstitution(face, vg.cp, vg.g.glyphIndex);
                const bool rotateHorizontalForm = face && isRotatedWhenNoVertForm(vg.cp)
                    && shapedGlyphNeedsRotation(face, vg.cp, vg.g.glyphIndex, px);
                if (face && (forceRotate || rotateHorizontalForm)) {
                    const uint32_t baseGid = face->glyphIndexForChar(vg.cp);
                    if (baseGid != 0) vg.g.glyphIndex = baseGid;
                    if (face->glyphHMetrics(vg.g.glyphIndex, px, &hm)
                        && hm.width > 0.25f && hm.height > 0.25f) {
                        vg.g.rotated = true;
                        gx = penX - (hm.bearingY - hm.height * 0.5f);
                        gy = penY + vg.g.advance * 0.5f - (hm.bearingX + hm.width * 0.5f);
                    }
                } else if (isCenteredPunctuation(vg.cp) && face
                    && face->glyphHMetrics(vg.g.glyphIndex, px, &hm)
                    && hm.width > 0.25f && hm.height > 0.25f) {
                    vg.g.hasTextOrigin = true;
                    vg.g.textX = gx;
                    vg.g.textY = gy;
                    const bool sentenceMark = vg.cp == 0x3001 || vg.cp == 0x3002
                        || vg.cp == 0xFF0C || vg.cp == 0xFF0E || vg.cp == 0xFF61
                        || vg.cp == 0xFF64 || (vg.cp >= 0xFE50 && vg.cp <= 0xFE52);
                    if (centerPunctuation || !sentenceMark) {
                        gx = penX - (hm.bearingX + hm.width * 0.5f);
                        gy = penY + vg.g.advance * 0.5f + hm.bearingY - hm.height * 0.5f;
                    } else {
                        const float pad = px * 0.12f;
                        gx = penX + px * 0.5f - pad - (hm.bearingX + hm.width);
                        gy = penY + pad + hm.bearingY;
                    }
                } else if (isCjkPunctuation(vg.cp) && vertMissing
                           && face->glyphHMetrics(vg.g.glyphIndex, px, &hm)
                           && hm.width > 0.25f) {
                    vg.g.hasTextOrigin = true;
                    vg.g.textX = gx;
                    vg.g.textY = gy;
                    gx = penX - (hm.bearingX + hm.width * 0.5f);
                }
            }
            vg.g.x = gx;
            vg.g.y = gy;
            currentPage.glyphs.push_back(vg.g);
            penY += vg.g.advance;
        }
        size_t i = from;
        float spanTop = sliceTop;
        while (i <= to) {
            FontSlot curSlot = glyphs[i].g.fontSlot;
            std::string spanUtf8;
            float spanBottom = spanTop;
            size_t j = i;
            while (j <= to && glyphs[j].g.fontSlot == curSlot) {
                spanUtf8 += glyphs[j].g.sourceUtf8;
                spanBottom += glyphs[j].g.advance;
                ++j;
            }
            PageGlyphs::TextSpan sp;
            sp.utf8 = std::move(spanUtf8);
            sp.x = penX - colWidth * 0.5f;
            sp.y = spanTop;
            sp.w = colWidth;
            sp.h = spanBottom - spanTop;
            sp.fontSize = glyphs[i].g.fontPx;
            sp.baseline = 0;
            sp.fontSlot = curSlot;
            currentPage.textSpans.push_back(std::move(sp));
            spanTop = spanBottom;
            i = j;
        }

        if (centerVertically) {
            const float availableH = (float)(geo.height - geo.marginBottom) - sliceTop;
            const float usedH = penY - sliceTop;
            const float offset = (availableH - usedH) * 0.5f;
            if (offset > 1.0f) {
                for (size_t gi = glyphBase; gi < currentPage.glyphs.size(); ++gi)
                    currentPage.glyphs[gi].y += offset;
                for (size_t si = spanBase; si < currentPage.textSpans.size(); ++si)
                    currentPage.textSpans[si].y += offset;
                for (size_t ri = rectBase; ri < currentPage.rects.size(); ++ri)
                    currentPage.rects[ri].y += offset;
                penY += offset;
            }
        }
    }

    void placeFigure(int imageIndex, float imgW, float imgH) {
        const float top = (float)geo.marginTop;
        const float colH = (float)(geo.height - geo.marginBottom) - top;
        if (imgH > colH) {
            const float s = colH / imgH;
            imgW *= s; imgH *= s;
        }
        continueColumn = false;
        openBlock(figureGap());
        const int cols = std::max(1, (int)std::ceil((imgW - 0.5f) / colWidth));
        const float slotW = cols * colWidth;
        float slotRight = penX + curColWidth * 0.5f;
        if (slotRight - slotW < (float)geo.marginLeft - 0.5f) {
            if (!currentPage.glyphs.empty() || !currentPage.images.empty()) newPage();
            slotRight = penX + curColWidth * 0.5f;
        }
        const float x = slotRight - (slotW + imgW) * 0.5f;
        const float y = top + (colH - imgH) * 0.5f;
        currentPage.images.push_back(PlacedImage{imageIndex, x, y, imgW, imgH});
        penX = slotRight - slotW + colWidth * 0.5f;
        curColWidth = colWidth;
        colTop = top;
        penY = top + colH;
        closeBlock(figureGap());
    }

    void placeInlineImage(int imageIndex, float imgW, float imgH) {
        const float maxColH = (float)(geo.height - geo.marginBottom);
        if (imgW > colWidth) {
            const float s = colWidth / imgW;
            imgW *= s; imgH *= s;
        }
        continueColumn = false;
        pendingGap = 0.f;
        if (columnDirty()) newColumn();
        const float x = penX - imgW * 0.5f;
        currentPage.images.push_back(PlacedImage{imageIndex, x, penY, imgW, imgH});
        penY += imgH;
        if (penY + (float)opts.fontSize > maxColH) newColumn();
        continueColumn = true;
    }

    void layoutBlock(const Block& b) {
        std::vector<VGlyph> all;
        std::vector<bool>   isHardBreak;
        for (const auto& r : b.runs) {
            if (r.kind == InlineKind::LineBreak) {
                VGlyph vg{};
                vg.g.advance = 0.0f;
                vg.g.fontSlot = FontSlot::Regular;
                vg.g.fontPx = (float)opts.fontSize;
                vg.isWhitespace = true;
                vg.breakAfter = true;
                all.push_back(vg);
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
            shapeRunVertical(cornerBracketQuotes(r.text), slot, fonts, px, all);
            const bool strong = r.kind == InlineKind::Strong
                             || b.kind == BlockKind::Heading;
            const bool nativeBold = slot == FontSlot::Bold && fonts.bold;
            for (size_t gi = before; gi < all.size(); ++gi) {
                all[gi].g.linkTarget = r.linkTarget;
                all[gi].g.syntheticBold = strong
                    && !(nativeBold && all[gi].g.fontSlot == FontSlot::Bold);
                all[gi].g.oblique = r.kind == InlineKind::Emphasis;
                if (r.kind == InlineKind::Code && b.kind != BlockKind::CodeBlock)
                    all[gi].g.inkGray = 132;
                const float shift = r.kind == InlineKind::Superscript ? (float)(basePx * 0.28)
                                  : r.kind == InlineKind::Subscript ? -(float)(basePx * 0.18)
                                  : 0.f;
                if (shift != 0.f) {
                    if (all[gi].g.rotated) all[gi].yoff += shift;
                    else all[gi].g.x += shift;
                }
            }
            isHardBreak.resize(all.size(), false);
        }
        if (all.empty()) return;

        {
            double blockBasePx = opts.fontSize;
            if (b.cssFontSizeEm >= 0)
                blockBasePx = opts.fontSize * (double)b.cssFontSizeEm;
            else if (b.kind == BlockKind::Heading && b.level >= 1 && b.level <= 6)
                blockBasePx = opts.fontSize * opts.headingScale[b.level - 1];
            colWidth = (float)blockBasePx
                     * (float)((b.cssLineHeightMul >= 0) ? b.cssLineHeightMul
                                                         : opts.lineHeightMul);
        }
        float inset = (float)(b.quoteDepth * 2.0 * opts.fontSize);
        if (b.kind == BlockKind::OrderedItem || b.kind == BlockKind::UnorderedItem)
            inset += (float)(b.listDepth * 1.5 * opts.fontSize);
        const bool isHeading = b.kind == BlockKind::Heading;
        const bool isCode = b.kind == BlockKind::CodeBlock;

        const float gapBefore = blockGapBefore(b);
        const float gapAfter  = blockGapAfter(b);

        blockTopInset = 0.f;
        openBlock(gapBefore);
        blockTopInset = inset;
        if (!columnDirty()) penY = std::max(penY, colTop + blockTopInset);
        if (isHeading && b.level <= 2 && !columnDirty()
            && (!currentPage.glyphs.empty() || !currentPage.images.empty())) {
            newColumn();
        }

        if (b.kind == BlockKind::Heading) {
            std::string flat;
            for (auto& r : b.runs) flat += cornerBracketQuotes(r.text);
            HeadingAnchor a{};
            a.title = flat;
            a.level = b.level;
            a.pageIndex0 = pageIdx0;
            a.yOnPage = penX;
            result.headings.push_back(std::move(a));
        }

        const float maxColH = (float)(geo.height - geo.marginBottom);
        if (penY + (float)opts.fontSize > maxColH) newColumn();
        bool centerHeading = false;
        if (isHeading) {
            float total = 0.f;
            for (const auto& vg : all) total += vg.g.advance;
            centerHeading = penY + total <= maxColH;
        }

        const double indentChars = (b.cssTextIndentEm >= 0)
                                       ? (double)b.cssTextIndentEm
                                       : opts.firstLineIndentChars;
        if (b.kind == BlockKind::Paragraph && indentChars != 0.0) {
            float indentPx = (float)(indentChars * opts.fontSize);
            float newPenY = penY + indentPx;
            if (newPenY >= 0 && newPenY < maxColH) penY = newPenY;
        }

        auto canBreakAfter = [&](size_t k) {
            if (!all[k].breakAfter) {
                if (k + 1 >= all.size() || !isCjk(all[k + 1].cp) || all[k].isWhitespace)
                    return false;
            }
            if (linebreak::isLineEndForbidden(all[k].cp)) return false;
            if (k + 1 < all.size()
                && linebreak::isLineStartForbidden(all[k + 1].cp))
                return false;
            return true;
        };

        size_t i = 0;
        while (i < all.size()) {
            size_t colStart = i;
            float  colY = penY;
            int    lastBreak = -1;

            while (i < all.size()) {
                if (isHardBreak[i]) {
                    if (i > colStart) emitColumnSlice(all, colStart, i - 1, centerHeading, isCode);
                    newColumn();
                    ++i;
                    colStart = i;
                    colY = penY;
                    lastBreak = -1;
                    continue;
                }
                float adv = all[i].g.advance;
                if (colY + adv > maxColH && colY <= maxColH + 0.5f
                    && linebreak::isHangingPunctuation(all[i].cp) && !isCode) {
                    colY += adv;
                    if (canBreakAfter(i)) lastBreak = (int)i;
                    ++i;
                    continue;
                }
                if (colY + adv > maxColH) {
                    if (lastBreak >= 0) {
                        emitColumnSlice(all, colStart, (size_t)lastBreak, centerHeading, isCode);
                        newColumn();
                        i = (size_t)lastBreak + 1;
                        colStart = i;
                        colY = penY;
                        lastBreak = -1;
                        continue;
                    }
                    if (colY + adv <= maxColH + (float)opts.fontSize * 1.05f) {
                        colY += adv;
                        ++i;
                        continue;
                    }
                    if (i > colStart) {
                        emitColumnSlice(all, colStart, i - 1, centerHeading, isCode);
                        newColumn();
                        colStart = i;
                        colY = penY;
                        lastBreak = -1;
                    } else {
                        emitColumnSlice(all, colStart, i, centerHeading, isCode);
                        newColumn();
                        colStart = i + 1;
                        colY = penY;
                        ++i;
                        lastBreak = -1;
                    }
                    continue;
                }
                colY += adv;
                if (canBreakAfter(i)) lastBreak = (int)i;
                ++i;
            }
            if (i > colStart) {
                emitColumnSlice(all, colStart, i - 1, centerHeading, isCode);
            }
        }

        closeBlock(gapAfter);
        blockTopInset = 0.f;
    }
};

VerticalLayout::VerticalLayout(const Options& opts, const FontSet& fonts,
                               Logger* log)
    : impl_(std::make_unique<Impl>(opts, fonts, log)) {}

VerticalLayout::~VerticalLayout() = default;

namespace {

bool looksTraditional(const Document& doc) {
    std::string sample;
    for (const auto& b : doc.blocks) {
        for (const auto& r : b.runs) sample += r.text;
        if (sample.size() > 24000) break;
    }
    if (sample.empty()) return false;
    const std::string simp = convertZhUtf8(sample, ZhConvert::T2S);
    size_t han = 0, changed = 0, kana = 0;
    size_t i = 0, j = 0;
    while (i < sample.size() && j < simp.size()) {
        const uint32_t a = textjoin::decodeAt(sample, i);
        const uint32_t c = textjoin::decodeAt(simp, j);
        const int la = utf8CharLen(sample, (int)i), lc = utf8CharLen(simp, (int)j);
        if (a >= 0x4E00 && a <= 0x9FFF) {
            ++han;
            if (a != c) ++changed;
        } else if (a >= 0x3041 && a <= 0x30FF) {
            ++kana;
        }
        i += (size_t)std::max(1, la);
        j += (size_t)std::max(1, lc);
    }
    if (kana * 20 > han + kana) return false;
    return han >= 50 && changed * 25 >= han;
}

}

LayoutResult VerticalLayout::run(const Document& doc) {
    impl_->result.blockPageIndex.reserve(doc.blocks.size());
    impl_->centerPunctuation = impl_->opts.zhConvert == ZhConvert::S2T
        || (impl_->opts.zhConvert != ZhConvert::T2S && looksTraditional(doc));
    size_t pendingIndex = (size_t)-1;
    size_t pendingGlyphs = 0, pendingImages = 0;
    auto settlePending = [&]() {
        if (pendingIndex == (size_t)-1) return;
        int& slot = impl_->result.blockPageIndex[pendingIndex];
        if ((size_t)slot < impl_->result.pages.size()) {
            const PageGlyphs& p = impl_->result.pages[(size_t)slot];
            if (p.glyphs.size() == pendingGlyphs && p.images.size() == pendingImages)
                ++slot;
        }
        pendingIndex = (size_t)-1;
    };
    for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
        if ((bi & 31) == 0 && currentJobCancelled()) break;
        settlePending();
        const Block& b = doc.blocks[bi];
        impl_->result.blockPageIndex.push_back((int)impl_->result.pages.size());
        pendingIndex = bi;
        pendingGlyphs = impl_->currentPage.glyphs.size();
        pendingImages = impl_->currentPage.images.size();
        if (b.kind != BlockKind::PageBreak && b.boxId != impl_->boxId) {
            impl_->closeBox();
            if (b.boxId != 0) impl_->openBox(b);
        }
        if (b.kind == BlockKind::Table) {
            for (const auto& row : b.rows) {
                Block p;
                p.kind = BlockKind::Paragraph;
                p.quoteDepth = b.quoteDepth;
                p.cssTextIndentEm = 0.f;
                for (size_t ci = 0; ci < row.cells.size(); ++ci) {
                    if (ci > 0) p.runs.push_back(InlineRun{InlineKind::Text, "\xE3\x80\x80", 0});
                    for (auto r : row.cells[ci].runs) {
                        if (row.cells[ci].isHeader && r.kind == InlineKind::Text)
                            r.kind = InlineKind::Strong;
                        p.runs.push_back(std::move(r));
                    }
                }
                if (!p.runs.empty()) impl_->layoutBlock(p);
            }
            continue;
        }
        if (b.kind == BlockKind::PageBreak) {
            impl_->newPage();
            continue;
        }
        if (b.kind == BlockKind::ThematicBreak) {
            impl_->openBlock(0.f);
            impl_->newColumn();
            continue;
        }
        if (b.kind == BlockKind::Image && b.imageIndex >= 0
                && b.imageIndex < (int)doc.images.size()) {
            const auto& img = doc.images[b.imageIndex];
            if (img.width > 0 && img.height > 0) {
                const float pw = (float)impl_->geo.contentWidth();
                const float ph = (float)impl_->geo.contentHeight();
                if (b.imageFullPage) {
                    const float scale = std::min(pw / img.width, ph / img.height);
                    const float w = img.width * scale;
                    const float h = img.height * scale;
                    const bool pageDirty =
                        !impl_->currentPage.glyphs.empty()
                        || !impl_->currentPage.images.empty()
                        || !impl_->currentPage.hRules.empty();
                    if (pageDirty) impl_->newPage();
                    float x = (float)impl_->geo.marginLeft + (pw - w) * 0.5f;
                    float y = (float)impl_->geo.marginTop + (ph - h) * 0.5f;
                    impl_->currentPage.images.push_back(PlacedImage{b.imageIndex, x, y, w, h});
                    impl_->newPage();
                    continue;
                }
                const float dev = imageDeviceScale(impl_->geo);
                if (b.imageInline) {
                    const float scale = fitImageScale((float)img.width, (float)img.height,
                                                      pw, ph, 1.0f);
                    impl_->placeInlineImage(b.imageIndex, img.width * scale, img.height * scale);
                } else {
                    const float scale = fitImageScale((float)img.width, (float)img.height,
                                                      pw, ph, dev);
                    impl_->placeFigure(b.imageIndex, img.width * scale, img.height * scale);
                }
            }
            continue;
        }
        impl_->layoutBlock(b);
    }
    impl_->closeBox();
    settlePending();
    if (!impl_->currentPage.glyphs.empty() || !impl_->currentPage.images.empty()
            || !impl_->currentPage.rects.empty() || !impl_->currentPage.hRules.empty()
            || !impl_->result.pages.empty()) {
        impl_->result.pages.push_back(std::move(impl_->currentPage));
    }
    return std::move(impl_->result);
}

}
