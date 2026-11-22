#include "pdf_parser.h"
#include "pdf_reading_order.h"
#include "pdf_tables.h"
#include "cancel.h"
#include "util/text_join.h"
#include "layout/epub_webp.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hardcopy {

namespace {

// Per-codepoint "nominal size" cache, built in a first pass over the whole
// document and consulted in the second pass to classify sub/superscripts.
//
// A PDF has no explicit sub/superscript flag: a subscript or superscript is an
// ordinary character set in a SMALLER font size (via the 'Tf'/'Tm' text matrix)
// and shifted up/down from the baseline. So the reliable signal is not the
// character's surrounding line, but a comparison against the SAME character's
// full-size appearance elsewhere in the document: if a glyph is clearly smaller
// than the largest size that same Unicode codepoint normally appears at, it is
// a script (subscript if lowered, superscript if raised).
using CodepointSizeTable = std::unordered_map<uint32_t, float>;

// Record the sizes a codepoint appears at; keep the LARGEST (its "full-size"
// appearance). A true sub/superscript of a character is always physically
// smaller than that character's normal size, so the max is a robust nominal.
void recordCodepointSize(CodepointSizeTable* table, uint32_t cp, float size) {
    if (!table || cp <= 0x20 || size <= 0.0f) return;
    auto it = table->find(cp);
    if (it == table->end() || size > it->second) table->operator[](cp) = size;
}

// Classify one glyph. `nominalSize` is the cached full-size of the same
// codepoint (0 if unknown); `lineSize` is the enclosing line's dominant size
// and `lineBaseline` its baseline (for direction). A glyph is a script when it
// is clearly smaller than BOTH its codepoint's full-size AND the line's
// dominant size. Direction: a strong downward offset => subscript, a strong
// upward offset => superscript; when the baseline offset is ambiguous (which
// is common because the 'line baseline' is a median that the script glyphs
// themselves perturb), default smaller => subscript, since subscripts
// overwhelmingly dominate in technical prose.
InlineKind classifyGlyph(float charSize, float charBaseline, float nominalSize,
                         float lineSize, float lineBaseline) {
    if (nominalSize <= 0.0f || charSize <= 0.0f) return InlineKind::Text;
    // Must be clearly smaller than the line's dominant size too.
    if (lineSize > 0.0f && charSize >= lineSize * 0.88f) return InlineKind::Text;
    // A clear size reduction is required before we even consider a script.
    if (charSize >= nominalSize * 0.85f) return InlineKind::Text;
    // Direction from the baseline offset, in units of the line's size.
    const float rise = charBaseline - lineBaseline;
    const float em = (lineSize > 0.0f) ? lineSize : nominalSize;
    if (rise >  0.30f * em) return InlineKind::Superscript;
    if (rise < -0.30f * em) return InlineKind::Subscript;
    // Smaller with no decisive offset: default to subscript (the common case).
    return InlineKind::Subscript;
}

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s.push_back((char)cp);
    } else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (cp >> 18)));
        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

bool isBoldFont(fz_context* ctx, fz_font* font) {
    if (!font) return false;
    if (fz_font_is_bold(ctx, font)) return true;
    const char* name = fz_font_name(ctx, font);
    if (!name) return false;
    const char* boldPatterns[] = {
        "Bold", "BOLD", "bold", "Heavy", "HEAVY", "heavy",
        "Black", "BLACK", "black", "Demi", "DEMI", "demi",
        "Semibold", "SemiBold", "SEMIBOLD", nullptr
    };
    for (int i = 0; boldPatterns[i]; ++i) {
        if (std::strstr(name, boldPatterns[i])) return true;
    }
    return false;
}

float roundSize(float sz) { return std::round(sz * 2.0f) / 2.0f; }

bool isCjk(int c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F);
}

fz_point charCenter(const fz_stext_char* ch) {
    const float x0 = std::min(ch->quad.ll.x, ch->quad.ul.x);
    const float x1 = std::max(ch->quad.lr.x, ch->quad.ur.x);
    return fz_make_point((x0 + x1) * 0.5f, ch->origin.y - ch->size * 0.3f);
}

struct CellChar { const fz_stext_line* line; const fz_stext_char* ch; };

std::vector<InlineRun> cellRuns(fz_context* ctx, const std::vector<CellChar>& chars,
                                int* boldChars, int* inkChars, float* sizeSum,
                                const CodepointSizeTable* cpSizes) {
    std::vector<InlineRun> runs;
    std::string buf;
    bool curBold = false, started = false;
    InlineKind curKind = InlineKind::Text;
    const fz_stext_line* curLine = nullptr;
    int last = 0;
    // Compute the cell's dominant font size and baseline to classify scripts.
    std::vector<float> csizes, cbaselines;
    for (const auto& cc : chars) {
        if (cc.ch->c <= 32) continue;
        csizes.push_back(cc.ch->size);
        cbaselines.push_back(cc.ch->origin.y);
    }
    float lineSize = 0.0f, lineBaseline = 0.0f;
    if (!csizes.empty()) {
        std::nth_element(csizes.begin(), csizes.begin() + csizes.size() / 2, csizes.end());
        std::nth_element(cbaselines.begin(), cbaselines.begin() + cbaselines.size() / 2, cbaselines.end());
        lineSize = csizes[csizes.size() / 2];
        lineBaseline = cbaselines[cbaselines.size() / 2];
    }
    (void)lineSize; // only the baseline is used for direction; size comes from the cache
    auto flush = [&] {
        if (buf.empty()) return;
        runs.push_back(InlineRun{curBold ? InlineKind::Strong : curKind, std::move(buf), 0});
        buf.clear();
    };
    for (const auto& cc : chars) {
        const int c = cc.ch->c;
        if (c <= 0 || c == 0xFFFD) continue;
        if (started && cc.line != curLine && last != ' ' && c != ' '
            && !isCjk(last) && !isCjk(c))
            buf.push_back(' ');
        curLine = cc.line;
        if (!started && c == ' ') continue;
        const bool bold = isBoldFont(ctx, cc.ch->font);
        float nominal = 0.0f;
        if (cpSizes) {
            auto it = cpSizes->find((uint32_t)c);
            if (it != cpSizes->end()) nominal = it->second;
        }
        const InlineKind kind = classifyGlyph(cc.ch->size, cc.ch->origin.y, nominal, lineSize, lineBaseline);
        if (started && (bold != curBold || kind != curKind) && c != ' ') flush();
        if (c != ' ') { curBold = bold; curKind = kind; }
        appendUtf8(buf, (uint32_t)c);
        started = true;
        last = c;
        if (c > ' ') {
            ++*inkChars;
            *boldChars += bold;
            *sizeSum += cc.ch->size;
        }
    }
    flush();
    while (!runs.empty() && !runs.back().text.empty() && runs.back().text.back() == ' ') {
        runs.back().text.pop_back();
        if (runs.back().text.empty()) runs.pop_back();
    }
    return runs;
}

void walkOutline(fz_context* ctx, fz_document* doc, fz_outline* ol, int depth,
                 std::vector<HeadingAnchor>* out) {
    for (; ol; ol = ol->next) {
        if (ol->title && *ol->title) {
            fz_location loc = {-1, -1};
            if (ol->uri) {
                fz_var(loc);
                fz_try(ctx) {
                    loc = fz_resolve_link(ctx, doc, ol->uri,
                                          nullptr, nullptr);
                } fz_catch(ctx) {
                    loc = ol->page;
                }
            } else {
                loc = ol->page;
            }
            int page0 = fz_page_number_from_location(ctx, doc, loc);
            if (page0 >= 0) {
                HeadingAnchor a;
                a.title = ol->title;
                a.level = depth;
                a.pageIndex0 = page0;
                a.yOnPage = 0.0f;
                out->push_back(std::move(a));
            }
        }
        if (ol->down) walkOutline(ctx, doc, ol->down, depth + 1, out);
    }
}

void readMetadata(fz_context* ctx, fz_document* doc, Document* out) {
    char buf[1024];
    int n = fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, buf, sizeof(buf));
    if (n > 0 && out->title.empty())  out->title.assign(buf, (size_t)(n - 1));
    n = fz_lookup_metadata(ctx, doc, FZ_META_INFO_AUTHOR, buf, sizeof(buf));
    if (n > 0 && out->author.empty()) out->author.assign(buf, (size_t)(n - 1));
}

bool appendImageBlock(fz_context* ctx, fz_stext_block* source,
                      Document* out, Logger* log) {
    if (!source || source->type != FZ_STEXT_BLOCK_IMAGE
        || !source->u.i.image) return false;

    fz_image* image = source->u.i.image;
    fz_buffer* buffer = nullptr;
    fz_pixmap* pixmap = nullptr;
    bool ok = false;
    fz_var(buffer);
    fz_var(pixmap);
    fz_var(ok);

    fz_try(ctx) {
        fz_compressed_buffer* compressed =
            fz_compressed_image_buffer(ctx, image);
        if (image->mask) {
            pixmap = fz_new_pixmap(ctx, fz_device_rgb(ctx), image->w, image->h,
                                   nullptr, 0);
            fz_clear_pixmap_with_value(ctx, pixmap, 255);
            fz_device* draw = fz_new_draw_device(ctx, fz_identity, pixmap);
            fz_try(ctx) {
                const fz_matrix m = fz_scale((float)image->w, (float)image->h);
                const fz_rect area = fz_transform_rect(fz_unit_rect, m);
                fz_clip_image_mask(ctx, draw, image->mask, m, area);
                fz_fill_image(ctx, draw, image, m, 1.0f, fz_default_color_params);
                fz_pop_clip(ctx, draw);
                fz_close_device(ctx, draw);
            } fz_always(ctx) {
                fz_drop_device(ctx, draw);
            } fz_catch(ctx) {
                fz_rethrow(ctx);
            }
            const unsigned char* px = fz_pixmap_samples(ctx, pixmap);
            const size_t count = (size_t)fz_pixmap_stride(ctx, pixmap) * image->h;
            bool blank = true;
            for (size_t k = 0; k < count && blank; ++k) blank = px[k] >= 250;
            if (!blank)
                buffer = fz_new_buffer_from_pixmap_as_png(
                    ctx, pixmap, fz_default_color_params);
        } else if (compressed && compressed->buffer
            && (compressed->params.type == FZ_IMAGE_JPEG
                || compressed->params.type == FZ_IMAGE_PNG)) {
            buffer = fz_keep_buffer(ctx, compressed->buffer);
        } else {
            pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr,
                                              nullptr, nullptr);
            buffer = fz_new_buffer_from_pixmap_as_png(
                ctx, pixmap, fz_default_color_params);
        }

        unsigned char* bytes = nullptr;
        size_t length = fz_buffer_storage(ctx, buffer, &bytes);
        if (bytes && length > 0) {
            ImageData data;
            data.rawBytes.assign(bytes, bytes + length);
            data.width = image->w;
            data.height = image->h;
            int index = (int)out->images.size();
            out->images.push_back(std::move(data));

            Block block;
            block.kind = BlockKind::Image;
            block.imageIndex = index;
            out->blocks.push_back(std::move(block));
            ok = true;
        }
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pixmap);
        fz_drop_buffer(ctx, buffer);
    }
    fz_catch(ctx) {
        if (log) log->log(LogLevel::Warn, "pdfin",
                          fz_caught_message(ctx));
        ok = false;
    }
    return ok;
}

