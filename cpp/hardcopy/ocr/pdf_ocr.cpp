#include "ocr/pdf_ocr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <memory>
#include <set>
#include <sstream>

#include "cancel.h"
#include "ocr/ppocr.h"
#include "util/thread_pool.h"

namespace hardcopy {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kOcrDpi = 200.f;
constexpr float kMaxOcrPixels = 8e6f;
constexpr const char* kCacheVersion = "HCOCR1 ppocrv5-mobile";

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::filesystem::path cacheFile(const std::filesystem::path& dir, const std::string& identity) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.ocr", (unsigned long long)fnv1a(identity));
    return dir / name;
}

uint32_t decodeUtf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    if (s.empty()) return 32;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0 && s.size() >= 2) return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0 && s.size() >= 3)
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if ((p[0] & 0xF8) == 0xF0 && s.size() >= 4)
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 0xFFFD;
}

constexpr uintmax_t kCacheBudget = 64ull << 20;
void pruneCache(const std::filesystem::path& dir, const std::filesystem::path& keep) {
    std::error_code ec;
    struct Entry { std::filesystem::file_time_type time; uintmax_t size; std::filesystem::path path; };
    std::vector<Entry> files;
    uintmax_t total = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".ocr") continue;
        const uintmax_t size = e.file_size(ec);
        files.push_back({e.last_write_time(ec), size, e.path()});
        total += size;
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const auto& f : files) {
        if (total <= kCacheBudget) break;
        if (f.path == keep) continue;
        if (std::filesystem::remove(f.path, ec)) total -= f.size;
    }
}

struct Cache {
    std::filesystem::path path;
    std::map<int, PdfOcrPage> pages;
    std::set<int> textPages;
    bool writable = false;

    void load(const std::string& identity) {
        std::ifstream in(path);
        if (!in) return;
        std::string header;
        std::getline(in, header);
        if (header != std::string(kCacheVersion) + " " + identity) return;
        std::string tag;
        while (in >> tag) {
            int page = -1;
            if (tag == "T") { in >> page; textPages.insert(page); continue; }
            if (tag != "P") break;
            int count = 0;
            in >> page >> count;
            if (!in || count < 0 || count > 100000) break;
            PdfOcrPage p;
            for (int l = 0; l < count && in; ++l) {
                PdfOcrLine line;
                int n = 0;
                in >> line.baseline >> line.size >> n;
                if (!in || n < 0 || n > 100000) break;
                line.chars.resize(n);
                for (auto& ch : line.chars) in >> ch.c >> ch.x0 >> ch.x1;
                p.lines.push_back(std::move(line));
            }
            if (!in) break;
            pages[page] = std::move(p);
        }
    }
    void open(const std::string& identity) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        pruneCache(path.parent_path(), path);
        if (pages.empty() && textPages.empty()) {
            std::ofstream out(path, std::ios::trunc);
            out << kCacheVersion << " " << identity << "\n";
            writable = (bool)out;
        } else {
            writable = (bool)std::ofstream(path, std::ios::app);
        }
    }
    void appendPage(int page, const PdfOcrPage& p) {
        if (!writable) return;
        std::ofstream out(path, std::ios::app);
        out << "P " << page << " " << p.lines.size() << "\n";
        char buf[64];
        for (const auto& line : p.lines) {
            std::snprintf(buf, sizeof(buf), "%.2f %.2f %zu", line.baseline, line.size, line.chars.size());
            out << buf;
            for (const auto& ch : line.chars) {
                std::snprintf(buf, sizeof(buf), " %u %.2f %.2f", ch.c, ch.x0, ch.x1);
                out << buf;
            }
            out << "\n";
        }
    }
    void appendTextPage(int page) {
        if (writable) std::ofstream(path, std::ios::app) << "T " << page << "\n";
    }
};

PdfOcrPage toPagePoints(const std::vector<OcrTextLine>& lines, float scale, float ox, float oy) {
    PdfOcrPage page;
    std::vector<float> sizes;
    for (const auto& l : lines) {
        if (l.glyphs.empty()) continue;
        PdfOcrLine line;
        line.baseline = (l.baseline + oy) / scale;
        line.size = l.fontPx / scale;
        for (const auto& g : l.glyphs)
            line.chars.push_back({decodeUtf8(g.text), (g.x0 + ox) / scale, (g.x1 + ox) / scale});
        sizes.push_back(line.size);
        page.lines.push_back(std::move(line));
    }
    if (!sizes.empty()) {
        std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
        const float median = sizes[sizes.size() / 2];
        for (auto& line : page.lines)
            if (line.size > median * 0.85f && line.size < median * 1.15f) line.size = median;
    }
    return page;
}

}

