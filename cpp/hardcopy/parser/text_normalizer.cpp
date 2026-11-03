#include "text_normalizer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../util/text_join.h"

namespace hardcopy {
namespace {

bool isSpace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n'
        || cp == 0x00A0 || cp == 0x3000;
}

bool isTextRun(InlineKind kind) {
    return kind == InlineKind::Text || kind == InlineKind::Emphasis
        || kind == InlineKind::Strong || kind == InlineKind::Superscript
        || kind == InlineKind::Subscript || kind == InlineKind::Ruby;
}

size_t utf8Width(uint32_t cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

void appendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::vector<uint32_t> decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = textjoin::decodeAt(text, i);
        if (cp == 0) {
            cp = (uint8_t)text[i];
            ++i;
        } else {
            i += utf8Width(cp);
        }
        out.push_back(cp);
    }
    return out;
}

std::string encodeUtf8(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) appendUtf8(&out, cp);
    return out;
}

void stripLeadingSpaces(std::string* text) {
    size_t i = 0;
    while (i < text->size()) {
        uint32_t cp = textjoin::decodeAt(*text, i);
        if (cp == 0 || !isSpace(cp)) break;
        i += utf8Width(cp);
    }
    if (i > 0) text->erase(0, i);
}

void stripTrailingSpaces(std::string* text) {
    auto cps = decodeUtf8(*text);
    while (!cps.empty() && isSpace(cps.back())) cps.pop_back();
    *text = encodeUtf8(cps);
}

bool isCollapsibleAscii(uint32_t cp) {
    switch (cp) {
        case '.': case '\\': case '/': case '@': case '$':
        case '*': case '^': case '&': case '|':
            return true;
        default:
            return false;
    }
}

bool isAsciiDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

uint32_t prevSolid(const std::vector<uint32_t>& v, size_t i) {
    while (i > 0) {
        --i;
        if (!isSpace(v[i])) return v[i];
    }
    return 0;
}

uint32_t nextSolid(const std::vector<uint32_t>& v, size_t i) {
    for (++i; i < v.size(); ++i)
        if (!isSpace(v[i])) return v[i];
    return 0;
}

bool cjkContext(const std::vector<uint32_t>& v, size_t i) {
    return textjoin::isWideScript(prevSolid(v, i))
        || textjoin::isWideScript(nextSolid(v, i));
}