using CellGrid = std::vector<std::vector<std::vector<CellChar>>>;

void splitStackedRows(CellGrid* grid, std::vector<std::vector<PdfTableCell>>* meta) {
    CellGrid outGrid;
    std::vector<std::vector<PdfTableCell>> outMeta;
    const size_t cols = meta->empty() ? 0 : (*meta)[0].size();
    std::vector<int> down(cols, 0);
    for (size_t r = 0; r < grid->size(); ++r) {
        const auto& row = (*grid)[r];
        const auto& m = (*meta)[r];
        bool spanned = false;
        for (size_t c = 0; c < cols && c < m.size(); ++c)
            spanned |= down[c] > 0 || (!m[c].placeholder && m[c].rowspan > 1);
        for (auto& d : down) d = std::max(0, d - 1);
        for (size_t c = 0; c < cols && c < m.size(); ++c)
            if (!m[c].placeholder && m[c].rowspan > 1)
                for (size_t k = c; k < c + (size_t)m[c].colspan && k < cols; ++k) down[k] = m[c].rowspan - 1;
        std::vector<float> bases, sizes;
        for (const auto& cell : row)
            for (const auto& cc : cell)
                if (cc.ch->c > ' ') { bases.push_back(cc.ch->origin.y); sizes.push_back(cc.ch->size); }
        std::vector<float> clusters;
        if (!bases.empty()) {
            std::sort(bases.begin(), bases.end());
            std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
            const float tol = 0.35f * std::max(1.0f, sizes[sizes.size() / 2]);
            for (size_t i = 0; i < bases.size();) {
                size_t j = i;
                while (j < bases.size() && bases[j] - bases[i] <= tol) ++j;
                clusters.push_back(bases[i + (j - i) / 2]);
                i = j;
            }
        }
        auto clusterOf = [&](float y) {
            size_t best = 0;
            for (size_t k = 1; k < clusters.size(); ++k)
                if (std::fabs(clusters[k] - y) < std::fabs(clusters[best] - y)) best = k;
            return best;
        };
        int full = 0;
        bool shortEntries = false;
        if (clusters.size() >= 2) {
            for (size_t c = 0; c < row.size(); ++c) {
                std::vector<float> x0(clusters.size(), 1e9f), x1(clusters.size(), -1e9f);
                for (const auto& cc : row[c]) {
                    if (cc.ch->c <= ' ') continue;
                    const size_t k = clusterOf(cc.ch->origin.y);
                    x0[k] = std::min(x0[k], std::min(cc.ch->quad.ll.x, cc.ch->quad.ul.x));
                    x1[k] = std::max(x1[k], std::max(cc.ch->quad.lr.x, cc.ch->quad.ur.x));
                }
                bool all = true;
                float widest = 0;
                for (size_t k = 0; k < clusters.size(); ++k) {
                    all &= x1[k] > x0[k];
                    widest = std::max(widest, x1[k] - x0[k]);
                }
                if (!all) continue;
                ++full;
                const float cellW = m[c].rect.x1 - m[c].rect.x0;
                shortEntries |= cellW > 0 && widest <= 0.6f * cellW;
            }
        }
        if (spanned || full < 2 || !shortEntries) {
            outGrid.push_back(row);
            outMeta.push_back(m);
            continue;
        }
        for (size_t k = 0; k < clusters.size(); ++k) {
            std::vector<std::vector<CellChar>> sub(row.size());
            for (size_t c = 0; c < row.size(); ++c)
                for (const auto& cc : row[c])
                    if (clusterOf(cc.ch->origin.y) == k) sub[c].push_back(cc);
            outGrid.push_back(std::move(sub));
            outMeta.push_back(m);
        }
    }
    *grid = std::move(outGrid);
    *meta = std::move(outMeta);
}

struct PageChar {
    const fz_stext_line* line;
    const fz_stext_char* ch;
    int element;
    bool label;
};

bool isWordChar(int c) {
    if (c < 0x80) return std::isalnum(c) != 0;
    if (c < 0xC0) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;
    if (c >= 0x3000 && c <= 0x303F) return false;
    if (c >= 0xE000 && c <= 0xF8FF) return false;
    if (c >= 0xFE30 && c <= 0xFE4F) return false;
    if (c >= 0xFF00 && c <= 0xFF0F) return false;
    if (c >= 0xFF1A && c <= 0xFF20) return false;
    if (c >= 0xFF3B && c <= 0xFF40) return false;
    if (c >= 0xFF5B && c <= 0xFF65) return false;
    return true;
}

std::vector<PageChar> tagPageChars(fz_stext_page* stext, const PdfTableEvidence& ev,
                                   const std::unordered_set<const fz_stext_char*>& skip,
                                   int* visible, int* tagged) {
    const PdfGlyphLookup lookup(ev);
    std::vector<PageChar> out;
    for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
        if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next) {
            const size_t first = out.size();
            std::vector<char> matched;
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                if (ch->c <= 0 || ch->c == 0xFFFD) continue;
                const int g = lookup.find(ch->origin, ch->c);
                if (skip.count(ch)) {
                    if (isWordChar(ch->c)) {
                        ++*visible;
                        *tagged += g >= 0 && ev.textGlyphs[g].element >= 0;
                    }
                    continue;
                }
                PageChar pc{line, ch, -1, false};
                if (g >= 0) {
                    pc.element = ev.textGlyphs[g].element;
                    pc.label = ev.textGlyphs[g].label;
                }
                out.push_back(pc);
                matched.push_back(g >= 0);
            }
            const size_t n = out.size() - first;
            for (size_t k = 0; k < n; ++k) {
                if (matched[k]) continue;
                PageChar& pc = out[first + k];
                if (k > 0 && std::fabs(pc.ch->origin.x - out[first + k - 1].ch->origin.x) < 0.01f &&
                    std::fabs(pc.ch->origin.y - out[first + k - 1].ch->origin.y) < 0.01f) {
                    pc.element = out[first + k - 1].element;
                    pc.label = out[first + k - 1].label;
                    continue;
                }
                int p = -1, q = -1;
                for (size_t j = k; j-- > 0;) if (matched[j]) { p = (int)j; break; }
                for (size_t j = k + 1; j < n; ++j) if (matched[j]) { q = (int)j; break; }
                const PageChar* from = p >= 0 ? &out[first + p] : q >= 0 ? &out[first + q] : nullptr;
                if (from) { pc.element = from->element; pc.label = from->label; }
            }
            for (size_t k = 0; k < n; ++k) {
                const PageChar& pc = out[first + k];
                if (!isWordChar(pc.ch->c)) continue;
                ++*visible;
                *tagged += pc.element >= 0;
            }
        }
    }
    return out;
}

bool isBulletChar(uint32_t c) {
    switch (c) {
    case 0x2022: case 0x25CF: case 0x25E6: case 0x25AA: case 0x25AB: case 0x25A0:
    case 0x25A1: case 0x25CB: case 0x25C6: case 0x25C7: case 0x25BA: case 0x25B6:
    case 0x2023: case 0x2043: case 0x2013: case 0x2014: case 0x00B7: case 0x30FB:
    case 0x2219: case 0x27A2: case 0x2713: case 0x2714: case 0x2756: case '*': case '-':
        return true;
    default:
        return c >= 0xF000 && c <= 0xF0FF;
    }
}

size_t orderedLabel(const std::string& s, int* number) {
    *number = 0;
    size_t i = 0;
    const bool paren = !s.empty() && s[0] == '(';
    const bool wideParen = s.compare(0, 3, "\xEF\xBC\x88") == 0;
    if (paren) i = 1;
    else if (wideParen) i = 3;
    size_t start = i;
    while (i < s.size() && i - start < 3 && s[i] >= '0' && s[i] <= '9') ++i;
    if (i > start) {
        *number = std::atoi(s.substr(start, i - start).c_str());
    } else if (i < s.size() && std::isalpha((unsigned char)s[i])) {
        while (i < s.size() && i - start < 5 && std::isalpha((unsigned char)s[i])) ++i;
        const std::string word = s.substr(start, i - start);
        const bool roman = word.find_first_not_of("ivxlcdm") == std::string::npos ||
                           word.find_first_not_of("IVXLCDM") == std::string::npos;
        if (word.size() > 1 && !roman) return 0;
    } else {
        static const char* kNums[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
        size_t j = start;
        for (int guard = 0; guard < 3; ++guard) {
            bool hit = false;
            for (int k = 0; k < 10; ++k) {
                const size_t len = std::strlen(kNums[k]);
                if (s.compare(j, len, kNums[k]) == 0) { j += len; hit = true; break; }
            }
            if (!hit) break;
        }
        if (j == start || s.compare(j, 3, "\xE3\x80\x81") != 0) return 0;
        return j + 3;
    }
    if (i == start) return 0;
    if (paren) return i < s.size() && s[i] == ')' ? i + 1 : 0;
    if (wideParen) return s.compare(i, 3, "\xEF\xBC\x89") == 0 ? i + 3 : 0;
    if (i < s.size() && (s[i] == '.' || s[i] == ')')) return i + 1;
    if (s.compare(i, 3, "\xE3\x80\x81") == 0) return i + 3;
    return 0;
}

bool dropLineEndHyphen(std::string& t, const std::string& next,
                       const std::set<std::string>* keep = nullptr) {
    const uint32_t first = textjoin::firstCodepoint(next);
    if (!(first >= 'a' && first <= 'z')) return false;
    if (t.size() >= 2 && (unsigned char)t[t.size() - 2] == 0xC2 && (unsigned char)t.back() == 0xAD) {
        t.resize(t.size() - 2);
        return true;
    }
    if (t.size() < 3 || t.back() != '-') return false;
    const char before = t[t.size() - 2];
    if (!(before >= 'a' && before <= 'z')) return false;
    if (keep && !keep->empty()) {
        size_t a = t.size() - 1;
        while (a > 0 && std::isalpha((unsigned char)t[a - 1])) --a;
        size_t b = 0;
        while (b < next.size() && std::isalpha((unsigned char)next[b])) ++b;
        std::string word = t.substr(a) + next.substr(0, b);
        for (char& c : word) c = (char)std::tolower((unsigned char)c);
        if (keep->count(word)) return true;
    }
    t.pop_back();
    return true;
}

void collectCompounds(const std::vector<InlineRun>& runs, std::set<std::string>* out) {
    std::string text;
    for (const auto& r : runs) text += r.text;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] != '-' || !std::isalpha((unsigned char)text[i - 1]) ||
            !std::isalpha((unsigned char)text[i + 1]))
            continue;
        size_t a = i;
        while (a > 0 && std::isalpha((unsigned char)text[a - 1])) --a;
        size_t b = i + 1;
        while (b < text.size() && std::isalpha((unsigned char)text[b])) ++b;
        std::string word = text.substr(a, b - a);
        for (char& c : word) c = (char)std::tolower((unsigned char)c);
        out->insert(std::move(word));
    }
}