int stextVisibleChars(const fz_stext_page* page) {
    int n = 0;
    for (auto* b = page ? page->first_block : nullptr; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (auto* l = b->u.t.first_line; l; l = l->next)
            for (auto* ch = l->first_char; ch; ch = ch->next) n += ch->c > 32;
    }
    return n;
}

std::filesystem::path pdfOcrCacheDir(const std::string& configured) {
    if (!configured.empty()) return configured;
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec || !std::filesystem::is_directory(temp, ec)) return {};
    return temp / "hardcopy-ocr";
}

std::string pdfSourceIdentity(const std::filesystem::path& input, const std::string& sourceIdentity) {
    if (!sourceIdentity.empty()) return sourceIdentity;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(input, ec);
    const auto size = std::filesystem::file_size(input, ec);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "|%llu|%llu",
                  (unsigned long long)mtime.time_since_epoch().count(), (unsigned long long)size);
    return input.string() + buf;
}

bool pdfOcrCacheExists(const std::filesystem::path& cacheDir, const std::string& identity) {
    if (cacheDir.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(cacheFile(cacheDir, identity), ec);
}

bool preparePdfOcr(fz_context* ctx, fz_document* doc, const PdfOcrRequest& request,
                   Logger* log, PdfOcrText* out) {
    char msg[256];
    Cache cache;
    if (!request.cacheDir.empty()) {
        cache.path = cacheFile(request.cacheDir, request.identity);
        cache.load(request.identity);
    }
    const int count = fz_count_pages(ctx, doc);
    for (const auto& [page, result] : cache.pages)
        if (page >= 0 && page < count) (*out)[page] = result;
    std::snprintf(msg, sizeof(msg), "ocr cache %s: %zu pages", cache.path.string().c_str(), cache.pages.size());
    if (log) log->log(LogLevel::Info, "ocr", msg);
    if (!request.compute) return true;

    std::vector<int> todo;
    std::vector<int> wanted = request.pages;
    if (wanted.empty())
        for (int i = 0; i < count; ++i) wanted.push_back(i);
    for (int i : wanted) {
        if (i < 0 || i >= count || out->count(i)) continue;
        if (!request.ignoreText && cache.textPages.count(i)) continue;
        todo.push_back(i);
    }
    if (todo.empty()) return true;
    std::shared_ptr<PpOcr> ocr = PpOcr::create(log, 1);
    if (!ocr) return true;
    if (!cache.path.empty()) cache.open(request.identity);

    const int threads = std::max(1, request.threads);
    std::unique_ptr<ThreadPool> pool;
    if (threads > 1) pool = std::make_unique<ThreadPool>(threads);
    struct Job {
        int page;
        float scale, ox, oy;
        std::future<std::vector<OcrTextLine>> lines;
    };
    std::deque<Job> inFlight;
    const auto start = Clock::now();
    int done = 0, recognized = 0, chars = 0;
    auto finish = [&](Job& job) {
        const auto lines = job.lines.get();
        PdfOcrPage page = toPagePoints(lines, job.scale, job.ox, job.oy);
        for (const auto& l : page.lines) chars += (int)l.chars.size();
        cache.appendPage(job.page, page);
        (*out)[job.page] = std::move(page);
        ++recognized;
    };
    auto report = [&] { if (request.progress) request.progress(done, (int)todo.size()); };
    report();
    for (int index : todo) {
        if (jobCancelled(request.jobId)) {
            while (!inFlight.empty()) { inFlight.front().lines.wait(); inFlight.pop_front(); }
            return false;
        }
        fz_page* page = nullptr;
        fz_pixmap* pix = nullptr;
        bool hasText = false;
        Bitmap8 bitmap;
        float scale = kOcrDpi / 72.f;
        int originX = 0, originY = 0;
        fz_var(page); fz_var(pix);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            if (!request.ignoreText) {
                fz_stext_options so{};
                fz_stext_page* text = fz_new_stext_page_from_page(ctx, page, &so);
                hasText = stextVisibleChars(text) >= kOcrMaxSourceChars;
                fz_drop_stext_page(ctx, text);
            }
            if (!hasText) {
                const fz_rect b = fz_bound_page(ctx, page);
                const float area = std::max(1.f, (b.x1 - b.x0) * (b.y1 - b.y0));
                scale = std::min(scale, std::sqrt(kMaxOcrPixels / area));
                pix = fz_new_pixmap_from_page(ctx, page, fz_scale(scale, scale), fz_device_gray(ctx), 0);
                originX = fz_pixmap_x(ctx, pix);
                originY = fz_pixmap_y(ctx, pix);
                bitmap.width = fz_pixmap_width(ctx, pix);
                bitmap.height = fz_pixmap_height(ctx, pix);
                bitmap.pixels.resize((size_t)bitmap.width * bitmap.height);
                const unsigned char* src = fz_pixmap_samples(ctx, pix);
                const int stride = fz_pixmap_stride(ctx, pix);
                for (int y = 0; y < bitmap.height; ++y)
                    std::memcpy(&bitmap.pixels[(size_t)y * bitmap.width], src + (size_t)y * stride, bitmap.width);
            }
        } fz_always(ctx) {
            fz_drop_pixmap(ctx, pix);
            fz_drop_page(ctx, page);
        } fz_catch(ctx) {
            if (log) log->log(LogLevel::Warn, "ocr", fz_caught_message(ctx));
            hasText = true;
        }
        if (hasText) {
            if (!request.ignoreText) cache.appendTextPage(index);
            ++done; report();
            continue;
        }
        Job job;
        job.page = index;
        job.scale = scale;
        job.ox = (float)originX;
        job.oy = (float)originY;
        auto task = [ocr, bitmap = std::move(bitmap)]() {
            std::vector<OcrTextLine> lines;
            OcrStats stats;
            ocr->recognize(bitmap, ImageRect{0, 0, bitmap.width, bitmap.height}, 1, &lines, &stats);
            return lines;
        };
        if (pool) {
            job.lines = pool->submit(std::move(task));
        } else {
            std::promise<std::vector<OcrTextLine>> ready;
            ready.set_value(task());
            job.lines = ready.get_future();
        }
        inFlight.push_back(std::move(job));
        while (inFlight.size() > (size_t)threads) {
            finish(inFlight.front()); inFlight.pop_front();
            ++done; report();
        }
    }
    while (!inFlight.empty()) {
        finish(inFlight.front()); inFlight.pop_front();
        ++done; report();
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::snprintf(msg, sizeof(msg), "ocr pass: %d of %d pages recognized, %d chars, %lldms, threads=%d",
                  recognized, (int)todo.size(), chars, ms, threads);
    if (log) log->log(LogLevel::Info, "ocr", msg);
    return true;
}

bool preparePdfOcr(const std::filesystem::path& input, const PdfOcrRequest& request,
                   Logger* log, PdfOcrText* out) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return true;
    fz_document* doc = nullptr;
    bool result = true;
    fz_var(doc);
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = fz_open_document(ctx, input.string().c_str());
        result = preparePdfOcr(ctx, doc, request, log, out);
    } fz_always(ctx) {
        fz_drop_document(ctx, doc);
    } fz_catch(ctx) {
        if (log) log->log(LogLevel::Warn, "ocr", fz_caught_message(ctx));
    }
    fz_drop_context(ctx);
    return result;
}

