#include "streaming_pdf_writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "../logger.h"

namespace hardcopy {

namespace {

std::string toUtf16BeEscaped(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() * 2 + 4);
    auto putByte = [&](uint8_t b) {
        if (b == '(' || b == ')' || b == '\\') out.push_back('\\');
        out.push_back((char)b);
    };
    putByte(0xFE);
    putByte(0xFF);
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t b0 = (uint8_t)utf8[i];
        uint32_t cp = 0;
        int n = 0;
        if (b0 < 0x80)            { cp = b0;       n = 1; }
        else if ((b0 & 0xE0)==0xC0){ cp = b0&0x1F; n = 2; }
        else if ((b0 & 0xF0)==0xE0){ cp = b0&0x0F; n = 3; }
        else if ((b0 & 0xF8)==0xF0){ cp = b0&0x07; n = 4; }
        else                       { ++i; continue; }
        if (i + n > utf8.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (uint8_t(utf8[i+k]) & 0x3F);
        i += n;
        if (cp < 0x10000) {
            putByte((uint8_t)(cp >> 8));
            putByte((uint8_t)(cp & 0xFF));
        } else {
            uint32_t v = cp - 0x10000;
            uint16_t hi = (uint16_t)(0xD800 | (v >> 10));
            uint16_t lo = (uint16_t)(0xDC00 | (v & 0x3FF));
            putByte((uint8_t)(hi >> 8)); putByte((uint8_t)(hi & 0xFF));
            putByte((uint8_t)(lo >> 8)); putByte((uint8_t)(lo & 0xFF));
        }
    }
    return out;
}

std::string pdfDateNow() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[80];
    std::snprintf(buf, sizeof(buf), "D:%04d%02d%02d%02d%02d%02dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

uint32_t nextCodepoint(const std::string& utf8, size_t& pos) {
    if (pos >= utf8.size()) return 0;
    uint8_t b0 = (uint8_t)utf8[pos];
    uint32_t cp = 0;
    int n = 0;
    if (b0 < 0x80)             { cp = b0;       n = 1; }
    else if ((b0 & 0xE0)==0xC0){ cp = b0&0x1F; n = 2; }
    else if ((b0 & 0xF0)==0xE0){ cp = b0&0x0F; n = 3; }
    else if ((b0 & 0xF8)==0xF0){ cp = b0&0x07; n = 4; }
    else { ++pos; return 0xFFFD; }
    if (pos + n > utf8.size()) { pos = utf8.size(); return 0xFFFD; }
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (uint8_t(utf8[pos+k]) & 0x3F);
    pos += n;
    return cp;
}

int countCodepoints(const std::string& utf8) {
    int n = 0;
    for (size_t pos = 0; pos < utf8.size();) { nextCodepoint(utf8, pos); ++n; }
    return n;
}

const char* kHexDigits = "0123456789ABCDEF";

void appendHex4(std::string& s, uint16_t v) {
    char h[4] = { kHexDigits[(v >> 12) & 0xF], kHexDigits[(v >> 8) & 0xF],
                  kHexDigits[(v >> 4) & 0xF],  kHexDigits[v & 0xF] };
    s.append(h, 4);
}

void appendInt(std::string& s, long long v) {
    char buf[24];
    char* p = buf + sizeof(buf);
    const bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) *--p = '-';
    s.append(p, (size_t)(buf + sizeof(buf) - p));
}

void appendFixed1(std::string& s, float v) {
    const long long tenths = std::llround((double)v * 10.0);
    const bool neg = tenths < 0;
    const long long a = neg ? -tenths : tenths;
    if (neg && a != 0) s.push_back('-');
    appendInt(s, a / 10);
    if (a % 10) { s.push_back('.'); s.push_back((char)('0' + a % 10)); }
}

constexpr int kTextAscent    = 880;
constexpr int kTextDescent   = -200;
constexpr int kTextCapHeight = 700;
constexpr float kPenTolerancePx = 0.3f;
constexpr int kSpaceWidthStep = 10;

bool isSpaceLike(uint32_t cp) {
    return cp == 0x20 || cp == 0x09 || cp == 0xA0 || cp == 0x3000 || cp == 0x2002
        || cp == 0x2003 || cp == 0x2009 || cp == 0x200A || cp == 0x202F;
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x >> 16); put16(v, x & 0xFFFF);
}
uint32_t sfntChecksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; ++k)
            w = (w << 8) | (i + k < n ? p[i + k] : 0);
        sum += w;
    }
    return sum;
}

