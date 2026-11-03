#include "txt_parser.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

#include "../util/text_join.h"
#include "text_encoding.h"

namespace hardcopy {

namespace {

const char kIdeographicSpace[] = "\xE3\x80\x80";

size_t skipWs(const std::string& s, size_t i) {
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (s.compare(i, 3, kIdeographicSpace) == 0) { i += 3; continue; }
        break;
    }
    return i;
}

bool isBlank(const std::string& s) {
    return skipWs(s, 0) >= s.size();
}

void stripBom(std::string& s) {
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) {
        s.erase(0, 3);
    }
}

std::string stripTrailing(std::string s) {
    while (!s.empty()) {
        if (s.back() == '\r' || s.back() == ' ' || s.back() == '\t') {
            s.pop_back();
        } else if (s.size() >= 3 &&
                   s.compare(s.size() - 3, 3, kIdeographicSpace) == 0) {
            s.resize(s.size() - 3);
        } else {
            break;
        }
    }
    return s;
}

std::string trimLine(const std::string& s) {
    size_t b = skipWs(s, 0);
    if (b >= s.size()) return {};
    return stripTrailing(s.substr(b));
}

bool hasParagraphIndent(const std::string& line) {
    return skipWs(line, 0) > 0 && !isBlank(line);
}

void appendParagraph(Document* doc, std::string text) {
    text = stripTrailing(std::move(text));
    if (text.empty()) return;
    Block b;
    b.kind = BlockKind::Paragraph;
    b.runs.push_back(InlineRun{InlineKind::Text, std::move(text), 0});
    doc->blocks.push_back(std::move(b));
}

size_t utf8Length(const std::string& s) {
    size_t count = 0;
    for (size_t i = 0; i < s.size(); ++count) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) ++i;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else ++i;
    }
    return count;
}

bool hasSemanticLineEnding(const std::string& raw) {
    std::string s = stripTrailing(raw);
    if (s.empty()) return false;
    uint32_t cp = textjoin::lastCodepoint(s);
    switch (cp) {
        case '.': case '!': case '?': case ':': case ';':
        case '"': case '\'': case ')':
        case 0x3002:
        case 0xFF01:
        case 0xFF1F:
        case 0xFF1A:
        case 0xFF1B:
        case 0x2026:
        case 0x2014:
        case 0x201D:
        case 0x2019:
        case 0x300D:
        case 0x300F:
        case 0xFF09:
        case 0x300B:
        case 0xFF5E:
        case '~':
            return true;
        default:
            return false;
    }
}

bool endsMidSentence(const std::string& trimmed) {
    switch (textjoin::lastCodepoint(trimmed)) {
        case ',':
        case 0xFF0C:
        case 0x3001:
            return true;
        default:
            return false;
    }
}

size_t cpBytes(uint32_t cp) {
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

bool isSpaceCp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x3000 || cp == 0x00A0;
}

bool isChapterNumeral(uint32_t cp) {
    if (cp >= '0' && cp <= '9') return true;
    if (cp >= 0xFF10 && cp <= 0xFF19) return true;
    static const uint32_t kNums[] = {
        0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94,
        0x516D, 0x4E03, 0x516B, 0x4E5D, 0x5341,
        0x767E, 0x5343, 0x4E07, 0x96F6, 0x3007,
        0x5169, 0x4E24,
        0x58F9, 0x8D30, 0x53C1, 0x8086, 0x4F0D,
        0x9646, 0x67D2, 0x634C, 0x7396, 0x62FE,
        0x4F70, 0x4EDF, 0x8CB3, 0x9678,
    };
    for (uint32_t n : kNums) if (cp == n) return true;
    return false;
}

bool isTitleSeparator(uint32_t cp) {
    switch (cp) {
        case ':': case '.': case '-': case '_':
        case 0xFF1A:
        case 0x3001:
        case 0xFF0E:
        case 0x00B7:
        case 0x2014:
        case 0x2013:
        case 0x300A:
        case 0x3010:
        case 0xFF08:
        case '(':
            return true;
        default:
            return isSpaceCp(cp);
    }
}

bool startsWithAscii(const std::string& t, const char* word) {
    const size_t n = std::strlen(word);
    if (t.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower((unsigned char)t[i]) != word[i]) return false;
    }
    return t.size() == n || !std::isalpha((unsigned char)t[n]);
}

