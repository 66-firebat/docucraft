#include "raster_reflow.h"
#include "streaming_pdf_writer.h"
#include "page_image_dump.h"
#include "../encode/jpeg.h"
#include "../render/pdf_bitmap_processor.h"
#include "../ocr/pdf_ocr.h"
#include "../render/pdf_content_bounds.h"
#include "../render/pdf_columns.h"
#include "../render/pdf_line_spacing.h"
#include "../cancel.h"
#if HARDCOPY_HAS_PARALLEL
#include "../util/thread_pool.h"
#endif
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
extern "C" {
#include <mupdf/fitz.h>
}

namespace hardcopy {
namespace {
struct JobCancelled : std::runtime_error {
    JobCancelled() : std::runtime_error("cancelled") {}
};
struct Line { std::string key; fz_rect box; };

constexpr float kBaseFillMinSidePt = 8.0f;
constexpr float kBaseFillCoverage = 0.6f;
constexpr float kBaseStrokeMinPt = 8.0f;
constexpr float kScanPageCoverage = 0.5f;
constexpr int kScanMaxVisibleChars = 200;

struct LayerDevice {
    fz_device super;
    fz_device* target;
    fz_rect page;
    int maskDepth;
    int tileDepth;
    int visibleChars;
    float imageArea;
    bool scanPage;
    int baseItems;
    std::vector<fz_rect>* images;
};

LayerDevice* layer(fz_device* dev) { return reinterpret_cast<LayerDevice*>(dev); }
bool passAll(const LayerDevice* d) { return d->maskDepth > 0 || d->tileDepth > 0; }
float rectArea(fz_rect r) { return fz_is_empty_rect(r) ? 0.f : (r.x1-r.x0)*(r.y1-r.y0); }

bool nearWhite(fz_context* ctx, fz_colorspace* cs, const float* color) {
    if (!cs || !color) return false;
    float gray = 0; bool ok = false;
    fz_var(ok);
    fz_try(ctx) {
        fz_convert_color(ctx, cs, color, fz_device_gray(ctx), &gray, nullptr, fz_default_color_params);
        ok = true;
    } fz_catch(ctx) {}
    return ok && gray >= 0.98f;
}

struct AreaWalk {
    fz_matrix ctm;
    fz_point start{}, cur{};
    double sub = 0, best = 0;
    void edge(fz_point p) {
        sub += (double)cur.x*p.y - (double)p.x*cur.y;
        cur = p;
    }
    void finish() {
        edge(start);
        best = std::max(best, std::fabs(sub) / 2);
        sub = 0;
    }
    void line(float x, float y) { edge(fz_transform_point_xy(x, y, ctm)); }
};
void awMove(fz_context*, void* arg, float x, float y) {
    auto* a = static_cast<AreaWalk*>(arg);
    a->finish();
    a->start = a->cur = fz_transform_point_xy(x, y, a->ctm);
}
void awLine(fz_context*, void* arg, float x, float y) { static_cast<AreaWalk*>(arg)->line(x, y); }
void awCurve(fz_context*, void* arg, float x1, float y1, float x2, float y2, float x3, float y3) {
    auto* a = static_cast<AreaWalk*>(arg);
    a->line(x1, y1); a->line(x2, y2); a->line(x3, y3);
}
void awClose(fz_context*, void* arg) {
    auto* a = static_cast<AreaWalk*>(arg);
    a->edge(a->start);
}

bool isAreaFill(fz_context* ctx, const fz_path* path, fz_matrix ctm) {
    const fz_rect box = fz_bound_path(ctx, path, nullptr, ctm);
    const float w = box.x1 - box.x0, h = box.y1 - box.y0;
    if (!(w >= kBaseFillMinSidePt && h >= kBaseFillMinSidePt)) return false;
    AreaWalk walk; walk.ctm = ctm;
    fz_path_walker walker{};
    walker.moveto = awMove; walker.lineto = awLine;
    walker.curveto = awCurve; walker.closepath = awClose;
    fz_walk_path(ctx, path, &walker, &walk);
    walk.finish();
    return walk.best >= kBaseFillCoverage * (double)w * h;
}

void lyFillPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm,
                fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    if (!d->target) return;
    if (!passAll(d)) {
        if (!isAreaFill(ctx, path, ctm)) return;
        if (alpha > 0 && !nearWhite(ctx, cs, color)) ++d->baseItems;
    }
    fz_fill_path(ctx, d->target, path, evenOdd, ctm, cs, color, alpha, cp);
}
void lyStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                  fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    if (!d->target) return;
    if (!passAll(d)) {
        const float width = stroke ? stroke->linewidth * fz_matrix_expansion(ctm) : 0.f;
        if (width < kBaseStrokeMinPt) return;
        if (alpha > 0 && !nearWhite(ctx, cs, color)) ++d->baseItems;
    }
    fz_stroke_path(ctx, d->target, path, stroke, ctm, cs, color, alpha, cp);
}
void lyClipPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm, fz_rect scissor) {
    if (auto* t = layer(dev)->target) fz_clip_path(ctx, t, path, evenOdd, ctm, scissor);
}
void lyClipStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                      fz_matrix ctm, fz_rect scissor) {
    if (auto* t = layer(dev)->target) fz_clip_stroke_path(ctx, t, path, stroke, ctm, scissor);
}
int textChars(const fz_text* text) {
    int n = 0;
    for (auto* span = text ? text->head : nullptr; span; span = span->next) n += span->len;
    return n;
}
void lyFillText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    if (!d->target) { if (alpha > 0 && !d->maskDepth) d->visibleChars += textChars(text); return; }
    if (passAll(d)) fz_fill_text(ctx, d->target, text, ctm, cs, color, alpha, cp);
}
void lyStrokeText(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                  fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    if (!d->target) { if (alpha > 0 && !d->maskDepth) d->visibleChars += textChars(text); return; }
    if (passAll(d)) fz_stroke_text(ctx, d->target, text, stroke, ctm, cs, color, alpha, cp);
}
void lyClipText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect scissor) {
    if (auto* t = layer(dev)->target) fz_clip_text(ctx, t, text, ctm, scissor);
}
void lyClipStrokeText(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                      fz_matrix ctm, fz_rect scissor) {
    if (auto* t = layer(dev)->target) fz_clip_stroke_text(ctx, t, text, stroke, ctm, scissor);
}
void lyIgnoreText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm) {
    if (auto* t = layer(dev)->target) fz_ignore_text(ctx, t, text, ctm);
}
void lyFillShade(fz_context* ctx, fz_device* dev, fz_shade* shade, fz_matrix ctm, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    if (!d->target) return;
    if (!passAll(d) && alpha > 0) ++d->baseItems;
    fz_fill_shade(ctx, d->target, shade, ctm, alpha, cp);
}
void lyFillImage(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    const fz_rect box = fz_intersect_rect(fz_transform_rect(fz_unit_rect, ctm), d->page);
    if (!d->target) {
        if (!passAll(d) && alpha > 0 && image->bpc > 1) d->imageArea += rectArea(box);
        return;
    }
    if (!passAll(d)) {
        if (image->bpc <= 1 || d->scanPage) return;
        if (alpha > 0) {
            ++d->baseItems;
            if (d->images && !fz_is_empty_rect(box)) d->images->push_back(box);
        }
    }
    fz_fill_image(ctx, d->target, image, ctm, alpha, cp);
}
void lyFillImageMask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                     fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    auto* d = layer(dev);
    if (d->target && passAll(d)) fz_fill_image_mask(ctx, d->target, image, ctm, cs, color, alpha, cp);
}
void lyClipImageMask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_rect scissor) {
    if (auto* t = layer(dev)->target) fz_clip_image_mask(ctx, t, image, ctm, scissor);
}
void lyPopClip(fz_context* ctx, fz_device* dev) {
    if (auto* t = layer(dev)->target) fz_pop_clip(ctx, t);
}
void lyBeginMask(fz_context* ctx, fz_device* dev, fz_rect area, int luminosity, fz_colorspace* cs,
                 const float* bc, fz_color_params cp) {
    auto* d = layer(dev);
    ++d->maskDepth;
    if (d->target) fz_begin_mask(ctx, d->target, area, luminosity, cs, bc, cp);
}
void lyEndMask(fz_context* ctx, fz_device* dev, fz_function* fn) {
    auto* d = layer(dev);
    if (d->maskDepth > 0) --d->maskDepth;
    if (d->target) fz_end_mask_tr(ctx, d->target, fn);
}
void lyBeginGroup(fz_context* ctx, fz_device* dev, fz_rect area, fz_colorspace* cs, int isolated,
                  int knockout, int blendmode, float alpha) {
    if (auto* t = layer(dev)->target) fz_begin_group(ctx, t, area, cs, isolated, knockout, blendmode, alpha);
}
void lyEndGroup(fz_context* ctx, fz_device* dev) {
    if (auto* t = layer(dev)->target) fz_end_group(ctx, t);
}
int lyBeginTile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep, float ystep,
                fz_matrix ctm, int id) {
    auto* d = layer(dev);
    ++d->tileDepth;
    if (!d->target) return 0;
    if (!d->maskDepth) ++d->baseItems;
    return fz_begin_tile_id(ctx, d->target, area, view, xstep, ystep, ctm, id);
}
void lyEndTile(fz_context* ctx, fz_device* dev) {
    auto* d = layer(dev);
    if (d->tileDepth > 0) --d->tileDepth;
    if (d->target) fz_end_tile(ctx, d->target);
}
void lyRenderFlags(fz_context* ctx, fz_device* dev, int set, int clear) {
    if (auto* t = layer(dev)->target) fz_render_flags(ctx, t, set, clear);
}
void lyDefaultColorspaces(fz_context* ctx, fz_device* dev, fz_default_colorspaces* cs) {
    if (auto* t = layer(dev)->target) fz_set_default_colorspaces(ctx, t, cs);
}
void lyBeginLayer(fz_context* ctx, fz_device* dev, const char* name) {
    if (auto* t = layer(dev)->target) fz_begin_layer(ctx, t, name);
}
void lyEndLayer(fz_context* ctx, fz_device* dev) {
    if (auto* t = layer(dev)->target) fz_end_layer(ctx, t);
}