struct RunBuilder {
    std::vector<InlineRun> runs;
    std::string buf;
    bool bold = false;
    InlineKind kind = InlineKind::Text;
    bool empty() const { return runs.empty() && buf.empty(); }
    void flush() {
        if (buf.empty()) return;
        runs.push_back(InlineRun{bold ? InlineKind::Strong : kind, std::move(buf), 0});
        buf.clear();
    }
    std::string& tail() { return !buf.empty() || runs.empty() ? buf : runs.back().text; }
    void trimTrailingSpace() {
        while (!buf.empty() && buf.back() == ' ') buf.pop_back();
        if (!buf.empty()) return;
        while (!runs.empty()) {
            std::string& t = runs.back().text;
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (!t.empty()) return;
            runs.pop_back();
        }
    }
    void add(uint32_t c, bool b) {
        if (c == ' ' || c == '\t') {
            if (empty() || (!tail().empty() && tail().back() == ' ')) return;
            buf.push_back(' ');
            return;
        }
        if (b != bold && !buf.empty()) flush();
        bold = b;
        appendUtf8(buf, c);
    }
    void add(uint32_t c, bool b, InlineKind k) {
        if (c == ' ' || c == '\t') {
            if (empty() || (!tail().empty() && tail().back() == ' ')) return;
            buf.push_back(' ');
            return;
        }
        // Flush when the script kind changes, so each sub/superscript span is
        // its own InlineRun (the layout only honours the kind at run level).
        if ((b != bold || k != kind) && !buf.empty()) flush();
        bold = b;
        kind = k;
        appendUtf8(buf, c);
    }
    void wrap(uint32_t next) {
        if (empty()) return;
        trimTrailingSpace();
        std::string& t = tail();
        std::string nextText;
        appendUtf8(nextText, next);
        if (dropLineEndHyphen(t, nextText)) return;
        if (textjoin::needsSpace(t, nextText)) add(' ', bold);
    }
    std::vector<InlineRun> take() {
        trimTrailingSpace();
        flush();
        return std::move(runs);
    }
};

bool isContainerStructure(int t) {
    switch (t) {
    case FZ_STRUCTURE_DOCUMENT: case FZ_STRUCTURE_PART: case FZ_STRUCTURE_ART:
    case FZ_STRUCTURE_SECT: case FZ_STRUCTURE_DOCUMENTFRAGMENT: case FZ_STRUCTURE_TOC:
    case FZ_STRUCTURE_LIST: case FZ_STRUCTURE_TABLE: case FZ_STRUCTURE_TR:
    case FZ_STRUCTURE_THEAD: case FZ_STRUCTURE_TBODY: case FZ_STRUCTURE_TFOOT:
    case FZ_STRUCTURE_FIGURE:
        return true;
    default:
        return false;
    }
}

int headingLevelForSize(float size, float bodySize) {
    const float ratio = size / std::max(1.0f, bodySize);
    if (ratio >= 2.0f) return 1;
    if (ratio >= 1.6f) return 2;
    if (ratio >= 1.4f) return 3;
    if (ratio >= 1.25f) return 4;
    return 5;
}

std::string listBullet(int depth) {
    static const char* kBullets[] = {"\xE2\x80\xA2 ", "\xE2\x97\xA6 ", "\xE2\x96\xAA "};
    return kBullets[std::min(std::max(depth, 0), 2)];
}

fz_rect charBox(const fz_stext_char* ch) {
    return fz_rect_from_quad(ch->quad);
}

bool isDashBullet(uint32_t c) {
    return c == '-' || c == 0x2013 || c == 0x2014 || c == 0x00B7 || c == 0x30FB;
}

size_t splitListLabel(const std::vector<const PageChar*>& chars, std::string* label, bool* ordered,
                      int* number, bool strict = false) {
    size_t k = 0;
    while (k < chars.size() && chars[k]->ch->c <= 32) ++k;
    if (k >= chars.size()) return 0;
    auto skipSpaces = [&](size_t j) {
        while (j < chars.size() && chars[j]->ch->c <= 32) ++j;
        return j;
    };
    const fz_stext_char* first = chars[k]->ch;
    const size_t next = skipSpaces(k + 1);
    const bool gap = next < chars.size() && chars[next]->line == chars[k]->line &&
        fz_rect_from_quad(chars[next]->ch->quad).x0 - charBox(first).x1 >= 0.35f * first->size;
    const bool alnum = first->c < 128 && std::isalnum(first->c);
    const bool dash = strict && isDashBullet((uint32_t)first->c);
    if ((isBulletChar((uint32_t)first->c) && !dash) || (gap && !alnum && first->c < 0x2E80)) {
        *ordered = false;
        return next;
    }
    std::string head;
    std::vector<size_t> ends;
    for (size_t j = k; j < chars.size() && j < k + 10; ++j) {
        appendUtf8(head, (uint32_t)chars[j]->ch->c);
        ends.push_back(head.size());
    }
    const size_t len = orderedLabel(head, number);
    if (len == 0) {
        if (gap && first->c >= '0' && first->c <= '9') {
            *ordered = true;
            *number = first->c - '0';
            appendUtf8(*label, (uint32_t)first->c);
            return next;
        }
        return 0;
    }
    size_t j = 0;
    while (j < ends.size() && ends[j] < len) ++j;
    if (j >= ends.size() || ends[j] != len) return 0;
    const size_t after = k + j + 1;
    if (after < chars.size() && chars[after]->ch->c > 32) return 0;
    const char c0 = head[0] == '(' ? head[1] : head[0];
    if (strict && len <= 3 && c0 >= 'A' && c0 <= 'Z' && !std::strchr("IVX", c0)) return 0;
    *ordered = true;
    *label = head.substr(0, len);
    return skipSpaces(after);
}

void appendContinuation(Block* b, std::vector<InlineRun> more,
                        const std::set<std::string>* compounds = nullptr) {
    if (more.empty()) return;
    while (!b->runs.empty()) {
        std::string& t = b->runs.back().text;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty()) break;
        b->runs.pop_back();
    }
    if (!b->runs.empty()) {
        std::string& t = b->runs.back().text;
        if (!dropLineEndHyphen(t, more.front().text, compounds) &&
            textjoin::needsSpace(t, more.front().text))
            t.push_back(' ');
    }
    for (auto& r : more) {
        if (!b->runs.empty() && b->runs.back().kind == r.kind) b->runs.back().text += r.text;
        else b->runs.push_back(std::move(r));
    }
}

struct TaggedFlow {
    PdfStructPath lastPath;
    int lastBlock = -1;
};

struct TaggedPageStats {
    int elements = 0, images = 0, droppedImages = 0, icons = 0, merged = 0;
};

bool isLabelText(const std::string& s, bool* ordered, int* number) {
    if (s.empty() || s.size() > 12) return false;
    const uint32_t c0 = textjoin::firstCodepoint(s);
    std::string one;
    appendUtf8(one, c0);
    if (one.size() == s.size() && isBulletChar(c0)) {
        *ordered = false;
        *number = 0;
        return true;
    }
    *ordered = true;
    return orderedLabel(s, number) == s.size();
}

PdfStructPath parentPath(const PdfStructPath& p) {
    return p.empty() ? p : PdfStructPath(p.begin(), p.end() - 1);
}

