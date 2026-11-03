#include "pipeline.h"

#include <algorithm>
#include <array>
#include <vector>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#if HARDCOPY_HAS_JPEG
#include "encode/jpeg.h"
#include "encode/flate.h"
#include "pdf/page_image_dump.h"
#include <fstream>
#include <time.h>
#endif

#if HARDCOPY_HAS_TEXT_MODE
#include "document.h"
#include "include/hardcopy/presets.h"
#include "layout/horizontal_layout.h"
#include "parser/bilingual_parser.h"
#include "parser/md_parser.h"
#include "parser/text_normalizer.h"
#include "source_export.h"
#include "parser/txt_parser.h"
#include "parser/zh_convert.h"
#include "pdf/streaming_pdf_writer.h"
#include "render/contrast_lut.h"
#include "render/font_face.h"
#include "render/page_raster.h"
#include "render/ruling_pattern.h"
#include "render/pdf_bitmap_processor.h"

#if HARDCOPY_HAS_PARALLEL
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#endif

#if HARDCOPY_HAS_VERTICAL_LAYOUT
#include "layout/vertical_layout.h"
#endif
#if HARDCOPY_HAS_DOCX
#include "parser/docx_parser.h"
#endif
#if HARDCOPY_HAS_PDFIN
#include "parser/pdf_parser.h"
#include "layout/epub_mupdf.h"
#include "pdf/raster_reflow.h"
#include "cancel.h"
extern "C" {
#include <mupdf/fitz.h>
}
#endif
#if HARDCOPY_HAS_PARALLEL
#include "util/thread_pool.h"
#endif
#endif


#if HARDCOPY_HAS_EPUB
#include "parser/epub_parser.h"
#endif

#if HARDCOPY_HAS_COMIC
#include "comic.h"
#include "parser/cbz_parser.h"
#include "parser/mobi_parser.h"
#include "util/zip_writer.h"
#if HARDCOPY_HAS_PDFIN
#include "parser/pdf_comic_parser.h"
#endif
#if HARDCOPY_HAS_DITHER
#include "image/dither.h"
#include "image/image_decoder.h"
#endif
#endif

namespace hardcopy {

namespace {

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

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool isBilingualJson(const std::filesystem::path& p) {
    std::string n = p.filename().string();
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    static const std::string kSuffix = ".bilingual.json";
    return n.size() > kSuffix.size()
        && n.compare(n.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

void emit(ink_progress_cb cb, void* ud, const char* job, ink_stage_t s, int pct) {
    if (cb) cb(job, (int)s, pct, ud);
}

enum class SessionKind { None, Text, Comic, EpubMupdf, PdfRaster };

struct ConvertSession {
    std::string fileKey;
    std::string parseKey;
    std::string layoutKey;
    std::string renderKey;
    std::string fullKey;
    std::filesystem::path lastOutput;
    SessionKind kind = SessionKind::None;
#if HARDCOPY_HAS_TEXT_MODE
    Document text;
    LayoutResult layout;
#endif
#if HARDCOPY_HAS_COMIC
    ComicDocument comic;
#endif
#if HARDCOPY_HAS_TEXT_MODE && HARDCOPY_HAS_PDFIN
    std::unique_ptr<MupdfEpubEngine> epub;
#endif
};

ConvertSession g_session;

void resetSession() {
    g_session = ConvertSession{};
}

std::string fileIdentity(const std::filesystem::path& input, const Options& opts) {
    if (!opts.sourceIdentity.empty()) return opts.sourceIdentity;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(input, ec);
    const auto size = std::filesystem::file_size(input, ec);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "|%llu|%llu",
                  (unsigned long long)mtime.time_since_epoch().count(),
                  (unsigned long long)size);
    return input.string() + buf;
}

std::string optionsFingerprint(const Options& o) {
    char buf[1024];
    std::snprintf(
        buf, sizeof(buf),
        "%dx%d|m%d,%d,%d,%d|fs%.3f|lh%.3f|ps%.3f|ind%.3f|o%d|p=%s|q%d|"
        "tl%d|bm%d|sp%d|ru%d|wt%d|sb%d|cb%.4f|se%.4f|ft%d|bg=%s|cm%d|da%d|dl%d|"
        "cw%d|ch%d|pdf%d|ee%d|zh%d|rp%d|f=%s|fs=%s|fk=%s|ff=%s",
        o.pageWidth, o.pageHeight,
        o.marginLeft, o.marginRight, o.marginTop, o.marginBottom,
        o.fontSize, o.lineHeightMul, o.paragraphSpacing, o.firstLineIndentChars,
        (int)o.orientation, o.preset.c_str(), o.jpegQuality,
        (int)o.embedTextLayer, (int)o.embedBookmarks, (int)o.splitLandscape,
        (int)o.drawRuling, (int)o.whiteText, (int)o.syntheticBold,
        o.contrastBoost, o.strokeExpand,
        (int)o.footerProgress, o.backgroundPath.c_str(), (int)o.comicMode,
        o.ditherAlgo, o.ditherLevels, o.comicResizeW, o.comicResizeH,
        (int)o.pdfMode, (int)o.epubEngine, (int)o.zhConvert, (int)o.reversePages,
        o.fontPath.c_str(), o.fontPathSans.c_str(), o.fontPathKai.c_str(),
        o.fontPathFallback.c_str());
    return std::string(buf) + "|fm=" + o.fontPathMono + "|ruling-v2:" + o.rulingStyle + "|pdf-shared-v2|" + std::to_string(o.pdfTrim)
        + "|" + std::to_string(o.pdfHeaderCutPercent)
        + "|" + std::to_string(o.pdfFooterCutPercent)
        + "|" + std::to_string(o.pdfMaxOutputPages)
        + "|" + std::to_string(o.pdfPreviewSourcePage)
        + "|" + std::to_string(o.pdfWatermarkThreshold)
        + "|" + std::to_string(o.pdfGamma)
        + "|" + std::to_string(o.pdfContrast)
        + "|ps" + std::to_string(o.pdfStroke)
        + "|pic" + std::to_string(o.pdfImageContrast)
        + "|pft" + std::to_string(o.pdfFillTone)
        + "|pc" + std::to_string(o.pdfColumns)
        + "|pao" + std::to_string(o.pdfAutoOrientation)
        + "|ocr" + std::to_string(o.pdfOcr) + std::to_string(o.pdfIgnoreText)
        + "|plh" + std::to_string(o.pdfLineHeight)
        + "|ppe" + std::to_string(o.pdfParaSpacingEm)
        + "|icb" + std::to_string(o.imageContrastBoost)
        + "|ct" + std::to_string(o.comicTone) + std::to_string(o.comicSkipBlank)
        + "|cm" + std::to_string(o.comicMarginPercent) + "s" + std::to_string(o.comicSpread)
        + "|bi" + std::to_string(o.bilingual) + o.bilingualLayout;
}

std::string parseFingerprint(const std::filesystem::path& in, const Options& opts) {
    const std::string ext = lowerExt(in);
    std::string s = ext;
    const bool comic = opts.comicMode
#if HARDCOPY_HAS_COMIC
        || ext == ".cbz" || ext == ".zip" || ext == ".mobi" || ext == ".prc"
#endif
        ;
    s += comic ? "|comic" : "|text";
    if (ext == ".pdf") {
        s += opts.pdfMode == PdfMode::Raster ? "|raster" : "|reflow";
        if (opts.pdfIgnoreText) s += "|ignore-text";
    }
    if (ext == ".txt") {
        if (opts.preset == "fresh") s += "|poetry";
        else if (opts.preset == "web") s += "|web";
        else s += "|normal";
    }
    if (ext == ".epub" && !comic) {
        const bool mupdf = opts.epubEngine != EpubEngine::Legacy
                        && opts.orientation == Orientation::HorizontalLtr
                        && opts.zhConvert == ZhConvert::None;
        s += mupdf ? "|mupdf" : "|legacy";
        if (opts.zhConvert != ZhConvert::None)
            s += opts.zhConvert == ZhConvert::S2T ? "|s2t" : "|t2s";
    } else if (opts.zhConvert != ZhConvert::None) {
        s += opts.zhConvert == ZhConvert::S2T ? "|s2t" : "|t2s";
    }
    if (!comic && opts.preset == "web") s += "|compact-normalize-v1";
    if (isBilingualJson(in)) s += "|bilingual-v1";
    return s;
}

std::string layoutFingerprint(const Options& o) {
    char buf[768];
    std::snprintf(
        buf, sizeof(buf),
        "%dx%d|m%d,%d,%d,%d|fs%.3f|lh%.3f|ps%.3f|ind%.3f|o%d|p=%s|sp%d|ft%d|"
        "f=%s|fs=%s|fk=%s|ff=%s|bg=%s|wt%d",
        o.pageWidth, o.pageHeight,
        o.marginLeft, o.marginRight, o.marginTop, o.marginBottom,
        o.fontSize, o.lineHeightMul, o.paragraphSpacing, o.firstLineIndentChars,
        (int)o.orientation, o.preset.c_str(), (int)o.splitLandscape,
        (int)o.footerProgress,
        o.fontPath.c_str(), o.fontPathSans.c_str(), o.fontPathKai.c_str(),
        o.fontPathFallback.c_str(), o.backgroundPath.c_str(), (int)o.whiteText);
    return std::string(buf) + "|bi" + std::to_string(o.bilingual) + o.bilingualLayout;
}

int renderWorkers(const Options& opts, int pageCount) {
    const int hardwareThreads = std::max(1, (int)std::thread::hardware_concurrency());
    const int requested = opts.threadCount > 0
                        ? opts.threadCount
                        : std::min(4, hardwareThreads);
    return std::min(std::max(1, requested), std::max(1, pageCount));
}

#if HARDCOPY_HAS_TEXT_MODE
int pdfSheetWidth(const Options& o) {
    return o.splitLandscape ? o.pageWidth * 2 : o.pageWidth;
}

int outlinePageIndex(const Options& o, int leafIndex0, int pageCount) {
    const int idx = outputPageIndex0(leafIndex0, pageCount, o.reversePages);
    return o.splitLandscape ? idx / 2 : idx;
}

std::vector<HeadingAnchor> epubOutlineForLayout(const Document& doc,
                                                const LayoutResult& layout) {
    std::vector<HeadingAnchor> result;
    for (const auto& entry : doc.epubToc) {
        if (entry.sourceSpineIndex < 0) continue;
        int page = -1;
        int fragmentPage = -1;
        for (size_t bi = 0; bi < doc.blocks.size()
                          && bi < layout.blockPageIndex.size(); ++bi) {
            const Block& b = doc.blocks[bi];
            if (b.sourceSpineIndex != entry.sourceSpineIndex) continue;
            if (page < 0) page = layout.blockPageIndex[bi];
            if (entry.fragment.empty()) break;
            if (std::find(b.anchorIds.begin(), b.anchorIds.end(), entry.fragment)
                    != b.anchorIds.end()) {
                fragmentPage = layout.blockPageIndex[bi];
                break;
            }
        }
        if (fragmentPage >= 0) page = fragmentPage;
        if (page < 0 || page >= (int)layout.pages.size()) continue;
        HeadingAnchor a;
        a.title = entry.title;
        a.level = entry.level;
        a.pageIndex0 = page;
        a.yOnPage = 0.0f;
        result.push_back(std::move(a));
    }
    return result;
}

std::string outlineMatchKey(const std::string& s, size_t limit) {
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
            if (!punct) key.append(s, i, len);
        }
        i += len;
    }
    return key;
}

std::vector<HeadingAnchor> pdfOutlineForLayout(const Document& doc, const LayoutResult& layout) {
    bool mapped = false;
    for (const auto& b : doc.blocks) mapped |= b.sourceSpineIndex >= 0;
    if (!mapped) return doc.importedOutline;
    std::vector<HeadingAnchor> result;
    const size_t n = std::min(doc.blocks.size(), layout.blockPageIndex.size());
    for (const auto& entry : doc.importedOutline) {
        const std::string title = outlineMatchKey(entry.title, 48);
        int page = -1, fallback = -1;
        for (size_t bi = 0; bi < n; ++bi) {
            const Block& b = doc.blocks[bi];
            if (b.sourceSpineIndex < entry.pageIndex0 || b.kind == BlockKind::PageBreak) continue;
            if (fallback < 0) fallback = layout.blockPageIndex[bi];
            if (b.sourceSpineIndex > entry.pageIndex0) break;
            if (title.empty()) break;
            std::string text;
            for (const auto& r : b.runs) text += r.text;
            const std::string key = outlineMatchKey(text, title.size());
            if (!key.empty() && key.size() >= std::min<size_t>(title.size(), 4) &&
                title.compare(0, key.size(), key) == 0) {
                page = layout.blockPageIndex[bi];
                break;
            }
        }
        if (page < 0) page = fallback;
        if (page < 0 || page >= (int)layout.pages.size()) continue;
        HeadingAnchor a = entry;
        a.pageIndex0 = page;
        a.yOnPage = 0.0f;
        result.push_back(std::move(a));
    }
    return result;
}

std::vector<PdfLink> legacyLinksForPage(const Document& doc,
                                        const LayoutResult& layout,
                                        int pageIndex0, bool vertical) {
    if (pageIndex0 < 0 || pageIndex0 >= (int)layout.pages.size()) return {};
    struct Box { float x0, y0, x1, y1; };
    std::map<std::string, Box> boxes;
    for (const auto& g : layout.pages[(size_t)pageIndex0].glyphs) {
        if (g.linkTarget.empty() || g.sourceUtf8.empty()) continue;
        float x0, y0, x1, y1;
        if (!vertical) {
            x0 = g.x;
            x1 = g.x + std::max(1.0f, g.advance);
            y0 = g.y - std::max(1.0f, g.fontPx * 0.95f);
            y1 = g.y + std::max(1.0f, g.fontPx * 0.18f);
        } else {
            x0 = g.x - std::max(1.0f, g.fontPx * 0.52f);
            x1 = g.x + std::max(1.0f, g.fontPx * 0.52f);
            y0 = g.y - std::max(1.0f, g.fontPx * 0.15f);
            y1 = g.y + std::max(1.0f, g.advance);
        }
        auto it = boxes.find(g.linkTarget);
        if (it == boxes.end()) boxes.emplace(g.linkTarget, Box{x0, y0, x1, y1});
        else {
            it->second.x0 = std::min(it->second.x0, x0);
            it->second.y0 = std::min(it->second.y0, y0);
            it->second.x1 = std::max(it->second.x1, x1);
            it->second.y1 = std::max(it->second.y1, y1);
        }
    }
    std::vector<PdfLink> links;
    for (const auto& kv : boxes) {
        std::string target = kv.first;
        const size_t hash = target.find('#');
        const std::string targetPath = hash == std::string::npos
                                     ? target : target.substr(0, hash);
        const std::string fragment = hash == std::string::npos
                                   ? std::string() : target.substr(hash + 1);
        int targetPage = -1;
        for (size_t bi = 0; bi < doc.blocks.size()
                          && bi < layout.blockPageIndex.size(); ++bi) {
            const Block& b = doc.blocks[bi];
            if (b.sourcePath != targetPath) continue;
            if (targetPage < 0) targetPage = layout.blockPageIndex[bi];
            if (fragment.empty()) break;
            if (std::find(b.anchorIds.begin(), b.anchorIds.end(), fragment)
                    != b.anchorIds.end()) {
                targetPage = layout.blockPageIndex[bi];
                break;
            }
        }
        PdfLink link;
        link.x0 = kv.second.x0;
        link.y0 = kv.second.y0;
        link.x1 = kv.second.x1;
        link.y1 = kv.second.y1;
        if (targetPage >= 0) link.targetPage0 = targetPage;
        else link.uri = target;
        links.push_back(std::move(link));
    }
    return links;
}

void applyBookSpread(Options* o, Logger* log) {
    if (!o || !o->splitLandscape) return;
    const int portW = std::min(o->pageWidth, o->pageHeight);
    const int portH = std::max(o->pageWidth, o->pageHeight);
    o->pageWidth = std::max(1, portH / 2);
    o->pageHeight = std::max(1, portW);
    if (o->preset == "note") {
        if (const ReadingPreset* p = findReadingPreset("note")) {
            const ResolvedPresetMargins margins =
                resolveReadingPresetMargins(*p, o->pageWidth, o->pageHeight);
            o->marginLeft = margins.left;
            o->marginRight = margins.right;
            o->marginTop = margins.top;
            o->marginBottom = margins.bottom;
        }
    }
    if (log) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "book spread: leaf=%dx%d sheet=%dx%d",
            o->pageWidth, o->pageHeight, o->pageWidth * 2, o->pageHeight);
        log->log(LogLevel::Info, "pipeline", msg);
    }
}