int chapterHeadingLevel(const std::string& t) {
    const size_t len = utf8Length(t);
    if (len == 0 || len > 40) return 0;
    switch (textjoin::lastCodepoint(t)) {
        case 0x3002: case 0xFF0C: case 0x3001: case 0xFF1B: case ',': case ';':
            return 0;
        default:
            break;
    }

    if (startsWithAscii(t, "chapter")) return 2;
    if (startsWithAscii(t, "part") || startsWithAscii(t, "volume")
        || startsWithAscii(t, "book")) {
        return t.find(' ') != std::string::npos && len <= 30 ? 1 : 0;
    }

    static const char* kStandalone[] = {
        "\xE5\xBA\x8F\xE7\xAB\xA0",
        "\xE6\xA5\x94\xE5\xAD\x90",
        "\xE5\xBC\x95\xE5\xAD\x90",
        "\xE5\xBA\x8F\xE8\xA8\x80",
        "\xE5\x89\x8D\xE8\xA8\x80",
        "\xE7\x95\xAA\xE5\xA4\x96",
        "\xE5\xB0\xBE\xE5\xA3\xB0",
        "\xE5\xB0\xBE\xE8\x81\xB2",
        "\xE5\x90\x8E\xE8\xAE\xB0",
        "\xE5\xBE\x8C\xE8\xA8\x98",
        "\xE5\xBA\x8F",
    };
    for (const char* w : kStandalone) {
        const size_t n = std::strlen(w);
        if (t.compare(0, n, w) != 0) continue;
        if (t.size() == n) return 2;
        if (n == 3) continue;
        if (isTitleSeparator(textjoin::decodeAt(t, n)) && len <= 30) return 2;
    }

    const uint32_t kDi = 0x7B2C;
    if (textjoin::firstCodepoint(t) != kDi) return 0;
    size_t i = 3;
    while (i < t.size() && isSpaceCp(textjoin::decodeAt(t, i)))
        i += cpBytes(textjoin::decodeAt(t, i));
    int numCount = 0;
    while (i < t.size()) {
        uint32_t cp = textjoin::decodeAt(t, i);
        if (cp == 0 || !isChapterNumeral(cp)) break;
        i += cpBytes(cp);
        if (++numCount > 12) return 0;
    }
    if (numCount == 0) return 0;
    while (i < t.size() && isSpaceCp(textjoin::decodeAt(t, i)))
        i += cpBytes(textjoin::decodeAt(t, i));
    const uint32_t unit = textjoin::decodeAt(t, i);
    int level = 0;
    bool looseUnit = false;
    switch (unit) {
        case 0x5377:
            level = 1; looseUnit = true; break;
        case 0x90E8:
        case 0x7BC7:
            level = 1; break;
        case 0x7AE0:
            level = 2; looseUnit = true; break;
        case 0x8282:
        case 0x7BC0:
        case 0x56DE:
        case 0x8A71:
        case 0x8BDD:
        case 0x5E55:
        case 0x96C6:
            level = 2; break;
        default:
            return 0;
    }
    i += cpBytes(unit);
    if (i >= t.size()) return level;
    if (isTitleSeparator(textjoin::decodeAt(t, i))) return level;
    return (looseUnit && len <= 20) ? level : 0;
}

void appendHeading(Document* doc, std::string text, int level) {
    text = stripTrailing(std::move(text));
    if (text.empty()) return;
    Block b;
    b.kind = BlockKind::Heading;
    b.level = level;
    b.runs.push_back(InlineRun{InlineKind::Text, std::move(text), 0});
    doc->blocks.push_back(std::move(b));
}