namespace {

void unlinkBlock(fz_stext_page* page, fz_stext_block* b) {
    if (b->prev) b->prev->next = b->next; else page->first_block = b->next;
    if (b->next) b->next->prev = b->prev; else page->last_block = b->prev;
}

struct ImageInk {
    int samples = 0;
    int dark = 0;
    int darkOutside = 0;
    int toneOutside = 0;
    int rules = 0;
    int inkedCells = 0;
    int paper = 255;
};

bool sampleImageInk(fz_context* ctx, const fz_stext_block* b, const std::vector<fz_rect>& boxes,
                    const fz_rect& page, float edge, bool paperRelative, float cellPt,
                    ImageInk* ink) {
    fz_pixmap* pix = nullptr;
    fz_pixmap* gray = nullptr;
    ImageInk st;
    fz_var(pix); fz_var(gray);
    fz_try(ctx) {
        pix = fz_get_pixmap_from_image(ctx, b->u.i.image, nullptr, nullptr, nullptr, nullptr);
        gray = fz_convert_pixmap(ctx, pix, fz_device_gray(ctx), nullptr, nullptr, fz_default_color_params, 0);
        const int w = fz_pixmap_width(ctx, gray), h = fz_pixmap_height(ctx, gray);
        const int stride = fz_pixmap_stride(ctx, gray);
        const unsigned char* px = fz_pixmap_samples(ctx, gray);
        const int step = std::max(1, (int)std::sqrt((double)w * h / 40000.0));
        int toneLevel = 235, darkLevel = 160;
        if (paperRelative) {
            int hist[256] = {0}, n = 0;
            for (int y = step / 2; y < h; y += step)
                for (int x = step / 2; x < w; x += step) { ++hist[px[(size_t)y * stride + x]]; ++n; }
            int below = 0, paper = 255;
            for (int v = 0; v < 256; ++v) {
                below += hist[v];
                if (below * 10 >= n * 9) { paper = v; break; }
            }
            st.paper = paper;
            toneLevel = std::min(235, paper - 20);
            darkLevel = std::min(160, paper - 60);
        }
        const int reach = std::max(8, std::max(w, h) / 80);
        const int across = std::max(3, reach / 4);
        auto inked = [&](int xx, int yy) {
            return xx >= 0 && yy >= 0 && xx < w && yy < h && px[(size_t)yy * stride + xx] < toneLevel;
        };
        auto onLine = [&](int x, int y) {
            bool horizontal = !inked(x, y - across) && !inked(x, y + across);
            bool vertical = !inked(x - across, y) && !inked(x + across, y);
            for (int d = 1; d <= reach && (horizontal || vertical); ++d) {
                horizontal = horizontal && inked(x - d, y) && inked(x + d, y);
                vertical = vertical && inked(x, y - d) && inked(x, y + d);
            }
            return horizontal || vertical;
        };
        const float side = 0.08f * (page.x1 - page.x0);
        const float bw = std::max(1.f, b->bbox.x1 - b->bbox.x0);
        const int cellPx = std::max(8, (int)(cellPt * w / bw));
        const int cols = (w + cellPx - 1) / cellPx, rows = (h + cellPx - 1) / cellPx;
        std::vector<int> cellSamples, cellInk;
        if (paperRelative) { cellSamples.assign((size_t)cols * rows, 0); cellInk.assign((size_t)cols * rows, 0); }
        for (int y = step / 2; y < h; y += step)
            for (int x = step / 2; x < w; x += step) {
                ++st.samples;
                const size_t cell = paperRelative ? (size_t)(y / cellPx) * cols + x / cellPx : 0;
                if (paperRelative) ++cellSamples[cell];
                const unsigned char v = px[(size_t)y * stride + x];
                if (v >= toneLevel) continue;
                const fz_point p = fz_transform_point(fz_make_point((x + .5f) / w, (y + .5f) / h),
                                                      b->u.i.transform);
                if (p.x < page.x0 || p.x > page.x1 || p.y < page.y0 + edge || p.y > page.y1 - edge) continue;
                bool inText = false;
                for (const auto& r : boxes)
                    if (fz_is_point_inside_rect(p, r)) { inText = true; break; }
                if (!inText && paperRelative) {
                    if (p.x < page.x0 + side || p.x > page.x1 - side) continue;
                    if (onLine(x, y)) { ++st.rules; continue; }
                }
                const bool dark = v < darkLevel;
                st.dark += dark;
                st.darkOutside += dark && !inText;
                st.toneOutside += !inText;
                if (paperRelative && !inText) ++cellInk[cell];
            }
        auto flagged = [&](int cx, int cy) {
            if (cx < 0 || cx >= cols) return false;
            const size_t c = (size_t)cy * cols + cx;
            return cellInk[c] >= 6 && cellInk[c] * 50 >= cellSamples[c];
        };
        for (int cy = 0; cy < rows && paperRelative; ++cy)
            for (int cx = 0; cx < cols; ++cx)
                st.inkedCells += flagged(cx, cy) && (flagged(cx - 1, cy) || flagged(cx + 1, cy));
    } fz_always(ctx) {
        fz_drop_pixmap(ctx, gray);
        fz_drop_pixmap(ctx, pix);
    } fz_catch(ctx) {
        return false;
    }
    *ink = st;
    return true;
}

void debugImageInk(const char* what, const fz_stext_block* b, const ImageInk& ink, bool drop) {
    if (!std::getenv("HC_OCR_DEBUG")) return;
    std::fprintf(stderr, "%s image %.0f,%.0f %.0fx%.0f outside=%d/%d dark=%d tone=%d rules=%d cells=%d paper=%d %s\n",
                 what, b->bbox.x0, b->bbox.y0, b->bbox.x1 - b->bbox.x0, b->bbox.y1 - b->bbox.y0,
                 ink.darkOutside, ink.samples, ink.dark, ink.toneOutside, ink.rules, ink.inkedCells, ink.paper,
                 drop ? "drop" : "keep");
}

bool isTextOrBlankImage(fz_context* ctx, const fz_stext_block* b, const std::vector<fz_rect>& boxes,
                        const fz_rect& page, float edge) {
    ImageInk ink;
    if (!sampleImageInk(ctx, b, boxes, page, edge, false, 0.f, &ink)) return false;
    const bool drop = ink.samples > 0 && ink.darkOutside <= ink.samples * 3 / 100;
    debugImageInk("ocr", b, ink, drop);
    return drop;
}

}

