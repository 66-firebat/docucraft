#include "source_export.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#include <nlohmann/json.hpp>

namespace hardcopy {

namespace {

using nlohmann::json;

size_t codepointCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

std::string trimText(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string prefixCodepoints(const std::string& s, size_t limit) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < limit) {
        ++i;
        while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
        ++n;
    }
    return s.substr(0, i);
}

std::string matchKey(const std::string& s, size_t limit) {
    std::string key;
    for (size_t i = 0; i < s.size() && key.size() < limit;) {
        const unsigned char c = (unsigned char)s[i];
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        if (c < 0x80) {
            if (std::isalnum(c)) key.push_back((char)std::tolower(c));
        } else {
            const bool punct = (c == 0xE2 && i + 1 < s.size() && (unsigned char)s[i + 1] < 0x82)
                || (c == 0xE3 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0x80)
                || (c == 0xEF && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xBC);
            if (!punct) key.append(s, i, std::min(len, s.size() - i));
        }
        i += len;
    }
    return key;
}

std::string kindName(const Block& b) {
    switch (b.kind) {
        case BlockKind::Paragraph:     return "p";
        case BlockKind::Heading:       return "h" + std::to_string(std::clamp(b.level, 1, 6));
        case BlockKind::UnorderedItem: return "li";
        case BlockKind::OrderedItem:   return "oli";
        case BlockKind::CodeBlock:     return "code";
        case BlockKind::MathBlock:     return "math";
        case BlockKind::ThematicBreak: return "hr";
        case BlockKind::PageBreak:     return "pagebreak";
        case BlockKind::Blockquote:    return "quote";
        case BlockKind::Image:         return "image";
        case BlockKind::Table:         return "table";
    }
    return "p";
}

bool hasListMarker(const Block& b) {
    if (b.kind != BlockKind::OrderedItem && b.kind != BlockKind::UnorderedItem) return false;
    if (b.runs.empty() || b.runs[0].kind == InlineKind::LineBreak) return false;
    const std::string& m = b.runs[0].text;
    return !m.empty() && m.size() <= 8 && m.back() == ' ';
}

std::string blockText(const Block& b) {
    return runsPlainText(b.runs, hasListMarker(b) ? 1 : 0);
}

struct Section {
    std::string title;
    int level = 1;
    int firstBlock = 0;
    int lastBlock = -1;
};

std::vector<Section> buildSections(const Document& doc, int pages) {
    const int n = (int)doc.blocks.size();
    std::vector<Section> starts;
    if (!doc.importedOutline.empty()) {
        int shallow = 0;
        for (const auto& e : doc.importedOutline) shallow += e.level <= 2;
        const int maxLevel = shallow <= 400 ? 2 : 1;
        for (const auto& e : doc.importedOutline) {
            if (e.level > maxLevel) continue;
            const std::string title = matchKey(e.title, 48);
            int start = -1, fallback = -1;
            for (int bi = 0; bi < n; ++bi) {
                const Block& b = doc.blocks[bi];
                if (b.sourceSpineIndex < e.pageIndex0) continue;
                if (fallback < 0) fallback = bi;
                if (b.sourceSpineIndex > e.pageIndex0 || title.empty()) break;
                const std::string key = matchKey(blockText(b), title.size());
                if (!key.empty() && key.size() >= std::min<size_t>(title.size(), 4) &&
                    title.compare(0, key.size(), key) == 0) {
                    start = bi;
                    break;
                }
            }
            if (start < 0) start = fallback;
            if (start < 0) continue;
            Section s;
            s.title = trimText(e.title);
            s.level = std::max(1, e.level);
            s.firstBlock = start;
            starts.push_back(std::move(s));
        }
    }
    if (starts.size() < 2) {
        std::vector<Section> heads;
        for (int bi = 0; bi < n; ++bi) {
            const Block& b = doc.blocks[bi];
            if (b.kind != BlockKind::Heading || b.level > 2) continue;
            Section s;
            s.title = trimText(blockText(b));
            s.level = std::max(1, b.level);
            s.firstBlock = bi;
            heads.push_back(std::move(s));
        }
        if (heads.size() >= 2 || starts.empty()) starts = std::move(heads);
    }
    if (starts.size() < 2 && pages > 20) {
        starts.clear();
        int nextPage = 0;
        for (int bi = 0; bi < n; ++bi) {
            const int p = doc.blocks[bi].sourceSpineIndex;
            if (p < nextPage) continue;
            Section s;
            s.firstBlock = bi;
            starts.push_back(s);
            nextPage = (p / 20 + 1) * 20;
        }
    }
    if (starts.size() < 2) {
        starts.clear();
        size_t chars = 0;
        for (int bi = 0; bi < n; ++bi) {
            if (bi == 0 || chars >= 20000) {
                Section s;
                s.firstBlock = bi;
                starts.push_back(s);
                chars = 0;
            }
            const Block& b = doc.blocks[bi];
            chars += codepointCount(blockText(b));
        }
    }
    std::stable_sort(starts.begin(), starts.end(),
                     [](const Section& a, const Section& b) { return a.firstBlock < b.firstBlock; });
    std::vector<Section> out;
    for (auto& s : starts) {
        if (!out.empty() && out.back().firstBlock == s.firstBlock) continue;
        out.push_back(std::move(s));
    }
    if (n > 0 && (out.empty() || out.front().firstBlock > 0)) {
        Section front;
        front.firstBlock = 0;
        out.insert(out.begin(), front);
    }
    for (size_t k = 0; k < out.size(); ++k)
        out[k].lastBlock = k + 1 < out.size() ? out[k + 1].firstBlock - 1 : n - 1;
    return out;
}

bool writeFileAtomic(const std::filesystem::path& path, const void* data, size_t size) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(static_cast<const char*>(data), (std::streamsize)size);
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

uint32_t lastSentenceCodepoint(const std::string& s) {
    size_t end = s.size();
    while (end > 0) {
        size_t start = end - 1;
        while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) --start;
        const unsigned char c0 = (unsigned char)s[start];
        uint32_t cp = c0;
        if (c0 >= 0xF0 && end - start >= 4)
            cp = ((c0 & 0x07u) << 18) | (((unsigned char)s[start + 1] & 0x3Fu) << 12) |
                 (((unsigned char)s[start + 2] & 0x3Fu) << 6) | ((unsigned char)s[start + 3] & 0x3Fu);
        else if (c0 >= 0xE0 && end - start >= 3)
            cp = ((c0 & 0x0Fu) << 12) | (((unsigned char)s[start + 1] & 0x3Fu) << 6) |
                 ((unsigned char)s[start + 2] & 0x3Fu);
        else if (c0 >= 0xC0 && end - start >= 2)
            cp = ((c0 & 0x1Fu) << 6) | ((unsigned char)s[start + 1] & 0x3Fu);
        const bool skip = cp == ' ' || cp == '\n' || cp == '"' || cp == '\'' || cp == ')' ||
                          cp == ']' || cp == 0x201D || cp == 0x2019 || cp == 0x300D ||
                          cp == 0x300F || cp == 0xFF09;
        if (!skip) return cp;
        end = start;
    }
    return 0;
}

}