void emitTaggedPage(fz_context* ctx, fz_stext_page* stext, const PdfTableEvidence& ev,
                    const std::vector<PageChar>& chars, std::vector<Block>& tableBlocks,
                    const std::vector<fz_rect>& tableBoxes,
                    const std::vector<PdfStructPath>& tableKeys, float bodySize,
                    float headingThreshold, int pageTextChars, Document* out, Logger* log,
                    TaggedFlow* flow, TaggedPageStats* stats, int* imageCount,
                    int* coveredPageImageCount) {
    struct Item {
        PdfStructPath key;
        int sub = 0;
        int kind = 0;
        int index = -1;
        fz_rect box = fz_empty_rect;
    };
    const size_t ne = ev.elements.size();
    std::vector<std::vector<const PageChar*>> byElem(ne);
    std::vector<fz_rect> elemBox(ne, fz_empty_rect);
    std::vector<std::string> shortText(ne);
    for (const auto& pc : chars) {
        if (pc.element < 0) continue;
        byElem[pc.element].push_back(&pc);
        if (pc.ch->c > 32) {
            const fz_rect b = charBox(pc.ch);
            fz_rect& eb = elemBox[pc.element];
            eb = fz_is_empty_rect(eb) ? b : fz_union_rect(eb, b);
            if (shortText[pc.element].size() <= 12) appendUtf8(shortText[pc.element], (uint32_t)pc.ch->c);
        }
    }
    std::vector<Item> items, loose;
    for (size_t e = 0; e < ne; ++e) {
        if (fz_is_empty_rect(elemBox[e])) continue;
        Item it;
        it.key = ev.elements[e].order.empty() ? ev.elements[e].path : ev.elements[e].order;
        it.index = (int)e;
        it.box = elemBox[e];
        items.push_back(std::move(it));
    }
    for (size_t t = 0; t < tableBlocks.size(); ++t) {
        Item it;
        it.kind = 1;
        it.index = (int)t;
        it.box = tableBoxes[t];
        it.key = tableKeys[t];
        (it.key.empty() ? loose : items).push_back(std::move(it));
    }
    const float pageW = stext->mediabox.x1 - stext->mediabox.x0;
    const float pageH = stext->mediabox.y1 - stext->mediabox.y0;
    std::vector<fz_stext_block*> imageBlocks;
    for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
        if (tb->type != FZ_STEXT_BLOCK_IMAGE) continue;
        const fz_rect b = tb->bbox;
        const float area = std::max(0.0f, b.x1 - b.x0) * std::max(0.0f, b.y1 - b.y0);
        if (pageTextChars >= 16 && area >= std::max(1.0f, pageW * pageH) * 0.65f) {
            ++*coveredPageImageCount;
            continue;
        }
        int element = -1;
        const PdfImageMark* found = nullptr;
        float best = 4.0f;
        for (const auto& mark : ev.images) {
            const float d = std::fabs(mark.box.x0 - b.x0) + std::fabs(mark.box.y0 - b.y0) +
                            std::fabs(mark.box.x1 - b.x1) + std::fabs(mark.box.y1 - b.y1);
            if (d < best) { best = d; element = mark.element; found = &mark; }
        }
        if (element < 0) {
            if (area < 0.02f * std::max(1.0f, pageW * pageH)) {
                ++stats->droppedImages;
                continue;
            }
            Item it;
            it.kind = 2;
            it.index = (int)imageBlocks.size();
            it.box = b;
            imageBlocks.push_back(tb);
            loose.push_back(std::move(it));
            continue;
        }
        bool inText = false;
        for (const auto& step : ev.elements[element].path) {
            const int t = step.first;
            inText |= t == FZ_STRUCTURE_P || (t >= FZ_STRUCTURE_H && t <= FZ_STRUCTURE_H6) ||
                      t == FZ_STRUCTURE_LISTITEM || t == FZ_STRUCTURE_TD || t == FZ_STRUCTURE_TH ||
                      t == FZ_STRUCTURE_DIV;
        }
        if (inText && std::max(b.x1 - b.x0, b.y1 - b.y0) <= 2.5f * bodySize) {
            ++stats->icons;
            continue;
        }
        Item it;
        it.kind = 2;
        it.index = (int)imageBlocks.size();
        it.key = found->order;
        it.sub = 1;
        it.box = b;
        imageBlocks.push_back(tb);
        items.push_back(std::move(it));
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.key != b.key) return pdfStructPathLess(a.key, b.key);
        return a.sub < b.sub;
    });
    for (auto& it : loose) {
        size_t at = items.size();
        for (int pass = 0; pass < 2 && at == items.size(); ++pass) {
            float bestY = 1e9f;
            for (size_t k = 0; k < items.size(); ++k) {
                const fz_rect& b = items[k].box;
                if (items[k].kind != 0 || b.y0 < it.box.y0 - 2.0f) continue;
                if (pass == 0 && (b.x1 <= it.box.x0 || b.x0 >= it.box.x1)) continue;
                if (b.y0 < bestY) { bestY = b.y0; at = k; }
            }
        }
        items.insert(items.begin() + (long)at, std::move(it));
    }

    struct Label { std::string text; bool ordered = false; int number = 0; };
    std::vector<int> labelFor(items.size(), -1);
    std::vector<Label> labels;
    std::vector<PdfStructPath> itemParents;
    for (size_t k = 0; k + 1 < items.size(); ++k) {
        if (items[k].kind != 0 || items[k + 1].kind != 0) continue;
        Label l;
        bool nextOrdered = false;
        int nextNumber = 0;
        if (!isLabelText(shortText[items[k].index], &l.ordered, &l.number)) continue;
        if (isLabelText(shortText[items[k + 1].index], &nextOrdered, &nextNumber)) continue;
        if (ev.elements[items[k].index].type == FZ_STRUCTURE_LISTITEM) continue;
        const PdfStructPath parent = parentPath(ev.elements[items[k].index].path);
        if (parent.empty() || parent != parentPath(ev.elements[items[k + 1].index].path)) continue;
        l.text = shortText[items[k].index];
        labelFor[k + 1] = (int)labels.size();
        labels.push_back(std::move(l));
        itemParents.push_back(parent);
    }
    auto labelDepth = [&](const PdfStructPath& parent) {
        int depth = 0;
        for (const auto& p : itemParents)
            depth += p.size() < parent.size() && std::equal(p.begin(), p.end(), parent.begin());
        return std::min(depth, 4);
    };

    bool first = true;
    if (std::getenv("HC_STRUCT_DEBUG") && log) {
        for (const auto& it : items) {
            std::string m = "  item kind=" + std::to_string(it.kind) + " key=";
            for (const auto& st : it.key) m += std::to_string(st.first) + ":" + std::to_string(st.second) + "/";
            char box[64];
            std::snprintf(box, sizeof(box), " box=%.0f,%.0f ", it.box.x0, it.box.y0);
            m += box;
            if (it.kind == 0) m += shortText[it.index];
            log->log(LogLevel::Info, "pdfin", m.c_str());
        }
    }
    for (size_t k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        if (it.kind == 1) {
            out->blocks.push_back(std::move(tableBlocks[it.index]));
            flow->lastBlock = -1;
            first = false;
            continue;
        }
        if (it.kind == 2) {
            if (appendImageBlock(ctx, imageBlocks[it.index], out, log)) {
                ++*imageCount;
                ++stats->images;
                flow->lastBlock = -1;
            }
            first = false;
            continue;
        }
        if (k + 1 < items.size() && labelFor[k + 1] >= 0) continue;
        const PdfStructElement& el = ev.elements[it.index];
        const auto& ec = byElem[it.index];
        const Label* forced = labelFor[k] >= 0 ? &labels[labelFor[k]] : nullptr;
        ++stats->elements;
        const bool cont = first && !forced && flow->lastBlock >= 0 && flow->lastPath == el.path;
        first = false;
        if (isContainerStructure(el.type) && !cont && !forced) {
            size_t c = 0;
            while (c < ec.size()) {
                const fz_stext_line* line = ec[c]->line;
                RunBuilder rb;
                float size = 0;
                int n = 0;
                for (; c < ec.size() && ec[c]->line == line; ++c) {
                    rb.add((uint32_t)ec[c]->ch->c, isBoldFont(ctx, ec[c]->ch->font));
                    if (ec[c]->ch->c > 32) { size += ec[c]->ch->size; ++n; }
                }
                Block b;
                b.runs = rb.take();
                if (b.runs.empty()) continue;
                if (n > 0 && size / n >= headingThreshold) {
                    b.kind = BlockKind::Heading;
                    b.level = headingLevelForSize(size / n, bodySize);
                }
                out->blocks.push_back(std::move(b));
            }
            flow->lastBlock = -1;
            continue;
        }
        Block b;
        if ((el.type == FZ_STRUCTURE_LISTITEM || forced) && !cont) {
            std::string label;
            bool ordered = false;
            int number = 0;
            std::vector<const PageChar*> body;
            for (const auto* pc : ec) {
                if (pc->label) appendUtf8(label, (uint32_t)pc->ch->c);
                else body.push_back(pc);
            }
            while (!label.empty() && (label.back() == ' ' || label.back() == '\t')) label.pop_back();
            while (!label.empty() && label.front() == ' ') label.erase(0, 1);
            if (forced) {
                label = forced->text;
                ordered = forced->ordered;
                number = forced->number;
                if (!ordered) label.clear();
            } else if (!label.empty()) {
                if (isBulletChar(textjoin::firstCodepoint(label))) {
                    label.clear();
                } else {
                    ordered = orderedLabel(label, &number) > 0 || std::isdigit((unsigned char)label[0]);
                    if (!ordered && label.size() > 7) label.clear();
                }
            }
            size_t from = 0;
            if (!forced && label.empty() && !ordered) {
                std::string found;
                from = splitListLabel(body, &found, &ordered, &number);
                label = found;
            }
            RunBuilder rb;
            const fz_stext_line* line = nullptr;
            for (size_t c = from; c < body.size(); ++c) {
                const PageChar* pc = body[c];
                if (pc->ch->c > 32) {
                    if (line && pc->line != line) rb.wrap((uint32_t)pc->ch->c);
                    line = pc->line;
                }
                rb.add((uint32_t)pc->ch->c, isBoldFont(ctx, pc->ch->font));
            }
            b.runs = rb.take();
            if (b.runs.empty() && label.empty()) continue;
            b.listDepth = forced ? labelDepth(parentPath(el.path)) : el.listDepth;
            std::string marker;
            if (ordered) {
                b.kind = BlockKind::OrderedItem;
                b.level = number;
                marker = !label.empty() && label.size() <= 7 ? label + " "
                                                             : std::to_string(number) + ". ";
            } else {
                b.kind = BlockKind::UnorderedItem;
                marker = listBullet(b.listDepth);
            }
            b.runs.insert(b.runs.begin(), InlineRun{InlineKind::Text, marker, 0});
        } else {
            RunBuilder rb;
            const fz_stext_line* line = nullptr;
            float size = 0;
            int n = 0, lines = 0;
            for (const PageChar* pc : ec) {
                if (pc->ch->c > 32) {
                    if (line && pc->line != line) rb.wrap((uint32_t)pc->ch->c);
                    if (pc->line != line) ++lines;
                    line = pc->line;
                    size += pc->ch->size;
                    ++n;
                }
                rb.add((uint32_t)pc->ch->c, isBoldFont(ctx, pc->ch->font));
            }
            b.runs = rb.take();
            if (b.runs.empty()) continue;
            if (cont) {
                appendContinuation(&out->blocks[flow->lastBlock], std::move(b.runs));
                ++stats->merged;
                continue;
            }
            const float avg = n > 0 ? size / n : bodySize;
            size_t bytes = 0;
            for (const auto& r : b.runs) bytes += r.text.size();
            switch (el.type) {
            case FZ_STRUCTURE_H1: case FZ_STRUCTURE_H2: case FZ_STRUCTURE_H3:
            case FZ_STRUCTURE_H4: case FZ_STRUCTURE_H5: case FZ_STRUCTURE_H6:
                b.kind = BlockKind::Heading;
                b.level = 1 + (el.type - FZ_STRUCTURE_H1);
                break;
            case FZ_STRUCTURE_TITLE:
                b.kind = BlockKind::Heading;
                b.level = 1;
                break;
            case FZ_STRUCTURE_H:
                b.kind = BlockKind::Heading;
                b.level = std::max(1, headingLevelForSize(avg, bodySize) - 1);
                break;
            default:
                if (avg >= headingThreshold && lines <= 3 && bytes <= 200) {
                    b.kind = BlockKind::Heading;
                    b.level = headingLevelForSize(avg, bodySize);
                }
                break;
            }
            for (const auto& step : el.path) b.quoteDepth |= step.first == FZ_STRUCTURE_BLOCKQUOTE;
        }
        out->blocks.push_back(std::move(b));
        flow->lastPath = el.path;
        flow->lastBlock = (int)out->blocks.size() - 1;
    }
}

bool isCjkNumeral(int c) {
    return c == 0x4E00 || c == 0x4E8C || c == 0x4E09 || c == 0x56DB || c == 0x4E94 ||
           c == 0x516D || c == 0x4E03 || c == 0x516B || c == 0x4E5D || c == 0x5341 ||
           c == 0x767E || c == 0x96F6;
}