struct SpreadSink {
    StreamingPdfWriter& pdf;
    bool split = false;
    bool embedText = false;
    int leafW = 0;
    int leafH = 0;
    using Codec = StreamingPdfWriter::ImageCodec;
    std::vector<uint8_t> pendingJpeg;
    std::vector<PlacedGlyph> pendingGlyphs;
    std::vector<PdfLink> pendingLinks;
    Codec pendingCodec = Codec::Jpeg;
    bool havePending = false;

    bool emit(const std::vector<uint8_t>& leftJpeg,
              const std::vector<PlacedGlyph>& leftGlyphs,
              const std::vector<PdfLink>& leftLinks,
              const std::vector<uint8_t>* rightJpeg,
              const std::vector<PlacedGlyph>* rightGlyphs,
              const std::vector<PdfLink>* rightLinks,
              Codec leftCodec, Codec rightCodec) {
        std::vector<PlacedGlyph> glyphs;
        if (embedText) {
            glyphs = leftGlyphs;
            if (rightGlyphs) {
                glyphs.reserve(glyphs.size() + rightGlyphs->size());
                for (auto g : *rightGlyphs) {
                    g.x += (float)leafW;
                    if (g.hasTextOrigin) g.textX += (float)leafW;
                    glyphs.push_back(std::move(g));
                }
            }
        }
        const uint8_t* right = (rightJpeg && !rightJpeg->empty()) ? rightJpeg->data() : nullptr;
        const size_t rightSize = right ? rightJpeg->size() : 0;
        std::vector<PdfLink> links = leftLinks;
        if (rightLinks) {
            links.reserve(links.size() + rightLinks->size());
            for (auto link : *rightLinks) {
                link.x0 += static_cast<float>(leafW);
                link.x1 += static_cast<float>(leafW);
                links.push_back(std::move(link));
            }
        }
        return pdf.addJpegSpread(leftJpeg.data(), leftJpeg.size(),
                                 right, rightSize, leafW, leafH, glyphs, links,
                                 leftCodec, rightCodec) >= 0;
    }

    bool push(std::vector<uint8_t> jpeg, std::vector<PlacedGlyph> glyphs = {},
              std::vector<PdfLink> links = {}, Codec codec = Codec::Jpeg) {
        if (jpeg.empty()) return false;
        if (!split) {
            if (embedText)
                return pdf.addJpegPage(jpeg.data(), jpeg.size(), glyphs, links, codec) >= 0;
            return pdf.addJpegPage(jpeg.data(), jpeg.size(), {}, links, codec) >= 0;
        }
        if (!havePending) {
            pendingJpeg = std::move(jpeg);
            pendingGlyphs = std::move(glyphs);
            pendingLinks = std::move(links);
            pendingCodec = codec;
            havePending = true;
            return true;
        }
        const bool ok = emit(pendingJpeg, pendingGlyphs, pendingLinks, &jpeg, &glyphs, &links,
                             pendingCodec, codec);
        havePending = false;
        pendingJpeg.clear();
        pendingGlyphs.clear();
        pendingLinks.clear();
        return ok;
    }

    bool finish() {
        if (!split || !havePending) return true;
        const bool ok = emit(pendingJpeg, pendingGlyphs, pendingLinks, nullptr, nullptr, nullptr,
                             pendingCodec, pendingCodec);
        havePending = false;
        pendingJpeg.clear();
        pendingGlyphs.clear();
        pendingLinks.clear();
        return ok;
    }
};

uint64_t threadCpuMs() {
#if defined(_WIN32)
    return 0;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

std::string cpuProbe() {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) {
        return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0;
    };
    volatile uint32_t iters = 10000000u;
    const auto t0 = Clock::now();
    uint64_t x = 88172645463325252ULL;
    for (uint32_t i = 0, n = iters; i < n; ++i)
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto t1 = Clock::now();
    std::vector<uint8_t> mem(16u << 20);
    std::memset(mem.data(), 0, mem.size());
    const auto t2 = Clock::now();
    std::memset(mem.data(), (int)(x & 0x7F) | 1, mem.size());
    const auto t3 = Clock::now();
    volatile uint8_t sink = mem[(size_t)(x % mem.size())];
    (void)sink;
    std::string freq;
#if defined(__linux__)
    for (int c = 0; c < 16; ++c) {
        char path[96];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        std::ifstream in(path);
        long khz = 0;
        if (!(in >> khz)) break;
        if (!freq.empty()) freq += ",";
        freq += std::to_string(khz / 1000);
    }
#endif
    char buf[256];
    std::snprintf(buf, sizeof(buf), "cpu probe: alu=%.1fms (10M lcg) memset=%.0fMB/s%s%s",
                  ms(t1 - t0), 16.0 / std::max(0.001, ms(t3 - t2) / 1000.0),
                  freq.empty() ? "" : " freq-mhz=", freq.c_str());
    return buf;
}

using FontBytes = std::shared_ptr<const std::vector<uint8_t>>;

FontBytes readFontFile(const std::string& path, Logger* log) {
    if (path.empty()) return nullptr;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (log) log->log(LogLevel::Warn, "pipeline", ("cannot read font: " + path).c_str());
        return nullptr;
    }
    const std::streamsize size = in.tellg();
    if (size <= 0) return nullptr;
    auto buf = std::make_shared<std::vector<uint8_t>>((size_t)size);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(buf->data()), size)) return nullptr;
    return buf;
}

FontBytes wrapFontData(const std::vector<uint8_t>& data) {
    if (data.empty()) return nullptr;
    return std::make_shared<const std::vector<uint8_t>>(data);
}

struct LoadedFontBytes {
    FontBytes regular, bold, mono, sans, kai, fallback;
};

LoadedFontBytes loadFontBytes(const Options& o, Logger* log) {
    LoadedFontBytes f;
    std::map<std::string, FontBytes> byPath;
    auto pick = [&](const std::vector<uint8_t>& data, const std::string& path) -> FontBytes {
        if (!data.empty()) return wrapFontData(data);
        if (path.empty()) return nullptr;
        auto it = byPath.find(path);
        if (it != byPath.end()) return it->second;
        FontBytes bytes = readFontFile(path, log);
        byPath.emplace(path, bytes);
        return bytes;
    };
    f.regular  = pick(o.fontData, o.fontPath);
    f.bold     = pick(o.fontDataBold, o.fontPathBold);
    f.mono     = pick(o.fontDataMono, o.fontPathMono);
    f.sans     = pick(o.fontDataSans, o.fontPathSans);
    f.kai      = pick(o.fontDataKai, o.fontPathKai);
    f.fallback = pick(o.fontDataFallback, o.fontPathFallback);
    if (log) {
        size_t total = 0;
        for (const auto& kv : byPath) if (kv.second) total += kv.second->size();
        char msg[128];
        std::snprintf(msg, sizeof(msg), "fonts loaded into memory: %zu files, %.1f MB",
                      byPath.size(), total / 1048576.0);
        log->log(LogLevel::Info, "pipeline", msg);
    }
    return f;
}

constexpr double kJpegImageAreaMin = 0.02;