fz_device* newLayerDevice(fz_context* ctx, fz_device* target, fz_rect page) {
    auto* d = fz_new_derived_device(ctx, LayerDevice);
    d->super.fill_path = lyFillPath;
    d->super.stroke_path = lyStrokePath;
    d->super.clip_path = lyClipPath;
    d->super.clip_stroke_path = lyClipStrokePath;
    d->super.fill_text = lyFillText;
    d->super.stroke_text = lyStrokeText;
    d->super.clip_text = lyClipText;
    d->super.clip_stroke_text = lyClipStrokeText;
    d->super.ignore_text = lyIgnoreText;
    d->super.fill_shade = lyFillShade;
    d->super.fill_image = lyFillImage;
    d->super.fill_image_mask = lyFillImageMask;
    d->super.clip_image_mask = lyClipImageMask;
    d->super.pop_clip = lyPopClip;
    d->super.begin_mask = lyBeginMask;
    d->super.end_mask = lyEndMask;
    d->super.begin_group = lyBeginGroup;
    d->super.end_group = lyEndGroup;
    d->super.begin_tile = lyBeginTile;
    d->super.end_tile = lyEndTile;
    d->super.render_flags = lyRenderFlags;
    d->super.set_default_colorspaces = lyDefaultColorspaces;
    d->super.begin_layer = lyBeginLayer;
    d->super.end_layer = lyEndLayer;
    d->target = target;
    d->page = page;
    return &d->super;
}

struct Source {
    fz_context* ctx;
    fz_document* doc = nullptr;
    fz_page* page = nullptr;
    fz_display_list* list = nullptr;
    fz_display_list* baseList = nullptr;
    std::vector<fz_rect> pictures;
    fz_stext_page* text = nullptr;
    fz_rect bounds{};
    explicit Source(fz_context* c) : ctx(c) {}
    ~Source() {
        if (text) fz_drop_stext_page(ctx, text);
        if (baseList) fz_drop_display_list(ctx, baseList);
        if (list) fz_drop_display_list(ctx, list);
        if (page) fz_drop_page(ctx, page);
    }
    int pageIndex = -1;
    void open(fz_document* document, int index, bool ignoreText = false,
              const PdfOcrPage* ocr = nullptr) {
        doc = document;
        pageIndex = index;
        bool ok = false; fz_var(ok);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, index);
            bounds = fz_bound_page(ctx, page);
            list = fz_new_display_list_from_page(ctx, page);
            if (ignoreText) {
                text = fz_new_stext_page(ctx, bounds);
            } else {
                fz_stext_options options{};
                text = fz_new_stext_page_from_display_list(ctx, list, &options);
            }
            if (ocr && (ignoreText || stextVisibleChars(text) < kOcrMaxSourceChars))
                addOcrToStext(ctx, text, *ocr, false, false);
            ok = true;
        } fz_catch(ctx) {}
        if (!ok) throw std::runtime_error("PDF page load failed");
    }
    std::string buildBase() {
        fz_device* dev = nullptr; fz_display_list* out = nullptr;
        int visible = 0, items = 0; float imageArea = 0; bool scan = false;
        fz_var(dev); fz_var(out);
        pictures.clear();
        fz_try(ctx) {
            dev = newLayerDevice(ctx, nullptr, bounds);
            fz_run_display_list(ctx, list, dev, fz_identity, fz_infinite_rect, nullptr);
            fz_close_device(ctx, dev);
            visible = layer(dev)->visibleChars;
            imageArea = layer(dev)->imageArea;
            fz_drop_device(ctx, dev); dev = nullptr;
            scan = imageArea >= kScanPageCoverage * rectArea(bounds) && visible < kScanMaxVisibleChars;
            out = fz_new_display_list(ctx, bounds);
            fz_device* rec = fz_new_list_device(ctx, out);
            fz_try(ctx) {
                dev = newLayerDevice(ctx, rec, bounds);
                layer(dev)->scanPage = scan;
                layer(dev)->images = &pictures;
                fz_run_display_list(ctx, list, dev, fz_identity, fz_infinite_rect, nullptr);
                fz_close_device(ctx, dev);
                items = layer(dev)->baseItems;
                fz_close_device(ctx, rec);
            } fz_always(ctx) {
                fz_drop_device(ctx, rec);
            } fz_catch(ctx) {
                fz_rethrow(ctx);
            }
            if (items > 0) { baseList = out; out = nullptr; }
        } fz_catch(ctx) {
            items = -1;
        }
        if (dev) fz_drop_device(ctx, dev);
        if (out) fz_drop_display_list(ctx, out);
        if (!baseList) pictures.clear();
        char msg[160];
        std::snprintf(msg, sizeof(msg), "visibleChars=%d imageArea=%.0f%% scan=%s baseItems=%d pictures=%d",
            visible, 100.f * imageArea / std::max(1.f, rectArea(bounds)), scan ? "yes" : "no",
            items, (int)pictures.size());
        return msg;
    }
    void renderInto(int width, int height, fz_matrix matrix, uint8_t* samples, int stride,
                    fz_display_list* which = nullptr) {
        if (!which) which = list;
        fz_pixmap* pix = nullptr; fz_device* dev = nullptr;
        bool ok = false; fz_var(pix); fz_var(dev); fz_var(ok);
        fz_try(ctx) {
            pix = fz_new_pixmap_with_data(ctx, fz_device_gray(ctx), width, height, nullptr, 0, stride, samples);
            for (int y = 0; y < height; ++y)
                std::fill_n(samples + (size_t)y * stride, width, uint8_t(255));
            dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_display_list(ctx, which, dev, matrix,
                fz_rect{0, 0, (float)width, (float)height}, nullptr);
            fz_close_device(ctx, dev);
            ok = true;
        } fz_catch(ctx) {}
        if (dev) fz_drop_device(ctx, dev);
        if (!ok) {
            if (pix) fz_drop_pixmap(ctx, pix);
            throw std::runtime_error("PDF strip render failed");
        }
        fz_drop_pixmap(ctx, pix);
    }
    Bitmap8 render(int width, int height, fz_matrix matrix) {
        Bitmap8 out;
        out.width = width; out.height = height;
        out.pixels.resize((size_t)width * height);
        renderInto(width, height, matrix, out.pixels.data(), width);
        return out;
    }
    void links(std::vector<PdfLink>* out) const {
        if (!out) return;
        out->clear();
        fz_link* list = nullptr;
        fz_try(ctx) { list = fz_load_links(ctx, page); } fz_catch(ctx) { return; }
        for (fz_link* link = list; link; link = link->next) {
            if (!link->uri) continue;
            PdfLink item;
            item.x0 = link->rect.x0;
            item.y0 = link->rect.y0;
            item.x1 = link->rect.x1;
            item.y1 = link->rect.y1;
            item.uri = link->uri;
            int target = -1;
            fz_link_dest dest = fz_make_link_dest_none();
            fz_try(ctx) {
                dest = fz_resolve_link_dest(ctx, doc, link->uri);
                target = fz_page_number_from_location(ctx, doc, dest.loc);
            } fz_catch(ctx) {}
            if (target >= 0) { item.targetPage0 = target; item.uri.clear(); }
            const bool hasTop = dest.type == FZ_LINK_DEST_XYZ || dest.type == FZ_LINK_DEST_FIT_H ||
                dest.type == FZ_LINK_DEST_FIT_BH || dest.type == FZ_LINK_DEST_FIT_R;
            if (target >= 0 && hasTop && !std::isnan(dest.y)) {
                item.hasTargetPoint = true;
                item.targetX = dest.x;
                item.targetY = dest.y;
            }
            out->push_back(std::move(item));
        }
        if (list) fz_drop_link(ctx, list);
    }

    Bitmap8 probe() {
        const float s = 720.0f / (bounds.x1 - bounds.x0);
        const int h = std::max(1, (int)std::ceil((bounds.y1 - bounds.y0) * s));
        if (h > 16384) throw std::runtime_error("PDF page aspect ratio exceeds probe limit");
        return render(720, h, fz_matrix{s, 0, 0, s, -bounds.x0*s, -bounds.y0*s});
    }
    std::vector<Line> edgeLines() const {
        std::vector<Line> lines;
        const float height = bounds.y1 - bounds.y0;
        for (auto* b = text->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (auto* l = b->u.t.first_line; l; l = l->next) {
                bool top = l->bbox.y1 < bounds.y0 + height * .15f;
                bool bottom = l->bbox.y0 > bounds.y1 - height * .15f;
                if (!top && !bottom) continue;
                std::string key = top ? "T:" : "B:";
                key += std::to_string((int)std::lround((l->bbox.y0-bounds.y0)/height*100));
                key += ':';
                for (auto* ch=l->first_char; ch; ch=ch->next) {
                    if (ch->c <= 32) continue;
                    const bool numeral = (ch->c >= '0' && ch->c <= '9') ||
                        ch->c==0x4e00 || ch->c==0x4e8c || ch->c==0x4e09 ||
                        ch->c==0x56db || ch->c==0x4e94 || ch->c==0x516d ||
                        ch->c==0x4e03 || ch->c==0x516b || ch->c==0x4e5d ||
                        ch->c==0x5341 || ch->c==0x767e || ch->c==0x96f6;
                    if (numeral) { if(key.back()!='#') key += '#'; continue; }
                    char utf[8]; int n=fz_runetochar(utf, ch->c); key.append(utf,n);
                }
                if (key.back() != ':') lines.push_back({key,l->bbox});
            }
        }
        return lines;
    }
};

