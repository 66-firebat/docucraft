#include "bilingual_parser.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "../image/image_decoder.h"

namespace hardcopy {

namespace {

using nlohmann::json;

std::string str(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int num(const json& o, const char* key, int def = 0) {
    auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<int>() : def;
}

bool flag(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_boolean() && it->get<bool>();
}

bool blank(const std::string& s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    return true;
}

void appendTextRuns(const std::string& text, InlineKind kind, std::vector<InlineRun>* runs) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) runs->push_back(InlineRun{kind, std::move(line), (int32_t)start});
        if (end == std::string::npos) break;
        runs->push_back(InlineRun{InlineKind::LineBreak, {}, (int32_t)end});
        start = end + 1;
    }
    while (!runs->empty() && runs->back().kind == InlineKind::LineBreak) runs->pop_back();
}

void codeRuns(const std::string& text, std::vector<InlineRun>* runs) {
    std::string code = text;
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
    size_t start = 0;
    while (start <= code.size()) {
        size_t end = code.find('\n', start);
        std::string line = code.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) line = " ";
        runs->push_back(InlineRun{InlineKind::Code, std::move(line), (int32_t)start});
        if (end == std::string::npos) break;
        runs->push_back(InlineRun{InlineKind::LineBreak, {}, (int32_t)end});
        start = end + 1;
    }
}

std::vector<TableRow> tableRows(const json& rows, const json* spans) {
    std::vector<TableRow> out;
    if (!rows.is_array()) return out;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (!rows[r].is_array()) continue;
        TableRow row;
        for (size_t c = 0; c < rows[r].size(); ++c) {
            TableCell cell;
            const json& v = rows[r][c];
            if (v.is_string()) appendTextRuns(v.get<std::string>(), InlineKind::Text, &cell.runs);
            if (spans && spans->is_array() && r < spans->size() && (*spans)[r].is_array() &&
                c < (*spans)[r].size()) {
                const json& s = (*spans)[r][c];
                if (s.is_array() && s.size() >= 3 && s[0].is_number() && s[1].is_number() &&
                    s[2].is_number()) {
                    cell.colspan = std::max(1, s[0].get<int>());
                    cell.rowspan = std::max(1, s[1].get<int>());
                    cell.isHeader = s[2].get<int>() != 0;
                }
            }
            row.cells.push_back(std::move(cell));
        }
        out.push_back(std::move(row));
    }
    return out;
}

bool sameShape(const json& a, const json& b) {
    if (!a.is_array() || !b.is_array() || a.size() != b.size()) return false;
    for (size_t r = 0; r < a.size(); ++r)
        if (!a[r].is_array() || !b[r].is_array() || a[r].size() != b[r].size()) return false;
    return true;
}

bool readFile(const std::filesystem::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}

}