double imageAreaFraction(const PageGlyphs& page, int pageW, int pageH) {
    if (pageW <= 0 || pageH <= 0) return 0.0;
    double area = 0.0;
    for (const auto& im : page.images) {
        const double x0 = std::clamp((double)im.x, 0.0, (double)pageW);
        const double y0 = std::clamp((double)im.y, 0.0, (double)pageH);
        const double x1 = std::clamp((double)im.x + im.w, 0.0, (double)pageW);
        const double y1 = std::clamp((double)im.y + im.h, 0.0, (double)pageH);
        area += std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
    }
    return area / ((double)pageW * pageH);
}

double imageAreaFraction(const std::vector<ImageRect>& rects, int pageW, int pageH) {
    if (pageW <= 0 || pageH <= 0) return 0.0;
    double area = 0.0;
    for (const auto& r : rects) {
        const int x0 = std::clamp(r.x0, 0, pageW), x1 = std::clamp(r.x1, 0, pageW);
        const int y0 = std::clamp(r.y0, 0, pageH), y1 = std::clamp(r.y1, 0, pageH);
        area += (double)std::max(0, x1 - x0) * std::max(0, y1 - y0);
    }
    return area / ((double)pageW * pageH);
}

bool encodePageImage(const Bitmap8& bm, double imageAreaFrac, const Options& o,
                     std::vector<uint8_t>* out,
                     StreamingPdfWriter::ImageCodec* codec) {
    const bool wantFlate = o.pageCodec == PageCodec::Flate
                        || (o.pageCodec == PageCodec::Auto
                            && imageAreaFrac < kJpegImageAreaMin);
    if (wantFlate && encodeFlateGray4(bm, out)) {
        *codec = StreamingPdfWriter::ImageCodec::FlateGray4;
        return true;
    }
    *codec = StreamingPdfWriter::ImageCodec::Jpeg;
    return encodeJpegGray(bm, o.jpegQuality, out);
}

SpreadSink makeSpreadSink(StreamingPdfWriter& pdf, const Options& o) {
    SpreadSink sink{pdf};
    sink.split = o.splitLandscape;
    sink.embedText = o.embedTextLayer;
    sink.leafW = o.pageWidth;
    sink.leafH = o.pageHeight;
    return sink;
}
#endif

bool copyCachedOutput(const std::filesystem::path& output, Logger* log) {
    std::error_code ec;
    if (g_session.lastOutput.empty() ||
        !std::filesystem::exists(g_session.lastOutput, ec))
        return false;
    if (std::filesystem::equivalent(g_session.lastOutput, output, ec))
        return true;
    std::filesystem::copy_file(
        g_session.lastOutput, output,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (log) log->log(LogLevel::Error, "pipeline", "cached output copy failed");
        return false;
    }
    if (log) log->log(LogLevel::Info, "pipeline", "reused previous output (identical options)");
    return true;
}

void rememberOutput(const std::filesystem::path& output,
                    const std::string& fullKey) {
    g_session.lastOutput = output;
    g_session.fullKey = fullKey;
}

#if HARDCOPY_HAS_TEXT_MODE
bool parseInput(const std::filesystem::path& in, Document* doc,
                const Options& opts, Logger* log, const void* pdfOcr = nullptr) {
    std::string ext = lowerExt(in);
    bool parsed = false;
    if (isBilingualJson(in)) {
        return parseBilingual(in, doc, log);
    }
    if (ext == ".txt") {
        TxtMode mode = TxtMode::Normal;
        if (opts.preset == "fresh") mode = TxtMode::Poetry;
        else if (opts.preset == "web") mode = TxtMode::Web;
        parsed = parseTxt(in, doc, mode);
    } else if (ext == ".md" || ext == ".markdown") {
        parsed = parseMarkdown(in, doc);
#if HARDCOPY_HAS_EPUB
    } else if (ext == ".epub") {
        parsed = parseEpub(in, doc, log);
#endif
#if HARDCOPY_HAS_DOCX
    } else if (ext == ".docx") {
        parsed = parseDocx(in, doc, log);
#endif
#if HARDCOPY_HAS_PDFIN
    } else if (ext == ".pdf") {
        parsed = parsePdf(in, doc, log, static_cast<const PdfOcrText*>(pdfOcr), opts.pdfIgnoreText);
#endif
    } else {
        if (log) log->log(LogLevel::Error, "pipeline", "unsupported input extension");
        return false;
    }
    if (!parsed) return false;

    if (ext != ".md" && ext != ".markdown")
        normalizeSourceParagraphIndents(doc);
    if (opts.preset == "web") normalizeCompactText(doc);
    if (opts.zhConvert != ZhConvert::None)
        convertDocumentZh(doc, opts.zhConvert);
    return true;
}
#endif

#if HARDCOPY_HAS_TEXT_MODE && HARDCOPY_HAS_PDFIN
bool noteMupdfCrossesDivider(const std::vector<PlacedGlyph>& glyphs,
                             const std::vector<ImageRect>& imageRects,
                             const Options& opts, int pageW, int pageH) {
    if (opts.preset != "note") return false;
    if (pageH >= pageW) {
        const float dividerY = (float)noteDividerPosition(pageH);
        for (const auto& g : glyphs)
            if (!g.sourceUtf8.empty() && g.y > dividerY) return true;
        for (const auto& r : imageRects)
            if (r.y1 > dividerY + 1.0f) return true;
    } else {
        const float dividerX = (float)noteDividerPosition(pageW);
        for (const auto& g : glyphs)
            if (!g.sourceUtf8.empty() && g.x > dividerX) return true;
        for (const auto& r : imageRects)
            if (r.x1 > dividerX + 1.0f) return true;
    }
    return false;
}

void logContrastSelection(Logger* log, const char* path, double setting,
                          const uint8_t (&lut)[256]) {
    if (!log) return;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "%s contrast level=%.0f/20 lut[128]=%d lut[200]=%d lut[50]=%d",
        path, std::clamp(setting, 0.0, 1.0) * 20.0,
        lut[128], lut[200], lut[50]);
    log->log(LogLevel::Info, "pipeline", msg);
}

void drawMupdfRuling(Bitmap8* bitmap, const std::vector<PlacedGlyph>& glyphs,
                     const Options& opts) {
    if (!bitmap || !opts.drawRuling || glyphs.empty()) return;

    struct Row {
        float baseline;
        float fontPx;
        int count;
        std::array<int, 16> fontCounts{};
    };
    const int contentRight = bitmap->width - opts.marginRight;
    const int contentBottom = opts.preset == "note"
                            ? noteDividerPosition(bitmap->height)
                            : bitmap->height - footerContentBottomInsetPx(opts);
    const float contentLeftF = (float)opts.marginLeft;
    const float contentRightF = (float)contentRight;
    const float contentWidthF = std::max(1.0f, contentRightF - contentLeftF);

    struct TextBlock {
        uint32_t id;
        float x0, y0, x1, y1;
        int glyphCount;
    };
    std::vector<TextBlock> textBlocks;
    for (const auto& glyph : glyphs) {
        if (glyph.textBlockId == 0xFFFFFFFFu) continue;
        auto it = std::find_if(textBlocks.begin(), textBlocks.end(),
            [&](const TextBlock& block) { return block.id == glyph.textBlockId; });
        if (it == textBlocks.end()) {
            textBlocks.push_back(TextBlock{glyph.textBlockId,
                                           glyph.blockX0, glyph.blockY0,
                                           glyph.blockX1, glyph.blockY1, 1});
        } else {
            it->x0 = std::min(it->x0, glyph.blockX0);
            it->y0 = std::min(it->y0, glyph.blockY0);
            it->x1 = std::max(it->x1, glyph.blockX1);
            it->y1 = std::max(it->y1, glyph.blockY1);
            ++it->glyphCount;
        }
    }

    struct RulingExclusion {
        std::vector<uint32_t> blockIds;
        float x0, y0, y1;
    };
    std::vector<RulingExclusion> exclusions;
    std::vector<uint32_t> groupedBlockIds;
    const float rowStep = (float)(opts.fontSize * opts.lineHeightMul);
    for (const auto& block : textBlocks) {
        const float blockW = block.x1 - block.x0;
        const float blockH = block.y1 - block.y0;
        const bool onRight = block.x0 >= contentLeftF + contentWidthF * 0.46f;
        const bool sideWidth = blockW <= contentWidthF * 0.52f;
        const bool multiLine = blockH >= rowStep * 1.45f && block.glyphCount >= 4;
        if (!onRight || !sideWidth || !multiLine) continue;

        if (std::find(groupedBlockIds.begin(), groupedBlockIds.end(), block.id)
            != groupedBlockIds.end()) continue;

        std::vector<uint32_t> groupIds{block.id};
        float groupX0 = block.x0;
        float groupX1 = block.x1;
        float groupY0 = block.y0;
        float groupY1 = block.y1;
        bool grew = true;
        while (grew) {
            grew = false;
            for (const auto& nearby : textBlocks) {
                if (std::find(groupIds.begin(), groupIds.end(), nearby.id)
                    != groupIds.end()) continue;
                const float nearbyW = nearby.x1 - nearby.x0;
                const bool nearbyRight = nearby.x0
                    >= contentLeftF + contentWidthF * 0.46f;
                if (!nearbyRight || nearbyW > contentWidthF * 0.52f) continue;
                const float overlapX = std::min(groupX1, nearby.x1)
                                     - std::max(groupX0, nearby.x0);
                const float minW = std::min(groupX1 - groupX0, nearbyW);
                const float gapY = nearby.y0 > groupY1 ? nearby.y0 - groupY1
                                 : groupY0 > nearby.y1 ? groupY0 - nearby.y1
                                 : 0.0f;
                if (overlapX < minW * 0.25f || gapY > rowStep * 1.35f) continue;
                groupIds.push_back(nearby.id);
                groupX0 = std::min(groupX0, nearby.x0);
                groupX1 = std::max(groupX1, nearby.x1);
                groupY0 = std::min(groupY0, nearby.y0);
                groupY1 = std::max(groupY1, nearby.y1);
                grew = true;
            }
        }

        bool hasBodyAtLeft = false;
        const float leftLimit = groupX0 - (float)opts.fontSize * 0.65f;
        for (const auto& glyph : glyphs) {
            if (glyph.sourceUtf8.empty()) continue;
            if (std::find(groupIds.begin(), groupIds.end(), glyph.textBlockId)
                != groupIds.end()) continue;
            if (glyph.y >= groupY0 - rowStep * 0.35f
                && glyph.y <= groupY1 + rowStep * 0.35f
                && glyph.x < leftLimit) {
                hasBodyAtLeft = true;
                break;
            }
        }
        if (!hasBodyAtLeft) continue;
        groupedBlockIds.insert(groupedBlockIds.end(), groupIds.begin(), groupIds.end());
        exclusions.push_back(RulingExclusion{std::move(groupIds),
            std::max(contentLeftF, groupX0 - (float)opts.fontSize),
            groupY0 - rowStep * 0.85f,
            groupY1 + rowStep * 0.85f});
    }

    std::vector<Row> rows;
    for (const auto& glyph : glyphs) {
        if (glyph.sourceUtf8.empty() || glyph.fontPx <= 0.0f) continue;
        const bool inSideFloat = std::any_of(exclusions.begin(), exclusions.end(),
            [&](const RulingExclusion& exclusion) {
                return std::find(exclusion.blockIds.begin(), exclusion.blockIds.end(),
                                 glyph.textBlockId) != exclusion.blockIds.end();
            });
        if (inSideFloat) continue;
        const size_t fontIndex = std::min<size_t>((uint8_t)glyph.fontSlot, 15);
        const float tolerance = std::max(2.0f, glyph.fontPx * 0.25f);
        Row* match = nullptr;
        for (auto& row : rows) {
            if (std::abs(row.baseline - glyph.y) <= tolerance) {
                match = &row;
                break;
            }
        }
        if (match) {
            match->baseline = (match->baseline * match->count + glyph.y)
                            / (match->count + 1);
            match->fontPx = std::max(match->fontPx, glyph.fontPx);
            ++match->fontCounts[fontIndex];
            ++match->count;
        } else {
            Row row{};
            row.baseline = glyph.y;
            row.fontPx = glyph.fontPx;
            row.count = 1;
            row.fontCounts[fontIndex] = 1;
            rows.push_back(row);
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.baseline < b.baseline; });
    struct Rule {
        float y;
        float cellHeight;
    };
    struct GuideRow {
        float centreY;
        float fontPx;
        FontSlot fontSlot;
    };
    std::vector<GuideRow> guideRows;
    guideRows.reserve(rows.size());
    for (const auto& row : rows) {
        size_t dominant = 0;
        for (size_t slot = 1; slot < row.fontCounts.size(); ++slot) {
            if (row.fontCounts[slot] > row.fontCounts[dominant]) dominant = slot;
        }
        guideRows.push_back(GuideRow{row.baseline - row.fontPx * 0.35f,
                                     row.fontPx, (FontSlot)(uint8_t)dominant});
    }

    auto isRegularBodyRow = [&](const GuideRow& row) {
        const bool bodySlot = row.fontSlot == FontSlot::Regular
                           || row.fontSlot == FontSlot::Fallback;
        const float sizeTolerance = std::max(1.0f, (float)opts.fontSize * 0.08f);
        return bodySlot
            && std::abs(row.fontPx - (float)opts.fontSize) <= sizeTolerance;
    };
    auto sharesGridStep = [&](const GuideRow& upper, const GuideRow& lower) {
        if (!isRegularBodyRow(upper) || !isRegularBodyRow(lower)) return false;
        const float expected = (float)(opts.fontSize * opts.lineHeightMul);
        const float actual = lower.centreY - upper.centreY;
        const float tolerance = std::max(3.0f, expected * 0.16f);
        return std::abs(actual - expected) <= tolerance;
    };

    std::vector<Rule> rules;
    rules.reserve(guideRows.size() + 1);
    for (size_t start = 0; start < guideRows.size();) {
        if (!isRegularBodyRow(guideRows[start])) {
            ++start;
            continue;
        }
        size_t end = start;
        while (end + 1 < guideRows.size()
               && sharesGridStep(guideRows[end], guideRows[end + 1]))
            ++end;
        if (end > start) {
            if (start == 0) {
                const float step = guideRows[start + 1].centreY
                                 - guideRows[start].centreY;
                rules.push_back(Rule{guideRows[start].centreY - step * 0.5f, step});
            }
            for (size_t i = start; i < end; ++i) {
                const float step = guideRows[i + 1].centreY - guideRows[i].centreY;
                rules.push_back(Rule{(guideRows[i].centreY
                                    + guideRows[i + 1].centreY) * 0.5f, step});
            }
            if (end + 1 == guideRows.size()) {
                const float step = guideRows[end].centreY
                                 - guideRows[end - 1].centreY;
                rules.push_back(Rule{guideRows[end].centreY + step * 0.5f, step});
            }
        }
        start = end + 1;
    }

    int lastRuleY = -1000;
    for (const auto& rule : rules) {
        const int lineY = std::clamp((int)std::lround(rule.y), 0,
                                     std::max(0, contentBottom - 1));
        if (std::abs(lineY - lastRuleY) <= 1) continue;
        lastRuleY = lineY;
        const int inset = std::max(8, (int)std::lround(rule.cellHeight * 0.08f));
        const int x0 = std::max(0, opts.marginLeft + inset);
        int x1 = std::min(bitmap->width, contentRight - inset);
        for (const auto& exclusion : exclusions) {
            if ((float)lineY >= exclusion.y0 && (float)lineY <= exclusion.y1)
                x1 = std::min(x1, (int)std::floor(exclusion.x0));
        }
        const int dot = std::max(2, (int)std::lround(rule.cellHeight * 0.035f));
        const int gap = std::max(5, (int)std::lround(rule.cellHeight * 0.09f));
        paintRulingPattern(bitmap->pixels.data(), bitmap->width, contentBottom,
                          x0, x1, lineY, dot, gap, 219, false,
                          opts.rulingStyle == "dashed");
    }
}