struct Cuts { float top=0, bottom=0; };
struct EdgeModel {
    Cuts rasterCuts;
    std::set<std::string> textKeys;
};
EdgeModel detectCuts(fz_context* ctx, fz_document* doc, int count, bool ignoreText,
                     const PdfOcrText& ocr) {
    const int samples=std::min(8,count);
    if (samples<2) return {};
    struct Hit { int count=0; float cut=0; bool top=false; };
    std::map<std::string,Hit> hits;
    for (int i=0;i<samples;++i) {
        const auto found=ocr.find(i);
        Source src(ctx); src.open(doc,i,ignoreText,found==ocr.end() ? nullptr : &found->second);
        std::set<std::string> seen;
        float h=src.bounds.y1-src.bounds.y0;
        for (const auto& line:src.edgeLines()) {
            if (!seen.insert(line.key).second) continue;
            auto& hit=hits[line.key]; ++hit.count;
            hit.top=line.key[0]=='T';
            hit.cut=std::max(hit.cut, hit.top ?
                (line.box.y1-src.bounds.y0+2)/h : (src.bounds.y1-line.box.y0+2)/h);
        }
        if (src.edgeLines().empty()) {
            auto bm=src.probe();
            for (int edge=0;edge<2;++edge) {
                int first=-1,last=-1,gap=0;
                for(int k=0;k<bm.height*15/100;++k) {
                    int y=edge ? bm.height-1-k : k, ink=0;
                    for(int x=0;x<bm.width;++x) ink += bm.pixels[(size_t)y*bm.width+x]<190;
                    if(ink>=3) { if(first<0) first=k; last=k; gap=0; }
                    else if(first>=0 && ++gap>=6) break;
                }
                if(first<0 || gap<6 || last-first<2) continue;
                std::string key=edge?"BI:":"TI:";
                for(int gy=0;gy<8;++gy) for(int gx=0;gx<64;++gx) {
                    int k=first+(last-first)*gy/7, y=edge?bm.height-1-k:k;
                    int ink=0;
                    for(int x=gx*bm.width/64;x<(gx+1)*bm.width/64;++x)
                        ink += bm.pixels[(size_t)y*bm.width+x]<190;
                    key += ink ? '1':'0';
                }
                auto& hit=hits[key]; ++hit.count; hit.top=!edge;
                hit.cut=std::max(hit.cut,(last+2.f)/bm.height);
            }
        }
    }
    EdgeModel out;
    for(const auto& entry:hits) if(entry.second.count>=std::max(2,(samples*3+3)/4)) {
        const auto& h=entry.second;
        if(entry.first.size()>1 && entry.first[1]=='I') {
            if(h.top) out.rasterCuts.top=std::max(out.rasterCuts.top,h.cut);
            else out.rasterCuts.bottom=std::max(out.rasterCuts.bottom,h.cut);
        } else out.textKeys.insert(entry.first);
    }
    return out;
}

Cuts pageCuts(const Source& src, const Bitmap8& probe, const EdgeModel& model) {
    Cuts out=model.rasterCuts;
    const float h=src.bounds.y1-src.bounds.y0;
    for(const auto& line:src.edgeLines()) if(model.textKeys.count(line.key)) {
        if(line.key[0]=='T') out.top=std::max(out.top,(line.box.y1-src.bounds.y0+2)/h);
        else out.bottom=std::max(out.bottom,(src.bounds.y1-line.box.y0+2)/h);
    }
    for(int edge=0;edge<2;++edge) {
        float& cut=edge ? out.bottom : out.top;
        if(cut<=0) continue;
        const int start=std::max(0,(int)std::floor(cut*probe.height)-2);
        const int end=std::min(probe.height,start+(int)std::ceil(8*probe.height/h));
        for(int k=start;k<end;++k) {
            const int y=edge ? probe.height-1-k : k;
            int ink=0;
            for(int x=0;x<probe.width;++x) ink += probe.pixels[(size_t)y*probe.width+x]<190;
            if(ink>probe.width*.55f) cut=std::max(cut,(k+2.f)/probe.height);
        }
    }
    return out;
}

fz_rect cropBody(const Bitmap8& bm, fz_rect page, Cuts cuts, bool trimVertical) {
    const int top=std::clamp((int)std::ceil(cuts.top*bm.height),0,bm.height-1);
    const int bottom=std::clamp((int)std::floor((1-cuts.bottom)*bm.height),top+1,bm.height);
    const auto bounds = detectPdfContentBounds(bm.pixels.data(), bm.width,
                                                bm.height, top, bottom);
    int x0=bounds.left, x1=bounds.right, y0=bounds.top, y1=bounds.bottom;
    if(x1<=x0 || y1<=y0) return fz_empty_rect;
    const float s=(page.x1-page.x0)/bm.width;
    x0=std::max(0,x0); x1=std::min(bm.width,x1);
    if(trimVertical) { y0=std::max(top,y0); y1=std::min(bottom,y1); }
    else { y0=top; y1=bottom; }
    return fz_rect{page.x0+x0*s,page.y0+y0*s,page.x0+x1*s,
        std::min(page.y1,page.y0+y1*s)};
}

struct Breaks { std::vector<int> strict, tight, ink; };