std::vector<uint8_t> buildMetricsOnlyTrueType(const std::vector<uint16_t>& widths) {
    const uint32_t numGlyphs = (uint32_t)widths.size() + 1;
    uint32_t maxAdvance = 1000;
    for (uint16_t w : widths) maxAdvance = std::max<uint32_t>(maxAdvance, w);

    struct Table { const char* tag; std::vector<uint8_t> data; };
    std::vector<Table> tables;

    tables.push_back({"glyf", std::vector<uint8_t>(4, 0)});

    std::vector<uint8_t> head;
    put32(head, 0x00010000);
    put32(head, 0x00010000);
    put32(head, 0);
    put32(head, 0x5F0F3CF5);
    put16(head, 0x000B);
    put16(head, 1000);
    for (int i = 0; i < 4; ++i) put32(head, 0);
    put16(head, 0);
    put16(head, (uint16_t)(int16_t)kTextDescent);
    put16(head, 1000);
    put16(head, (uint16_t)kTextAscent);
    put16(head, 0);
    put16(head, 8);
    put16(head, 2);
    put16(head, 0);
    put16(head, 0);
    tables.push_back({"head", std::move(head)});

    std::vector<uint8_t> hhea;
    put32(hhea, 0x00010000);
    put16(hhea, (uint16_t)kTextAscent);
    put16(hhea, (uint16_t)(int16_t)kTextDescent);
    put16(hhea, 0);
    put16(hhea, maxAdvance);
    put16(hhea, 0); put16(hhea, 0); put16(hhea, 0);
    put16(hhea, 1); put16(hhea, 0); put16(hhea, 0);
    for (int i = 0; i < 4; ++i) put16(hhea, 0);
    put16(hhea, 0);
    put16(hhea, numGlyphs);
    tables.push_back({"hhea", std::move(hhea)});

    std::vector<uint8_t> hmtx;
    hmtx.reserve(numGlyphs * 4);
    put16(hmtx, 1000); put16(hmtx, 0);
    for (uint16_t w : widths) { put16(hmtx, w); put16(hmtx, 0); }
    tables.push_back({"hmtx", std::move(hmtx)});

    tables.push_back({"loca", std::vector<uint8_t>((numGlyphs + 1) * 2, 0)});

    std::vector<uint8_t> maxp;
    put32(maxp, 0x00010000);
    put16(maxp, numGlyphs);
    put16(maxp, 0); put16(maxp, 0);
    put16(maxp, 0); put16(maxp, 0);
    put16(maxp, 1); put16(maxp, 0);
    put16(maxp, 0); put16(maxp, 0); put16(maxp, 0);
    put16(maxp, 0); put16(maxp, 0);
    put16(maxp, 0); put16(maxp, 0);
    tables.push_back({"maxp", std::move(maxp)});

    const uint32_t numTables = (uint32_t)tables.size();
    uint32_t entrySelector = 0;
    while ((2u << entrySelector) <= numTables) ++entrySelector;
    const uint32_t searchRange = (1u << entrySelector) * 16;
    std::vector<uint8_t> font;
    put32(font, 0x00010000);
    put16(font, numTables);
    put16(font, searchRange);
    put16(font, entrySelector);
    put16(font, numTables * 16 - searchRange);
    uint32_t offset = 12 + numTables * 16;
    size_t headOffset = 0;
    for (const auto& t : tables) {
        font.insert(font.end(), t.tag, t.tag + 4);
        put32(font, sfntChecksum(t.data.data(), t.data.size()));
        put32(font, offset);
        put32(font, (uint32_t)t.data.size());
        if (std::strcmp(t.tag, "head") == 0) headOffset = offset;
        offset += ((uint32_t)t.data.size() + 3) & ~3u;
    }
    for (const auto& t : tables) {
        font.insert(font.end(), t.data.begin(), t.data.end());
        while (font.size() % 4) font.push_back(0);
    }
    const uint32_t adjust = 0xB1B0AFBAu - sfntChecksum(font.data(), font.size());
    font[headOffset + 8]  = (uint8_t)(adjust >> 24);
    font[headOffset + 9]  = (uint8_t)(adjust >> 16);
    font[headOffset + 10] = (uint8_t)(adjust >> 8);
    font[headOffset + 11] = (uint8_t)adjust;
    return font;
}

}

StreamingPdfWriter::StreamingPdfWriter(const std::filesystem::path& outPath,
                                       int pageWidthPx, int pageHeightPx,
                                       Logger* log)
    : outPath_(outPath), tempPath_(outPath.string() + ".part"),
      pageW_(pageWidthPx), pageH_(pageHeightPx), log_(log) {
    offsets_.push_back(0);
}