std::string edgeKey(const fz_stext_line* line, fz_rect page) {
    const float h = page.y1 - page.y0;
    if (h <= 0) return {};
    const bool top = line->bbox.y1 < page.y0 + 0.08f * h;
    const bool bottom = line->bbox.y0 > page.y1 - 0.08f * h;
    if (!top && !bottom) return {};
    std::string key = top ? "T" : "B";
    key += std::to_string((int)std::lround((line->bbox.y0 - page.y0) / h * 100));
    key += ':';
    const size_t head = key.size();
    for (const fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
        if (ch->c <= 32) continue;
        if ((ch->c >= '0' && ch->c <= '9') || isCjkNumeral(ch->c)) {
            if (key.back() != '#') key += '#';
            continue;
        }
        appendUtf8(key, (uint32_t)ch->c);
    }
    return key.size() > head ? key : std::string();
}

bool isPageNumberText(std::string s) {
    for (const char* prefix : {"Page", "page", "PAGE", "p.", "\xE7\xAC\xAC"})
        if (s.compare(0, std::strlen(prefix), prefix) == 0) { s.erase(0, std::strlen(prefix)); break; }
    for (const char* suffix : {"\xE9\xA1\xB5", "\xE9\xA0\x81"}) {
        const size_t n = std::strlen(suffix);
        if (s.size() >= n && s.compare(s.size() - n, n, suffix) == 0) { s.resize(s.size() - n); break; }
    }
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s.resize(slash);
    const size_t of = s.find("of");
    if (of != std::string::npos && of > 0) s.resize(of);
    std::string core;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0x93 || (unsigned char)s[i + 2] == 0x94)) { i += 3; continue; }
        if (std::strchr("-|[]().", c)) { ++i; continue; }
        core.push_back((char)c);
        ++i;
    }
    if (core.empty() || core.size() > 7) return false;
    if (core.find_first_not_of("0123456789") == std::string::npos) return core.size() <= 4;
    return core.find_first_not_of("ivxlcdm") == std::string::npos ||
           core.find_first_not_of("IVXLCDM") == std::string::npos;
}

bool endsSentence(const std::vector<const PageChar*>& chars) {
    int k = (int)chars.size() - 1;
    while (k >= 0 && chars[k]->ch->c <= 32) --k;
    if (k < 0) return false;
    auto closing = [](int c) {
        return c == 0x201D || c == 0x2019 || c == '"' || c == '\'' || c == 0x300D ||
               c == 0x300F || c == 0xFF09 || c == ')' || c == ']';
    };
    int c = chars[k]->ch->c;
    if (closing(c) && k > 0) c = chars[k - 1]->ch->c;
    return c == '.' || c == '!' || c == '?' || c == ':' || c == 0x3002 || c == 0xFF01 ||
           c == 0xFF1F || c == 0xFF1A || c == 0x2026 || c == 0xFF0E;
}

struct UntaggedAtom {
    bool line = false;
    Block block;
    std::vector<InlineRun> runs;
    std::vector<InlineRun> bodyRuns;
    std::string label;
    bool ordered = false;
    int number = 0;
    bool labelOnly = false;
    float markerX = 0;
    ReadingLine geo;
    fz_rect box = fz_empty_rect;
    int page = 0;
};

bool buildLineAtom(fz_context* ctx, fz_stext_line* line,
                   const std::unordered_set<const fz_stext_char*>& skip, UntaggedAtom* a,
                   const CodepointSizeTable* cpSizes, const PdfTableEvidence* ev,
                   const std::vector<bool>* consumed) {
    std::vector<PageChar> store;
    for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
        if (ch->c <= 0 || ch->c == 0xFFFD || skip.count(ch)) continue;
        store.push_back({line, ch, -1, false});
    }
    std::vector<const PageChar*> chars;
    std::vector<float> sizes, baselines;
    bool bold = true;
    for (const auto& pc : store) {
        chars.push_back(&pc);
        if (pc.ch->c <= 32) continue;
        const fz_rect b = charBox(pc.ch);
        a->box = fz_is_empty_rect(a->box) ? b : fz_union_rect(a->box, b);
        sizes.push_back(pc.ch->size);
        baselines.push_back(pc.ch->origin.y);
        bold &= isBoldFont(ctx, pc.ch->font);
    }
    if (sizes.empty()) return false;
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    std::nth_element(baselines.begin(), baselines.begin() + baselines.size() / 2, baselines.end());
    a->line = true;
    a->geo.x0 = a->box.x0;
    a->geo.x1 = a->box.x1;
    a->geo.size = sizes[sizes.size() / 2];
    a->geo.baseline = baselines[baselines.size() / 2];
    a->box.y0 = a->geo.baseline - 0.8f * a->geo.size;
    a->box.y1 = a->geo.baseline + 0.2f * a->geo.size;
    a->geo.bold = bold;
    a->geo.sentenceEnd = endsSentence(chars);
    float wordEnd = a->box.x0;
    for (const PageChar* pc : chars) {
        if (pc->ch->c <= 32) { if (wordEnd > a->box.x0) break; continue; }
        wordEnd = charBox(pc->ch).x1;
        if (textjoin::isWideScript((uint32_t)pc->ch->c)) break;
    }
    a->geo.firstWordW = wordEnd - a->box.x0;
    RunBuilder all;
    const float lineBase = a->geo.baseline;
    for (const PageChar* pc : chars) {
        float nominal = 0.0f;
        if (cpSizes) {
            auto it = cpSizes->find((uint32_t)pc->ch->c);
            if (it != cpSizes->end()) nominal = it->second;
        }
        all.add((uint32_t)pc->ch->c, isBoldFont(ctx, pc->ch->font),
                classifyGlyph(pc->ch->size, pc->ch->origin.y, nominal, a->geo.size, lineBase));
    }
    a->runs = all.take();

    // ---- recover sub/superscript glyphs the stext layer dropped ---------
    // MuPDF's structured-text device sometimes omits glyphs set in a smaller
    // sub/superscript font (e.g. the CMR8/CMR6 subscript fonts in LaTeX math).
    // Those glyphs ARE present in the raw device layer ('ev->textGlyphs',
    // collected by recordText from fz_text_span/fz_text_item) with a correct
    // Unicode ('ucs') and size/position ('box'). Recover them by position and
    // merge so 'x<sub>1</sub>' / 'x<sup>2</sup>' read correctly.
    if (ev && !ev->textGlyphs.empty()) {
        const fz_rect lb = line->bbox;
        const float yLo = lb.y0, yHi = lb.y1;
        // Collect device glyphs in this line's vertical band that stext did
        // not already surface (we match by position + codepoint against the
        // stext chars we just consumed).
        std::vector<const PdfGlyph*> orphans;
        for (size_t gi = 0; gi < ev->textGlyphs.size(); ++gi) {
            if (consumed && (*consumed)[gi]) continue;   // already matched to stext elsewhere
            const auto& g = ev->textGlyphs[gi];
            if (g.ucs <= 32 || g.ucs == 0xFFFD) continue;
            // glyph's vertical center must be within the line band
            const float cy = (g.box.y0 + g.box.y1) * 0.5f;
            if (cy < yLo || cy > yHi) continue;
            // skip if an stext char already covers this glyph (avoid duplicates)
            bool covered = false;
            for (const PageChar* pc : chars) {
                if (std::fabs(pc->ch->origin.y - g.y) < 0.35f
                    && std::fabs(pc->ch->origin.x - g.x) < 0.35f
                    && (uint32_t)pc->ch->c == (uint32_t)g.ucs) {
                    covered = true;
                    break;
                }
            }
            if (covered) continue;
            orphans.push_back(&g);
        }
        if (!orphans.empty()) {
            std::sort(orphans.begin(), orphans.end(),
                      [](const PdfGlyph* a, const PdfGlyph* b) { return a->x < b->x; });
            // Rebuild runs with the recovered glyphs interleaved by x.
            RunBuilder merged;
            size_t oi = 0;
            auto emit = [&](uint32_t c, float size, float baseY, bool bold, float x) {
                (void)x;
                float nominal = 0.0f;
                if (cpSizes) {
                    auto it = cpSizes->find(c);
                    if (it != cpSizes->end()) nominal = it->second;
                }
                merged.add(c, bold, classifyGlyph(size, baseY, nominal, a->geo.size, lineBase));
            };
            for (const PageChar* pc : chars) {
                const float cx = pc->ch->origin.x;
                while (oi < orphans.size() && orphans[oi]->x < cx) {
                    emit((uint32_t)orphans[oi]->ucs, orphans[oi]->box.y1 - orphans[oi]->box.y0,
                         orphans[oi]->y, false, orphans[oi]->x);
                    ++oi;
                }
                emit((uint32_t)pc->ch->c, pc->ch->size, pc->ch->origin.y,
                     isBoldFont(ctx, pc->ch->font), cx);
            }
            while (oi < orphans.size()) {
                emit((uint32_t)orphans[oi]->ucs, orphans[oi]->box.y1 - orphans[oi]->box.y0,
                     orphans[oi]->y, false, orphans[oi]->x);
                ++oi;
            }
            a->runs = merged.take();
        }
    }

    const size_t from = splitListLabel(chars, &a->label, &a->ordered, &a->number, true);
    if (from > 0) {
        size_t k = from;
        while (k < chars.size() && chars[k]->ch->c <= 32) ++k;
        a->markerX = a->box.x0;
        if (k >= chars.size()) {
            a->labelOnly = true;
        } else {
            a->geo.itemTextX = charBox(chars[k]->ch).x0;
            a->geo.itemOrdered = a->ordered;
            RunBuilder body;
            for (size_t j = from; j < chars.size(); ++j) {
                float nominal = 0.0f;
                if (cpSizes) {
                    auto it = cpSizes->find((uint32_t)chars[j]->ch->c);
                    if (it != cpSizes->end()) nominal = it->second;
                }
                body.add((uint32_t)chars[j]->ch->c, isBoldFont(ctx, chars[j]->ch->font),
                         classifyGlyph(chars[j]->ch->size, chars[j]->ch->origin.y,
                                       nominal, a->geo.size, lineBase));
            }
            a->bodyRuns = body.take();
        }
    } else {
        a->label.clear();
        a->ordered = false;
        uint32_t only = 0;
        int visible = 0;
        for (const PageChar* pc : chars)
            if (pc->ch->c > 32) { only = (uint32_t)pc->ch->c; ++visible; }
        if (visible == 1 && isBulletChar(only) && !isDashBullet(only)) {
            a->labelOnly = true;
            a->markerX = a->box.x0;
        }
    }
    return true;
}