void parseProse(std::stringstream& ss, Document* out) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    int nonBlank = 0;
    int semanticEndings = 0;
    int longLines = 0;
    int indentedLines = 0;
    std::vector<int> lengthHist(256, 0);
    for (const auto& raw : lines) {
        if (isBlank(raw)) continue;
        ++nonBlank;
        if (hasSemanticLineEnding(raw)) ++semanticEndings;
        if (hasParagraphIndent(raw)) ++indentedLines;
        const size_t len = utf8Length(trimLine(raw));
        if (len >= 100) ++longLines;
        if (len < lengthHist.size()) ++lengthHist[len];
    }
    int wrapWidth = 0;
    {
        int best = 0;
        for (int w = 16; w + 1 < (int)lengthHist.size(); ++w) {
            const int c = lengthHist[w - 1] + lengthHist[w] + lengthHist[w + 1];
            if (c > best) { best = c; wrapWidth = w; }
        }
        if (nonBlank < 6 || best * 4 < nonBlank) wrapWidth = 0;
    }
    const bool indentMarksParagraphs = indentedLines * 4 >= nonBlank
        && indentedLines < nonBlank;
    const bool physicalLinesAreParagraphs = wrapWidth == 0 && nonBlank >= 3
        && (semanticEndings * 2 >= nonBlank || longLines * 3 >= nonBlank);

    std::string buf;
    std::string prevTrimmed;
    for (const auto& raw : lines) {
        if (isBlank(raw)) {
            appendParagraph(out, std::move(buf));
            buf.clear();
            prevTrimmed.clear();
            continue;
        }
        const bool indented = hasParagraphIndent(raw);
        std::string text = trimLine(raw);
        if (text.empty()) continue;

        if (int lvl = chapterHeadingLevel(text)) {
            appendParagraph(out, std::move(buf));
            buf.clear();
            prevTrimmed.clear();
            appendHeading(out, std::move(text), lvl);
            continue;
        }

        bool join = false;
        if (!buf.empty() && !indented) {
            if (endsMidSentence(prevTrimmed) || indentMarksParagraphs) {
                join = true;
            } else if (wrapWidth > 0) {
                join = (int)utf8Length(prevTrimmed) >= wrapWidth - 1
                    && !hasSemanticLineEnding(prevTrimmed);
            } else {
                join = !physicalLinesAreParagraphs;
            }
        }
        if (join) {
            textjoin::appendSoftWrap(buf, text);
        } else {
            appendParagraph(out, std::move(buf));
            buf = text;
        }
        prevTrimmed = std::move(text);
    }
    appendParagraph(out, std::move(buf));
}

bool tryBracketHeading(const std::string& trimmed, Document* out) {
    const uint8_t* p = (const uint8_t*)trimmed.data();
    size_t n = trimmed.size();
    if (n < 6) return false;
    if (p[0] != 0xE3 || p[1] != 0x80 || p[2] != 0x90) return false;
    for (size_t i = 3; i + 2 < n; ++i) {
        if (p[i] == 0xE3 && p[i+1] == 0x80 && p[i+2] == 0x91) {
            std::string title = trimmed.substr(3, i - 3);
            if (title.empty()) return false;
            Block b;
            b.kind = BlockKind::Heading;
            b.level = 2;
            b.runs.push_back(InlineRun{InlineKind::Text, std::move(title), 0});
            out->blocks.push_back(std::move(b));
            std::string rest = trimmed.substr(i + 3);
            rest = stripTrailing(std::move(rest));
            auto rb = rest.find_first_not_of(" \t\r\xE2\x80\x94");
            if (rb != std::string::npos && rb < rest.size()) {
                appendParagraph(out, rest.substr(rb));
            }
            return true;
        }
    }
    return false;
}

void parsePoetry(std::stringstream& ss, Document* out) {
    std::string line;
    bool firstNonBlank = true;
    while (std::getline(ss, line)) {
        std::string t = trimLine(line);
        if (t.empty()) continue;
        if (firstNonBlank) {
            firstNonBlank = false;
            if (t.find("http") != std::string::npos ||
                t.find("www.") != std::string::npos) {
                continue;
            }
        }
        if (tryBracketHeading(t, out)) continue;
        appendParagraph(out, std::move(t));
    }
}

}

int plainTextChapterLevel(const std::string& trimmedLine) {
    return chapterHeadingLevel(trimLine(trimmedLine));
}

bool parseTxtFromString(const std::string& srcIn, Document* out,
                        TxtMode mode) {
    if (!out) return false;
    std::string src = srcIn;
    stripBom(src);

    std::stringstream ss(src);
    switch (mode) {
        case TxtMode::Poetry: parsePoetry(ss, out); break;
        default:              parseProse(ss, out);  break;
    }
    return true;
}

bool parseTxt(const std::filesystem::path& path, Document* out,
              TxtMode mode) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    return parseTxtFromString(decodeTextToUtf8(buf.str()), out, mode);
}

}