Breaks safeBreaks(Source& src, fz_rect crop, float outputScale, int stripHeight) {
    const float s=720.f/(crop.x1-crop.x0);
    const int h=std::max(1,(int)std::ceil((crop.y1-crop.y0)*s));
    if(h>16384) throw std::runtime_error("PDF crop aspect ratio exceeds probe limit");
    const auto bm=src.render(720,h,fz_matrix{s,0,0,s,-crop.x0*s,-crop.y0*s});
    std::vector<bool> occupied(h,false);
    for(int y=0;y<h;++y) for(int x=0;x<bm.width;++x)
        if(bm.pixels[(size_t)y*bm.width+x]<235) { occupied[y]=true; break; }
    const std::vector<bool> ink=occupied;
    std::vector<bool> tight=occupied;
    auto mark=[&](std::vector<bool>& rows,float top,float bottom) {
        const int y0=std::clamp((int)std::floor((top-crop.y0)*s)-1,0,h);
        const int y1=std::clamp((int)std::ceil((bottom-crop.y0)*s)+1,0,h);
        for(int y=y0;y<y1;++y) rows[y]=true;
    };
    for(auto* b=src.text->first_block;b;b=b->next) {
        if(b->type!=FZ_STEXT_BLOCK_TEXT) continue;
        for(auto* l=b->u.t.first_line;l;l=l->next) {
            if(l->bbox.x1<crop.x0 || l->bbox.x0>crop.x1) continue;
            mark(occupied,l->bbox.y0,l->bbox.y1);
            if(std::fabs(l->dir.y)>0.01f || l->dir.x<=0) { mark(tight,l->bbox.y0,l->bbox.y1); continue; }
            for(auto* ch=l->first_char;ch;ch=ch->next)
                if(ch->c>32 && ch->origin.x>=crop.x0 && ch->origin.x<=crop.x1)
                    mark(tight,ch->origin.y-ch->size*0.72f,ch->origin.y+ch->size*0.22f);
        }
    }
    constexpr int kBlankRows=3;
    auto cuts=[&](const std::vector<bool>& rows) {
        std::vector<int> out;
        for(int y=0;y<h;) {
            while(y<h && rows[y]) ++y;
            if(y>=h) break;
            const int begin=y;
            while(y<h && !rows[y]) ++y;
            const int end=y;
            if(end-begin<kBlankRows) continue;
            const int center=begin+(end-begin)/2;
            const int cut=(int)std::lround(center*outputScale/s);
            if(begin>0 && end<h && cut>0 && cut<stripHeight &&
               (out.empty() || cut>out.back()))
                out.push_back(cut);
        }
        return out;
    };
    return {cuts(occupied),cuts(tight),cuts(ink)};
}

bool clearSeam(Source& src, fz_rect crop, float scale, int width, int offset) {
    constexpr int guard=2;
    if(offset<guard) return false;
    const auto band=src.render(width,2*guard,
        fz_matrix{scale,0,0,scale,-crop.x0*scale,-crop.y0*scale-offset+guard});
    return std::all_of(band.pixels.begin(),band.pixels.end(),
        [](uint8_t pixel) { return pixel>=235; });
}

bool isMajorTitleLine(fz_stext_line* line) {
    int chars[24]{}, count=0;
    for(auto* ch=line->first_char; ch && count<24; ch=ch->next)
        if(ch->c>32) chars[count++]=ch->c;
    if(count>=2 && chars[0]>='0' && chars[0]<='9' &&
       (chars[1]=='.' || chars[1]==')')) return false;
    bool section=count>=2 && chars[0]>=0x4e00 && chars[0]<=0x9fff && chars[1]==0x3001;
    bool chapter=false;
    for(int i=0;i+2<count;++i)
        if(chars[i]==0x6a21 && chars[i+1]==0x62df && chars[i+2]==0x5377) chapter=true;
    return section || chapter;
}

bool startsMajorTitle(Source& src, fz_rect crop) {
    const float limit=crop.y0+160.0f;
    for(auto* b=src.text->first_block;b;b=b->next) {
        if(b->type!=FZ_STEXT_BLOCK_TEXT) continue;
        for(auto* line=b->u.t.first_line;line;line=line->next)
            if(line->bbox.y0>=crop.y0 && line->bbox.y0<limit && isMajorTitleLine(line)) return true;
    }
    return false;
}

void addGlyphs(Source& src, fz_rect crop, float scale, int offset, int height,
               int left, int top, std::vector<PlacedGlyph>& out) {
    for(auto* b=src.text->first_block;b;b=b->next) {
        if(b->type!=FZ_STEXT_BLOCK_TEXT) continue;
        for(auto* l=b->u.t.first_line;l;l=l->next) for(auto* ch=l->first_char;ch;ch=ch->next) {
            const float y=(ch->origin.y-crop.y0)*scale;
            if(ch->c<=0 || y<offset || y>=offset+height ||
               ch->origin.x<crop.x0 || ch->origin.x>=crop.x1) continue;
            PlacedGlyph g{};
            g.x=left+(ch->origin.x-crop.x0)*scale;
            g.y=top+y-offset; g.fontPx=ch->size*scale;
            g.advance=std::hypot(ch->quad.ur.x-ch->quad.ul.x,ch->quad.ur.y-ch->quad.ul.y)*scale;
            g.fontSlot=FontSlot::Regular;
            char utf[8]; int n=fz_runetochar(utf,ch->c); g.sourceUtf8.assign(utf,n);
            out.push_back(std::move(g));
        }
    }
}
}