bool renderRegionImage(fz_context* ctx, fz_page* page, fz_rect r, Document* out, Block* block) {
    float scale = 4.0f;
    const float side = std::max(r.x1 - r.x0, r.y1 - r.y0);
    if (side * scale > 2400.0f) scale = 2400.0f / std::max(1.0f, side);
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_buffer* png = nullptr;
    bool ok = false;
    fz_var(pix);
    fz_var(dev);
    fz_var(png);
    fz_var(ok);
    fz_try(ctx) {
        const fz_matrix m = fz_scale(scale, scale);
        const fz_irect ir = fz_round_rect(fz_transform_rect(r, m));
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), ir, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        dev = fz_new_draw_device(ctx, m, pix);
        fz_run_page(ctx, page, dev, fz_identity, nullptr);
        fz_close_device(ctx, dev);
        png = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
        unsigned char* bytes = nullptr;
        const size_t length = fz_buffer_storage(ctx, png, &bytes);
        if (bytes && length > 0) {
            ImageData data;
            data.rawBytes.assign(bytes, bytes + length);
            data.width = std::max(1, (int)std::lround((r.x1 - r.x0) * 1.5f));
            data.height = std::max(1, (int)std::lround((r.y1 - r.y0) * 1.5f));
            block->kind = BlockKind::Image;
            block->imageIndex = (int)out->images.size();
            out->images.push_back(std::move(data));
            ok = true;
        }
    } fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
        fz_drop_buffer(ctx, png);
    } fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

float rectDistance(fz_rect a, fz_rect b) {
    const float dx = std::max(0.0f, std::max(a.x0 - b.x1, b.x0 - a.x1));
    const float dy = std::max(0.0f, std::max(a.y0 - b.y1, b.y0 - a.y1));
    return std::max(dx, dy);
}

void mergeFigures(std::vector<fz_rect>* figs, float gap, float sideGap) {
    auto close = [&](fz_rect a, fz_rect b) {
        if (rectDistance(a, b) <= gap) return true;
        const float overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
        return overlap > 0.5f * std::min(a.y1 - a.y0, b.y1 - b.y0) && rectDistance(a, b) <= sideGap;
    };
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t a = 0; a < figs->size() && !merged; ++a)
            for (size_t b = a + 1; b < figs->size(); ++b)
                if (close((*figs)[a], (*figs)[b])) {
                    (*figs)[a] = fz_union_rect((*figs)[a], (*figs)[b]);
                    figs->erase(figs->begin() + (long)b);
                    merged = true;
                    break;
                }
    }
}

int absorbVectorFigures(fz_context* ctx, fz_page* page, const PdfTableEvidence& ev,
                        const std::vector<fz_rect>& seeds, const std::vector<fz_rect>& tables,
                        float bodySize, int pageIndex, std::vector<UntaggedAtom>* atoms,
                        Document* out) {
    const fz_rect bounds = fz_bound_page(ctx, page);
    std::vector<fz_rect> figures = findPdfFigures(ev, bounds, seeds, tables, bodySize);
    if (figures.empty()) return 0;
    if (std::getenv("HC_LAYOUT_DEBUG"))
        for (const auto& f : figures)
            std::fprintf(stderr, "  figure candidate page=%d %.0f,%.0f,%.0f,%.0f\n", pageIndex + 1,
                         f.x0, f.y0, f.x1, f.y1);
    mergeFigures(&figures, 1.2f * bodySize, 3.0f * bodySize);
    auto isLabel = [&](const UntaggedAtom& a, fz_rect fig) {
        if (!a.line) return false;
        const float w = a.box.x1 - a.box.x0, d = rectDistance(a.box, fig);
        if (a.geo.size <= 0.9f * bodySize && w <= 0.5f * (fig.x1 - fig.x0) && d <= 1.2f * bodySize)
            return true;
        return a.geo.size <= 1.05f * bodySize && !a.geo.bold && !a.geo.sentenceEnd &&
               ((w <= 5 * bodySize && d <= 0.8f * bodySize) || (w <= 3 * bodySize && d <= 1.2f * bodySize));
    };
    for (fz_rect& fig : figures)
        for (int pass = 0; pass < 3; ++pass)
            for (const UntaggedAtom& a : *atoms) {
                if (!a.line && a.block.kind == BlockKind::Table) continue;
                const fz_point c{(a.box.x0 + a.box.x1) * 0.5f, (a.box.y0 + a.box.y1) * 0.5f};
                if (fz_is_point_inside_rect(c, fz_expand_rect(fig, 1.0f)) || isLabel(a, fig))
                    fig = fz_union_rect(fig, a.box);
            }
    mergeFigures(&figures, 0.0f, 0.0f);
    int made = 0;
    for (const fz_rect& fig : figures) {
        std::vector<char> take(atoms->size(), 0);
        int bodyLines = 0;
        for (size_t k = 0; k < atoms->size(); ++k) {
            const UntaggedAtom& a = (*atoms)[k];
            if (!a.line && a.block.kind == BlockKind::Table) continue;
            const fz_point c{(a.box.x0 + a.box.x1) * 0.5f, (a.box.y0 + a.box.y1) * 0.5f};
            if (!fz_is_point_inside_rect(c, fz_expand_rect(fig, 1.0f))) continue;
            take[k] = 1;
            bodyLines += a.line && a.geo.size >= 0.95f * bodySize && a.box.x1 - a.box.x0 >= 15 * bodySize;
        }
        if (bodyLines >= 3) continue;
        UntaggedAtom image;
        if (!renderRegionImage(ctx, page, fz_expand_rect(fig, 2.0f), out, &image.block)) continue;
        image.box = fig;
        image.page = pageIndex;
        std::vector<UntaggedAtom> kept;
        for (size_t k = 0; k < atoms->size(); ++k)
            if (!take[k]) kept.push_back(std::move((*atoms)[k]));
        kept.push_back(std::move(image));
        *atoms = std::move(kept);
        ++made;
    }
    return made;
}

void appendOrdered(std::vector<UntaggedAtom>& atoms, int regionBase, std::vector<UntaggedAtom>* pending,
                   float minGutter, int* regions) {
    std::vector<ReadingBox> boxes;
    for (const auto& a : atoms) boxes.push_back({a.box.x0, a.box.y0, a.box.x1, a.box.y1});
    const ReadingOrder ro = readingOrder(boxes, minGutter);
    *regions = (int)ro.regions.size();
    for (int k : ro.order) {
        UntaggedAtom& a = atoms[k];
        if (a.line) a.geo.region = regionBase + ro.region[k];
        if (a.line && !pending->empty()) {
            UntaggedAtom& prev = pending->back();
            const float em = std::max(1.0f, a.geo.size);
            const float down = a.geo.baseline - prev.geo.baseline;
            if (prev.line && prev.labelOnly && prev.geo.region == a.geo.region &&
                a.box.x0 > prev.box.x1 && std::fabs(down) < 1.6f * em &&
                (std::fabs(down) < 0.4f * em || down > 0)) {
                std::vector<InlineRun> runs = prev.runs;
                if (!runs.empty()) runs.back().text += ' ';
                for (auto& r : a.runs) runs.push_back(r);
                prev.bodyRuns = std::move(a.runs);
                prev.runs = std::move(runs);
                prev.labelOnly = false;
                prev.box = fz_union_rect(prev.box, a.box);
                const float itemX = a.box.x0;
                const bool ordered = prev.ordered;
                prev.geo = a.geo;
                prev.geo.x0 = prev.box.x0;
                prev.geo.itemTextX = itemX;
                prev.geo.itemOrdered = ordered;
                continue;
            }
        }
        pending->push_back(std::move(a));
    }
}

void flushUntagged(std::vector<UntaggedAtom>* pending, Document* out, float bodySize,
                   float headingThreshold) {
    std::vector<UntaggedAtom>& atoms = *pending;
    std::vector<ReadingLine> lines;
    std::vector<int> lineOf(atoms.size(), -1), atomOf;
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!atoms[i].line) continue;
        lineOf[i] = (int)lines.size();
        atomOf.push_back((int)i);
        lines.push_back(atoms[i].geo);
    }
    std::vector<ReadingParagraph> paras;
    const std::vector<int> paraOf = segmentLines(lines, &paras);
    std::set<std::string> compounds;
    for (const auto& a : atoms)
        if (a.line) collectCompounds(a.runs, &compounds);
    std::vector<std::vector<int>> members(paras.size());
    for (size_t k = 0; k < lines.size(); ++k) members[paraOf[k]].push_back(atomOf[k]);
    std::vector<char> placed(paras.size(), 0);
    std::map<int, float> outerMarker;
    for (size_t p = 0; p < paras.size(); ++p) {
        if (!paras[p].item) continue;
        const UntaggedAtom& f = atoms[members[p].front()];
        auto it = outerMarker.find(f.geo.region);
        if (it == outerMarker.end() || f.markerX < it->second) outerMarker[f.geo.region] = f.markerX;
    }
    for (size_t i = 0; i < atoms.size(); ++i) {
        UntaggedAtom& a = atoms[i];
        if (!a.line) {
            a.block.sourceSpineIndex = a.page;
            out->blocks.push_back(std::move(a.block));
            continue;
        }
        const int p = paraOf[lineOf[i]];
        if (placed[p]) continue;
        placed[p] = 1;
        Block b;
        float sizeSum = 0;
        bool allBold = true;
        const auto& ms = members[p];
        const UntaggedAtom& first = atoms[ms.front()];
        size_t bytes = 0;
        for (const int k : ms) {
            const UntaggedAtom& m = atoms[k];
            sizeSum += m.geo.size;
            allBold &= m.geo.bold;
            for (const auto& r : m.runs) bytes += r.text.size();
        }
        const float avg = sizeSum / (float)ms.size();
        const bool lastEnds = atoms[ms.back()].geo.sentenceEnd;
        bool heading = false;
        if (avg >= headingThreshold && ms.size() <= 4 && bytes <= 300) {
            heading = true;
        } else if (allBold && ms.size() <= 2 && bytes <= 120 && !lastEnds && avg >= 0.95f * bodySize) {
            heading = true;
        }
        const bool item = !heading && paras[p].item && first.geo.itemTextX >= 0;
        for (size_t j = 0; j < ms.size(); ++j) {
            const UntaggedAtom& m = atoms[ms[j]];
            std::vector<InlineRun> runs = j == 0 && item ? m.bodyRuns : m.runs;
            if (j == 0) b.runs = std::move(runs);
            else appendContinuation(&b, std::move(runs), &compounds);
        }
        if (b.runs.empty()) continue;
        b.sourceSpineIndex = first.page;
        if (heading) {
            b.kind = BlockKind::Heading;
            b.level = headingLevelForSize(avg, bodySize);
        } else if (item) {
            const float em = std::max(1.0f, first.geo.size);
            const float indent = first.markerX - outerMarker[first.geo.region];
            b.listDepth = std::clamp((int)std::lround(indent / (1.3f * em)), 0, 3);
            std::string marker;
            if (first.ordered) {
                b.kind = BlockKind::OrderedItem;
                b.level = first.number;
                marker = !first.label.empty() && first.label.size() <= 7 ? first.label + " "
                                                                         : std::to_string(first.number) + ". ";
            } else {
                b.kind = BlockKind::UnorderedItem;
                marker = listBullet(b.listDepth);
            }
            b.runs.insert(b.runs.begin(), InlineRun{InlineKind::Text, marker, 0});
            out->blocks.push_back(std::move(b));
            continue;
        }
        out->blocks.push_back(std::move(b));
    }
    pending->clear();
}

}