StreamingPdfWriter::~StreamingPdfWriter() {
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    std::error_code ec;
    std::filesystem::remove(tempPath_, ec);
}

bool StreamingPdfWriter::open() {
#ifdef _WIN32
    fp_ = _wfopen(tempPath_.wstring().c_str(), L"wb");
#else
    fp_ = std::fopen(tempPath_.string().c_str(), "wb");
#endif
    if (!fp_) {
        if (log_) log_->log(LogLevel::Error, "pdf",
            ("StreamingPdfWriter: fopen failed: " + tempPath_.string()).c_str());
        return false;
    }

    static const char header[] = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    writeBytes_(header, sizeof(header) - 1);

    catalogId_   = allocObjId_();
    pagesTreeId_ = allocObjId_();
    return true;
}

int StreamingPdfWriter::allocObjId_() {
    int id = nextId_++;
    if ((int)offsets_.size() <= id) offsets_.resize(id + 1, 0);
    return id;
}

void StreamingPdfWriter::beginObject_(int id) {
    offsets_[id] = cursor_;
    writeFmt_("%d 0 obj\n", id);
}

void StreamingPdfWriter::endObject_() {
    writeStr_("endobj\n");
}

void StreamingPdfWriter::writeBytes_(const void* data, std::size_t n) {
    if (!fp_ || n == 0) return;
    std::fwrite(data, 1, n, fp_);
    cursor_ += n;
}

void StreamingPdfWriter::writeStr_(const char* s) {
    if (!s) return;
    writeBytes_(s, std::strlen(s));
}

void StreamingPdfWriter::writeFmt_(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) writeBytes_(buf, (std::size_t)std::min(n, (int)sizeof(buf) - 1));
}

void StreamingPdfWriter::setMetadata(const std::string& title,
                                     const std::string& author) {
    metaTitle_  = title;
    metaAuthor_ = author;
}

uint16_t StreamingPdfWriter::cidFor_(uint32_t codepoint, int widthUnits) {
    widthUnits = std::clamp(widthUnits, 0, 0xFFFF);
    const uint64_t key = ((uint64_t)codepoint << 16) | (uint64_t)widthUnits;
    auto it = cidByKey_.find(key);
    if (it != cidByKey_.end()) return it->second;
    if (cids_.size() >= 0xFFFE) {
        if (!cidSpaceFullWarned_ && log_) {
            cidSpaceFullWarned_ = true;
            log_->log(LogLevel::Warn, "pdf",
                      "text layer: CID table full, reusing widths");
        }
        auto cp = cidByCodepoint_.find(codepoint);
        if (cp != cidByCodepoint_.end()) return cp->second;
        return 0;
    }
    cids_.push_back({codepoint, (uint16_t)widthUnits});
    const uint16_t cid = (uint16_t)cids_.size();
    cidByKey_.emplace(key, cid);
    cidByCodepoint_.emplace(codepoint, cid);
    return cid;
}