static ink_status_t runPdfRasterReflowSized(const std::filesystem::path& input,
    const std::filesystem::path& output, const Options& opts,
    const char* jobId, ink_progress_cb progress, void* user, Logger* log) {
    using Clock = std::chrono::steady_clock;
    auto timed = [&](const char* stage, int page, Clock::time_point start) {
        char msg[160];
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
        std::snprintf(msg, sizeof(msg), "timing stage=%s page=%d ms=%lld", stage, page, (long long)ms);
        log->log(LogLevel::Info, "pdf-reflow", msg);
    };
    const auto jobStart = Clock::now();
    auto report=[&](ink_stage_t stage,int percent) { if(progress) progress(jobId,stage,percent,user); };
    const int width=opts.pageWidth-opts.marginLeft-opts.marginRight;
    const int height=opts.pageHeight-opts.marginTop-opts.marginBottom;
    const int physicalHeight=opts.pageHeight-opts.marginTop;
    if(width<1 || height<1 || opts.pageWidth>16384 || opts.pageHeight>16384 ||
       (int64_t)opts.pageWidth*opts.pageHeight>100000000 || opts.pdfMaxOutputPages<0 ||
       opts.pdfPreviewSourcePage < -1 ||
       opts.pdfHeaderCutPercent < -1 || opts.pdfHeaderCutPercent > 40 ||
       opts.pdfFooterCutPercent < -1 || opts.pdfFooterCutPercent > 40)
        return INK_ERR_INVALID_OPTIONS;
    fz_context* ctx=fz_new_context(nullptr,nullptr,32*1024*1024);
    if(!ctx) return INK_ERR_RENDER;
    fz_document* doc=nullptr; fz_outline* outline=nullptr;
    bool opened=false; fz_var(doc); fz_var(outline); fz_var(opened);
    fz_try(ctx) { fz_register_document_handlers(ctx); doc=fz_open_document(ctx,input.string().c_str()); opened=true; }
    fz_catch(ctx) { log->log(LogLevel::Error,"pdf-reflow",fz_caught_message(ctx)); }
    if(!opened) { fz_drop_context(ctx); return INK_ERR_OPEN_INPUT; }
    ink_status_t status=INK_OK;
    try {
        int count=0; bool ok=false; fz_var(count); fz_var(ok);
        fz_try(ctx) { count=fz_count_pages(ctx,doc); outline=fz_load_outline(ctx,doc); ok=true; }
        fz_catch(ctx) {}
        if(!ok || count<1) throw std::runtime_error("PDF page tree read failed");
        report(INK_STAGE_PARSE,0);
        PdfOcrText ocrText;
        if(opts.pdfOcr) {
            PdfOcrRequest request;
            request.compute=true;
            request.ignoreText=opts.pdfIgnoreText;
            request.identity=pdfSourceIdentity(input,opts.sourceIdentity);
            request.cacheDir=pdfOcrCacheDir(opts.ocrCacheDir);
            const int hardware=std::max(1,(int)std::thread::hardware_concurrency());
            request.threads=opts.threadCount>0 ? opts.threadCount : std::min(4,hardware);
            request.jobId=jobId;
            if(opts.pdfPreviewSourcePage>=0)
                request.pages.push_back(std::clamp(opts.pdfPreviewSourcePage,0,count-1));
            else if(opts.pdfMaxOutputPages>0)
                for(int i=0;i<std::min(count,opts.pdfMaxOutputPages*(opts.splitLandscape?2:1)+1);++i)
                    request.pages.push_back(opts.reversePages ? count-1-i : i);
            request.progress=[&](int done,int total) {
                report(INK_STAGE_OCR,total>0 ? 100*done/total : 100);
            };
            const auto ocrStart=Clock::now();
            if(!preparePdfOcr(ctx,doc,request,log,&ocrText)) throw JobCancelled();
            timed("ocr",0,ocrStart);
        }
        EdgeModel edgeModel;
        if(opts.pdfTrim && (opts.pdfHeaderCutPercent<0 || opts.pdfFooterCutPercent<0))
            edgeModel=detectCuts(ctx,doc,count,opts.pdfIgnoreText,ocrText);
        timed("open-and-detect-cuts", 0, jobStart);
        char message[512];
        std::snprintf(message,sizeof(message),
            "shared-v3 verified-seams inputPages=%d target=%dx%d margins=%d,%d,%d,%d content=%dx%d requestedCuts=%d%%,%d%% previewLimit=%d",
            count,opts.pageWidth,opts.pageHeight,opts.marginLeft,opts.marginRight,
            opts.marginTop,opts.marginBottom,width,height,opts.pdfHeaderCutPercent,opts.pdfFooterCutPercent,opts.pdfMaxOutputPages);
        log->log(LogLevel::Info,"pdf-reflow",message);
        report(INK_STAGE_PARSE,100);
        const bool spread=opts.splitLandscape;
        auto sheetOf=[spread](int leaf) { return spread ? leaf/2 : leaf; };
        StreamingPdfWriter pdf(output,spread ? opts.pageWidth*2 : opts.pageWidth,opts.pageHeight,log);
        if(!pdf.open()) throw std::runtime_error("PDF output open failed");
        char title[1024]{},author[1024]{};
        fz_try(ctx) {
            fz_lookup_metadata(ctx,doc,FZ_META_INFO_TITLE,title,sizeof(title));
            fz_lookup_metadata(ctx,doc,FZ_META_INFO_AUTHOR,author,sizeof(author));
        } fz_catch(ctx) {}
        pdf.setMetadata(title,author);
        auto pageImages=PageImageDump::create(opts,output,input,log);
        if(pageImages) pdf.setFileId(pageImages->id());
        PageImageDump* const pageDump=pageImages.get();
        Bitmap8 canvas; canvas.width=opts.pageWidth; canvas.height=opts.pageHeight;
        canvas.pixels.assign((size_t)canvas.width*canvas.height,255);
        Bitmap8 baseCanvas;
        std::vector<ImageRect> canvasPictures;
        int used=0; std::vector<PlacedGlyph> glyphs;
        std::vector<PdfLink> pageLinks;
        std::vector<std::pair<int, std::vector<PdfLink>>> linkBatches;
        std::vector<int> pageMap(count,-1);
        struct Placed { int page; fz_rect src; int leaf; };
        std::vector<Placed> placed;
        std::vector<std::vector<fz_rect>> pageLines(count);
        auto leafAt=[&](int page,float x,float y)->int {
            if(page<0 || page>=count) return -1;
            if(!std::isnan(y)) {
                const bool hasX=!std::isnan(x);
                float py=y+2.f;
                const fz_rect* line=nullptr;
                auto near=[&](const fz_rect& l) { return l.y1>y+1.f && l.y0-y<=48.f; };
                auto dist=[&](const fz_rect& l) {
                    if(!hasX || (x>=l.x0 && x<=l.x1)) return 0.f;
                    return x<l.x0 ? l.x0-x : 1e6f;
                };
                float column=1e9f;
                for(const auto& l:pageLines[page]) if(near(l)) column=std::min(column,dist(l));
                for(const auto& l:pageLines[page])
                    if(near(l) && dist(l)<=column+36.f && (!line || l.y0<line->y0)) line=&l;
                float px=x;
                if(line) { py=(line->y0+line->y1)/2; px=(line->x0+line->x1)/2; }
                const bool hasPx=!std::isnan(px);
                auto gap=[&](const Placed& p) {
                    return hasPx ? std::max(0.f,std::max(p.src.x0-px,px-p.src.x1)) : 0.f;
                };
                const Placed* best=nullptr;
                for(const auto& p:placed)
                    if(p.page==page && py>=p.src.y0 && py<p.src.y1 && (!best || gap(p)<gap(*best)))
                        best=&p;
                if(best) return best->leaf;
                for(const auto& p:placed) if(p.page==page && p.src.y0>=py) return p.leaf;
            }
            return pageMap[page];
        };
        int emitted=0;
        auto full=[&] { return opts.pdfMaxOutputPages>0 && emitted>=opts.pdfMaxOutputPages*(spread?2:1); };
        PdfBitmapOptions image;
        image.trim=false; image.watermarkThreshold=opts.pdfWatermarkThreshold;
        image.gamma=opts.pdfGamma>0 ? std::max(1.5, opts.pdfGamma) : 1.5;
        image.contrast=opts.pdfContrast;
        image.strokeExpand=opts.pdfStroke;
        image.imageContrast=std::clamp(opts.pdfImageContrast,0.0,1.0);
        image.fillTone=std::clamp(opts.pdfFillTone,-1.0,1.0);
        struct Encoded {
            std::vector<uint8_t> jpeg;
            bool ok=false;
            long long filterMs=0, encodeMs=0;
            std::vector<std::string> logs;
        };
        struct BufferLogger : Logger {
            std::vector<std::string>* lines;
            void log(LogLevel, const char* tag, const char* msg) override {
                lines->push_back(std::string("[")+tag+"] "+msg);
            }
        };
        const bool whiteText=opts.whiteText;
        const int quality=opts.jpegQuality;
        auto encodePage=[image,whiteText,quality,pageDump](Bitmap8 page,Bitmap8 base,std::vector<ImageRect> pictures,int leaf) {
            Encoded out;
            BufferLogger buffered; buffered.lines=&out.logs;
            const auto t0=Clock::now();
            PdfBaseLayer layers;
            layers.pixels=&base;
            layers.images=std::move(pictures);
            processPdfBitmap(&page,image,&buffered,base.pixels.empty() ? nullptr : &layers);
            if(whiteText) for(auto& p:page.pixels) p=255-p;
            if(pageDump) pageDump->save(leaf,page);
            const auto t1=Clock::now();
            out.ok=encodeJpegGray(page,quality,&out.jpeg);
            const auto t2=Clock::now();
            out.filterMs=std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();
            out.encodeMs=std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count();
            return out;
        };
        struct Pending {
            std::future<Encoded> result;
            std::vector<PlacedGlyph> glyphs;
            std::vector<PdfLink> links;
            int page=0;
        };
        std::deque<Pending> pending;
        int encodeWorkers=0;
#if HARDCOPY_HAS_PARALLEL
        {
            const int hardware=std::max(1,(int)std::thread::hardware_concurrency());
            const int threads=opts.threadCount>0 ? opts.threadCount : std::min(4,hardware);
            encodeWorkers=threads>1 ? threads-1 : 0;
        }
        std::unique_ptr<ThreadPool> pool;
        if(encodeWorkers>0) pool=std::make_unique<ThreadPool>(encodeWorkers);
#endif
        const size_t maxPending=(size_t)encodeWorkers+1;
        struct Leaf {
            std::vector<uint8_t> jpeg;
            std::vector<PlacedGlyph> glyphs;
            std::vector<PdfLink> links;
        };
        Leaf leftLeaf; bool haveLeft=false;
        auto writeSheet=[&](Leaf& left, Leaf* right) {
            int page=-1;
            if(!spread) {
                page=pdf.addJpegPage(left.jpeg.empty() ? nullptr : left.jpeg.data(),left.jpeg.size(),left.glyphs);
            } else {
                const float leafW=(float)opts.pageWidth;
                if(right) {
                    for(auto& g:right->glyphs) {
                        g.x+=leafW;
                        if(g.hasTextOrigin) g.textX+=leafW;
                        left.glyphs.push_back(std::move(g));
                    }
                    for(auto& link:right->links) {
                        link.x0+=leafW; link.x1+=leafW;
                        left.links.push_back(std::move(link));
                    }
                }
                const bool rightOk=right && !right->jpeg.empty();
                page=pdf.addJpegSpread(left.jpeg.empty() ? nullptr : left.jpeg.data(),left.jpeg.size(),
                    rightOk ? right->jpeg.data() : nullptr,rightOk ? right->jpeg.size() : 0,
                    opts.pageWidth,opts.pageHeight,left.glyphs);
                if(right && !rightOk) page=-1;
            }
            if(page<0) throw std::runtime_error("PDF page write failed");
            if(!left.links.empty()) linkBatches.emplace_back(page,std::move(left.links));
        };
        auto writeOne=[&](Pending& p) {
            Encoded e=p.result.get();
            for(const auto& line:e.logs) log->log(LogLevel::Info,"pdf-filter",line.c_str());
            const auto writeStart=Clock::now();
            Leaf leaf;
            if(e.ok) leaf.jpeg=std::move(e.jpeg);
            leaf.glyphs=std::move(p.glyphs);
            leaf.links=std::move(p.links);
            if(!spread) writeSheet(leaf,nullptr);
            else if(!haveLeft) { leftLeaf=std::move(leaf); haveLeft=true; }
            else { writeSheet(leftLeaf,&leaf); leftLeaf=Leaf{}; haveLeft=false; }
            const long long writeMs=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-writeStart).count();
            std::snprintf(message,sizeof(message),"timing stage=filters page=%d ms=%lld",p.page,e.filterMs);
            log->log(LogLevel::Info,"pdf-reflow",message);
            std::snprintf(message,sizeof(message),"timing stage=jpeg-and-write page=%d ms=%lld",p.page,e.encodeMs+writeMs);
            log->log(LogLevel::Info,"pdf-reflow",message);
        };
        auto drain=[&] {
            while(!pending.empty()) { writeOne(pending.front()); pending.pop_front(); }
        };
        auto flush=[&] {
            if(!used) return;
            Bitmap8 page; page.width=canvas.width; page.height=canvas.height;
            page.pixels.swap(canvas.pixels);
            canvas.pixels.assign((size_t)canvas.width*canvas.height,255);
            Bitmap8 base=std::move(baseCanvas);
            baseCanvas=Bitmap8{};
            std::vector<ImageRect> pictures;
            pictures.swap(canvasPictures);
            Pending p;
            p.glyphs.swap(glyphs);
            p.links.swap(pageLinks);
            p.page=++emitted;
            used=0;
#if HARDCOPY_HAS_PARALLEL
            if(pool) {
                p.result=pool->submit([encodePage,page=std::move(page),base=std::move(base),
                                       pictures=std::move(pictures),leaf=p.page]() mutable {
                    return encodePage(std::move(page),std::move(base),std::move(pictures),leaf);
                });
                pending.push_back(std::move(p));
                while(pending.size()>maxPending) { writeOne(pending.front()); pending.pop_front(); }
                return;
            }
#endif
            std::promise<Encoded> done;
            done.set_value(encodePage(std::move(page),std::move(base),std::move(pictures),p.page));
            p.result=done.get_future();
            writeOne(p);
        };
        auto drawStrip=[&](Source& src,int w,int h,fz_matrix m,int left,int top) {
            src.renderInto(w,h,m,canvas.pixels.data()+(size_t)top*canvas.width+left,canvas.width);
            placed.push_back({src.pageIndex,
                fz_transform_rect(fz_rect{0,0,(float)w,(float)h},fz_invert_matrix(m)),emitted});
            if(!src.baseList) return;
            if(baseCanvas.pixels.empty()) {
                baseCanvas.width=canvas.width; baseCanvas.height=canvas.height;
                baseCanvas.pixels.assign((size_t)canvas.width*canvas.height,255);
            }
            src.renderInto(w,h,m,baseCanvas.pixels.data()+(size_t)top*baseCanvas.width+left,
                           baseCanvas.width,src.baseList);
            for(const auto& pic:src.pictures) {
                const fz_rect r=fz_transform_rect(pic,m);
                ImageRect out;
                out.x0=left+std::clamp((int)std::floor(r.x0),0,w);
                out.x1=left+std::clamp((int)std::ceil(r.x1),0,w);
                out.y0=top+std::clamp((int)std::floor(r.y0),0,h);
                out.y1=top+std::clamp((int)std::ceil(r.y1),0,h);
                if(out.x1>out.x0 && out.y1>out.y0) canvasPictures.push_back(out);
            }
        };
        std::snprintf(message,sizeof(message),"encode-workers=%d",encodeWorkers);
        log->log(LogLevel::Info,"pdf-reflow",message);
        report(INK_STAGE_LAYOUT,100);
        bool haveGutter=false; float gutterPt0=0, gutterPt1=0;
        auto checkCancel=[&] { if(jobCancelled(jobId)) throw JobCancelled(); };
        for(int n=0;n<count && !full();++n) {
            checkCancel();
            if(opts.pdfPreviewSourcePage>=0 && n>0) break;
            int index=opts.pdfPreviewSourcePage>=0 ?
                std::clamp(opts.pdfPreviewSourcePage,0,count-1) :
                (opts.reversePages?count-1-n:n);
            const auto loadStart=Clock::now();
            const auto ocrPage=ocrText.find(index);
            Source src(ctx);
            src.open(doc,index,opts.pdfIgnoreText,ocrPage==ocrText.end() ? nullptr : &ocrPage->second);
            timed("page-load",index+1,loadStart);
            if(opts.pdfImageContrast>=0) {
                const auto layersStart=Clock::now();
                const std::string layers=src.buildBase();
                timed("layers",index+1,layersStart);
                std::snprintf(message,sizeof(message),"layers page=%d %s",index+1,layers.c_str());
                log->log(LogLevel::Info,"pdf-reflow",message);
            }
            const auto probeStart=Clock::now();
            const auto probe=src.probe();
            Cuts cuts=pageCuts(src,probe,edgeModel);
            if(opts.pdfHeaderCutPercent>=0) cuts.top=opts.pdfHeaderCutPercent/100.f;
            if(opts.pdfFooterCutPercent>=0) cuts.bottom=opts.pdfFooterCutPercent/100.f;
            std::snprintf(message,sizeof(message),"page=%d effectiveCuts=%.3f%%,%.3f%%",index+1,cuts.top*100,cuts.bottom*100);
            log->log(LogLevel::Info,"pdf-reflow",message);
            fz_rect crop=cropBody(probe,src.bounds,cuts,opts.pdfTrim);
            timed("probe-and-crop",index+1,probeStart);
            if(fz_is_empty_rect(crop)) continue;
            std::vector<PdfLink> sourceLinks;
            src.links(&sourceLinks);
            for(auto* b=src.text->first_block;b;b=b->next) {
                if(b->type!=FZ_STEXT_BLOCK_TEXT) continue;
                for(auto* l=b->u.t.first_line;l;l=l->next) {
                    if(l->dir.x<=0.99f) continue;
                    bool ink=false;
                    for(auto* ch=l->first_char;ch && !ink;ch=ch->next) ink=ch->c>32;
                    if(ink) pageLines[index].push_back(l->bbox);
                }
            }
            const bool majorTitleAtStart=startsMajorTitle(src,crop);
            if(majorTitleAtStart && used>0) flush();
            struct Region { fz_rect rect; float fit; const char* kind; };
            std::vector<Region> regions;
            if(opts.pdfColumns>0) {
                const float ps=probe.width/(src.bounds.x1-src.bounds.x0);
                ColumnDetectInput in;
                in.pixels=probe.pixels.data(); in.width=probe.width; in.height=probe.height;
                in.pxPerPt=ps;
                in.body={(crop.x0-src.bounds.x0)*ps,(crop.y0-src.bounds.y0)*ps,
                         (crop.x1-src.bounds.x0)*ps,(crop.y1-src.bounds.y0)*ps};
                for(auto* b=src.text->first_block;b;b=b->next) {
                    if(b->type!=FZ_STEXT_BLOCK_TEXT) continue;
                    for(auto* l=b->u.t.first_line;l;l=l->next)
                        in.lines.push_back({(l->bbox.x0-src.bounds.x0)*ps,(l->bbox.y0-src.bounds.y0)*ps,
                                            (l->bbox.x1-src.bounds.x0)*ps,(l->bbox.y1-src.bounds.y0)*ps});
                }
                in.hasGutterHint=haveGutter;
                in.gutterHint={gutterPt0*ps,0,gutterPt1*ps,0};
                std::string why;
                ColumnRect gutter{};
                const auto detectStart=Clock::now();
                const auto found=detectColumnRegions(in,&why,&gutter);
                timed("columns-detect",index+1,detectStart);
                if(!found.empty()) { haveGutter=true; gutterPt0=gutter.x0/ps; gutterPt1=gutter.x1/ps; }
                for(const auto& r:found) {
                    fz_rect rect{src.bounds.x0+r.rect.x0/ps,src.bounds.y0+r.rect.y0/ps,
                                 src.bounds.x0+r.rect.x1/ps,src.bounds.y0+r.rect.y1/ps};
                    rect=fz_intersect_rect(rect,crop);
                    if(!fz_is_empty_rect(rect)) regions.push_back({rect,r.fitWidth/ps,columnRegionKindName(r.kind)});
                }
                std::snprintf(message,sizeof(message),"columns page=%d %s",index+1,why.c_str());
                log->log(LogLevel::Info,"pdf-reflow",message);
            }
            if(regions.empty()) regions.push_back({crop,crop.x1-crop.x0,"body"});
            pageMap[index]=emitted;
            for(size_t ri=0;ri<regions.size() && !full();++ri) {
                checkCancel();
                const fz_rect crop=regions[ri].rect;
                float scale=width/regions[ri].fit;
                bool fitted=false;
                if(!opts.pdfTrim && regions.size()==1 &&
                   (crop.y1-crop.y0)*scale>physicalHeight) {
                    scale=(height-0.5f)/(crop.y1-crop.y0); fitted=true;
                }
                const int regionWidth=std::clamp((int)std::ceil((crop.x1-crop.x0)*scale),1,width);
                const int regionLeft=opts.marginLeft+(fitted ? (width-regionWidth)/2 : 0);
                const double h=std::ceil((crop.y1-crop.y0)*scale);
                if(h>10000000) throw std::runtime_error("PDF content aspect ratio exceeds strip limit");
                const int scaledHeight=std::max(1,(int)h);
                const auto breaksStart=Clock::now();
                const auto breaks=safeBreaks(src,crop,scale,scaledHeight);
                timed("safe-breaks",index+1,breaksStart);
                if(regions.size()==1)
                    std::snprintf(message,sizeof(message),
                        "page=%d sourcePt=%.2fx%.2f cropPt=%.2f,%.2f,%.2f,%.2f scale=%.6f strip=%dx%d",
                        index+1,src.bounds.x1-src.bounds.x0,src.bounds.y1-src.bounds.y0,
                        crop.x0,crop.y0,crop.x1,crop.y1,scale,regionWidth,scaledHeight);
                else
                    std::snprintf(message,sizeof(message),
                        "page=%d region=%d/%d kind=%s sourcePt=%.2fx%.2f cropPt=%.2f,%.2f,%.2f,%.2f scale=%.6f strip=%dx%d",
                        index+1,(int)ri+1,(int)regions.size(),regions[ri].kind,
                        src.bounds.x1-src.bounds.x0,src.bounds.y1-src.bounds.y0,
                        crop.x0,crop.y0,crop.x1,crop.y1,scale,regionWidth,scaledHeight);
                log->log(LogLevel::Info,"pdf-reflow",message);
                struct Range { int y0, y1, gap; };
                std::vector<Range> ranges;
                int leadGap=0;
                if(opts.pdfLineHeight>0 && scaledHeight<=20000) {
                    const auto spacingStart=Clock::now();
                    const auto region=src.render(regionWidth,scaledHeight,
                        fz_matrix{scale,0,0,scale,-crop.x0*scale,-crop.y0*scale});
                    SpacingInput si;
                    si.pixels=region.pixels.data(); si.width=region.width;
                    si.height=region.height; si.stride=region.width;
                    si.lineHeight=opts.pdfLineHeight; si.paraSpacingEm=opts.pdfParaSpacingEm;
                    for(auto* b=src.text->first_block;b;b=b->next) {
                        if(b->type!=FZ_STEXT_BLOCK_TEXT) continue;
                        for(auto* l=b->u.t.first_line;l;l=l->next) {
                            if(l->dir.x<0.99f || std::fabs(l->dir.y)>0.02f) continue;
                            for(auto* ch=l->first_char;ch;ch=ch->next) {
                                if(ch->c<=32 || ch->origin.x<crop.x0 || ch->origin.x>=crop.x1 ||
                                   ch->origin.y<crop.y0 || ch->origin.y>crop.y1+ch->size) continue;
                                SpacingGlyph g;
                                g.x=(ch->origin.x-crop.x0)*scale;
                                g.baseline=(ch->origin.y-crop.y0)*scale;
                                g.size=ch->size*scale;
                                g.advance=std::hypot(ch->quad.ur.x-ch->quad.ul.x,
                                                     ch->quad.ur.y-ch->quad.ul.y)*scale;
                                si.glyphs.push_back(g);
                            }
                        }
                    }
                    std::string why;
                    const auto plan=planLineSpacing(si,&why);
                    for(const auto& piece:plan.pieces) ranges.push_back({piece.y0,piece.y1,piece.gapBefore});
                    leadGap=plan.leadGap;
                    timed("line-spacing",index+1,spacingStart);
                    std::snprintf(message,sizeof(message),"line-spacing page=%d region=%d pieces=%d %s",
                        index+1,(int)ri+1,(int)ranges.size(),why.c_str());
                    log->log(LogLevel::Info,"pdf-reflow",message);
                }
                const bool spaced=!ranges.empty();
                if(!spaced) ranges.push_back({0,scaledHeight,0});
                for(size_t pi=0;pi<ranges.size() && !full();++pi) {
                const int rangeEnd=ranges[pi].y1;
                if(spaced && used>0) {
                    const int gap=pi==0 ? leadGap : ranges[pi].gap;
                    const int pieceHeight=rangeEnd-ranges[pi].y0;
                    if(used+gap+pieceHeight<=physicalHeight) used+=gap;
                    else if(pieceHeight<=physicalHeight) flush();
                    else used=std::min(physicalHeight,used+gap);
                }
                const bool firstSlice=ri==0 && pi==0;
                for(int offset=ranges[pi].y0;offset<rangeEnd && !full();) {
                    const int reservedRemaining=std::max(0,height-used);
                    const int sourceRemaining=rangeEnd-offset;
                    const int available=std::max(0,physicalHeight-used);
                    const int hardTake=std::min(available,sourceRemaining);
                    const bool sourceRemainderFits = sourceRemaining <= available;
                    int take=hardTake;
                    if(!sourceRemainderFits) {
                        const auto seamStart=Clock::now();
                        int seamChecks=0;
                        auto pick=[&](const std::vector<int>& list,int floorTake) {
                            auto it=std::upper_bound(list.begin(),list.end(),offset+hardTake);
                            while(it!=list.begin()) {
                                const int boundary=*--it;
                                if(boundary<=offset+floorTake) break;
                                ++seamChecks;
                                if(clearSeam(src,crop,scale,regionWidth,boundary)) return boundary-offset;
                            }
                            return floorTake;
                        };
                        take=pick(breaks.strict,0);
                        const int enough=opts.pdfColumns>0 ? hardTake*9/10 : (used==0 ? 1 : 0);
                        const char* rule="strict";
                        if(take<enough) { const int t=pick(breaks.tight,take); if(t>take) { take=t; rule="tight"; } }
                        if(take<enough) { const int t=pick(breaks.ink,take); if(t>take) { take=t; rule="ink"; } }
                        timed("seam-search",index+1,seamStart);
                        std::snprintf(message,sizeof(message),"seam-checks page=%d n=%d",index+1,seamChecks);
                        log->log(LogLevel::Info,"pdf-reflow",message);
                        if(take<=0) {
                            if(used>0) { flush(); continue; }
                            fz_rect rest=crop; rest.y0+=offset/scale;
                            if(spaced) rest.y1=std::min(crop.y1,crop.y0+rangeEnd/scale);
                            const float fit=std::min(scale,physicalHeight/(rest.y1-rest.y0));
                            const int fw=std::min(width,std::max(1,(int)std::floor((rest.x1-rest.x0)*fit)));
                            const int fh=std::min(physicalHeight,std::max(1,(int)std::floor((rest.y1-rest.y0)*fit)));
                            const int left=opts.marginLeft+(width-fw)/2;
                            if(offset==ranges[0].y0 && firstSlice) pageMap[index]=emitted;
                            drawStrip(src,fw,fh,fz_matrix{fit,0,0,fit,-rest.x0*fit,-rest.y0*fit},
                                left,opts.marginTop);
                            if(opts.embedTextLayer) addGlyphs(src,rest,fit,0,fh,left,opts.marginTop,glyphs);
                            for (const auto& sourceLink : sourceLinks) {
                                const float x0 = std::max(sourceLink.x0, rest.x0);
                                const float y0 = std::max(sourceLink.y0, rest.y0);
                                const float x1 = std::min(sourceLink.x1, rest.x1);
                                const float y1 = std::min(sourceLink.y1, rest.y1);
                                if (x1 <= x0 || y1 <= y0) continue;
                                PdfLink link = sourceLink;
                                link.x0 = left + (x0 - rest.x0) * fit;
                                link.x1 = left + (x1 - rest.x0) * fit;
                                link.y0 = opts.marginTop + (y0 - rest.y0) * fit;
                                link.y1 = opts.marginTop + (y1 - rest.y0) * fit;
                                pageLinks.push_back(std::move(link));
                            }
                            used=fh; offset=rangeEnd;
                            log->log(LogLevel::Info,"pdf-reflow","oversize intact region fitted to one output page");
                            flush(); continue;
                        }
                        std::snprintf(message,sizeof(message),
                            "safeBreak page=%d offset=%d take=%d hard=%d sourceY=%.2f rule=%s",
                            index+1, offset, take, hardTake, crop.y0 + (offset+take)/scale, rule);
                        log->log(LogLevel::Info,"pdf-reflow",message);
                    }
                    if(take<1 || take>hardTake || used+take>physicalHeight)
                        throw std::runtime_error("PDF strip exceeds target space");
                    std::snprintf(message,sizeof(message),
                        "capacity output=%d used=%d reserved=%d physical=%d bottomMarginReleased=%s",
                        emitted+1,used,height,physicalHeight,
                        take>reservedRemaining ? "true" : "false");
                    log->log(LogLevel::Info,"pdf-reflow",message);
                    if(offset==ranges[0].y0 && firstSlice) pageMap[index]=emitted;
                    std::snprintf(message,sizeof(message),
                        "slice source=%d output=%d offset=%d take=%d used=%d scale=%.8f crop=%.5f,%.5f,%.5f,%.5f",
                        index+1,emitted+1,offset,take,used,scale,crop.x0,crop.y0,crop.x1,crop.y1);
                    log->log(LogLevel::Info,"pdf-reflow",message);
                    const auto renderStart=Clock::now();
                    drawStrip(src,regionWidth,take,
                        fz_matrix{scale,0,0,scale,-crop.x0*scale,-crop.y0*scale-offset},
                        regionLeft,opts.marginTop+used);
                    timed("render-strip",index+1,renderStart);
                    if(opts.embedTextLayer) addGlyphs(src,crop,scale,offset,take,regionLeft,opts.marginTop+used,glyphs);
                    const float sliceY0 = crop.y0 + offset / scale;
                    const float sliceY1 = sliceY0 + take / scale;
                    for (const auto& sourceLink : sourceLinks) {
                        const float x0 = std::max(sourceLink.x0, crop.x0);
                        const float y0 = std::max(sourceLink.y0, sliceY0);
                        const float x1 = std::min(sourceLink.x1, crop.x1);
                        const float y1 = std::min(sourceLink.y1, sliceY1);
                        if (x1 <= x0 || y1 <= y0) continue;
                        PdfLink link = sourceLink;
                        link.x0 = regionLeft + (x0 - crop.x0) * scale;
                        link.x1 = regionLeft + (x1 - crop.x0) * scale;
                        link.y0 = opts.marginTop + used + (y0 - crop.y0) * scale - offset;
                        link.y1 = opts.marginTop + used + (y1 - crop.y0) * scale - offset;
                        pageLinks.push_back(std::move(link));
                    }
                    used+=take; offset+=take;
                    if(used>=physicalHeight || !sourceRemainderFits) flush();
                }
                }
            }
            if(!opts.pdfTrim) flush();
            report(INK_STAGE_RENDER,100*(n+1)/count);
        }
        if(!full()) flush();
        if(emitted==0) { used=1; flush(); }
        drain();
        if(haveLeft) { writeSheet(leftLeaf,nullptr); haveLeft=false; }
        for (auto& batch : linkBatches) {
            for (auto& link : batch.second) {
                if (link.targetPage0 >= 0 && link.targetPage0 < count) {
                    const int leaf = link.hasTargetPoint
                        ? leafAt(link.targetPage0, link.targetX, link.targetY)
                        : pageMap[link.targetPage0];
                    link.targetPage0 = leaf < 0 ? -1 : sheetOf(leaf);
                }
            }
            pdf.addLinks(batch.first, batch.second);
        }
        if(opts.embedBookmarks) {
            auto walk=[&](auto&& self,fz_outline* node,int level)->void {
                for(;node;node=node->next) {
                    int source=-1; float x=node->x, y=node->y;
                    fz_var(source); fz_var(x); fz_var(y);
                    fz_try(ctx) {
                        fz_location location=node->uri?fz_resolve_link(ctx,doc,node->uri,&x,&y):node->page;
                        source=fz_page_number_from_location(ctx,doc,location);
                    } fz_catch(ctx) {}
                    const int leaf=source>=0 && source<count ? leafAt(source,x,y) : -1;
                    if(leaf>=0 && node->title)
                        pdf.addOutline(node->title,level,sheetOf(leaf));
                    self(self,node->down,level+1);
                }
            };
            walk(walk,outline,1);
        }
        report(INK_STAGE_PACKAGE,50);
        const auto closeStart=Clock::now();
        if(!pdf.close()) throw std::runtime_error("PDF finalization failed");
        if(pageImages) pageImages->finish(pdf,spread ? 2 : 1);
        timed("finalize",0,closeStart);
        timed("total",0,jobStart);
        std::snprintf(message,sizeof(message),"shared-v3 outputPages=%d",pdf.pageCount());
        log->log(LogLevel::Info,"pdf-reflow",message);
        report(INK_STAGE_DONE,100);
    } catch(const JobCancelled&) {
        log->log(LogLevel::Info,"pdf-reflow","cancelled"); status=INK_ERR_CANCELLED;
    } catch(const std::exception& e) {
        log->log(LogLevel::Error,"pdf-reflow",e.what()); status=INK_ERR_RENDER;
    }
    if(outline) fz_drop_outline(ctx,outline);
    fz_drop_document(ctx,doc); fz_drop_context(ctx);
    return status;
}

