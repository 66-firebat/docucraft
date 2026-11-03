#include "text_encoding.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

#if HARDCOPY_HAS_PDFIN
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#endif

namespace hardcopy {

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string decodeUtf16(const std::string& s, size_t start, bool bigEndian) {
    std::string out;
    out.reserve(s.size() * 3 / 2);
    auto unit = [&](size_t i) -> uint32_t {
        const uint8_t a = (uint8_t)s[i], b = (uint8_t)s[i + 1];
        return bigEndian ? (uint32_t)(a << 8 | b) : (uint32_t)(b << 8 | a);
    };
    for (size_t i = start; i + 1 < s.size(); i += 2) {
        uint32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
            const uint32_t lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
        appendUtf8(out, u);
    }
    return out;
}

size_t countInvalidUtf8(const std::string& s, size_t* multibyte) {
    size_t bad = 0, mb = 0;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { ++i; continue; }
        int len = 0;
        uint32_t minCp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; minCp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; minCp = 0x800; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) { len = 4; minCp = 0x10000; }
        else { ++bad; ++i; continue; }
        if (i + len > n) break;
        uint32_t cp = c & (0x7F >> len);
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            ++bad;
            ++i;
            continue;
        }
        ++mb;
        i += len;
    }
    if (multibyte) *multibyte = mb;
    return bad;
}

int guessBomlessUtf16(const std::string& s) {
    const size_t n = std::min<size_t>(s.size() & ~(size_t)1, 8192);
    if (n < 16) return 0;
    size_t zeroEven = 0, zeroOdd = 0;
    for (size_t i = 0; i < n; i += 2) {
        if (s[i] == 0) ++zeroEven;
        if (s[i + 1] == 0) ++zeroOdd;
    }
    const size_t units = n / 2;
    if (zeroOdd * 10 >= units * 3 && zeroEven * 20 < units) return 1;
    if (zeroEven * 10 >= units * 3 && zeroOdd * 20 < units) return 2;
    return 0;
}

#if HARDCOPY_HAS_PDFIN
bool decodeWithMupdf(const std::string& s, const char* toCid, const char* toUni,
                     std::string* out, size_t* replacements) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) return false;
    bool ok = false;
    std::string buf;
    size_t rep = 0;
    fz_try(ctx) {
        pdf_cmap* cid = pdf_load_system_cmap(ctx, toCid);
        pdf_cmap* uni = pdf_load_system_cmap(ctx, toUni);
        unsigned char* p = (unsigned char*)s.data();
        unsigned char* e = p + s.size();
        buf.reserve(s.size() * 3 / 2);
        while (p < e) {
            unsigned int raw = 0;
            const int n = pdf_decode_cmap(cid, p, e, &raw);
            p += n > 0 ? n : 1;
            int u = pdf_lookup_cmap(uni, (unsigned)pdf_lookup_cmap(cid, raw));
            if (u < 0) {
                if (raw < 0x80) u = (int)raw;
                else { u = 0xFFFD; ++rep; }
            }
            appendUtf8(buf, (uint32_t)u);
        }
        ok = true;
    }
    fz_catch(ctx) {
        ok = false;
    }
    fz_drop_context(ctx);
    if (ok) {
        *out = std::move(buf);
        *replacements = rep;
    }
    return ok;
}
#endif

#if HARDCOPY_HAS_PDFIN
const char kCommonHan[] =
    "的一是不了在人有我他这個个们們中来來上大为為和国國地到以说說时時要就出会會可也你"
    "对對生能而子那得于於着著下自之年过過发發后後作里裡用道行所然家种種事成方多经經么麼"
    "去法学學如都同现現当當没沒动動面起看定天分还還进進好小部其些主样樣理心她本前开開但因"
    "只从從想实實日者意无無力它与與长長把机機十民第公此已工使情明性知全三又关關点點正业業外"
    "将將两兩高间間由问問很最重并物手应應向头頭文体體见見被什二等产產或新己制身果加西月话話"
    "合回特代内內信表化老给給世位次度门門任常先海通教儿兒原东東声聲提立及比员員解水名真论論"
    "处處走义義各入几幾口认認条條平系气氣题題活更别別打女变變四神总總何电電数數安少报報才结結"
    "反受目太量再感建务務做接必场場件计計管期市直德资資命山金指许許统統区區保至队隊形社便空决"
    "決治展马馬科司五基眼书書非则則听聽白却界达達光放强強即像难難且权權思王象完设設式色路记記"
    "南品住告类類求据據程北边邊死张張该該交规規万萬取拉格望觉覺术術领領共确確传傳师師观觀清今"
    "切院让讓识識候带帶导導争爭运運笑飞飛风風步改收根干造言联聯持组組每济濟车車亲親极極林服快"
    "办辦议議往元英士证證近失转轉夫令准布始怎呢存未远遠叫台单單影具罗羅字爱愛击擊流备備兵连連"
    "调調深商算质質团團集百需价價花党黨华華城石级級整府离離况況请請技际際约約示复復病息究线線"
    "似官火断斷精满滿支视視消越器容照须須九增研写寫称稱企八功吗嗎包片史委乎查轻輕易早曾除农農"
    "找装裝广廣显顯吧阿李标標谈談吃图圖念六引历歷首医醫局突专專费費号號尼"
    "，。、；：？！「」『』“”（）《》…—";