bool StreamingPdfWriter::appendTextContent_(const std::vector<PlacedGlyph>& glyphs,
                                            std::string& out) {
    if (glyphs.empty()) return false;
    if (!type0FontObjId_) {
        fontDescObjId_  = allocObjId_();
        cidFontObjId_   = allocObjId_();
        toUnicodeObjId_ = allocObjId_();
        type0FontObjId_ = allocObjId_();
    }

    const size_t start = out.size();
    out.reserve(start + glyphs.size() * 6 + 64);
    out += "BT\n3 Tr\n";

    bool  inRun = false;
    bool  hexOpen = false;
    bool  runRotated = false;
    float runFontPx = -1.0f;
    float runCross = 0.0f;
    float pen = 0.0f;
    bool  any = false;

    auto closeHex = [&] {
        if (hexOpen) out.push_back('>');
        hexOpen = false;
    };
    auto closeRun = [&] {
        closeHex();
        if (inRun) out += "] TJ\n";
        inRun = false;
    };

    std::string merged;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const PlacedGlyph& g = glyphs[i];
        if (g.sourceUtf8.empty()) continue;
        const float fontPx = g.fontPx > 0 ? g.fontPx : 12.0f;
        const float gx = g.hasTextOrigin ? g.textX : g.x;
        const float gy = g.hasTextOrigin ? g.textY : g.y;
        const float along = g.rotated ? gy : gx;
        const float cross = g.rotated ? gx : gy;
        const float widthPx = g.advance >= 0 ? g.advance : fontPx;

        const std::string* text = &g.sourceUtf8;
        merged.clear();
        if (widthPx > 0) {
            const float end = along + widthPx;
            for (size_t j = i + 1; j < glyphs.size(); ++j) {
                const PlacedGlyph& h = glyphs[j];
                if (h.sourceUtf8.empty() || h.advance != 0 || h.rotated != g.rotated
                    || std::fabs(h.fontPx - g.fontPx) > 0.01f
                    || std::fabs((h.rotated ? h.x : h.y) - cross) > 0.05f
                    || std::fabs((h.rotated ? h.y : h.x) - end) > 0.05f)
                    break;
                if (merged.empty()) merged = g.sourceUtf8;
                merged += h.sourceUtf8;
                i = j;
            }
            if (!merged.empty()) text = &merged;
        }

        bool continueRun = inRun && g.rotated == runRotated
            && std::fabs(fontPx - runFontPx) < 0.01f
            && std::fabs(cross - runCross) < 0.05f;
        float delta = 0.0f;
        if (continueRun) {
            delta = along - pen;
            if (std::fabs(delta) > 2.0f * fontPx) continueRun = false;
        }
        if (!continueRun) {
            closeRun();
            if (std::fabs(fontPx - runFontPx) >= 0.01f) {
                out += "/F0 ";
                appendFixed1(out, fontPx);
                out += " Tf\n";
                runFontPx = fontPx;
            }
            if (g.rotated) {
                out += "0 -1 1 0 ";
                appendFixed1(out, gx);
                out.push_back(' ');
                appendFixed1(out, (float)pageH_ - gy);
            } else {
                out += "1 0 0 1 ";
                appendFixed1(out, gx);
                out.push_back(' ');
                appendFixed1(out, (float)pageH_ - gy);
            }
            out += " Tm\n[";
            inRun = true;
            runRotated = g.rotated;
            runCross = cross;
            pen = along;
            delta = 0.0f;
        } else if (std::fabs(delta) > kPenTolerancePx) {
            const long long adj = std::llround(-(double)delta / fontPx * 1000.0);
            if (adj != 0) {
                closeHex();
                appendInt(out, adj);
                pen -= (float)adj / 1000.0f * fontPx;
            }
        }

        const int n = countCodepoints(*text);
        const float perPx = n > 0 ? widthPx / (float)n : widthPx;
        const int widthUnits = (int)std::lround((double)perPx / fontPx * 1000.0);
        size_t pos = 0;
        while (pos < text->size()) {
            const uint32_t cp = nextCodepoint(*text, pos);
            if (cp == 0) continue;
            int units = widthUnits;
            if (isSpaceLike(cp))
                units = (units + kSpaceWidthStep / 2) / kSpaceWidthStep * kSpaceWidthStep;
            const uint16_t cid = cidFor_(cp, units);
            if (!hexOpen) { out.push_back('<'); hexOpen = true; }
            appendHex4(out, cid);
            const int w = cid ? cids_[cid - 1].width : 1000;
            pen += (float)w / 1000.0f * fontPx;
            any = true;
        }
    }
    closeRun();
    out += "ET\n";

    if (!any) { out.resize(start); return false; }
    return true;
}

namespace {
const char* filterName(StreamingPdfWriter::ImageCodec c) {
    return c == StreamingPdfWriter::ImageCodec::FlateGray4 ? "FlateDecode" : "DCTDecode";
}
int bitsPerComponent(StreamingPdfWriter::ImageCodec c) {
    return c == StreamingPdfWriter::ImageCodec::FlateGray4 ? 4 : 8;
}
}

int StreamingPdfWriter::addJpegPage(const uint8_t* jpegData, std::size_t jpegSize,
                                    const std::vector<PlacedGlyph>& glyphs,
                                    const std::vector<PdfLink>& links,
                                    ImageCodec codec) {
    if (!fp_ || !jpegData || jpegSize == 0) return -1;

    int imgId = allocObjId_();
    beginObject_(imgId);
    writeFmt_(
        "<</Type/XObject/Subtype/Image"
        "/Width %d/Height %d"
        "/ColorSpace/DeviceGray/BitsPerComponent %d"
        "/Filter/%s/Length %zu>>\n"
        "stream\n",
        pageW_, pageH_, bitsPerComponent(codec), filterName(codec), jpegSize);
    writeBytes_(jpegData, jpegSize);
    writeStr_("\nendstream\n");
    endObject_();

    std::string& content = textScratch_;
    content.clear();
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "q\n%d 0 0 %d 0 0 cm\n/Im0 Do\nQ\n", pageW_, pageH_);
        content = buf;
    }
    const bool hasText = appendTextContent_(glyphs, content);

    int contId = allocObjId_();
    beginObject_(contId);
    writeFmt_("<</Length %zu>>\nstream\n", content.size());
    writeBytes_(content.data(), content.size());
    writeStr_("endstream\n");
    endObject_();

    const int pageId = allocObjId_();
    pageObjIds_.push_back(pageId);
    pageRecords_.push_back({pageId, imgId, -1, contId, false, hasText, links});
    return static_cast<int>(pageObjIds_.size()) - 1;
}