static bool sourceMostlyLandscape(const std::filesystem::path& input, Logger* log) {
    fz_context* ctx=fz_new_context(nullptr,nullptr,16*1024*1024);
    if(!ctx) return false;
    fz_document* doc=nullptr; fz_var(doc);
    int landscape=0, portrait=0;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc=fz_open_document(ctx,input.string().c_str());
        const int count=fz_count_pages(ctx,doc);
        const int samples=std::min(count,40);
        for(int i=0;i<samples;++i) {
            const int index=samples<count ? (int)((int64_t)i*count/samples) : i;
            fz_page* page=nullptr; fz_var(page);
            fz_try(ctx) {
                page=fz_load_page(ctx,doc,index);
                const fz_rect b=fz_bound_page(ctx,page);
                const float w=b.x1-b.x0, h=b.y1-b.y0;
                if(w>h*1.05f) ++landscape; else if(h>w*1.05f) ++portrait;
            } fz_always(ctx) { fz_drop_page(ctx,page); }
            fz_catch(ctx) {}
        }
    } fz_always(ctx) { fz_drop_document(ctx,doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    char message[128];
    std::snprintf(message,sizeof(message),"orientation probe landscape=%d portrait=%d",landscape,portrait);
    log->log(LogLevel::Info,"pdf-reflow",message);
    return landscape>portrait;
}

ink_status_t runPdfRasterReflow(const std::filesystem::path& input,
    const std::filesystem::path& output, const Options& opts,
    const char* jobId, ink_progress_cb progress, void* user, Logger* log) {
    if(!opts.pdfAutoOrientation || opts.splitLandscape || opts.pageWidth==opts.pageHeight)
        return runPdfRasterReflowSized(input,output,opts,jobId,progress,user,log);
    const bool targetLandscape=opts.pageWidth>opts.pageHeight;
    if(sourceMostlyLandscape(input,log)==targetLandscape)
        return runPdfRasterReflowSized(input,output,opts,jobId,progress,user,log);
    Options turned=opts;
    std::swap(turned.pageWidth,turned.pageHeight);
    char message[128];
    std::snprintf(message,sizeof(message),"orientation follows source: %dx%d -> %dx%d",
        opts.pageWidth,opts.pageHeight,turned.pageWidth,turned.pageHeight);
    log->log(LogLevel::Info,"pdf-reflow",message);
    return runPdfRasterReflowSized(input,output,turned,jobId,progress,user,log);
}
}