bool parsePdf(const std::filesystem::path& path, Document* out, Logger* log,
              const PdfOcrText* ocr, bool ignoreText) {
    auto injectOcr = [&](fz_context* c, int pageIndex, fz_stext_page* st, bool dropImages) {
        if (!ocr) return false;
        const auto it = ocr->find(pageIndex);
        if (it == ocr->end()) return false;
        if (!ignoreText && stextVisibleChars(st) >= kOcrMaxSourceChars) return false;
        addOcrToStext(c, st, it->second, ignoreText, dropImages);
        return true;
    };
    if (!out) return false;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        if (log) log->log(LogLevel::Error, "pdfin", "fz_new_context failed");
        return false;
    }
    fz_register_document_handlers(ctx);

    fz_document* doc = nullptr;
    bool ok = false;

    fz_try(ctx) {
        doc = openMupdfDocument(ctx, path, log);
        if (!doc) fz_throw(ctx, FZ_ERROR_GENERIC, "open_document returned NULL");
        const bool reflowable = fz_is_document_reflowable(ctx, doc);
        if (reflowable) fz_layout_document(ctx, doc, 600, 3000, 12);
        readMetadata(ctx, doc, out);
        preparePdfStructure(ctx, doc);
        bool docTagged = false;
        if (pdf_document* pdf = pdf_specifics(ctx, doc))
            docTagged = pdf_dict_get(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/StructTreeRoot"),
                                     PDF_NAME(K)) != nullptr;

        fz_outline* ol = fz_load_outline(ctx, doc);
        if (ol) {
            walkOutline(ctx, doc, ol, 1, &out->importedOutline);
            fz_drop_outline(ctx, ol);
        }

        std::map<int, int> sizeHist;
        int npages = fz_count_pages(ctx, doc);
        std::map<std::string, int> edgeHits;
        int edgePages = 0;
        {
            const int kSampleTarget = 24;
            int first = (npages > 8) ? 2 : 0;
            int avail = npages - first;
            if (avail <= 0) { first = 0; avail = npages; }
            int stride = (avail + kSampleTarget - 1) / kSampleTarget;
            if (stride < 1) stride = 1;

            for (int i = first; i < npages; i += stride) {
                fz_page* page = fz_load_page(ctx, doc, i);
                fz_stext_options sopts{};
                fz_stext_page* stext = fz_new_stext_page_from_page(ctx, page, &sopts);
                injectOcr(ctx, i, stext, false);
                std::set<std::string> seen;
                ++edgePages;
                for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                    if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next) {
                        const std::string key = edgeKey(line, stext->mediabox);
                        if (!key.empty() && seen.insert(key).second) ++edgeHits[key];
                        for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                            if (ch->c <= 0x20 || ch->c == 0xFFFD) continue;
                            int key = (int)(roundSize(ch->size) * 10.0f);
                            sizeHist[key]++;
                        }
                    }
                }
                fz_drop_stext_page(ctx, stext);
                fz_drop_page(ctx, page);
            }
        }

        float bodySize = 10.0f;
        {
            int maxCount = 0;
            for (auto& [key, cnt] : sizeHist) {
                if (cnt > maxCount) {
                    maxCount = cnt;
                    bodySize = key / 10.0f;
                }
            }
        }
        float headingThreshold = bodySize * 1.15f;
        std::set<std::string> runningKeys;
        for (const auto& hit : edgeHits)
            if (hit.second >= std::max(2, (edgePages * 2 + 4) / 5)) runningKeys.insert(hit.first);

        if (log) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "pdf reflow: %d pages, body font=%.1fpt, headingThresh=%.1fpt, "
                "%zu bookmarks, %zu running header/footer lines",
                npages, bodySize, headingThreshold,
                out->importedOutline.size(), runningKeys.size());
            log->log(LogLevel::Info, "pdfin", msg);
        }

        // --- FIRST PASS: build the per-codepoint nominal-size table ---------
        // A full scan of every page is needed so that a subscript/superscript
        // can be compared against the SAME character's full-size appearance,
        // regardless of order (the sub/superscript may be encountered before
        // the character's normal appearance).
        CodepointSizeTable cpSizes;
        {
            fz_stext_options sopts{};
            for (int i = 0; i < npages; ++i) {
                if (currentJobCancelled()) break;
                fz_page* page = fz_load_page(ctx, doc, i);
                fz_stext_page* stext = fz_new_stext_page_from_page(ctx, page, &sopts);
                for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                    if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next)
                        for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
                            recordCodepointSize(&cpSizes, (uint32_t)ch->c, ch->size);
                }
                fz_drop_stext_page(ctx, stext);
                fz_drop_page(ctx, page);
            }
        }
        if (log) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "pdf reflow: codepoint size table = %zu entries",
                          cpSizes.size());
            log->log(LogLevel::Info, "pdfin", msg);
        }

        int imageCount = 0;
        int coveredPageImageCount = 0;
        TaggedFlow flow;
        std::vector<UntaggedAtom> pending;
        int regionBase = 0;
        for (int i = 0; i < npages; ++i) {
            if (currentJobCancelled()) break;
            fz_page* page = fz_load_page(ctx, doc, i);
            fz_stext_options sopts{};
            sopts.flags = FZ_STEXT_PRESERVE_IMAGES;
            const fz_rect pageBounds = fz_bound_page(ctx, page);
            fz_stext_page* stext = fz_new_stext_page(ctx, pageBounds);
            PdfTableEvidence evidence;
            {
                fz_device* sdev = fz_new_stext_device(ctx, stext, &sopts);
                fz_device* tdev = nullptr;
                fz_var(tdev);
                fz_try(ctx) {
                    tdev = newPdfTableEvidenceDevice(ctx, sdev, &evidence);
                    fz_run_page_contents(ctx, page, tdev, fz_identity, nullptr);
                    fz_close_device(ctx, tdev);
                    fz_close_device(ctx, sdev);
                } fz_always(ctx) {
                    fz_drop_device(ctx, tdev);
                    fz_drop_device(ctx, sdev);
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }
            if (!injectOcr(ctx, i, stext, true)) {
                const float invisible = evidence.textChars > 0
                    ? (float)evidence.invisibleChars / evidence.textChars : 0.f;
                const int dropped = dropScanPageImages(ctx, stext, invisible);
                if (dropped > 0) {
                    coveredPageImageCount += dropped;
                    if (log) {
                        char msg[128];
                        std::snprintf(msg, sizeof(msg),
                                      "pdf scan page=%d images dropped=%d invisible=%.2f",
                                      i + 1, dropped, invisible);
                        log->log(LogLevel::Info, "pdfin", msg);
                    }
                }
            }
            resolvePdfStructure(&evidence, ctx, doc, i);

            std::string tableWhy;
            const auto pageTables = findPdfTables(evidence, pageBounds, ctx, doc, &tableWhy);
            std::unordered_set<const fz_stext_char*> tableChars;
            std::vector<Block> tableBlocks;
            std::vector<fz_rect> tableBoxes;
            std::vector<PdfStructPath> tableKeys;
            std::vector<fz_rect> rejectedGrids;
            for (size_t ti = 0; ti < pageTables.size(); ++ti) {
                const PdfTable& table = pageTables[ti];
                const fz_rect area = fz_expand_rect(table.bbox, 2.0f);
                std::vector<std::vector<std::vector<CellChar>>> cellText(table.rows.size());
                for (size_t r = 0; r < table.rows.size(); ++r)
                    cellText[r].resize(table.rows[r].size());
                for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                    if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                    if (fz_is_empty_rect(fz_intersect_rect(tb->bbox, area))) continue;
                    for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next)
                        for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                            if (tableChars.count(ch)) continue;
                            const fz_point p = charCenter(ch);
                            if (!fz_is_point_inside_rect(p, area)) continue;
                            bool placed = false;
                            for (size_t r = 0; r < table.rows.size() && !placed; ++r)
                                for (size_t c = 0; c < table.rows[r].size(); ++c) {
                                    const PdfTableCell& cell = table.rows[r][c];
                                    if (cell.placeholder || !fz_is_point_inside_rect(p, cell.rect)) continue;
                                    cellText[r][c].push_back({line, ch});
                                    placed = true;
                                    break;
                                }
                        }
                }
                const bool ruled = std::string(table.source) == "ruled";
                std::vector<std::vector<PdfTableCell>> meta = table.rows;
                if (ruled) splitStackedRows(&cellText, &meta);
                Block tblock;
                tblock.kind = BlockKind::Table;
                int filled = 0;
                std::vector<int> rowBold(cellText.size()), rowInk(cellText.size());
                std::vector<float> rowSize(cellText.size());
                for (size_t r = 0; r < cellText.size(); ++r) {
                    TableRow row;
                    for (size_t c = 0; c < cellText[r].size(); ++c) {
                        if (meta[r][c].placeholder) continue;
                        TableCell cell;
                        cell.isHeader = meta[r][c].header;
                        cell.colspan = meta[r][c].colspan;
                        cell.rowspan = meta[r][c].rowspan;
                        cell.runs = cellRuns(ctx, cellText[r][c], &rowBold[r], &rowInk[r], &rowSize[r], &cpSizes);
                        filled += !cell.runs.empty();
                        row.cells.push_back(std::move(cell));
                    }
                    tblock.rows.push_back(std::move(row));
                }
                int cellCount = 0, multiRows = 0;
                for (const auto& row : table.rows)
                    for (const auto& cell : row) cellCount += !cell.placeholder;
                for (const auto& row : tblock.rows) {
                    int withText = 0;
                    for (const auto& cell : row.cells) withText += !cell.runs.empty();
                    multiRows += withText >= 2;
                }
                if (filled < 3 || filled < cellCount * 0.4 || (ruled && multiRows < 2)) {
                    if (ruled) rejectedGrids.push_back(table.bbox);
                    if (log) {
                        char msg[160];
                        std::snprintf(msg, sizeof(msg),
                            "pdf table page=%d source=%s rejected filled=%d/%d bbox=%.0f,%.0f,%.0f,%.0f",
                            i + 1, table.source, filled, cellCount,
                            table.bbox.x0, table.bbox.y0, table.bbox.x1, table.bbox.y1);
                        log->log(LogLevel::Info, "pdfin", msg);
                    }
                    continue;
                }
                if (ruled && tblock.rows.size() >= 2 && rowInk[0] > 0) {
                    int bodyInk = 0;
                    float bodySize = 0;
                    for (size_t r = 1; r < tblock.rows.size(); ++r) { bodyInk += rowInk[r]; bodySize += rowSize[r]; }
                    const bool bold = rowBold[0] >= rowInk[0] * 0.7;
                    const bool larger = bodyInk > 0 &&
                        rowSize[0] / rowInk[0] >= 1.08f * (bodySize / bodyInk);
                    if (bold || larger)
                        for (auto& cell : tblock.rows[0].cells) cell.isHeader = true;
                }
                for (const auto& row : cellText)
                    for (const auto& cell : row)
                        for (const auto& cc : cell) tableChars.insert(cc.ch);
                if (log) {
                    char msg[200];
                    std::snprintf(msg, sizeof(msg),
                        "pdf table page=%d source=%s rows=%zu cols=%zu filled=%d bbox=%.0f,%.0f,%.0f,%.0f",
                        i + 1, table.source, tblock.rows.size(),
                        tblock.rows.empty() ? (size_t)0 : tblock.rows[0].cells.size(), filled,
                        table.bbox.x0, table.bbox.y0, table.bbox.x1, table.bbox.y1);
                    log->log(LogLevel::Info, "pdfin", msg);
                }
                tableBlocks.push_back(std::move(tblock));
                tableBoxes.push_back(table.bbox);
                tableKeys.push_back(table.path);
            }
            if (log && (!pageTables.empty() || evidence.structureCalls > 0)) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "pdf tables page=%d %s kept=%zu",
                              i + 1, tableWhy.c_str(), tableBlocks.size());
                log->log(LogLevel::Info, "pdfin", msg);
            }

            int visibleChars = 0, taggedChars = 0;
            const std::vector<PageChar> pageChars =
                tagPageChars(stext, evidence, tableChars, &visibleChars, &taggedChars);

            // Mark which device-layer glyphs are already spoken for by stext,
            // so the sub/superscript recovery in buildLineAtom only picks up
            // glyphs the structured-text layer genuinely dropped (avoiding the
            // duplicate/interleaved text that results from re-adding body
            // glyphs that stext already emitted).
            std::vector<bool> consumedGlyphs(evidence.textGlyphs.size(), false);
            {
                const PdfGlyphLookup lookup(evidence);
                for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                    if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (fz_stext_line* l = tb->u.t.first_line; l; l = l->next)
                        for (fz_stext_char* ch = l->first_char; ch; ch = ch->next) {
                            if (ch->c <= 0 || ch->c == 0xFFFD) continue;
                            const int g = lookup.find(ch->origin, ch->c);
                            if (g >= 0) consumedGlyphs[g] = true;
                        }
                }
            }

            bool pageImages = false;
            for (fz_stext_block* tb = stext->first_block; tb && !pageImages; tb = tb->next)
                pageImages = tb->type == FZ_STEXT_BLOCK_IMAGE;
            const bool blankPage = docTagged && evidence.elements.empty() && !pageImages &&
                                   visibleChars <= 8;
            const bool taggedPage = !reflowable && (blankPage || (!evidence.elements.empty() &&
                (visibleChars == 0 || taggedChars >= visibleChars * 0.6)));
            if (log && !evidence.elements.empty()) {
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                    "pdf structure page=%d tagged=%s chars=%d inElements=%d artifacts=%d "
                    "elements=%zu stale=%d",
                    i + 1, taggedPage ? "yes" : "no", visibleChars, taggedChars,
                    evidence.artifactGlyphs, evidence.elements.size(), evidence.staleAssigned);
                log->log(LogLevel::Info, "pdfin", msg);
            }
            if (taggedPage) flushUntagged(&pending, out, bodySize, headingThreshold);
            const size_t pageFirstBlock = out->blocks.size();
            int pageTextChars = 0;
            for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next)
                    for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
                        pageTextChars += ch->c > 0x20 && ch->c != 0xFFFD;
            }
            if (std::getenv("HC_STRUCT_DEBUG") && log) {
                std::map<int, std::string> staleText;
                for (const auto& g : evidence.textGlyphs)
                    if (g.segment >= 0 && evidence.segments[g.segment].assigned)
                        appendUtf8(staleText[g.segment], (uint32_t)g.ucs);
                for (const auto& st : staleText) {
                    const std::string m = "  stale->element: " + st.second.substr(0, 80);
                    log->log(LogLevel::Info, "pdfin", m.c_str());
                }
            }
            if (taggedPage) {
                TaggedPageStats stats;
                emitTaggedPage(ctx, stext, evidence, pageChars, tableBlocks, tableBoxes, tableKeys,
                               bodySize, headingThreshold, pageTextChars, out, log, &flow, &stats,
                               &imageCount, &coveredPageImageCount);
                if (log) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg),
                        "pdf structure page=%d emitted elements=%d images=%d artifactImages=%d "
                        "icons=%d continued=%d",
                        i + 1, stats.elements, stats.images, stats.droppedImages, stats.icons,
                        stats.merged);
                    log->log(LogLevel::Info, "pdfin", msg);
                }
                for (size_t b = pageFirstBlock; b < out->blocks.size(); ++b)
                    out->blocks[b].sourceSpineIndex = i;
            } else {
                flow.lastBlock = -1;
                const fz_rect media = stext->mediabox;
                const float pageW = media.x1 - media.x0, pageH = media.y1 - media.y0;
                int rotatedChars = 0, allChars = 0;
                for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                    if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next)
                        for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                            if (ch->c <= 32) continue;
                            ++allChars;
                            rotatedChars += std::fabs(line->dir.y) > 0.3f;
                        }
                }
                const bool dropRotated = rotatedChars < allChars * 0.3;
                std::vector<UntaggedAtom> atoms;
                int headerLines = 0, pageNumbers = 0, rotatedLines = 0, icons = 0;
                for (fz_stext_block* tb = stext->first_block; tb; tb = tb->next) {
                    if (tb->type == FZ_STEXT_BLOCK_IMAGE) {
                        const fz_rect b = tb->bbox;
                        const float area = std::max(0.0f, b.x1 - b.x0) * std::max(0.0f, b.y1 - b.y0);
                        if (pageTextChars >= 16 && area >= std::max(1.0f, pageW * pageH) * 0.65f) {
                            ++coveredPageImageCount;
                            continue;
                        }
                        if (std::max(b.x1 - b.x0, b.y1 - b.y0) <= 2.5f * bodySize) {
                            ++icons;
                            continue;
                        }
                        if (!appendImageBlock(ctx, tb, out, log)) continue;
                        ++imageCount;
                        UntaggedAtom a;
                        a.block = std::move(out->blocks.back());
                        out->blocks.pop_back();
                        a.box = b;
                        a.page = i;
                        atoms.push_back(std::move(a));
                        continue;
                    }
                    if (tb->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (fz_stext_line* line = tb->u.t.first_line; line; line = line->next) {
                        if (dropRotated && std::fabs(line->dir.y) > 0.3f) { ++rotatedLines; continue; }
                        const std::string key = edgeKey(line, media);
                        if (!key.empty()) {
                            if (runningKeys.count(key)) { ++headerLines; continue; }
                            std::string text;
                            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
                                if (ch->c > 32) appendUtf8(text, (uint32_t)ch->c);
                            if (isPageNumberText(text)) { ++pageNumbers; continue; }
                        }
                        UntaggedAtom a;
                        a.page = i;
                        if (buildLineAtom(ctx, line, tableChars, &a, &cpSizes, &evidence, &consumedGlyphs)) atoms.push_back(std::move(a));
                    }
                }
                const int figures = absorbVectorFigures(ctx, page, evidence, rejectedGrids, tableBoxes,
                                                        bodySize, i, &atoms, out);
                imageCount += figures;
                for (size_t ti = 0; ti < tableBlocks.size(); ++ti) {
                    UntaggedAtom a;
                    a.block = std::move(tableBlocks[ti]);
                    a.block.sourceSpineIndex = i;
                    a.box = tableBoxes[ti];
                    a.page = i;
                    atoms.push_back(std::move(a));
                }
                int regions = 0;
                const size_t lineCount = atoms.size();
                const size_t before = pending.size();
                appendOrdered(atoms, regionBase, &pending, std::max(6.0f, 0.9f * bodySize), &regions);
                regionBase += regions;
                if (std::getenv("HC_LAYOUT_DEBUG") && log) {
                    fz_rect art = fz_empty_rect;
                    for (const auto& r : evidence.artwork) art = fz_is_empty_rect(art) ? r : fz_union_rect(art, r);
                    char am[128];
                    std::snprintf(am, sizeof(am), "  artwork page=%d n=%zu bounds=%.0f,%.0f,%.0f,%.0f", i + 1,
                                  evidence.artwork.size(), art.x0, art.y0, art.x1, art.y1);
                    log->log(LogLevel::Info, "pdfin", am);
                    for (size_t k = before; k < pending.size(); ++k) {
                        const UntaggedAtom& a = pending[k];
                        char head[160];
                        std::snprintf(head, sizeof(head), "  atom p%d r%d %.0f,%.0f-%.0f,%.0f s%.1f%s%s%s ",
                                      i + 1, a.line ? a.geo.region : -1, a.box.x0, a.box.y0, a.box.x1,
                                      a.box.y1, a.geo.size, a.geo.bold ? " B" : "",
                                      a.geo.sentenceEnd ? " ." : "", a.geo.itemTextX >= 0 ? " item" : "");
                        std::string m = head;
                        if (!a.line) m += a.block.kind == BlockKind::Table ? "[table]" : "[image]";
                        for (const auto& r : a.runs) m += r.text;
                        log->log(LogLevel::Info, "pdfin", m.substr(0, 220).c_str());
                    }
                }
                if (log) {
                    char msg[200];
                    std::snprintf(msg, sizeof(msg),
                        "pdf layout page=%d items=%zu regions=%d runningHeaders=%d pageNumbers=%d "
                        "rotated=%d vectorFigures=%d icons=%d",
                        i + 1, lineCount, regions, headerLines, pageNumbers, rotatedLines, figures, icons);
                    log->log(LogLevel::Info, "pdfin", msg);
                }
            }

            fz_drop_stext_page(ctx, stext);
            fz_drop_page(ctx, page);
        }
        flushUntagged(&pending, out, bodySize, headingThreshold);
        if (log) {
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "pdf reflow: preserved %d images, folded %d scanned page images",
                          imageCount, coveredPageImageCount);
            log->log(LogLevel::Info, "pdfin", msg);
        }
        ok = true;
    } fz_catch(ctx) {
        if (log) log->log(LogLevel::Error, "pdfin", fz_caught_message(ctx));
        ok = false;
    }

    if (doc) fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return ok;
}

}