int StreamingPdfWriter::addJpegSpread(const uint8_t* leftJpeg, std::size_t leftSize,
                                      const uint8_t* rightJpeg, std::size_t rightSize,
                                      int leafW, int leafH,
                                      const std::vector<PlacedGlyph>& glyphs,
                                      const std::vector<PdfLink>& links,
                                      ImageCodec leftCodec, ImageCodec rightCodec) {
    if (!fp_ || !leftJpeg || leftSize == 0 || leafW <= 0 || leafH <= 0) return -1;

    auto writeImage = [&](const uint8_t* data, std::size_t size, ImageCodec codec) -> int {
        int imgId = allocObjId_();
        beginObject_(imgId);
        writeFmt_(
            "<</Type/XObject/Subtype/Image"
            "/Width %d/Height %d"
            "/ColorSpace/DeviceGray/BitsPerComponent %d"
            "/Filter/%s/Length %zu>>\n"
            "stream\n",
            leafW, leafH, bitsPerComponent(codec), filterName(codec), size);
        writeBytes_(data, size);
        writeStr_("\nendstream\n");
        endObject_();
        return imgId;
    };

    const int imgL = writeImage(leftJpeg, leftSize, leftCodec);
    const int imgR = (rightJpeg && rightSize > 0) ? writeImage(rightJpeg, rightSize, rightCodec) : -1;

    std::string& content = textScratch_;
    content.clear();
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "q\n%d 0 0 %d 0 0 cm\n/ImL Do\nQ\n", leafW, leafH);
        content = buf;
        if (imgR >= 0) {
            std::snprintf(buf, sizeof(buf),
                "q\n%d 0 0 %d %d 0 cm\n/ImR Do\nQ\n", leafW, leafH, leafW);
            content += buf;
        }
    }
    const bool hasText = appendTextContent_(glyphs, content);

    int contId = allocObjId_();
    beginObject_(contId);
    writeFmt_("<</Length %zu>>\nstream\n", content.size());
    writeBytes_(content.data(), content.size());
    writeStr_("endstream\n");
    endObject_();

    const int pageId = allocObjId_();
    pageObjIds_.push_back(pageId);
    pageRecords_.push_back({pageId, imgL, imgR, contId, true, hasText, links});
    return static_cast<int>(pageObjIds_.size()) - 1;
}