ink_status_t runEpubMupdfPipeline(const std::filesystem::path& input,
                                  const std::filesystem::path& output,
                                  const Options& opts,
                                  const char* jobId,
                                  ink_progress_cb progressCb,
                                  void* progressUserdata,
                                  Logger* log) {
    const std::string pKey = g_session.fileKey + "|" + parseFingerprint(input, opts);
    const std::string lKey = pKey + "|" + layoutFingerprint(opts);
    MupdfEpubEngine* engine = nullptr;
    if (g_session.kind == SessionKind::EpubMupdf && g_session.epub &&
        g_session.parseKey == pKey && g_session.layoutKey == lKey) {
        engine = g_session.epub.get();
        log->log(LogLevel::Info, "pipeline", "mupdf epub layout cache hit");
        emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 100);
        emit(progressCb, progressUserdata, jobId, INK_STAGE_LAYOUT, 100);
    } else {
        emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 0);
        g_session.epub.reset();
        g_session.epub = std::make_unique<MupdfEpubEngine>();
        if (!g_session.epub->open(input, opts, log)) {
            g_session.epub.reset();
            return INK_ERR_PARSE;
        }
        engine = g_session.epub.get();
        g_session.kind = SessionKind::EpubMupdf;
        g_session.parseKey = pKey;
        g_session.layoutKey = lKey;
        g_session.text = {};
        g_session.layout = {};
#if HARDCOPY_HAS_COMIC
        g_session.comic = {};
#endif
        emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 100);
        emit(progressCb, progressUserdata, jobId, INK_STAGE_LAYOUT, 100);
    }
    if (jobCancelled(jobId)) {
        log->log(LogLevel::Info, "pipeline", "cancelled");
        return INK_ERR_CANCELLED;
    }

    uint8_t contrastLut[256];
    buildContrastLut(opts.contrastBoost, contrastLut);
    logContrastSelection(log, "mupdf-epub", opts.contrastBoost, contrastLut);
    uint8_t imageLut[256];
    buildContrastLut(opts.imageContrastBoost, imageLut);
    logContrastSelection(log, "mupdf-epub-img", opts.imageContrastBoost, imageLut);
    if (opts.syntheticBold)
        log->log(LogLevel::Warn, "pipeline",
                 "mupdf epub path: syntheticBold not supported, ignored");

    StreamingPdfWriter pdf(output, pdfSheetWidth(opts), opts.pageHeight, log);
    if (!pdf.open()) {
        log->log(LogLevel::Error, "pipeline", "pdf open failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    pdf.setMetadata(engine->title, engine->author);
    auto pageImages = PageImageDump::create(opts, output, input, log);
    if (pageImages) pdf.setFileId(pageImages->id());

    const int pageCount = engine->pageCount();
    auto mapEpubLinkTargets = [&](std::vector<PdfLink>* links) {
        if (!links) return;
        for (auto& link : *links) {
            if (link.targetPage0 < 0 || link.targetPage0 >= pageCount) continue;
            const int outputPage = outputPageIndex0(
                link.targetPage0, pageCount, opts.reversePages);
            link.targetPage0 = opts.splitLandscape ? outputPage / 2 : outputPage;
        }
    };
    SpreadSink sink = makeSpreadSink(pdf, opts);
    emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER, 0);

    FontFace footerRegular, footerFallback;
    FontFace* footerReg = nullptr;
    FontFace* footerFb  = nullptr;
    if (opts.footerProgress) {
        const bool regOk = !opts.fontData.empty()
            ? footerRegular.openFromMemory(opts.fontData.data(), opts.fontData.size())
            : footerRegular.open(opts.fontPath);
        const bool fbOk = !opts.fontDataFallback.empty()
            ? footerFallback.openFromMemory(opts.fontDataFallback.data(),
                                            opts.fontDataFallback.size())
            : (!opts.fontPathFallback.empty()
               && footerFallback.open(opts.fontPathFallback));
        footerReg = regOk ? &footerRegular : nullptr;
        footerFb  = fbOk ? &footerFallback : nullptr;
        if (!footerReg)
            log->log(LogLevel::Warn, "pipeline",
                     "footerProgress: no usable font; footer disabled");
    }

    std::atomic<int> noteOverflowPages{0};
#if HARDCOPY_HAS_PARALLEL
    struct RenderedPage {
        bool rendered = false;
        bool encoded = false;
        std::vector<uint8_t> jpeg;
        StreamingPdfWriter::ImageCodec codec = StreamingPdfWriter::ImageCodec::Jpeg;
        std::vector<PlacedGlyph> glyphs;
        std::vector<PdfLink> links;
    };
    struct PreparedPage {
        std::shared_ptr<MupdfEpubEngine::PageDisplayList> displayList;
        std::vector<PlacedGlyph> glyphs;
        std::vector<PdfLink> links;
        FooterStrip footerLeft, footerRight;
    };
    const int hardwareThreads = std::max(1, (int)std::thread::hardware_concurrency());
    const int requestedThreads = opts.threadCount > 0
                             ? opts.threadCount
                             : std::min(4, hardwareThreads);
    const int workers = std::min(std::max(1, requestedThreads),
                                 std::max(1, pageCount));
    const int maxInflight = std::max(1, workers * 2);
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "mupdf epub parallel render: workers=%d window=%d pages=%d",
                      workers, maxInflight, pageCount);
        log->log(LogLevel::Info, "pipeline", msg);
    }

    ThreadPool pool(workers);
    std::atomic<uint64_t> rasterMs{0}, rasterCpuMs{0};
    std::atomic<uint64_t> jpegMs{0}, jpegCpuMs{0};
    uint64_t prepareMs = 0;
    uint64_t pdfWriteMs = 0;
    int flatePages = 0, jpegPages = 0;
    log->log(LogLevel::Info, "pipeline", cpuProbe().c_str());
    const auto renderWallStart = std::chrono::steady_clock::now();
    std::vector<std::future<RenderedPage>> jobs((size_t)pageCount);
    auto submitPage = [&](int pageIndex0) {
        auto prepared = std::make_shared<PreparedPage>();
        const auto prepareStart = std::chrono::steady_clock::now();
        prepared->displayList = engine->preparePage(
            pageIndex0,
            (opts.embedTextLayer || opts.drawRuling) ? &prepared->glyphs : nullptr);
        engine->links(pageIndex0, &prepared->links);
        if (footerReg) {
            const double fp = footerFontPx(opts);
            char left[64];
            std::snprintf(left, sizeof(left), "第 %d 页", pageIndex0 + 1);
            char right[16];
            std::snprintf(right, sizeof(right), "%d%%",
                (int)std::lround((pageIndex0 + 1) * 100.0 / std::max(1, pageCount)));
            prepared->footerLeft = renderFooterStrip(
                left, fp, opts.syntheticBold, footerReg, footerFb);
            prepared->footerRight = renderFooterStrip(
                right, fp, opts.syntheticBold, footerReg, footerFb);
        }
        prepareMs += (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prepareStart).count();
        jobs[(size_t)pageIndex0] = pool.submit([&, prepared, pageIndex0] {
            RenderedPage page;
            if (!prepared->displayList) return page;
            thread_local std::unique_ptr<MupdfEpubEngine::PageRenderer> renderer;
            if (!renderer) renderer = engine->createPageRenderer();

            Bitmap8 bitmap;
            std::vector<ImageRect> imageRects;
            const auto rasterStart = std::chrono::steady_clock::now();
            const uint64_t rasterCpuStart = threadCpuMs();
            if (!renderer || !renderer->renderPage(*prepared->displayList, &bitmap,
                                                   &imageRects))
                return page;
            rasterMs.fetch_add((uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() - rasterStart).count(),
                std::memory_order_relaxed);
            rasterCpuMs.fetch_add(threadCpuMs() - rasterCpuStart, std::memory_order_relaxed);
            page.rendered = true;
            page.glyphs = std::move(prepared->glyphs);
            page.links = std::move(prepared->links);
            if (noteMupdfCrossesDivider(page.glyphs, imageRects, opts,
                                        bitmap.width, bitmap.height))
                noteOverflowPages.fetch_add(1, std::memory_order_relaxed);
            drawMupdfRuling(&bitmap, page.glyphs, opts);
            if (footerReg)
                drawFooterStrips(bitmap, opts, prepared->footerLeft,
                                 prepared->footerRight);
            applyContrastWithImageMask(bitmap.pixels.data(), bitmap.width,
                                       bitmap.height, contrastLut, imageLut,
                                       imageRects);
            if (opts.strokeExpand > 0.0)
                applyStrokeExpand(bitmap, opts.strokeExpand, false);
            applyNightInvert(bitmap.pixels.data(), bitmap.width, bitmap.height,
                             opts.whiteText);
            if (pageImages)
                pageImages->save(outputPageIndex0(pageIndex0, pageCount, opts.reversePages) + 1,
                                 bitmap);
            const auto jpegStart = std::chrono::steady_clock::now();
            const uint64_t jpegCpuStart = threadCpuMs();
            page.encoded = encodePageImage(bitmap,
                                           imageAreaFraction(imageRects, bitmap.width, bitmap.height),
                                           opts,
                                           &page.jpeg, &page.codec);
            jpegMs.fetch_add((uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() - jpegStart).count(),
                std::memory_order_relaxed);
            jpegCpuMs.fetch_add(threadCpuMs() - jpegCpuStart, std::memory_order_relaxed);
            return page;
        });
    };

    int nextOut = 0;
    auto submitOut = [&](int outIdx) {
        submitPage(outputPageIndex0(outIdx, pageCount, opts.reversePages));
    };
    while (nextOut < pageCount && nextOut < maxInflight) submitOut(nextOut++);
    for (int out = 0; out < pageCount; ++out) {
        if (jobCancelled(jobId)) {
            if (log) log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        const int i = outputPageIndex0(out, pageCount, opts.reversePages);
        RenderedPage page = jobs[(size_t)i].get();
        if (page.codec == StreamingPdfWriter::ImageCodec::FlateGray4) ++flatePages; else ++jpegPages;
        if (!page.rendered) {
            log->log(LogLevel::Error, "pipeline", "mupdf render failed");
            return INK_ERR_RENDER;
        }
        if (!page.encoded || page.jpeg.empty()) {
            char emsg[128];
            std::snprintf(emsg, sizeof(emsg),
                          "jpeg encode failed at page %d/%d (likely OOM)",
                          i + 1, pageCount);
            log->log(LogLevel::Error, "pipeline", emsg);
            return INK_ERR_ENCODE;
        }
        const auto pdfWriteStart = std::chrono::steady_clock::now();
        mapEpubLinkTargets(&page.links);
        if (!sink.push(std::move(page.jpeg), std::move(page.glyphs), std::move(page.links),
                       page.codec)) {
            log->log(LogLevel::Error, "pipeline", "pdf write failed");
            return INK_ERR_WRITE_OUTPUT;
        }
        pdfWriteMs += (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pdfWriteStart).count();
        emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER,
             100 * (out + 1) / std::max(1, pageCount));
        if (nextOut < pageCount) submitOut(nextOut++);
    }
    {
        const uint64_t wallMs = (uint64_t)std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - renderWallStart).count();
        char msg[384];
        std::snprintf(msg, sizeof(msg),
                      "mupdf epub render timing: wall=%llums prepare+text=%llums "
                      "raster=%llums (cpu %llums) encode=%llums (cpu %llums) pdf-write=%llums "
                      "pages flate=%d jpeg=%d flate-backend=%s",
                      (unsigned long long)wallMs,
                      (unsigned long long)prepareMs,
                      (unsigned long long)rasterMs.load(std::memory_order_relaxed),
                      (unsigned long long)rasterCpuMs.load(std::memory_order_relaxed),
                      (unsigned long long)jpegMs.load(std::memory_order_relaxed),
                      (unsigned long long)jpegCpuMs.load(std::memory_order_relaxed),
                      (unsigned long long)pdfWriteMs,
                      flatePages, jpegPages, flateBackendName());
        log->log(LogLevel::Info, "pipeline", msg);
        log->log(LogLevel::Info, "pipeline", cpuProbe().c_str());
    }