std::string normalizePunctuation(const std::string& text, bool* quoteOpen) {
    std::vector<uint32_t> src = decodeUtf8(text);
    std::vector<uint32_t> mapped;
    mapped.reserve(src.size());

    for (size_t i = 0; i < src.size(); ++i) {
        uint32_t cp = src[i];
        const uint32_t before = i > 0 ? src[i - 1] : 0;
        const uint32_t after = i + 1 < src.size() ? src[i + 1] : 0;
        const bool betweenDigits = isAsciiDigit(before) && isAsciiDigit(after);
        const bool inUrl = (cp == ':' && after == '/')
                        || (cp == '?' && after != 0 && !isSpace(after)
                            && !textjoin::isWideScript(after) && after < 0x80
                            && before < 0x80 && before != 0 && !isSpace(before));
        const bool afterFullwidth = !mapped.empty()
            && mapped.back() >= 0xFF01 && mapped.back() <= 0xFF1F;
        const bool convert = !betweenDigits && !inUrl
            && (afterFullwidth || cjkContext(src, i));
        if (convert) {
            switch (cp) {
                case ',': cp = 0xFF0C; break;
                case '?': cp = 0xFF1F; break;
                case '!': cp = 0xFF01; break;
                case ':': cp = 0xFF1A; break;
                case ';': cp = 0xFF1B; break;
                case '(': cp = 0xFF08; break;
                case ')': cp = 0xFF09; break;
                default: break;
            }
        }
        if (cp == 0x2505) cp = 0x2026;
        mapped.push_back(cp);
    }

    for (auto& cp : mapped) {
        if (cp != '"') continue;
        cp = *quoteOpen ? 0x201D : 0x201C;
        *quoteOpen = !*quoteOpen;
    }

    std::vector<uint32_t> out;
    out.reserve(mapped.size());
    for (size_t i = 0; i < mapped.size();) {
        uint32_t cp = mapped[i];
        if (isSpace(cp)) {
            size_t j = i + 1;
            while (j < mapped.size() && isSpace(mapped[j])) ++j;
            const bool cjk = textjoin::isWideScript(i > 0 ? mapped[i - 1] : 0)
                || textjoin::isWideScript(j < mapped.size() ? mapped[j] : 0);
            if (j - i >= 2 && cjk) {
                out.push_back(0x3000);
                out.push_back(0x3000);
            } else {
                out.push_back(j - i >= 2 ? ' ' : cp);
            }
            i = j;
            continue;
        }
        size_t j = i + 1;
        while (j < mapped.size() && mapped[j] == cp) ++j;
        const size_t count = j - i;
        const bool afterCjk = textjoin::isWideScript(prevSolid(mapped, i));

        if (cp == '<' && count >= 2) {
            out.push_back(0x300A);
            for (size_t n = 2; n < count; ++n) out.push_back(cp);
        } else if (cp == '>' && count >= 2) {
            out.push_back(0x300B);
            for (size_t n = 2; n < count; ++n) out.push_back(cp);
        } else if (cp == 0x3002 && count >= 2) {
            out.push_back(0x2026);
            out.push_back(0x2026);
        } else if (cp == '.' && count >= 3 && afterCjk) {
            out.push_back(0x2026);
            out.push_back(0x2026);
        } else if (cp == 0xFF0C || cp == 0xFF1F || cp == 0xFF01
                || cp == 0xFF1A || cp == 0xFF08 || cp == 0xFF09
                || (isCollapsibleAscii(cp) && afterCjk)) {
            out.push_back(cp);
        } else if (cp == 0x2026) {
            out.push_back(cp);
            if (count == 1) out.push_back(cp);
            else for (size_t n = 1; n < count; ++n) out.push_back(cp);
        } else {
            for (size_t n = 0; n < count; ++n) out.push_back(cp);
        }
        i = j;
    }

    std::vector<uint32_t> cleaned;
    cleaned.reserve(out.size());
    for (uint32_t cp : out) {
        if (!cleaned.empty()
            && ((cleaned.back() == 0x3002 && cp == 0x2026)
                || (cleaned.back() == 0x2026 && cp == 0x3002))) {
            cleaned.back() = 0x2026;
            continue;
        }
        cleaned.push_back(cp);
    }
    return encodeUtf8(cleaned);
}

void normalizeRuns(std::vector<InlineRun>* runs, bool punctuation) {
    bool quoteOpen = false;
    for (auto& run : *runs) {
        if (!isTextRun(run.kind)) continue;
        if (punctuation) run.text = normalizePunctuation(run.text, &quoteOpen);
    }

    for (auto it = runs->rbegin(); it != runs->rend(); ++it) {
        if (!isTextRun(it->kind)) break;
        stripTrailingSpaces(&it->text);
        if (!it->text.empty()) break;
    }
}

bool blockTextEmpty(const Block& block) {
    if (block.kind != BlockKind::Paragraph) return false;
    for (const auto& run : block.runs) {
        if (run.kind == InlineKind::LineBreak) continue;
        if (!run.text.empty()) return false;
    }
    return true;
}

}

void normalizeSourceParagraphIndents(Document* doc) {
    if (!doc) return;
    for (auto& block : doc->blocks) {
        if (block.kind != BlockKind::Paragraph) continue;
        for (auto& run : block.runs) {
            if (!isTextRun(run.kind)) break;
            stripLeadingSpaces(&run.text);
            if (!run.text.empty()) break;
        }
    }
}

void normalizeCompactText(Document* doc) {
    if (!doc) return;
    for (auto& block : doc->blocks) {
        if (block.kind == BlockKind::CodeBlock || block.kind == BlockKind::MathBlock)
            continue;
        normalizeRuns(&block.runs, true);
        if (block.kind == BlockKind::Table) {
            for (auto& row : block.rows)
                for (auto& cell : row.cells)
                    normalizeRuns(&cell.runs, true);
        }
    }
    doc->blocks.erase(
        std::remove_if(doc->blocks.begin(), doc->blocks.end(), blockTextEmpty),
        doc->blocks.end());
}

}