bool isHan(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)
        || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF)
        || (cp >= 0x2010 && cp <= 0x2027);
}

uint32_t nextCp(const std::string& s, size_t* i) {
    const uint8_t c = (uint8_t)s[*i];
    int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
            : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (*i + len > s.size()) len = 1;
    uint32_t cp = len == 1 ? c : (uint32_t)(c & (0x7F >> len));
    for (int k = 1; k < len; ++k) cp = (cp << 6) | ((uint8_t)s[*i + k] & 0x3F);
    *i += len;
    return cp;
}

int commonHanScore(const std::string& decoded) {
    static const std::string table(kCommonHan);
    static std::vector<uint32_t> common;
    if (common.empty()) {
        for (size_t i = 0; i < table.size();) common.push_back(nextCp(table, &i));
        std::sort(common.begin(), common.end());
    }
    size_t total = 0, hits = 0;
    const size_t limit = std::min<size_t>(decoded.size(), 256 * 1024);
    for (size_t i = 0; i < limit;) {
        const uint32_t cp = nextCp(decoded, &i);
        if (cp < 0x80) continue;
        ++total;
        if (isHan(cp) && std::binary_search(common.begin(), common.end(), cp)) ++hits;
    }
    if (total == 0) return 0;
    return (int)(hits * 1000 / total);
}

#endif

void setName(std::string* name, const char* v) {
    if (name) *name = v;
}

}

std::string decodeTextToUtf8(const std::string& bytes, std::string* encodingName) {
    const size_t n = bytes.size();
    const uint8_t* b = (const uint8_t*)bytes.data();
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        setName(encodingName, "utf-8 (bom)");
        return bytes.substr(3);
    }
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        setName(encodingName, "utf-16le (bom)");
        return decodeUtf16(bytes, 2, false);
    }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        setName(encodingName, "utf-16be (bom)");
        return decodeUtf16(bytes, 2, true);
    }
    if (const int u16 = guessBomlessUtf16(bytes)) {
        setName(encodingName, u16 == 1 ? "utf-16le" : "utf-16be");
        return decodeUtf16(bytes, 0, u16 == 2);
    }

    size_t multibyte = 0;
    const size_t bad = countInvalidUtf8(bytes, &multibyte);
    if (bad == 0 || bad * 200 < multibyte) {
        setName(encodingName, bad == 0 ? "utf-8" : "utf-8 (with invalid bytes)");
        return bytes;
    }

#if HARDCOPY_HAS_PDFIN
    std::string gb, big5;
    size_t gbRep = 0, big5Rep = 0;
    const bool haveGb = decodeWithMupdf(bytes, "GBK2K-H", "Adobe-GB1-UCS2", &gb, &gbRep);
    const bool haveBig5 = decodeWithMupdf(bytes, "ETen-B5-H", "Adobe-CNS1-UCS2", &big5, &big5Rep);
    if (haveGb || haveBig5) {
        const int gbScore = haveGb ? commonHanScore(gb) : -1;
        const int big5Score = haveBig5 ? commonHanScore(big5) : -1;
        if (big5Score > gbScore) {
            setName(encodingName, "big5");
            return big5;
        }
        setName(encodingName, "gb18030");
        return gb;
    }
#endif
    setName(encodingName, "unknown (passed through)");
    return bytes;
}

}