#else
    for (int out = 0; out < pageCount; ++out) {
        if (jobCancelled(jobId)) {
            if (log) log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        const int i = outputPageIndex0(out, pageCount, opts.reversePages);
        Bitmap8 bm;
        std::vector<ImageRect> imageRects;
        if (!engine->renderPage(i, &bm, &imageRects)) {
            log->log(LogLevel::Error, "pipeline", "mupdf render failed");
            return INK_ERR_RENDER;
        }
        std::vector<PlacedGlyph> glyphs;
        std::vector<PdfLink> links;
        if (opts.embedTextLayer || opts.drawRuling) engine->textLayer(i, &glyphs);
        engine->links(i, &links);
        if (noteMupdfCrossesDivider(glyphs, imageRects, opts, bm.width, bm.height))
            noteOverflowPages.fetch_add(1, std::memory_order_relaxed);
        drawMupdfRuling(&bm, glyphs, opts);
        if (footerReg)
            drawFooterProgress(bm, opts, footerReg, footerFb, i + 1, pageCount);
        applyContrastWithImageMask(bm.pixels.data(), bm.width, bm.height,
                                   contrastLut, imageLut, imageRects);
        if (opts.strokeExpand > 0.0)
            applyStrokeExpand(bm, opts.strokeExpand, false);
        applyNightInvert(bm.pixels.data(), bm.width, bm.height, opts.whiteText);
        if (pageImages) pageImages->save(out + 1, bm);
        std::vector<uint8_t> jpeg;
        StreamingPdfWriter::ImageCodec codec;
        if (!encodePageImage(bm, imageAreaFraction(imageRects, bm.width, bm.height),
                             opts, &jpeg, &codec)) {
            char emsg[128];
            std::snprintf(emsg, sizeof(emsg),
                          "page encode failed at page %d/%d (likely OOM)",
                          i + 1, pageCount);
            log->log(LogLevel::Error, "pipeline", emsg);
            return INK_ERR_ENCODE;
        }
        mapEpubLinkTargets(&links);
        if (!sink.push(std::move(jpeg), std::move(glyphs), std::move(links), codec)) {
            log->log(LogLevel::Error, "pipeline", "pdf write failed");
            return INK_ERR_WRITE_OUTPUT;
        }
        emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER,
             100 * (out + 1) / std::max(1, pageCount));
    }
#endif
    if (const int n = noteOverflowPages.load(std::memory_order_relaxed)) {
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "note divider crossed by an unbreakable block on %d page(s)", n);
        log->log(LogLevel::Warn, "pipeline", msg);
    }

    if (!sink.finish()) {
        log->log(LogLevel::Error, "pipeline", "pdf write failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    if (opts.embedBookmarks) {
        for (const auto& a : engine->outline()) {
            pdf.addOutline(a.title, a.level,
                           outlinePageIndex(opts, a.pageIndex0, pageCount));
        }
    }

    emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE, 50);
    if (!pdf.close()) {
        log->log(LogLevel::Error, "pipeline", "pdf close failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    if (pageImages) pageImages->finish(pdf, opts.splitLandscape ? 2 : 1);
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE, 100);
    rememberOutput(output, fileIdentity(input, opts) + "|" + optionsFingerprint(opts));
    emit(progressCb, progressUserdata, jobId, INK_STAGE_DONE, 100);
    return INK_OK;
}

#endif

#if HARDCOPY_HAS_TEXT_MODE
void checkNotePagesAgainstDivider(const LayoutResult& layout, const Options& opts,
                                  Logger* log) {
    if (opts.preset != "note" || !log) return;
    const bool portrait = opts.pageHeight >= opts.pageWidth;
    const float divider = (float)noteDividerPosition(
        portrait ? opts.pageHeight : opts.pageWidth);
    int crossed = 0, first = -1;
    for (size_t i = 0; i < layout.pages.size(); ++i) {
        const auto& page = layout.pages[i];
        bool over = false;
        for (const auto& g : page.glyphs) {
            if ((portrait ? g.y : g.x) > divider) { over = true; break; }
        }
        for (const auto& im : page.images) {
            if ((portrait ? im.y + im.h : im.x + im.w) > divider + 1.0f) over = true;
        }
        if (over) {
            ++crossed;
            if (first < 0) first = (int)i + 1;
        }
    }
    if (crossed > 0) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "note divider crossed on %d page(s), first page %d", crossed, first);
        log->log(LogLevel::Warn, "pipeline", msg);
    }
}
#endif

#if HARDCOPY_HAS_COMIC && HARDCOPY_HAS_DITHER
bool comicPageIsBlank(const Bitmap8& bm) {
    if (bm.width <= 0 || bm.height <= 0) return false;
    uint32_t hist[256] = {};
    uint32_t n = 0;
    for (int y = 0; y < bm.height; y += 2) {
        const uint8_t* row = bm.pixels.data() + (size_t)y * bm.width;
        for (int x = 0; x < bm.width; x += 2) { ++hist[row[x]]; ++n; }
    }
    if (n == 0) return false;
    uint32_t acc = 0;
    int paper = 255;
    for (int v = 0; v < 256; ++v) {
        acc += hist[v];
        if (acc >= n - n / 10) { paper = v; break; }
    }
    if (paper < 128) return false;
    uint32_t ink = 0;
    for (int v = 0; v < paper - 64; ++v) ink += hist[v];
    return ink * 10000ull < n * 15ull;
}

void applyComicTone(Bitmap8* bm, int level) {
    if (!bm || level == 0) return;
    const double g = std::pow(2.0, level / 10.0);
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v)
        lut[v] = (uint8_t)std::lround(255.0 * std::pow(v / 255.0, g));
    for (auto& p : bm->pixels) p = lut[p];
}

void padComicTopBottom(Bitmap8* bm, int percent) {
    if (!bm || percent <= 0 || bm->width <= 0 || bm->height <= 0) return;
    const int pad = (int)std::lround(bm->height * percent / 100.0);
    if (pad <= 0) return;
    Bitmap8 out;
    out.width = bm->width;
    out.height = bm->height + 2 * pad;
    out.pixels.assign((size_t)out.width * out.height, 255);
    std::memcpy(out.pixels.data() + (size_t)pad * out.width, bm->pixels.data(), bm->pixels.size());
    *bm = std::move(out);
}

void blitComicPage(const Bitmap8& src, Bitmap8* dst, int x, int y) {
    for (int row = 0; row < src.height; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= dst->height) continue;
        std::memcpy(dst->pixels.data() + (size_t)dy * dst->width + x,
                    src.pixels.data() + (size_t)row * src.width, src.width);
    }
}

Bitmap8 joinComicSpread(const Bitmap8& left, const Bitmap8& right) {
    Bitmap8 out;
    out.width = left.width + right.width;
    out.height = std::max(left.height, right.height);
    out.pixels.assign((size_t)out.width * out.height, 255);
    blitComicPage(left, &out, 0, (out.height - left.height) / 2);
    blitComicPage(right, &out, left.width, (out.height - right.height) / 2);
    return out;
}

Bitmap8 centreComicPage(const Bitmap8& page) {
    Bitmap8 out;
    out.width = page.width * 2;
    out.height = page.height;
    out.pixels.assign((size_t)out.width * out.height, 255);
    blitComicPage(page, &out, page.width / 2, 0);
    return out;
}
#endif

#if HARDCOPY_HAS_COMIC
bool isComicExtension(const std::string& ext) {
    return ext == ".cbz" || ext == ".zip" || ext == ".mobi" || ext == ".prc";
}

bool parseComicInput(const std::filesystem::path& in, ComicDocument* doc, Logger* log) {
    std::string ext = lowerExt(in);
    if (ext == ".cbz" || ext == ".zip")
        return parseCbz(in, doc, log);
    if (ext == ".mobi" || ext == ".prc")
        return parseMobi(in, doc, log);
#if HARDCOPY_HAS_EPUB
    if (ext == ".epub")
        return parseEpubComic(in, doc, log);
#endif
#if HARDCOPY_HAS_PDFIN
    if (ext == ".pdf")
        return parsePdfComic(in, doc, log);
#endif
    if (log) log->log(LogLevel::Error, "pipeline", "unsupported comic format");
    return false;
}
#endif