void addOcrToStext(fz_context* ctx, fz_stext_page* page, const PdfOcrPage& ocr,
                   bool replaceText, bool dropCoveredImages) {
    if (!page || ocr.lines.empty()) return;
    std::vector<fz_rect> boxes;
    std::vector<float> sizes;
    for (const auto& l : ocr.lines) sizes.push_back(l.size);
    std::sort(sizes.begin(), sizes.end());
    const float edge = sizes.empty() ? 0.f : 2.f * sizes[sizes.size() / 2];
    for (const auto& l : ocr.lines) {
        if (l.chars.empty()) continue;
        const float m = 0.25f * l.size;
        boxes.push_back({l.chars.front().x0 - m, l.baseline - 0.88f * l.size - m,
                         l.chars.back().x1 + m, l.baseline + 0.12f * l.size + m});
    }
    for (fz_stext_block* b = page->first_block; b;) {
        fz_stext_block* next = b->next;
        if (b->type == FZ_STEXT_BLOCK_TEXT && replaceText) {
            for (auto* l = b->u.t.first_line; l; l = l->next)
                for (auto* ch = l->first_char; ch; ch = ch->next) fz_drop_font(ctx, ch->font);
            unlinkBlock(page, b);
        } else if (b->type == FZ_STEXT_BLOCK_IMAGE && dropCoveredImages &&
                   isTextOrBlankImage(ctx, b, boxes, page->mediabox, edge)) {
            fz_drop_image(ctx, b->u.i.image);
            unlinkBlock(page, b);
        }
        b = next;
    }
    fz_font* font = fz_new_base14_font(ctx, "Helvetica");
    fz_stext_block* block = nullptr;
    float lastBaseline = 0, lastSize = 0;
    for (const auto& l : ocr.lines) {
        if (l.chars.empty()) continue;
        if (!block || l.baseline - lastBaseline > 1.8f * std::max(l.size, lastSize) ||
            l.baseline < lastBaseline) {
            block = (fz_stext_block*)fz_pool_alloc(ctx, page->pool, sizeof(fz_stext_block));
            std::memset(block, 0, sizeof(*block));
            block->type = FZ_STEXT_BLOCK_TEXT;
            block->bbox = fz_empty_rect;
            block->prev = page->last_block;
            if (page->last_block) page->last_block->next = block; else page->first_block = block;
            page->last_block = block;
        }
        lastBaseline = l.baseline; lastSize = l.size;
        auto* line = (fz_stext_line*)fz_pool_alloc(ctx, page->pool, sizeof(fz_stext_line));
        std::memset(line, 0, sizeof(*line));
        line->dir = fz_make_point(1, 0);
        line->bbox = fz_empty_rect;
        line->prev = block->u.t.last_line;
        if (block->u.t.last_line) block->u.t.last_line->next = line; else block->u.t.first_line = line;
        block->u.t.last_line = line;
        const float top = l.baseline - 0.88f * l.size, bottom = l.baseline + 0.12f * l.size;
        for (const auto& c : l.chars) {
            auto* ch = (fz_stext_char*)fz_pool_alloc(ctx, page->pool, sizeof(fz_stext_char));
            std::memset(ch, 0, sizeof(*ch));
            ch->c = (int)c.c;
            ch->origin = fz_make_point(c.x0, l.baseline);
            ch->quad.ul = fz_make_point(c.x0, top);
            ch->quad.ur = fz_make_point(c.x1, top);
            ch->quad.ll = fz_make_point(c.x0, bottom);
            ch->quad.lr = fz_make_point(c.x1, bottom);
            ch->size = l.size;
            ch->font = fz_keep_font(ctx, font);
            if (line->last_char) line->last_char->next = ch; else line->first_char = ch;
            line->last_char = ch;
            line->bbox = fz_union_rect(line->bbox, fz_rect_from_quad(ch->quad));
        }
        block->bbox = fz_union_rect(block->bbox, line->bbox);
    }
    fz_drop_font(ctx, font);
}