bool parseBilingualText(const std::string& text, const std::filesystem::path& baseDir,
                        Document* out, Logger* log) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        if (log) {
            const std::string m = std::string("bilingual json: ") + e.what();
            log->log(LogLevel::Error, "bilingual", m.c_str());
        }
        return false;
    }
    if (!root.is_object() || !root.contains("blocks") || !root["blocks"].is_array()) {
        if (log) log->log(LogLevel::Error, "bilingual", "bilingual json: no blocks array");
        return false;
    }
    out->title = str(root, "title");
    out->author = str(root, "author");

    const json& blocks = root["blocks"];
    uint32_t nextPair = 1;
    int pairs = 0, untranslated = 0, missingImages = 0, tablesDst = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const json& o = blocks[i];
        if (!o.is_object()) continue;
        const std::string kind = str(o, "kind");
        const int index = (int)i;
        Block base;
        base.sourceSpineIndex = index;
        base.listDepth = std::max(0, num(o, "listDepth"));
        base.quoteDepth = std::max(0, num(o, "quoteDepth"));

        if (kind == "image") {
            const std::string rel = str(o, "image");
            ImageData img;
            if (rel.empty() || !readFile(baseDir / std::filesystem::u8path(rel), &img.rawBytes) ||
                !imageInfo(img.rawBytes.data(), img.rawBytes.size(), &img.width, &img.height)) {
                ++missingImages;
                continue;
            }
            img.altText = str(o, "alt");
            base.kind = BlockKind::Image;
            base.imageIndex = (int)out->images.size();
            base.imageFullPage = flag(o, "fullPage");
            base.imageInline = flag(o, "inline");
            out->images.push_back(std::move(img));
            out->blocks.push_back(std::move(base));
            continue;
        }
        if (kind == "hr" || kind == "pagebreak") {
            base.kind = kind == "hr" ? BlockKind::ThematicBreak : BlockKind::PageBreak;
            out->blocks.push_back(std::move(base));
            continue;
        }
        if (kind == "table") {
            const json* spans = o.contains("spans") ? &o["spans"] : nullptr;
            base.kind = BlockKind::Table;
            if (o.contains("rows")) base.rows = tableRows(o["rows"], spans);
            if (base.rows.empty()) continue;
            if (o.contains("rowsDst") && o["rowsDst"].is_array() && !o["rowsDst"].empty()) {
                const bool shaped = o.contains("rows") && sameShape(o["rows"], o["rowsDst"]);
                base.rowsDst = tableRows(o["rowsDst"], shaped ? spans : nullptr);
                tablesDst += !base.rowsDst.empty();
            }
            out->blocks.push_back(std::move(base));
            continue;
        }

        std::string src = o.contains("src") ? str(o, "src") : str(o, "text");
        if (kind == "code" || kind == "math") {
            if (blank(src)) continue;
            if (kind == "code") {
                base.kind = BlockKind::CodeBlock;
                codeRuns(src, &base.runs);
            } else {
                base.kind = BlockKind::MathBlock;
                appendTextRuns(src, InlineKind::Text, &base.runs);
            }
            out->blocks.push_back(std::move(base));
            continue;
        }

        std::string dst = str(o, "dst");
        if (blank(src) && blank(dst)) continue;
        if (blank(dst)) {
            dst = src;
            ++untranslated;
        } else if (blank(src)) {
            src = dst;
        }
        std::string marker;
        if (kind.size() == 2 && kind[0] == 'h' && kind[1] >= '1' && kind[1] <= '6') {
            base.kind = BlockKind::Heading;
            base.level = kind[1] - '0';
        } else if (kind == "li" || kind == "oli") {
            base.kind = kind == "li" ? BlockKind::UnorderedItem : BlockKind::OrderedItem;
            base.level = num(o, "level");
            marker = str(o, "marker");
            if (marker.empty() || marker.size() > 8 || marker.back() != ' ')
                marker = kind == "li" ? "\xE2\x80\xA2 " : std::to_string(std::max(1, base.level)) + ". ";
        } else if (kind == "quote") {
            base.kind = BlockKind::Blockquote;
            base.quoteDepth = std::max(1, base.quoteDepth);
        } else {
            base.kind = BlockKind::Paragraph;
        }
        base.pairId = nextPair++;
        Block left = base, right = base;
        left.pairSide = 0;
        right.pairSide = 1;
        if (!marker.empty()) {
            left.runs.push_back(InlineRun{InlineKind::Text, marker, 0});
            right.runs.push_back(InlineRun{InlineKind::Text, marker, 0});
        }
        appendTextRuns(src, InlineKind::Text, &left.runs);
        appendTextRuns(dst, InlineKind::Text, &right.runs);
        out->blocks.push_back(std::move(left));
        out->blocks.push_back(std::move(right));
        ++pairs;
    }

    if (root.contains("outline") && root["outline"].is_array()) {
        for (const json& e : root["outline"]) {
            if (!e.is_object()) continue;
            const std::string title = str(e, "title");
            const int block = num(e, "block", -1);
            if (title.empty() || block < 0) continue;
            HeadingAnchor a;
            a.title = title;
            a.level = std::max(1, num(e, "level", 1));
            a.pageIndex0 = block;
            a.yOnPage = 0.0f;
            out->importedOutline.push_back(std::move(a));
        }
    }

    if (log) {
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "bilingual: %d pairs, %zu blocks, %zu images, %d tables translated, "
                      "%d outline entries",
                      pairs, out->blocks.size(), out->images.size(), tablesDst,
                      (int)out->importedOutline.size());
        log->log(LogLevel::Info, "bilingual", msg);
        if (untranslated > 0) {
            std::snprintf(msg, sizeof(msg), "bilingual: %d blocks without translation keep the source text",
                          untranslated);
            log->log(LogLevel::Warn, "bilingual", msg);
        }
        if (missingImages > 0) {
            std::snprintf(msg, sizeof(msg), "bilingual: %d images missing or unreadable", missingImages);
            log->log(LogLevel::Warn, "bilingual", msg);
        }
    }
    return !out->blocks.empty();
}

bool parseBilingual(const std::filesystem::path& path, Document* out, Logger* log) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, &bytes)) {
        if (log) log->log(LogLevel::Error, "bilingual", "cannot read bilingual json");
        return false;
    }
    return parseBilingualText(std::string(bytes.begin(), bytes.end()), path.parent_path(), out, log);
}

}