#if HARDCOPY_HAS_TEXT_MODE && HARDCOPY_HAS_PDFIN
bool loadPdfOcrForParse(const std::filesystem::path& input, const Options& useOpts,
                        const char* jobId, ink_progress_cb progressCb, void* progressUserdata,
                        Logger* log, PdfOcrText* out, std::string* ocrKey) {
    if (lowerExt(input) != ".pdf") return true;
    PdfOcrRequest request;
    request.compute = useOpts.pdfOcr;
    request.ignoreText = useOpts.pdfIgnoreText;
    request.identity = pdfSourceIdentity(input, useOpts.sourceIdentity);
    request.cacheDir = pdfOcrCacheDir(useOpts.ocrCacheDir);
    const int hardware = std::max(1, (int)std::thread::hardware_concurrency());
    request.threads = useOpts.threadCount > 0 ? useOpts.threadCount : std::min(4, hardware);
    request.jobId = jobId;
    request.progress = [&](int done, int total) {
        emit(progressCb, progressUserdata, jobId, INK_STAGE_OCR, total > 0 ? 100 * done / total : 100);
    };
    if (request.compute || pdfOcrCacheExists(request.cacheDir, request.identity)) {
        if (!preparePdfOcr(input, request, log, out)) return false;
        *ocrKey = "|ocr" + std::to_string(out->size());
    }
    return true;
}
#endif

}

#if HARDCOPY_HAS_COMIC
ink_status_t runComicPipeline(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              const Options& opts,
                              const char* jobId,
                              ink_progress_cb progressCb,
                              void* progressUserdata,
                              Logger* log) {
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 0);
    const std::string pKey = g_session.fileKey + "|" + parseFingerprint(input, opts);
    ComicDocument comic;
    if (g_session.kind == SessionKind::Comic && g_session.parseKey == pKey &&
        !g_session.comic.pages.empty()) {
        comic = g_session.comic;
        log->log(LogLevel::Info, "pipeline", "comic parse cache hit");
    } else {
        if (!parseComicInput(input, &comic, log)) {
            log->log(LogLevel::Error, "pipeline", "comic parse failed");
            return INK_ERR_PARSE;
        }
        g_session.kind = SessionKind::Comic;
        g_session.parseKey = pKey;
        g_session.layoutKey.clear();
        g_session.comic = comic;
#if HARDCOPY_HAS_TEXT_MODE
        g_session.text = {};
        g_session.layout = {};
#endif
#if HARDCOPY_HAS_TEXT_MODE && HARDCOPY_HAS_PDFIN
        g_session.epub.reset();
#endif
    }
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "comic: %zu pages", comic.pages.size());
        log->log(LogLevel::Info, "pipeline", buf);
    }
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 100);
    if (jobCancelled(jobId)) {
        log->log(LogLevel::Info, "pipeline", "cancelled");
        return INK_ERR_CANCELLED;
    }

    emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE, 0);

    ZipWriter cbz;
    if (!cbz.open(output)) {
        log->log(LogLevel::Error, "pipeline", "failed to create output CBZ");
        return INK_ERR_WRITE_OUTPUT;
    }

    const int pageCount = (int)comic.pages.size();
    int written = 0;
    int skippedBlank = 0;

#if HARDCOPY_HAS_DITHER
    const bool layoutPages = opts.comicMarginPercent > 0 || opts.comicSpread != 0;
    const bool reencode = layoutPages || opts.ditherAlgo > 0 || opts.comicTone != 0;

    auto writeSheet = [&](Bitmap8 sheet) -> bool {
        padComicTopBottom(&sheet, opts.comicMarginPercent);
        if (opts.comicResizeW > 0 && opts.comicResizeH > 0) {
            int tw = opts.comicResizeW, th = opts.comicResizeH;
            if (sheet.width > sheet.height && tw < th) std::swap(tw, th);
            Bitmap8 scaled;
            scaleToFit(sheet, tw, th, &scaled);
            sheet = std::move(scaled);
        }
        if (opts.ditherAlgo == 1)
            ditherBayer(&sheet, opts.ditherLevels);
        else if (opts.ditherAlgo == 2)
            ditherFloydSteinberg(&sheet, opts.ditherLevels);
        std::vector<uint8_t> jpeg;
        if (!encodeJpegGray(sheet, opts.jpegQuality, &jpeg)) {
            log->log(LogLevel::Error, "pipeline", "comic jpeg encode failed");
            return false;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%04d.jpg", written + 1);
        if (!cbz.addEntry(name, jpeg.data(), jpeg.size())) {
            log->log(LogLevel::Error, "pipeline", "failed to write processed comic page");
            return false;
        }
        ++written;
        return true;
    };

    bool havePending = false;
    Bitmap8 pending;
    int placedPages = 0;
    int spreads = 0;
    auto flushPending = [&]() -> bool {
        if (!havePending) return true;
        havePending = false;
        Bitmap8 lone = centreComicPage(pending);
        pending = {};
        return writeSheet(std::move(lone));
    };
    auto placePage = [&](Bitmap8 page) -> bool {
        const bool first = placedPages++ == 0;
        if (opts.comicSpread == 0) return writeSheet(std::move(page));
        if (first || page.width > page.height) {
            if (!flushPending()) return false;
            if (page.width > page.height) return writeSheet(std::move(page));
            return writeSheet(centreComicPage(page));
        }
        if (!havePending) {
            pending = std::move(page);
            havePending = true;
            return true;
        }
        havePending = false;
        Bitmap8 sheet = opts.comicSpread == 2 ? joinComicSpread(page, pending)
                                              : joinComicSpread(pending, page);
        pending = {};
        ++spreads;
        return writeSheet(std::move(sheet));
    };
#endif

    for (int i = 0; i < pageCount; ++i) {
        if (jobCancelled(jobId)) {
            if (log) log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        const auto& d = comic.pages[i].data;

#if HARDCOPY_HAS_DITHER
        const bool checkBlank = opts.comicSkipBlank
                             && !(i == pageCount - 1 && written == 0 && !havePending);
        const bool webp = isWebpImage(d.data(), d.size());
        if (reencode || checkBlank || webp) {
            Bitmap8 bm;
            if (!decodeToGrayOnWhite(d.data(), d.size(), &bm)) {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "decode failed: %s",
                              comic.pages[i].filename.c_str());
                log->log(LogLevel::Warn, "pipeline", msg);
                if (!flushPending()) return INK_ERR_WRITE_OUTPUT;
            } else if (checkBlank && comicPageIsBlank(bm)) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "comic: skipped blank page %d (%s)",
                              i + 1, comic.pages[i].filename.c_str());
                log->log(LogLevel::Info, "pipeline", msg);
                ++skippedBlank;
                comic.pages[i].data.clear();
                comic.pages[i].data.shrink_to_fit();
                emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE,
                     100 * (i + 1) / std::max(1, pageCount));
                continue;
            } else if (reencode || webp) {
                applyComicTone(&bm, opts.comicTone);
                if (!placePage(std::move(bm))) return INK_ERR_WRITE_OUTPUT;
                comic.pages[i].data.clear();
                comic.pages[i].data.shrink_to_fit();
                emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE,
                     100 * (i + 1) / std::max(1, pageCount));
                continue;
            }
        }
#endif

        const char* ext = ".jpg";
        if (d.size() >= 4 && d[0] == 0x89 && d[1] == 'P')
            ext = ".png";
        else if (d.size() >= 3 && d[0] == 'G' && d[1] == 'I')
            ext = ".gif";
        else if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M')
            ext = ".bmp";
        else if (d.size() >= 12 && d[0] == 'R' && d[1] == 'I' &&
                 d[2] == 'F' && d[3] == 'F' && d[8] == 'W' &&
                 d[9] == 'E' && d[10] == 'B' && d[11] == 'P')
            ext = ".webp";

        char name[32];
        std::snprintf(name, sizeof(name), "%04d%s", written + 1, ext);

        if (!cbz.addEntry(name, d.data(), d.size())) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "failed to write page %d/%d",
                          i + 1, pageCount);
            log->log(LogLevel::Error, "pipeline", msg);
            return INK_ERR_WRITE_OUTPUT;
        }
        ++written;

        comic.pages[i].data.clear();
        comic.pages[i].data.shrink_to_fit();

        emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE,
             100 * (i + 1) / std::max(1, pageCount));
    }
#if HARDCOPY_HAS_DITHER
    if (!flushPending()) return INK_ERR_WRITE_OUTPUT;
    if (layoutPages) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "comic layout: margin=%d%% spread=%d images=%d spreads=%d",
                      opts.comicMarginPercent, opts.comicSpread, written, spreads);
        log->log(LogLevel::Info, "pipeline", msg);
    }
#endif

    if (skippedBlank > 0) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "comic: %d page(s) written, %d blank skipped",
                      written, skippedBlank);
        log->log(LogLevel::Info, "pipeline", msg);
    }
    if (!cbz.close()) {
        log->log(LogLevel::Error, "pipeline", "cbz close failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    rememberOutput(output, fileIdentity(input, opts) + "|" + optionsFingerprint(opts));
    emit(progressCb, progressUserdata, jobId, INK_STAGE_DONE, 100);
    return INK_OK;
}
#endif

ink_status_t runPipeline(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const Options& opts,
                         const char* jobId,
                         ink_progress_cb progressCb,
                         void* progressUserdata,
                         Logger* log) {
    NullLogger nullLog;
    if (!log) log = &nullLog;

    const std::string fileKey = fileIdentity(input, opts);
    const std::string fullKey = fileKey + "|" + optionsFingerprint(opts);
    if (g_session.fullKey == fullKey && copyCachedOutput(output, log)) {
        emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 100);
        emit(progressCb, progressUserdata, jobId, INK_STAGE_LAYOUT, 100);
        emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER, 100);
        emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE, 100);
        emit(progressCb, progressUserdata, jobId, INK_STAGE_DONE, 100);
        return INK_OK;
    }
    if (g_session.fileKey != fileKey) resetSession();
    g_session.fileKey = fileKey;

#if HARDCOPY_HAS_COMIC
    {
        std::string ext = lowerExt(input);
        if (opts.comicMode || isComicExtension(ext)) {
            return runComicPipeline(input, output, opts, jobId,
                                   progressCb, progressUserdata, log);
        }
    }
#endif

#if HARDCOPY_HAS_TEXT_MODE
    Options effOpts = opts;
#if HARDCOPY_HAS_PDFIN
    if (effOpts.fontPathMono.empty() && effOpts.fontDataMono.empty()) {
        if (fz_context* fctx = fz_new_context(nullptr, nullptr, 0)) {
            int len = 0;
            const unsigned char* data = fz_lookup_base14_font(fctx, "Courier", &len);
            if (data && len > 0) effOpts.fontDataMono.assign(data, data + len);
            fz_drop_context(fctx);
        }
    }
#endif
    if (isBilingualJson(input)) {
        effOpts.bilingual = true;
        effOpts.orientation = Orientation::HorizontalLtr;
        if (effOpts.splitLandscape && effOpts.bilingualSideBySide()) {
            effOpts.splitLandscape = false;
            log->log(LogLevel::Info, "pipeline", "bilingual: splitLandscape ignored");
        }
    }
    if (effOpts.drawRuling && effOpts.orientation == Orientation::HorizontalLtr)
        effOpts.paragraphSpacing = 0.0;
    applyBookSpread(&effOpts, log);
    const Options& useOpts = effOpts;

