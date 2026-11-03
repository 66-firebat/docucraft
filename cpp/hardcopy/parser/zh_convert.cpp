#include "zh_convert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../util/text_join.h"

namespace hardcopy {
namespace {

#include "zh_convert_table.inc"
#include "zh_phrase_table.inc"

constexpr char32_t kRawByte = 0x80000000u;

uint32_t lookup(const ZhPair* table, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (table[mid].from < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo < n && table[lo].from == cp) return table[lo].to;
    return cp;
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

void decodeUnits(const std::string& s, std::u32string* out) {
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = textjoin::decodeAt(s, i);
        if (cp == 0) {
            out->push_back(kRawByte | (uint8_t)s[i]);
            ++i;
            continue;
        }
        i += utf8Width(cp);
        out->push_back(cp);
    }
}

void encodeUnits(const char32_t* p, size_t n, std::string* out) {
    out->clear();
    out->reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        if (p[i] & kRawByte) out->push_back((char)(p[i] & 0xFF));
        else appendUtf8(out, p[i]);
    }
}

struct PhraseDict {
    std::u32string pool;
    std::unordered_map<std::u32string_view, std::u32string_view> map;
    std::unordered_map<char32_t, uint8_t> firstMax;

    PhraseDict(const ZhPhrase* phrases, size_t n) {
        std::u32string tmp;
        std::vector<std::pair<size_t, size_t>> spans;
        for (size_t i = 0; i < n; ++i) {
            for (const char* s : {phrases[i].from, phrases[i].to}) {
                tmp.clear();
                decodeUnits(s, &tmp);
                spans.push_back({pool.size(), tmp.size()});
                pool += tmp;
            }
        }
        map.reserve(n);
        for (size_t i = 0; i + 1 < spans.size(); i += 2) {
            const std::u32string_view from(pool.data() + spans[i].first, spans[i].second);
            const std::u32string_view to(pool.data() + spans[i + 1].first, spans[i + 1].second);
            if (from.size() < 2 || from.size() != to.size()) continue;
            map.emplace(from, to);
            uint8_t& m = firstMax[from[0]];
            m = (uint8_t)std::max<size_t>(m, std::min<size_t>(from.size(), 255));
        }
    }
};

const PhraseDict& phrases(ZhConvert mode) {
    static const PhraseDict s2t(kS2TPhrases, kS2TPhrasesCount);
    static const PhraseDict t2s(kT2SPhrases, kT2SPhrasesCount);
    return mode == ZhConvert::S2T ? s2t : t2s;
}

void convertUnits(std::u32string* units, ZhConvert mode) {
    const ZhPair* table = (mode == ZhConvert::S2T) ? kS2T : kT2S;
    const size_t n = (mode == ZhConvert::S2T) ? kS2TCount : kT2SCount;
    const PhraseDict& dict = phrases(mode);
    std::u32string& u = *units;
    for (size_t i = 0; i < u.size();) {
        if (u[i] & kRawByte) {
            ++i;
            continue;
        }
        const auto first = dict.firstMax.find(u[i]);
        if (first != dict.firstMax.end()) {
            size_t len = std::min<size_t>(first->second, u.size() - i);
            bool matched = false;
            for (; len >= 2; --len) {
                const auto hit = dict.map.find(std::u32string_view(u.data() + i, len));
                if (hit == dict.map.end()) continue;
                std::copy(hit->second.begin(), hit->second.end(), u.begin() + i);
                i += len;
                matched = true;
                break;
            }
            if (matched) continue;
        }
        u[i] = lookup(table, n, u[i]);
        ++i;
    }
}

void convertRuns(std::vector<InlineRun>* runs, ZhConvert mode) {
    std::u32string units;
    std::vector<size_t> ends;
    for (const auto& r : *runs) {
        decodeUnits(r.text, &units);
        ends.push_back(units.size());
    }
    if (units.empty()) return;
    convertUnits(&units, mode);
    size_t begin = 0;
    for (size_t i = 0; i < runs->size(); ++i) {
        if (ends[i] > begin) encodeUnits(units.data() + begin, ends[i] - begin, &(*runs)[i].text);
        begin = ends[i];
    }
}

}

std::string convertZhUtf8(const std::string& utf8, ZhConvert mode) {
    if (mode == ZhConvert::None || utf8.empty()) return utf8;
    std::u32string units;
    units.reserve(utf8.size());
    decodeUnits(utf8, &units);
    convertUnits(&units, mode);
    std::string out;
    encodeUnits(units.data(), units.size(), &out);
    return out;
}

void convertDocumentZh(Document* doc, ZhConvert mode) {
    if (!doc || mode == ZhConvert::None) return;
    auto conv = [mode](std::string* s) {
        if (s && !s->empty()) *s = convertZhUtf8(*s, mode);
    };
    conv(&doc->title);
    conv(&doc->author);
    for (auto& b : doc->blocks) {
        convertRuns(&b.runs, mode);
        for (auto& row : b.rows) {
            for (auto& cell : row.cells) convertRuns(&cell.runs, mode);
        }
    }
    for (auto& img : doc->images) conv(&img.altText);
    for (auto& h : doc->importedOutline) conv(&h.title);
    for (auto& e : doc->epubToc) conv(&e.title);
}

}