void joinSplitParagraphs(Document* doc) {
    std::vector<Block>& blocks = doc->blocks;
    if (blocks.size() < 2) return;
    std::vector<Block> out;
    out.reserve(blocks.size());
    for (auto& b : blocks) {
        if (!out.empty() && b.kind == BlockKind::Paragraph && out.back().kind == BlockKind::Paragraph &&
            b.quoteDepth == out.back().quoteDepth && b.boxId == out.back().boxId) {
            const std::string prev = runsPlainText(out.back().runs);
            const std::string next = trimText(runsPlainText(b.runs));
            const uint32_t last = lastSentenceCodepoint(prev);
            const bool ends = last == '.' || last == '!' || last == '?' || last == ':' ||
                              last == ';' || last == 0x2026 || last == 0x3002 || last == 0xFF01 ||
                              last == 0xFF1F || last == 0xFF1A || last == 0xFF1B;
            const unsigned char first = next.empty() ? 0 : (unsigned char)next[0];
            if (last != 0 && !ends && first >= 'a' && first <= 'z') {
                std::vector<InlineRun>& runs = out.back().runs;
                while (!runs.empty() && runs.back().kind == InlineKind::LineBreak) runs.pop_back();
                const bool hyphen = !prev.empty() && prev.back() == '-' &&
                                    (prev.size() < 2 || prev[prev.size() - 2] != '-');
                if (!hyphen) runs.push_back(InlineRun{InlineKind::Text, " ", 0});
                for (auto& r : b.runs) runs.push_back(std::move(r));
                for (auto& id : b.anchorIds) out.back().anchorIds.push_back(std::move(id));
                continue;
            }
        }
        out.push_back(std::move(b));
    }
    blocks = std::move(out);
}

std::string runsPlainText(const std::vector<InlineRun>& runs, size_t first) {
    std::string s;
    for (size_t i = first; i < runs.size(); ++i) {
        const InlineRun& r = runs[i];
        if (r.kind == InlineKind::LineBreak) s.push_back('\n');
        else if (r.kind != InlineKind::Ruby) s += r.text;
    }
    return s;
}

const char* imageFileExtension(const std::vector<uint8_t>& b) {
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8) return "jpg";
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return "png";
    if (b.size() >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') return "gif";
    if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 &&
        std::memcmp(b.data() + 8, "WEBP", 4) == 0) return "webp";
    if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M') return "bmp";
    return "bin";
}