#if HARDCOPY_HAS_PDFIN
    if (lowerExt(input) == ".pdf"
        && useOpts.pdfMode == PdfMode::Raster) {
        const auto status = runPdfRasterReflow(input, output, useOpts, jobId,
                                    progressCb, progressUserdata, log);
        if (status == INK_OK) rememberOutput(output, fullKey);
        return status;
    }

    if (lowerExt(input) == ".epub"
        && useOpts.epubEngine != EpubEngine::Legacy
        && useOpts.orientation == Orientation::HorizontalLtr
        && useOpts.zhConvert == ZhConvert::None) {
        ink_status_t st = runEpubMupdfPipeline(input, output, useOpts, jobId,
                                               progressCb, progressUserdata,
                                               log);
        if (st != INK_ERR_PARSE) return st;
        log->log(LogLevel::Warn, "pipeline",
                 "mupdf epub engine failed; falling back to legacy parser");
    }
#endif

    if (opts.fontPath.empty() && opts.fontData.empty()) {
        log->log(LogLevel::Error, "pipeline", "options.fontPath or fontData is required");
        return INK_ERR_INVALID_OPTIONS;
    }

    std::string ocrKey;
#if HARDCOPY_HAS_PDFIN
    PdfOcrText pdfOcrText;
    if (!loadPdfOcrForParse(input, useOpts, jobId, progressCb, progressUserdata, log,
                            &pdfOcrText, &ocrKey))
        return INK_ERR_CANCELLED;
    const void* pdfOcrForParse = &pdfOcrText;
#else
    const void* pdfOcrForParse = nullptr;
#endif

    const std::string pKey = g_session.fileKey + "|" + parseFingerprint(input, useOpts) + ocrKey;
    if (!(g_session.kind == SessionKind::Text && g_session.parseKey == pKey &&
          !g_session.text.blocks.empty())) {
        emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 0);
        Document parsed;
        const bool parsedOk = parseInput(input, &parsed, useOpts, log, pdfOcrForParse);
        if (jobCancelled(jobId)) {
            log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        if (!parsedOk) {
            log->log(LogLevel::Error, "pipeline", "parse failed");
            return INK_ERR_PARSE;
        }
        g_session.kind = SessionKind::Text;
        g_session.parseKey = pKey;
        g_session.layoutKey.clear();
        g_session.text = std::move(parsed);
        g_session.layout = {};
#if HARDCOPY_HAS_COMIC
        g_session.comic = {};
#endif
#if HARDCOPY_HAS_PDFIN
        g_session.epub.reset();
#endif
    } else {
        log->log(LogLevel::Info, "pipeline", "text parse cache hit");
    }
    Document& doc = g_session.text;
    if (doc.blocks.empty()) {
        log->log(LogLevel::Warn, "pipeline", "document has no blocks");
    }

    if (doc.hasCssPageMargin) {
        auto addCap = [&](int* margin, float em, int pageDim) {
            int add = (int)(em * effOpts.fontSize);
            int cap = pageDim * 15 / 100;
            *margin += std::min(add, cap);
        };
        addCap(&effOpts.marginTop,    doc.cssPageMarginEm[0], effOpts.pageHeight);
        addCap(&effOpts.marginRight,  doc.cssPageMarginEm[1], effOpts.pageWidth);
        addCap(&effOpts.marginBottom, doc.cssPageMarginEm[2], effOpts.pageHeight);
        addCap(&effOpts.marginLeft,   doc.cssPageMarginEm[3], effOpts.pageWidth);
        log->log(LogLevel::Info, "pipeline", "applied CSS body page margins");
    }
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 100);

    const LoadedFontBytes fontBytes = loadFontBytes(useOpts, log);
    std::vector<FontBytes> embeddedBytes;
    embeddedBytes.reserve(doc.embeddedFonts.size());
    for (const auto& ef : doc.embeddedFonts) embeddedBytes.push_back(wrapFontData(ef.bytes));

    FontFace regular, bold, mono, sans, kai;
    if (!fontBytes.regular || !regular.openFromMemory(fontBytes.regular)) {
        log->log(LogLevel::Error, "pipeline", "failed to open regular font");
        return INK_ERR_RENDER;
    }
    if (fontBytes.bold) bold.openFromMemory(fontBytes.bold);
    if (fontBytes.mono) mono.openFromMemory(fontBytes.mono);
    if (fontBytes.sans) sans.openFromMemory(fontBytes.sans);
    if (fontBytes.kai)  kai.openFromMemory(fontBytes.kai);

    FontFace fallbackFont;
    if (fontBytes.fallback) fallbackFont.openFromMemory(fontBytes.fallback);
    FontFace* fb = fallbackFont.isOpen() ? &fallbackFont : nullptr;

    std::vector<std::unique_ptr<FontFace>> embeddedFaces;
    embeddedFaces.reserve(doc.embeddedFonts.size());
    FontSet fonts;
    fonts.regular = &regular;
    fonts.bold = bold.isOpen() ? &bold : nullptr;
    fonts.mono = mono.isOpen() ? &mono : nullptr;
    fonts.fallback = fb;
    fonts.sans = sans.isOpen() ? &sans : nullptr;
    fonts.kai = kai.isOpen() ? &kai : nullptr;
    for (size_t i = 0; i < doc.embeddedFonts.size()
                    && i < (size_t)kMaxEmbeddedFonts; ++i) {
        auto face = std::make_unique<FontFace>();
        const auto& src = doc.embeddedFonts[i];
        if (!embeddedBytes[i] || !face->openFromMemory(embeddedBytes[i])) {
            log->log(LogLevel::Warn, "pipeline",
                     ("cannot open embedded font: " + src.sourcePath).c_str());
            embeddedFaces.push_back(std::move(face));
            continue;
        }
        FontSlot slot = embeddedSlot((int)i);
        fonts.embedded[i] = face.get();
        for (const auto& key : src.familyKeys) {
            auto it = std::find_if(fonts.families.begin(), fonts.families.end(),
                [&](const FamilyFontSlots& f) { return f.key == key; });
            if (it == fonts.families.end()) {
                FamilyFontSlots f;
                f.key = key;
                f.normal = slot;
                f.bold = slot;
                fonts.families.push_back(std::move(f));
            } else if (src.italic) {
            } else if (src.bold) {
                it->bold = slot;
            } else {
                it->normal = slot;
            }
        }
        embeddedFaces.push_back(std::move(face));
    }

    const std::string lKey = pKey + "|" + layoutFingerprint(useOpts);
    if (!(g_session.layoutKey == lKey && !g_session.layout.pages.empty())) {
        emit(progressCb, progressUserdata, jobId, INK_STAGE_LAYOUT, 0);
        LayoutResult built;
        if (useOpts.orientation == Orientation::VerticalRtl) {
#if HARDCOPY_HAS_VERTICAL_LAYOUT
            VerticalLayout vl(useOpts, fonts, log);
            built = vl.run(doc);
#else
            HorizontalLayout hl(useOpts, fonts, log);
            built = hl.run(doc);
#endif
        } else {
            HorizontalLayout hl(useOpts, fonts, log);
            built = hl.run(doc);
        }
        if (jobCancelled(jobId)) {
            log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        checkNotePagesAgainstDivider(built, useOpts, log);
        g_session.layout = std::move(built);
        g_session.layoutKey = lKey;
    } else {
        log->log(LogLevel::Info, "pipeline", "text layout cache hit");
    }
    LayoutResult& layout = g_session.layout;
    emit(progressCb, progressUserdata, jobId, INK_STAGE_LAYOUT, 100);
    if (layout.pages.empty()) {
        log->log(LogLevel::Warn, "pipeline", "layout produced no pages");
    }
    {
        size_t totalGlyphs = 0, totalLines = 0, totalSpans = 0;
        for (const auto& p : layout.pages) {
            totalGlyphs += p.glyphs.size();
            totalLines  += p.lines.size();
            totalSpans  += p.textSpans.size();
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "layout done: blocks=%zu pages=%zu glyphs=%zu lines=%zu spans=%zu",
            doc.blocks.size(), layout.pages.size(),
            totalGlyphs, totalLines, totalSpans);
        log->log(LogLevel::Info, "pipeline", buf);
    }

    emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER, 0);
#if !HARDCOPY_HAS_PARALLEL
    PageRaster raster(useOpts, fonts, log);