namespace {

bool isTextInk(const ImageInk& ink) {
    if (ink.samples <= 0 || ink.paper < 160) return false;
    return ink.inkedCells < 4 &&
           ink.darkOutside <= ink.samples * 3 / 100 &&
           (ink.darkOutside <= ink.dark / 10 || ink.darkOutside * 200 <= ink.samples) &&
           ink.toneOutside <= ink.samples * 5 / 100;
}

}

int dropScanPageImages(fz_context* ctx, fz_stext_page* page, float invisibleShare) {
    if (!page) return 0;
    const fz_rect media = page->mediabox;
    const float pageArea = std::max(1.0f, (media.x1 - media.x0) * (media.y1 - media.y0));
    std::vector<fz_stext_block*> images;
    float covered = 0;
    for (fz_stext_block* b = page->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_IMAGE) continue;
        images.push_back(b);
        const fz_rect r = fz_intersect_rect(b->bbox, media);
        if (!fz_is_empty_rect(r)) covered += (r.x1 - r.x0) * (r.y1 - r.y0);
    }
    if (images.empty()) return 0;
    std::vector<fz_point> centers;
    std::vector<fz_rect> boxes;
    std::vector<float> sizes;
    for (fz_stext_block* b = page->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
            float size = 0;
            for (fz_stext_char* ch = l->first_char; ch; ch = ch->next) {
                if (ch->c <= 0x20 || ch->c == 0xFFFD) continue;
                const fz_rect q = fz_rect_from_quad(ch->quad);
                centers.push_back(fz_make_point((q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2));
                size = std::max(size, ch->size);
            }
            if (size <= 0) continue;
            sizes.push_back(size);
            const float m = 0.25f * size;
            boxes.push_back({l->bbox.x0 - m, l->bbox.y0 - m, l->bbox.x1 + m, l->bbox.y1 + m});
        }
    }
    const int chars = (int)centers.size();
    if (chars < 16) return 0;
    std::vector<int> inside(images.size(), 0);
    int onImages = 0;
    for (const fz_point& p : centers) {
        bool any = false;
        for (size_t k = 0; k < images.size(); ++k)
            if (fz_is_point_inside_rect(p, images[k]->bbox)) { ++inside[k]; any = true; }
        onImages += any;
    }
    const bool scan = covered >= 0.65f * pageArea ||
                      (invisibleShare >= 0.8f && onImages >= 0.8f * chars);
    if (!scan) return 0;
    std::sort(sizes.begin(), sizes.end());
    const float edge = 2.f * sizes[sizes.size() / 2];
    int dropped = 0;
    for (size_t k = 0; k < images.size(); ++k) {
        fz_stext_block* b = images[k];
        const fz_rect r = fz_intersect_rect(b->bbox, media);
        const float area = fz_is_empty_rect(r) ? 0.f : (r.x1 - r.x0) * (r.y1 - r.y0);
        const bool pageItself = area >= 0.4f * pageArea && inside[k] >= 16 && inside[k] * 4 >= chars;
        if (!pageItself) {
            ImageInk ink;
            if (!sampleImageInk(ctx, b, boxes, media, edge, true, 1.5f * edge, &ink)) continue;
            const bool drop = isTextInk(ink);
            debugImageInk("scan", b, ink, drop);
            if (!drop) continue;
        }
        fz_drop_image(ctx, b->u.i.image);
        unlinkBlock(page, b);
        ++dropped;
    }
    return dropped;
}

}