bool writeSourceExport(const Document& doc, const std::string& format,
                       const std::filesystem::path& outDir, Logger* log) {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (!std::filesystem::is_directory(outDir)) {
        if (log) log->log(LogLevel::Error, "extract", "cannot create output directory");
        return false;
    }

    int pages = 0;
    for (const auto& b : doc.blocks) pages = std::max(pages, b.sourceSpineIndex + 1);
    const std::vector<Section> sections = buildSections(doc, pages);
    std::vector<int> sectionOf(doc.blocks.size(), 0);
    for (size_t k = 0; k < sections.size(); ++k)
        for (int bi = sections[k].firstBlock; bi <= sections[k].lastBlock; ++bi)
            sectionOf[bi] = (int)k;

    std::set<int> usedImages;
    json blocks = json::array();
    size_t totalChars = 0;
    for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
        const Block& b = doc.blocks[bi];
        json o;
        o["i"] = (int)bi;
        o["kind"] = kindName(b);
        int chars = 0;
        if (b.kind == BlockKind::Image) {
            if (b.imageIndex >= 0 && b.imageIndex < (int)doc.images.size() &&
                !doc.images[b.imageIndex].rawBytes.empty()) {
                const auto& img = doc.images[b.imageIndex];
                o["image"] = "img_" + std::to_string(b.imageIndex) + "." + imageFileExtension(img.rawBytes);
                if (!img.altText.empty()) o["alt"] = img.altText;
                usedImages.insert(b.imageIndex);
            }
            o["fullPage"] = b.imageFullPage;
            o["inline"] = b.imageInline;
        } else if (b.kind == BlockKind::Table) {
            json rows = json::array(), spans = json::array();
            for (const auto& row : b.rows) {
                json r = json::array(), s = json::array();
                for (const auto& cell : row.cells) {
                    const std::string t = trimText(runsPlainText(cell.runs));
                    chars += (int)codepointCount(t);
                    r.push_back(t);
                    s.push_back(json::array({cell.colspan, cell.rowspan, cell.isHeader ? 1 : 0}));
                }
                rows.push_back(std::move(r));
                spans.push_back(std::move(s));
            }
            o["rows"] = std::move(rows);
            o["spans"] = std::move(spans);
        } else if (b.kind != BlockKind::ThematicBreak && b.kind != BlockKind::PageBreak) {
            const std::string text = b.kind == BlockKind::CodeBlock || b.kind == BlockKind::MathBlock
                                         ? runsPlainText(b.runs) : trimText(blockText(b));
            chars = (int)codepointCount(text);
            o["text"] = text;
            if (hasListMarker(b)) o["marker"] = b.runs[0].text;
        }
        if (b.level != 0) o["level"] = b.level;
        if (b.listDepth != 0) o["listDepth"] = b.listDepth;
        if (b.quoteDepth != 0) o["quoteDepth"] = b.quoteDepth;
        if (b.sourceSpineIndex >= 0) o["sourcePage"] = b.sourceSpineIndex;
        o["section"] = sectionOf[bi];
        o["chars"] = chars;
        totalChars += (size_t)chars;
        blocks.push_back(std::move(o));
    }

    json secs = json::array();
    for (const auto& s : sections) {
        json o;
        o["title"] = s.title;
        o["level"] = s.level;
        o["firstBlock"] = s.firstBlock;
        o["lastBlock"] = s.lastBlock;
        int firstPage = -1, lastPage = -1;
        size_t chars = 0;
        std::string preview;
        for (int bi = s.firstBlock; bi <= s.lastBlock; ++bi) {
            const Block& b = doc.blocks[bi];
            if (b.sourceSpineIndex >= 0) {
                if (firstPage < 0) firstPage = b.sourceSpineIndex;
                lastPage = std::max(lastPage, b.sourceSpineIndex);
            }
            chars += (size_t)blocks[bi]["chars"].get<int>();
            if (preview.empty() && b.kind != BlockKind::Image && b.kind != BlockKind::Table &&
                b.kind != BlockKind::CodeBlock)
                preview = prefixCodepoints(trimText(blockText(b)), 40);
        }
        o["firstPage"] = firstPage;
        o["lastPage"] = lastPage;
        o["chars"] = chars;
        o["preview"] = preview;
        secs.push_back(std::move(o));
    }

    for (int idx : usedImages) {
        const auto& bytes = doc.images[idx].rawBytes;
        const std::filesystem::path p =
            outDir / ("img_" + std::to_string(idx) + "." + imageFileExtension(bytes));
        if (!writeFileAtomic(p, bytes.data(), bytes.size())) {
            if (log) log->log(LogLevel::Error, "extract", "image write failed");
            return false;
        }
    }

    json root;
    root["version"] = 1;
    root["title"] = doc.title;
    root["author"] = doc.author;
    root["format"] = format;
    root["pages"] = pages;
    root["chars"] = totalChars;
    root["blocks"] = std::move(blocks);
    root["sections"] = std::move(secs);
    const std::string text = root.dump(-1, ' ', false, json::error_handler_t::replace);
    if (!writeFileAtomic(outDir / "source.json", text.data(), text.size())) {
        if (log) log->log(LogLevel::Error, "extract", "source.json write failed");
        return false;
    }
    if (log) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "extract: %zu blocks, %zu sections, %zu images, %zu chars, %d pages",
                      doc.blocks.size(), sections.size(), usedImages.size(), totalChars, pages);
        log->log(LogLevel::Info, "extract", msg);
    }
    return true;
}

}