bool StreamingPdfWriter::close() {
    if (!fp_) return false;

    if (type0FontObjId_) {
        std::string cmap;
        cmap.reserve(cids_.size() * 16 + 512);
        cmap += "/CIDInit /ProcSet findresource begin\n"
                "12 dict begin\n"
                "begincmap\n"
                "/CIDSystemInfo<</Registry(Adobe)/Ordering(UCS)/Supplement 0>>def\n"
                "/CMapName /Adobe-Identity-UCS def\n"
                "/CMapType 2 def\n"
                "1 begincodespacerange\n"
                "<0000> <FFFF>\n"
                "endcodespacerange\n";
        size_t idx = 0;
        while (idx < cids_.size()) {
            const size_t chunk = std::min(cids_.size() - idx, (size_t)100);
            appendInt(cmap, (long long)chunk);
            cmap += " beginbfchar\n";
            for (size_t j = 0; j < chunk; ++j) {
                const uint32_t cp = cids_[idx + j].codepoint;
                cmap.push_back('<');
                appendHex4(cmap, (uint16_t)(idx + j + 1));
                cmap += "> <";
                if (cp <= 0xFFFF) {
                    appendHex4(cmap, (uint16_t)cp);
                } else {
                    const uint32_t v = cp - 0x10000;
                    appendHex4(cmap, (uint16_t)(0xD800 | (v >> 10)));
                    appendHex4(cmap, (uint16_t)(0xDC00 | (v & 0x3FF)));
                }
                cmap += ">\n";
            }
            cmap += "endbfchar\n";
            idx += chunk;
        }
        cmap += "endcmap\n"
                "CMapName currentdict /CMap defineresource pop\n"
                "end\nend\n";

        beginObject_(toUnicodeObjId_);
        writeFmt_("<</Length %zu>>\nstream\n", cmap.size());
        writeBytes_(cmap.data(), cmap.size());
        writeStr_("endstream\n");
        endObject_();

        std::vector<uint16_t> advances;
        advances.reserve(cids_.size());
        for (const auto& c : cids_) advances.push_back(c.width);
        const std::vector<uint8_t> fontFile = buildMetricsOnlyTrueType(advances);
        const int fontFileObjId = allocObjId_();
        beginObject_(fontFileObjId);
        writeFmt_("<</Length %zu/Length1 %zu>>\nstream\n", fontFile.size(), fontFile.size());
        writeBytes_(fontFile.data(), fontFile.size());
        writeStr_("\nendstream\n");
        endObject_();

        beginObject_(fontDescObjId_);
        writeFmt_(
            "<</Type/FontDescriptor/FontName/HardcopyText"
            "/Flags 4"
            "/FontBBox[0 %d 1000 %d]"
            "/ItalicAngle 0"
            "/Ascent %d/Descent %d/CapHeight %d"
            "/StemV 80/FontFile2 %d 0 R>>\n",
            kTextDescent, kTextAscent,
            kTextAscent, kTextDescent, kTextCapHeight, fontFileObjId);
        endObject_();

        std::string widths;
        widths.reserve(cids_.size() * 5 + 16);
        widths += "/W[1[";
        for (size_t i = 0; i < cids_.size(); ++i) {
            if (i) widths.push_back(' ');
            appendInt(widths, cids_[i].width);
        }
        widths += "]]";
        beginObject_(cidFontObjId_);
        writeFmt_(
            "<</Type/Font/Subtype/CIDFontType2"
            "/BaseFont/HardcopyText"
            "/CIDSystemInfo<</Registry(Adobe)/Ordering(Identity)/Supplement 0>>"
            "/FontDescriptor %d 0 R"
            "/CIDToGIDMap/Identity"
            "/DW 1000",
            fontDescObjId_);
        writeBytes_(widths.data(), widths.size());
        writeStr_(">>\n");
        endObject_();

        beginObject_(type0FontObjId_);
        writeFmt_(
            "<</Type/Font/Subtype/Type0"
            "/BaseFont/HardcopyText"
            "/Encoding/Identity-H"
            "/DescendantFonts[%d 0 R]"
            "/ToUnicode %d 0 R>>\n",
            cidFontObjId_, toUnicodeObjId_);
        endObject_();

        if (log_) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "text layer: %zu CIDs", cids_.size());
            log_->log(LogLevel::Info, "pdf", msg);
        }
    }

    auto hasUriScheme = [](const std::string& u) {
        if (u.empty() || !std::isalpha(static_cast<unsigned char>(u[0]))) return false;
        for (size_t i = 1; i < u.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(u[i]);
            if (c == ':') return true;
            if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return false;
        }
        return false;
    };
    int droppedRelativeLinks = 0;
    std::vector<std::vector<int>> linkIds(pageRecords_.size());
    for (size_t pageIndex = 0; pageIndex < pageRecords_.size(); ++pageIndex) {
        auto& ids = linkIds[pageIndex];
        for (const PdfLink& link : pageRecords_[pageIndex].links) {
            const bool hasTarget = link.targetPage0 >= 0 &&
                link.targetPage0 < static_cast<int>(pageObjIds_.size());
            if (!hasTarget && link.uri.empty()) continue;
            if (!hasTarget && !hasUriScheme(link.uri)) {
                ++droppedRelativeLinks;
                continue;
            }
            const float x0 = std::max(0.0f, std::min(link.x0, link.x1));
            const float x1 = std::min(static_cast<float>(pageW_), std::max(link.x0, link.x1));
            const float y0 = std::max(0.0f, std::min(link.y0, link.y1));
            const float y1 = std::min(static_cast<float>(pageH_), std::max(link.y0, link.y1));
            if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) continue;
            const int id = allocObjId_();
            ids.push_back(id);
            beginObject_(id);
            writeFmt_("<</Type/Annot/Subtype/Link/Rect[%.3f %.3f %.3f %.3f]",
                      x0, static_cast<float>(pageH_) - y1,
                      x1, static_cast<float>(pageH_) - y0);
            writeStr_("/Border[0 0 0]");
            if (hasTarget) {
                writeFmt_("/Dest[%d 0 R/Fit]", pageObjIds_[link.targetPage0]);
            } else {
                writeStr_("/A<</S/URI/URI(");
                for (unsigned char c : link.uri) {
                    if (c == '(' || c == ')' || c == '\\') writeBytes_("\\", 1);
                    if (c == '\r' || c == '\n') continue;
                    writeBytes_(&c, 1);
                }
                writeStr_(")>>");
            }
            writeStr_(">>\n");
            endObject_();
        }
    }
    if (droppedRelativeLinks > 0 && log_) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "dropped %d link(s) to targets missing from the source",
                      droppedRelativeLinks);
        log_->log(LogLevel::Warn, "pdf", msg);
    }
    for (size_t pageIndex = 0; pageIndex < pageRecords_.size(); ++pageIndex) {
        const PageRecord& page = pageRecords_[pageIndex];
        beginObject_(page.pageId);
        writeFmt_("<</Type/Page/Parent %d 0 R/MediaBox[0 0 %d %d]",
                  pagesTreeId_, pageW_, pageH_);
        if (page.spread) {
            if (page.hasText) {
                writeFmt_("/Resources<</XObject<</ImL %d 0 R", page.imageLeftId);
                if (page.imageRightId > 0) writeFmt_("/ImR %d 0 R", page.imageRightId);
                writeFmt_(">>/Font<</F0 %d 0 R>>/ProcSet[/PDF/ImageB/Text]>>",
                          type0FontObjId_);
            } else {
                writeFmt_("/Resources<</XObject<</ImL %d 0 R", page.imageLeftId);
                if (page.imageRightId > 0) writeFmt_("/ImR %d 0 R", page.imageRightId);
                writeStr_(">>/ProcSet[/PDF/ImageB]>>");
            }
        } else if (page.hasText) {
            writeFmt_("/Resources<</XObject<</Im0 %d 0 R>>/Font<</F0 %d 0 R>>/ProcSet[/PDF/ImageB/Text]>>",
                      page.imageLeftId, type0FontObjId_);
        } else {
            writeFmt_("/Resources<</XObject<</Im0 %d 0 R>>/ProcSet[/PDF/ImageB]>>",
                      page.imageLeftId);
        }
        writeFmt_("/Contents %d 0 R", page.contentId);
        if (!linkIds[pageIndex].empty()) {
            writeStr_("/Annots[");
            for (int id : linkIds[pageIndex]) writeFmt_("%d 0 R ", id);
            writeStr_("]");
        }
        writeStr_(">>\n");
        endObject_();
    }

    std::vector<int> outlineIds;
    outlineIds.reserve(outlines_.size());
    if (!outlines_.empty()) outlinesId_ = allocObjId_();
    for (size_t i = 0; i < outlines_.size(); ++i) outlineIds.push_back(allocObjId_());

    struct Rel { int parent; int prev; int next; int firstChild; int lastChild; int count; };
    std::vector<Rel> rel(outlines_.size(), {0,0,0,0,0,0});
    std::vector<int> stack;
    for (size_t i = 0; i < outlines_.size(); ++i) {
        int lvl = outlines_[i].level;
        while (!stack.empty() && outlines_[stack.back()].level >= lvl) stack.pop_back();
        int parentIdx = stack.empty() ? -1 : stack.back();
        rel[i].parent = (parentIdx < 0) ? outlinesId_ : outlineIds[parentIdx];
        if (parentIdx >= 0) {
            rel[parentIdx].count++;
            if (rel[parentIdx].firstChild == 0) rel[parentIdx].firstChild = outlineIds[i];
            rel[parentIdx].lastChild = outlineIds[i];
        }
        for (int k = (int)i - 1; k >= 0; --k) {
            if (rel[k].parent == rel[i].parent) {
                rel[i].prev = outlineIds[k];
                rel[k].next = outlineIds[i];
                break;
            }
        }
        stack.push_back((int)i);
    }
    int rootFirst = 0, rootLast = 0, rootCount = 0;
    for (size_t i = 0; i < outlines_.size(); ++i) {
        if (rel[i].parent == outlinesId_) {
            if (rootFirst == 0) rootFirst = outlineIds[i];
            rootLast = outlineIds[i];
            ++rootCount;
        }
    }

    for (size_t i = 0; i < outlines_.size(); ++i) {
        int id = outlineIds[i];
        beginObject_(id);
        writeStr_("<</Title");
        std::string title16 = toUtf16BeEscaped(outlines_[i].title);
        writeStr_("(");
        writeBytes_(title16.data(), title16.size());
        writeStr_(")");
        writeFmt_("/Parent %d 0 R", rel[i].parent);
        if (rel[i].prev)        writeFmt_("/Prev %d 0 R", rel[i].prev);
        if (rel[i].next)        writeFmt_("/Next %d 0 R", rel[i].next);
        if (rel[i].firstChild)  writeFmt_("/First %d 0 R", rel[i].firstChild);
        if (rel[i].lastChild)   writeFmt_("/Last %d 0 R", rel[i].lastChild);
        if (rel[i].count > 0)   writeFmt_("/Count %d", rel[i].count);
        int pageObj = pageObjIds_[outlines_[i].pageIndex0];
        writeFmt_("/Dest[%d 0 R/Fit]", pageObj);
        writeStr_(">>\n");
        endObject_();
    }

    if (outlinesId_) {
        beginObject_(outlinesId_);
        writeFmt_("<</Type/Outlines/First %d 0 R/Last %d 0 R/Count %d>>\n",
                  rootFirst, rootLast, rootCount);
        endObject_();
    }

    infoId_ = allocObjId_();
    beginObject_(infoId_);
    writeStr_("<<");
    if (!metaTitle_.empty()) {
        writeStr_("/Title(");
        std::string t = toUtf16BeEscaped(metaTitle_);
        writeBytes_(t.data(), t.size());
        writeStr_(")");
    }
    if (!metaAuthor_.empty()) {
        writeStr_("/Author(");
        std::string a = toUtf16BeEscaped(metaAuthor_);
        writeBytes_(a.data(), a.size());
        writeStr_(")");
    }
    writeStr_("/Producer(Hardcopy)");
    writeFmt_("/CreationDate(%s)", pdfDateNow().c_str());
    writeStr_(">>\n");
    endObject_();

    beginObject_(pagesTreeId_);
    writeStr_("<</Type/Pages/Kids[");
    for (size_t i = 0; i < pageObjIds_.size(); ++i) {
        if (i > 0) writeStr_(" ");
        writeFmt_("%d 0 R", pageObjIds_[i]);
    }
    writeFmt_("]/Count %zu>>\n", pageObjIds_.size());
    endObject_();

    beginObject_(catalogId_);
    writeStr_("<</Type/Catalog");
    writeFmt_("/Pages %d 0 R", pagesTreeId_);
    if (outlinesId_) {
        writeFmt_("/Outlines %d 0 R/PageMode/UseOutlines", outlinesId_);
    }
    writeStr_(">>\n");
    endObject_();

    uint64_t xrefStart = cursor_;
    int nObjects = nextId_;
    writeFmt_("xref\n0 %d\n", nObjects);
    writeStr_("0000000000 65535 f \n");
    for (int id = 1; id < nObjects; ++id) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010llu 00000 n \n",
                      (unsigned long long)offsets_[id]);
        writeStr_(buf);
    }

    writeStr_("trailer\n");
    writeFmt_("<</Size %d/Root %d 0 R/Info %d 0 R",
              nObjects, catalogId_, infoId_);
    if (!fileIdHex_.empty()) {
        writeStr_("/ID[<");
        writeStr_(fileIdHex_);
        writeStr_("><");
        writeStr_(fileIdHex_);
        writeStr_(">]");
    }
    writeStr_(">>\n");
    writeStr_("startxref\n");
    writeFmt_("%llu\n", (unsigned long long)xrefStart);
    writeStr_("%%EOF\n");

    std::fflush(fp_);
    std::fclose(fp_);
    fp_ = nullptr;
    std::error_code ec;
    std::filesystem::rename(tempPath_, outPath_, ec);
    if (ec) {
        std::error_code removeEc;
        std::filesystem::remove(outPath_, removeEc);
        ec.clear();
        std::filesystem::rename(tempPath_, outPath_, ec);
    }
    if (ec) {
        if (log_) log_->log(LogLevel::Error, "pdf",
            ("StreamingPdfWriter: publish failed: " + ec.message()).c_str());
        return false;
    }
    return true;
}

bool StreamingPdfWriter::addLinks(int pageIndex0, const std::vector<PdfLink>& links) {
    if (pageIndex0 < 0 || pageIndex0 >= static_cast<int>(pageRecords_.size())) return false;
    auto& dst = pageRecords_[static_cast<size_t>(pageIndex0)].links;
    dst.insert(dst.end(), links.begin(), links.end());
    return true;
}

bool StreamingPdfWriter::addOutline(const std::string& title, int level,
                                    int pageIndex0) {
    if (title.empty()) return false;
    if (pageIndex0 < 0 || pageIndex0 >= (int)pageObjIds_.size()) return false;
    outlines_.push_back({title, level, pageIndex0});
    return true;
}

}