#endif

    StreamingPdfWriter pdf(output, pdfSheetWidth(useOpts), useOpts.pageHeight, log);
    if (!pdf.open()) {
        log->log(LogLevel::Error, "pipeline", "pdf open failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    pdf.setMetadata(doc.title, doc.author);
    auto pageImages = PageImageDump::create(useOpts, output, input, log);
    if (pageImages) pdf.setFileId(pageImages->id());

    const int pageCount = (int)layout.pages.size();
    SpreadSink sink = makeSpreadSink(pdf, useOpts);

#if HARDCOPY_HAS_PARALLEL
    const int threads = renderWorkers(useOpts, std::max(1, pageCount));
    const int maxInflight = std::max(1, threads * 2);
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "legacy parallel render: workers=%d window=%d pages=%d",
                      threads, maxInflight, pageCount);
        log->log(LogLevel::Info, "pipeline", msg);
    }
    struct FontBundle {
        FontFace regular, bold, mono, sans, kai, fallback;
        std::vector<std::unique_ptr<FontFace>> embedded;
        FontSet set;
    };
    auto openShared = [](FontFace* face, const FontBytes& bytes) {
        return bytes && face->openFromMemory(bytes);
    };
    std::vector<std::unique_ptr<FontBundle>> bundles;
    bundles.reserve((size_t)threads);
    for (int i = 0; i < threads; ++i) {
        auto b = std::make_unique<FontBundle>();
        if (!openShared(&b->regular, fontBytes.regular)) {
            log->log(LogLevel::Error, "pipeline", "worker font open failed");
            return INK_ERR_RENDER;
        }
        openShared(&b->bold, fontBytes.bold);
        openShared(&b->mono, fontBytes.mono);
        openShared(&b->sans, fontBytes.sans);
        openShared(&b->kai, fontBytes.kai);
        openShared(&b->fallback, fontBytes.fallback);
        b->set.regular = &b->regular;
        b->set.bold = b->bold.isOpen() ? &b->bold : nullptr;
        b->set.mono = b->mono.isOpen() ? &b->mono : nullptr;
        b->set.sans = b->sans.isOpen() ? &b->sans : nullptr;
        b->set.kai = b->kai.isOpen() ? &b->kai : nullptr;
        b->set.fallback = b->fallback.isOpen() ? &b->fallback : nullptr;
        b->set.families = fonts.families;
        for (size_t n = 0; n < doc.embeddedFonts.size()
                        && n < (size_t)kMaxEmbeddedFonts; ++n) {
            auto ef = std::make_unique<FontFace>();
            if (embeddedBytes[n] && ef->openFromMemory(embeddedBytes[n]))
                b->set.embedded[n] = ef.get();
            b->embedded.push_back(std::move(ef));
        }
        bundles.push_back(std::move(b));
    }
    std::mutex bmu;
    std::condition_variable bcv;
    std::queue<FontBundle*> freeBundles;
    for (auto& b : bundles) freeBundles.push(b.get());

    struct EncodedPage {
        bool ok = false;
        std::vector<uint8_t> jpeg;
        StreamingPdfWriter::ImageCodec codec = StreamingPdfWriter::ImageCodec::Jpeg;
    };
    ThreadPool pool(threads);
    std::atomic<uint64_t> rasterMs{0}, rasterCpuMs{0};
    std::atomic<uint64_t> encodeMs{0}, encodeCpuMs{0};
    std::atomic<uint64_t> stClear{0}, stDecor{0}, stGlyphs{0}, stImages{0}, stTone{0}, stStroke{0};
    uint64_t pdfWriteMs = 0;
    int flatePages = 0, jpegPages = 0;
    log->log(LogLevel::Info, "pipeline", cpuProbe().c_str());
    const auto renderWallStart = std::chrono::steady_clock::now();
    std::vector<std::future<EncodedPage>> jobs((size_t)std::max(0, pageCount));
    auto submitPage = [&](int i) {
        jobs[(size_t)i] = pool.submit([&, i] {
            EncodedPage page;
            FontBundle* b = nullptr;
            {
                std::unique_lock<std::mutex> lk(bmu);
                bcv.wait(lk, [&]{ return !freeBundles.empty(); });
                b = freeBundles.front();
                freeBundles.pop();
            }
            PageRaster lraster(useOpts, b->set, nullptr);
            RasterStageUs stages;
            lraster.setStageStats(&stages);
            Bitmap8 bm;
            const auto rasterStart = std::chrono::steady_clock::now();
            const uint64_t cpuStart = threadCpuMs();
            const bool rendered = lraster.render(layout.pages[(size_t)i], &bm,
                                                 &doc.images, pageCount);
            stClear.fetch_add(stages.clear, std::memory_order_relaxed);
            stDecor.fetch_add(stages.decor, std::memory_order_relaxed);
            stGlyphs.fetch_add(stages.glyphs, std::memory_order_relaxed);
            stImages.fetch_add(stages.images, std::memory_order_relaxed);
            stTone.fetch_add(stages.tone, std::memory_order_relaxed);
            stStroke.fetch_add(stages.stroke, std::memory_order_relaxed);
            const auto encodeStart = std::chrono::steady_clock::now();
            const uint64_t cpuMid = threadCpuMs();
            if (rendered)
                page.ok = encodePageImage(
                    bm, imageAreaFraction(layout.pages[(size_t)i], bm.width, bm.height),
                    useOpts, &page.jpeg, &page.codec);
            const auto encodeEnd = std::chrono::steady_clock::now();
            const uint64_t cpuEnd = threadCpuMs();
            rasterMs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                encodeStart - rasterStart).count(), std::memory_order_relaxed);
            encodeMs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                encodeEnd - encodeStart).count(), std::memory_order_relaxed);
            rasterCpuMs.fetch_add(cpuMid - cpuStart, std::memory_order_relaxed);
            encodeCpuMs.fetch_add(cpuEnd - cpuMid, std::memory_order_relaxed);
            if (rendered && pageImages)
                pageImages->save(outputPageIndex0(i, pageCount, useOpts.reversePages) + 1, bm);
            {
                std::lock_guard<std::mutex> lk(bmu);
                freeBundles.push(b);
            }
            bcv.notify_one();
            return page;
        });
    };
    int nextOut = 0;
    auto submitOut = [&](int outIdx) {
        submitPage(outputPageIndex0(outIdx, pageCount, useOpts.reversePages));
    };
    while (nextOut < pageCount && nextOut < maxInflight) submitOut(nextOut++);
    for (int out = 0; out < pageCount; ++out) {
        if (jobCancelled(jobId)) {
            if (log) log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        const int i = outputPageIndex0(out, pageCount, useOpts.reversePages);
        EncodedPage page = jobs[(size_t)i].get();
        if (!page.ok || page.jpeg.empty()) {
            char emsg[128];
            std::snprintf(emsg, sizeof(emsg),
                "jpeg encode failed at page %d/%d (likely OOM)",
                i + 1, pageCount);
            log->log(LogLevel::Error, "pipeline", emsg);
            return INK_ERR_ENCODE;
        }
        auto legacyLinks = legacyLinksForPage(
            doc, layout, i, useOpts.orientation == Orientation::VerticalRtl);
        if (page.codec == StreamingPdfWriter::ImageCodec::FlateGray4) ++flatePages; else ++jpegPages;
        const auto pdfWriteStart = std::chrono::steady_clock::now();
        if (!sink.push(std::move(page.jpeg), layout.pages[(size_t)i].glyphs,
                        std::move(legacyLinks), page.codec)) {
            log->log(LogLevel::Error, "pipeline", "pdf write failed");
            return INK_ERR_WRITE_OUTPUT;
        }
        pdfWriteMs += (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pdfWriteStart).count();
        emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER,
             100 * (out + 1) / std::max(1, pageCount));
        if (nextOut < pageCount) submitOut(nextOut++);
    }
    {
        const uint64_t wallMs = (uint64_t)std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - renderWallStart).count();
        size_t cacheEntries = 0, cacheBytes = 0;
        uint64_t ftRenders = 0, ftRenderUs = 0;
        for (const auto& b : bundles)
            for (const FontFace* f : {&b->regular, &b->bold, &b->mono, &b->sans,
                                      &b->kai, &b->fallback}) {
                size_t e = 0, by = 0;
                uint64_t r = 0, us = 0;
                f->glyphCacheStats(&e, &by, &r, &us);
                cacheEntries += e;
                cacheBytes += by;
                ftRenders += r;
                ftRenderUs += us;
            }
        char msg[640];
        std::snprintf(msg, sizeof(msg),
                      "legacy render timing: wall=%llums raster=%llums (cpu %llums) "
                      "encode=%llums (cpu %llums) pdf-write=%llums "
                      "pages flate=%d jpeg=%d flate-backend=%s "
                      "glyph-cache=%zu entries %.1fMB over %d workers "
                      "raster-stages: clear=%llums decor=%llums glyphs=%llums "
                      "(freetype %llu renders %llums) images=%llums tone=%llums stroke=%llums",
                      (unsigned long long)wallMs,
                      (unsigned long long)rasterMs.load(std::memory_order_relaxed),
                      (unsigned long long)rasterCpuMs.load(std::memory_order_relaxed),
                      (unsigned long long)encodeMs.load(std::memory_order_relaxed),
                      (unsigned long long)encodeCpuMs.load(std::memory_order_relaxed),
                      (unsigned long long)pdfWriteMs,
                      flatePages, jpegPages, flateBackendName(),
                      cacheEntries, cacheBytes / 1048576.0, threads,
                      (unsigned long long)(stClear.load(std::memory_order_relaxed) / 1000),
                      (unsigned long long)(stDecor.load(std::memory_order_relaxed) / 1000),
                      (unsigned long long)(stGlyphs.load(std::memory_order_relaxed) / 1000),
                      (unsigned long long)ftRenders,
                      (unsigned long long)(ftRenderUs / 1000),
                      (unsigned long long)(stImages.load(std::memory_order_relaxed) / 1000),
                      (unsigned long long)(stTone.load(std::memory_order_relaxed) / 1000),
                      (unsigned long long)(stStroke.load(std::memory_order_relaxed) / 1000));
        log->log(LogLevel::Info, "pipeline", msg);
        log->log(LogLevel::Info, "pipeline", cpuProbe().c_str());
    }
#else
    for (int out = 0; out < pageCount; ++out) {
        if (jobCancelled(jobId)) {
            if (log) log->log(LogLevel::Info, "pipeline", "cancelled");
            return INK_ERR_CANCELLED;
        }
        const int i = outputPageIndex0(out, pageCount, useOpts.reversePages);
        Bitmap8 bm;
        if (!raster.render(layout.pages[i], &bm, &doc.images, pageCount)) {
            log->log(LogLevel::Error, "pipeline", "raster failed");
            return INK_ERR_RENDER;
        }
        if (pageImages) pageImages->save(out + 1, bm);
        std::vector<uint8_t> jpeg;
        StreamingPdfWriter::ImageCodec codec;
        if (!encodePageImage(bm, imageAreaFraction(layout.pages[i], bm.width, bm.height),
                             useOpts, &jpeg, &codec)) {
            char emsg[128];
            std::snprintf(emsg, sizeof(emsg),
                "page encode failed at page %d/%d (likely OOM)",
                i + 1, pageCount);
            log->log(LogLevel::Error, "pipeline", emsg);
            return INK_ERR_ENCODE;
        }
        auto legacyLinks = legacyLinksForPage(
            doc, layout, i, useOpts.orientation == Orientation::VerticalRtl);
        if (!sink.push(std::move(jpeg), layout.pages[i].glyphs,
                       std::move(legacyLinks), codec)) {
            log->log(LogLevel::Error, "pipeline", "pdf write failed");
            return INK_ERR_WRITE_OUTPUT;
        }
        emit(progressCb, progressUserdata, jobId, INK_STAGE_RENDER,
             100 * (out + 1) / std::max(1, pageCount));
    }
#endif

    if (!sink.finish()) {
        log->log(LogLevel::Error, "pipeline", "pdf write failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    if (useOpts.embedBookmarks) {
        const auto epubOutline = epubOutlineForLayout(doc, layout);
        const bool headingsRicher = epubOutline.size() <= 2
            && layout.headings.size() >= 6;
        const auto pdfOutline = pdfOutlineForLayout(doc, layout);
        const auto& outlineSource = !doc.importedOutline.empty()
                                        ? pdfOutline
                                        : (!epubOutline.empty() && !headingsRicher
                                           ? epubOutline : layout.headings);
        for (const auto& a : outlineSource) {
            pdf.addOutline(a.title, a.level,
                           outlinePageIndex(useOpts, a.pageIndex0, pageCount));
        }
    }

    emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE, 50);
    if (!pdf.close()) {
        log->log(LogLevel::Error, "pipeline", "pdf close failed");
        return INK_ERR_WRITE_OUTPUT;
    }
    if (pageImages) pageImages->finish(pdf, useOpts.splitLandscape ? 2 : 1);
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PACKAGE, 100);
    rememberOutput(output, fileIdentity(input, opts) + "|" + optionsFingerprint(opts));
    emit(progressCb, progressUserdata, jobId, INK_STAGE_DONE, 100);
    return INK_OK;
#else
    log->log(LogLevel::Error, "pipeline",
             "text conversion disabled (HARDCOPY_TEXT_MODE=OFF); use --comic");
    return INK_ERR_INVALID_OPTIONS;
#endif
}

ink_status_t runExtract(const std::filesystem::path& input,
                        const std::filesystem::path& outDir,
                        const Options& opts,
                        const char* jobId,
                        ink_progress_cb progressCb,
                        void* progressUserdata,
                        Logger* log) {
    NullLogger nullLog;
    if (!log) log = &nullLog;
#if HARDCOPY_HAS_TEXT_MODE
    const std::string ext = lowerExt(input);
    std::string format;
    if (ext == ".txt") format = "txt";
    else if (ext == ".md" || ext == ".markdown") format = "md";
#if HARDCOPY_HAS_PDFIN
    else if (ext == ".epub") format = "epub";
    else if (ext == ".pdf") format = "pdf";
#endif
    if (format.empty()) {
        log->log(LogLevel::Error, "extract", "unsupported input extension");
        return INK_ERR_INVALID_OPTIONS;
    }
    Options eo = opts;
    eo.zhConvert = ZhConvert::None;
    eo.pdfMode = PdfMode::Reflow;

    Document doc;
    bool ok = false;
#if HARDCOPY_HAS_PDFIN
    PdfOcrText pdfOcrText;
    std::string ocrKey;
    if (!loadPdfOcrForParse(input, eo, jobId, progressCb, progressUserdata, log, &pdfOcrText, &ocrKey))
        return INK_ERR_CANCELLED;
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 0);
    if (format == "epub") {
        ok = parsePdf(input, &doc, log);
        if (ok) normalizeSourceParagraphIndents(&doc);
    } else {
        ok = parseInput(input, &doc, eo, log, &pdfOcrText);
    }
#else
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 0);
    ok = parseInput(input, &doc, eo, log, nullptr);
#endif
    if (jobCancelled(jobId)) {
        log->log(LogLevel::Info, "extract", "cancelled");
        return INK_ERR_CANCELLED;
    }
    if (!ok) {
        log->log(LogLevel::Error, "extract", "parse failed");
        return INK_ERR_PARSE;
    }
    emit(progressCb, progressUserdata, jobId, INK_STAGE_PARSE, 100);
    if (format == "pdf" || format == "epub") joinSplitParagraphs(&doc);
    if (!writeSourceExport(doc, format, outDir, log)) return INK_ERR_WRITE_OUTPUT;
    emit(progressCb, progressUserdata, jobId, INK_STAGE_DONE, 100);
    return INK_OK;
#else
    (void)input; (void)outDir; (void)opts; (void)jobId; (void)progressCb; (void)progressUserdata;
    log->log(LogLevel::Error, "extract", "text conversion disabled (HARDCOPY_TEXT_MODE=OFF)");
    return INK_ERR_INVALID_OPTIONS;
#endif
}

}
