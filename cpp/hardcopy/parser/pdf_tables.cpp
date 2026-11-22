#include "pdf_tables.h"

extern "C" {
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>

namespace hardcopy {
namespace {

struct EvidenceDevice {
    fz_device super;
    fz_device* target;
    PdfTableEvidence* ev;
};

EvidenceDevice* evd(fz_device* dev) { return reinterpret_cast<EvidenceDevice*>(dev); }

constexpr float kThinRule = 2.5f;
constexpr float kMinRule = 4.0f;

bool nearWhite(fz_context* ctx, fz_colorspace* cs, const float* color) {
    if (!cs || !color) return false;
    float gray = 0;
    bool ok = false;
    fz_var(ok);
    fz_try(ctx) {
        fz_convert_color(ctx, cs, color, fz_device_gray(ctx), &gray, nullptr,
                         fz_default_color_params);
        ok = true;
    } fz_catch(ctx) {}
    return ok && gray >= 0.95f;
}

struct SubPath {
    std::vector<fz_point> pts;
    bool curved = false;
    bool closed = false;
};

struct PathCollect {
    fz_matrix ctm;
    std::vector<SubPath> subs;
    void add(float x, float y) {
        if (subs.empty()) subs.emplace_back();
        subs.back().pts.push_back(fz_transform_point_xy(x, y, ctm));
    }
};

void pcMove(fz_context*, void* arg, float x, float y) {
    auto* p = static_cast<PathCollect*>(arg);
    p->subs.emplace_back();
    p->add(x, y);
}
void pcLine(fz_context*, void* arg, float x, float y) { static_cast<PathCollect*>(arg)->add(x, y); }
void pcCurve(fz_context*, void* arg, float, float, float, float, float x3, float y3) {
    auto* p = static_cast<PathCollect*>(arg);
    p->add(x3, y3);
    p->subs.back().curved = true;
}
void pcClose(fz_context*, void* arg) {
    auto* p = static_cast<PathCollect*>(arg);
    if (!p->subs.empty()) p->subs.back().closed = true;
}

std::vector<SubPath> subpaths(fz_context* ctx, const fz_path* path, fz_matrix ctm) {
    PathCollect pc{ctm, {}};
    fz_path_walker walker{};
    walker.moveto = pcMove;
    walker.lineto = pcLine;
    walker.curveto = pcCurve;
    walker.closepath = pcClose;
    fz_walk_path(ctx, path, &walker, &pc);
    return std::move(pc.subs);
}

fz_rect pointsBounds(const std::vector<fz_point>& pts) {
    fz_rect r{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
    for (const auto& p : pts) {
        r.x0 = std::min(r.x0, p.x); r.y0 = std::min(r.y0, p.y);
        r.x1 = std::max(r.x1, p.x); r.y1 = std::max(r.y1, p.y);
    }
    return r;
}

bool hasDiagonal(const SubPath& s) {
    if (s.curved) return true;
    for (size_t i = 0; i + 1 < s.pts.size(); ++i) {
        const fz_point a = s.pts[i], b = s.pts[i + 1];
        if (std::fabs(a.x - b.x) > 0.5f && std::fabs(a.y - b.y) > 0.5f) return true;
    }
    return false;
}

bool isAxisRect(const SubPath& s) {
    if (s.curved || s.pts.size() < 4 || s.pts.size() > 5) return false;
    for (size_t i = 0; i < s.pts.size(); ++i) {
        const fz_point a = s.pts[i], b = s.pts[(i + 1) % s.pts.size()];
        if (std::fabs(a.x - b.x) > 0.5f && std::fabs(a.y - b.y) > 0.5f) return false;
    }
    return true;
}

void addRule(PdfTableEvidence* ev, float x0, float y0, float x1, float y1) {
    ev->rules.push_back({std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)});
}

void recordFill(fz_context* ctx, PdfTableEvidence* ev, const fz_path* path, fz_matrix ctm,
                fz_colorspace* cs, const float* color, float alpha) {
    if (alpha <= 0.05f || nearWhite(ctx, cs, color)) return;
    for (const auto& s : subpaths(ctx, path, ctm)) {
        if (s.pts.size() < 2) continue;
        const fz_rect b = pointsBounds(s.pts);
        const float w = b.x1 - b.x0, h = b.y1 - b.y0;
        if (hasDiagonal(s)) {
            if (std::max(w, h) >= 6.0f) ev->artwork.push_back(b);
            continue;
        }
        if (std::min(w, h) <= kThinRule && std::max(w, h) >= kMinRule) {
            addRule(ev, b.x0, b.y0, b.x1, b.y1);
        } else if (isAxisRect(s) && w >= kMinRule && h >= kMinRule) {
            addRule(ev, b.x0, b.y0, b.x1, b.y0);
            addRule(ev, b.x0, b.y1, b.x1, b.y1);
            addRule(ev, b.x0, b.y0, b.x0, b.y1);
            addRule(ev, b.x1, b.y0, b.x1, b.y1);
        }
    }
}

void recordStroke(fz_context* ctx, PdfTableEvidence* ev, const fz_path* path,
                  const fz_stroke_state* stroke, fz_matrix ctm,
                  fz_colorspace* cs, const float* color, float alpha) {
    if (alpha <= 0.05f || nearWhite(ctx, cs, color)) return;
    const float width = stroke ? stroke->linewidth * fz_matrix_expansion(ctm) : 1.0f;
    if (width > 4.0f) return;
    for (const auto& s : subpaths(ctx, path, ctm)) {
        if (s.pts.size() >= 2 && hasDiagonal(s)) {
            const fz_rect b = pointsBounds(s.pts);
            if (std::max(b.x1 - b.x0, b.y1 - b.y0) >= 6.0f) ev->artwork.push_back(b);
            continue;
        }
        const size_t n = s.pts.size();
        for (size_t i = 0; i < n; ++i) {
            if (i + 1 == n && !(s.closed && n > 2)) break;
            const fz_point a = s.pts[i], b = s.pts[(i + 1) % n];
            if (std::fabs(a.y - b.y) <= 0.5f && std::fabs(a.x - b.x) >= kMinRule)
                addRule(ev, a.x, a.y, b.x, a.y);
            else if (std::fabs(a.x - b.x) <= 0.5f && std::fabs(a.y - b.y) >= kMinRule)
                addRule(ev, a.x, a.y, a.x, b.y);
        }
    }
}

int currentSegment(PdfTableEvidence* ev) {
    if (ev->newSegment || ev->segments.empty()) {
        auto found = ev->pathIds.find(ev->stack);
        int id = 0;
        if (found != ev->pathIds.end()) {
            id = found->second;
        } else {
            id = (int)ev->paths.size();
            ev->paths.push_back(ev->stack);
            ev->pathIds.emplace(ev->stack, id);
        }
        ev->segments.push_back({ev->fresh, id});
        ev->newSegment = false;
    }
    return (int)ev->segments.size() - 1;
}

void recordText(fz_context* ctx, PdfTableEvidence* ev, const fz_text* text, fz_matrix ctm,
                bool invisible = false) {
    if (!text) return;
    const int segment = currentSegment(ev);
    for (const fz_text_span* span = text->head; span; span = span->next) {
        for (int i = 0; i < span->len; ++i) {
            const fz_text_item& item = span->items[i];
            if (item.ucs < 0 || item.ucs == 0xFFFD) continue;
            if (item.ucs > 32) {
                ++ev->textChars;
                ev->invisibleChars += invisible;
            }
            fz_matrix trm = span->trm;
            trm.e = item.x;
            trm.f = item.y;
            const fz_matrix m = fz_concat(trm, ctm);
            const float size = fz_matrix_expansion(m);
            float advance = 0.5f;
            if (item.gid >= 0) advance = fz_advance_glyph(ctx, span->font, item.gid, span->wmode);
            const fz_point a = fz_transform_vector(fz_make_point(advance, 0), m);
            PdfGlyph g;
            g.x = m.e;
            g.y = m.f;
            g.ucs = item.ucs;
            g.segment = segment;
            g.box = {std::min(m.e, m.e + a.x), m.f - 0.8f * size,
                     std::max(m.e, m.e + a.x), m.f + 0.25f * size};
            ev->textGlyphs.push_back(g);
        }
    }
}

void recordImage(PdfTableEvidence* ev, fz_matrix ctm) {
    PdfImageMark mark;
    mark.box = fz_transform_rect(fz_unit_rect, ctm);
    mark.segment = currentSegment(ev);
    ev->images.push_back(mark);
}

void dFillPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm,
               fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    recordFill(ctx, evd(dev)->ev, path, ctm, cs, color, alpha);
    fz_fill_path(ctx, evd(dev)->target, path, evenOdd, ctm, cs, color, alpha, cp);
}
void dStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                 fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    recordStroke(ctx, evd(dev)->ev, path, stroke, ctm, cs, color, alpha);
    fz_stroke_path(ctx, evd(dev)->target, path, stroke, ctm, cs, color, alpha, cp);
}
void dClipPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm, fz_rect scissor) {
    fz_clip_path(ctx, evd(dev)->target, path, evenOdd, ctm, scissor);
}
void dClipStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                     fz_matrix ctm, fz_rect scissor) {
    fz_clip_stroke_path(ctx, evd(dev)->target, path, stroke, ctm, scissor);
}
void dFillText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
               fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    recordText(ctx, evd(dev)->ev, text, ctm);
    fz_fill_text(ctx, evd(dev)->target, text, ctm, cs, color, alpha, cp);
}
void dStrokeText(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                 fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    recordText(ctx, evd(dev)->ev, text, ctm);
    fz_stroke_text(ctx, evd(dev)->target, text, stroke, ctm, cs, color, alpha, cp);
}
void dClipText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect scissor) {
    recordText(ctx, evd(dev)->ev, text, ctm);
    fz_clip_text(ctx, evd(dev)->target, text, ctm, scissor);
}
void dClipStrokeText(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                     fz_matrix ctm, fz_rect scissor) {
    recordText(ctx, evd(dev)->ev, text, ctm);
    fz_clip_stroke_text(ctx, evd(dev)->target, text, stroke, ctm, scissor);
}
void dIgnoreText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm) {
    recordText(ctx, evd(dev)->ev, text, ctm, true);
    fz_ignore_text(ctx, evd(dev)->target, text, ctm);
}
void dFillShade(fz_context* ctx, fz_device* dev, fz_shade* shade, fz_matrix ctm, float alpha, fz_color_params cp) {
    fz_fill_shade(ctx, evd(dev)->target, shade, ctm, alpha, cp);
}
void dFillImage(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha, fz_color_params cp) {
    recordImage(evd(dev)->ev, ctm);
    fz_fill_image(ctx, evd(dev)->target, image, ctm, alpha, cp);
}
void dFillImageMask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                    fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    recordImage(evd(dev)->ev, ctm);
    fz_fill_image_mask(ctx, evd(dev)->target, image, ctm, cs, color, alpha, cp);
}
void dClipImageMask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_rect scissor) {
    fz_clip_image_mask(ctx, evd(dev)->target, image, ctm, scissor);
}
void dPopClip(fz_context* ctx, fz_device* dev) { fz_pop_clip(ctx, evd(dev)->target); }
void dBeginMask(fz_context* ctx, fz_device* dev, fz_rect area, int luminosity, fz_colorspace* cs,
                const float* bc, fz_color_params cp) {
    fz_begin_mask(ctx, evd(dev)->target, area, luminosity, cs, bc, cp);
}
void dEndMask(fz_context* ctx, fz_device* dev, fz_function* fn) { fz_end_mask_tr(ctx, evd(dev)->target, fn); }
void dBeginGroup(fz_context* ctx, fz_device* dev, fz_rect area, fz_colorspace* cs, int isolated,
                 int knockout, int blendmode, float alpha) {
    fz_begin_group(ctx, evd(dev)->target, area, cs, isolated, knockout, blendmode, alpha);
}
void dEndGroup(fz_context* ctx, fz_device* dev) { fz_end_group(ctx, evd(dev)->target); }
int dBeginTile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep, float ystep,
               fz_matrix ctm, int id) {
    return fz_begin_tile_id(ctx, evd(dev)->target, area, view, xstep, ystep, ctm, id);
}
void dEndTile(fz_context* ctx, fz_device* dev) { fz_end_tile(ctx, evd(dev)->target); }
void dRenderFlags(fz_context* ctx, fz_device* dev, int set, int clear) {
    fz_render_flags(ctx, evd(dev)->target, set, clear);
}
void dDefaultColorspaces(fz_context* ctx, fz_device* dev, fz_default_colorspaces* cs) {
    fz_set_default_colorspaces(ctx, evd(dev)->target, cs);
}
void dBeginLayer(fz_context* ctx, fz_device* dev, const char* name) { fz_begin_layer(ctx, evd(dev)->target, name); }
void dEndLayer(fz_context* ctx, fz_device* dev) { fz_end_layer(ctx, evd(dev)->target); }
void dBeginStructure(fz_context* ctx, fz_device* dev, fz_structure standard, const char* raw, int idx) {
    auto* ev = evd(dev)->ev;
    ev->stack.emplace_back((int)standard, idx);
    ++ev->structureCalls;
    ev->fresh = true;
    ev->newSegment = true;
    fz_begin_structure(ctx, evd(dev)->target, standard, raw, idx);
}
void dEndStructure(fz_context* ctx, fz_device* dev) {
    auto* ev = evd(dev)->ev;
    if (!ev->stack.empty()) ev->stack.pop_back();
    ev->fresh = false;
    ev->newSegment = true;
    fz_end_structure(ctx, evd(dev)->target);
}
void dBeginMetatext(fz_context* ctx, fz_device* dev, fz_metatext meta, const char* text) {
    fz_begin_metatext(ctx, evd(dev)->target, meta, text);
}
void dEndMetatext(fz_context* ctx, fz_device* dev) { fz_end_metatext(ctx, evd(dev)->target); }

struct Seg { float pos, a, b; };

std::vector<Seg> mergeSegments(std::vector<Seg> segs) {
    std::sort(segs.begin(), segs.end(), [](const Seg& l, const Seg& r) {
        return l.pos < r.pos || (l.pos == r.pos && l.a < r.a);
    });
    std::vector<Seg> out;
    for (size_t i = 0; i < segs.size();) {
        size_t j = i;
        double sum = 0;
        while (j < segs.size() && segs[j].pos - segs[i].pos <= 1.5f) sum += segs[j++].pos;
        const float pos = (float)(sum / (double)(j - i));
        std::vector<Seg> group(segs.begin() + (long)i, segs.begin() + (long)j);
        std::sort(group.begin(), group.end(), [](const Seg& l, const Seg& r) { return l.a < r.a; });
        Seg cur{pos, group[0].a, group[0].b};
        for (size_t k = 1; k < group.size(); ++k) {
            if (group[k].a <= cur.b + 1.5f) cur.b = std::max(cur.b, group[k].b);
            else { out.push_back(cur); cur = {pos, group[k].a, group[k].b}; }
        }
        out.push_back(cur);
        i = j;
    }
    return out;
}

std::vector<float> clusterPositions(std::vector<float> v, float tol) {
    std::sort(v.begin(), v.end());
    std::vector<float> out;
    for (size_t i = 0; i < v.size();) {
        size_t j = i;
        double sum = 0;
        while (j < v.size() && v[j] - v[i] <= tol) sum += v[j++];
        out.push_back((float)(sum / (double)(j - i)));
        i = j;
    }
    return out;
}

struct UnionFind {
    std::vector<int> p;
    explicit UnionFind(int n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[b] = a; }
};

float overlapShare(fz_rect a, fz_rect b) {
    const fz_rect i = fz_intersect_rect(a, b);
    if (fz_is_empty_rect(i)) return 0;
    const float ai = (i.x1 - i.x0) * (i.y1 - i.y0);
    const float aa = (a.x1 - a.x0) * (a.y1 - a.y0);
    const float ab = (b.x1 - b.x0) * (b.y1 - b.y0);
    return ai / std::max(1e-3f, std::min(aa, ab));
}

void ruledTables(const PdfTableEvidence& ev, fz_rect page, std::vector<PdfTable>* out,
                 int* hCount, int* vCount) {
    std::vector<Seg> hs, vs;
    for (const auto& r : ev.rules) {
        const fz_rect clipped = fz_intersect_rect(fz_rect{r.x0, r.y0, r.x1, r.y1}, page);
        if (r.x1 - r.x0 >= r.y1 - r.y0) {
            if (r.x1 - r.x0 >= kMinRule && clipped.x1 > clipped.x0)
                hs.push_back({(r.y0 + r.y1) * 0.5f, clipped.x0, clipped.x1});
        } else if (r.y1 - r.y0 >= kMinRule && clipped.y1 > clipped.y0) {
            vs.push_back({(r.x0 + r.x1) * 0.5f, clipped.y0, clipped.y1});
        }
    }
    hs = mergeSegments(std::move(hs));
    vs = mergeSegments(std::move(vs));
    *hCount = (int)hs.size();
    *vCount = (int)vs.size();
    if (hs.size() < 2 || vs.size() < 2) return;
    const float tol = 2.0f;
    const int nh = (int)hs.size();
    UnionFind uf(nh + (int)vs.size());
    for (int h = 0; h < nh; ++h)
        for (int v = 0; v < (int)vs.size(); ++v)
            if (vs[v].pos >= hs[h].a - tol && vs[v].pos <= hs[h].b + tol &&
                hs[h].pos >= vs[v].a - tol && hs[h].pos <= vs[v].b + tol)
                uf.unite(h, nh + v);
    std::map<int, std::pair<std::vector<Seg>, std::vector<Seg>>> comps;
    for (int h = 0; h < nh; ++h) comps[uf.find(h)].first.push_back(hs[h]);
    for (int v = 0; v < (int)vs.size(); ++v) comps[uf.find(nh + v)].second.push_back(vs[v]);

    for (const auto& entry : comps) {
        const auto& ch = entry.second.first;
        const auto& cv = entry.second.second;
        if (ch.size() < 2 || cv.size() < 2) continue;
        std::vector<float> ys, xs;
        float hx0 = 1e9f, hx1 = -1e9f, vy0 = 1e9f, vy1 = -1e9f;
        for (const auto& s : ch) { ys.push_back(s.pos); hx0 = std::min(hx0, s.a); hx1 = std::max(hx1, s.b); }
        for (const auto& s : cv) { xs.push_back(s.pos); vy0 = std::min(vy0, s.a); vy1 = std::max(vy1, s.b); }
        ys = clusterPositions(ys, 3.0f);
        xs = clusterPositions(xs, 3.0f);
        if (hx0 < xs.front() - 3.0f) xs.insert(xs.begin(), hx0);
        if (hx1 > xs.back() + 3.0f) xs.push_back(hx1);
        if (vy0 < ys.front() - 3.0f) ys.insert(ys.begin(), vy0);
        if (vy1 > ys.back() + 3.0f) ys.push_back(vy1);
        const int R = (int)ys.size() - 1, C = (int)xs.size() - 1;
        if (R < 2 || C < 2) continue;
        if (xs.back() - xs.front() < 30.0f || ys.back() - ys.front() < 10.0f) continue;
        auto hasV = [&](float x, float y0, float y1) {
            const float slack = 0.3f * (y1 - y0) + 1.0f;
            for (const auto& s : cv)
                if (std::fabs(s.pos - x) <= 2.0f && s.a <= y0 + slack && s.b >= y1 - slack) return true;
            return false;
        };
        auto hasH = [&](float y, float x0, float x1) {
            const float slack = 0.3f * (x1 - x0) + 1.0f;
            for (const auto& s : ch)
                if (std::fabs(s.pos - y) <= 2.0f && s.a <= x0 + slack && s.b >= x1 - slack) return true;
            return false;
        };
        UnionFind cells(R * C);
        for (int r = 0; r < R; ++r)
            for (int c = 0; c + 1 < C; ++c)
                if (!hasV(xs[c + 1], ys[r], ys[r + 1])) cells.unite(r * C + c, r * C + c + 1);
        for (int r = 0; r + 1 < R; ++r)
            for (int c = 0; c < C; ++c)
                if (!hasH(ys[r + 1], xs[c], xs[c + 1])) cells.unite(r * C + c, (r + 1) * C + c);
        struct Region { int r0 = 1 << 30, r1 = -1, c0 = 1 << 30, c1 = -1; };
        std::map<int, Region> regions;
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < C; ++c) {
                Region& g = regions[cells.find(r * C + c)];
                g.r0 = std::min(g.r0, r); g.r1 = std::max(g.r1, r);
                g.c0 = std::min(g.c0, c); g.c1 = std::max(g.c1, c);
            }
        bool multiRow = false, multiCol = false;
        for (int r = 0; r < R && !multiRow; ++r)
            for (int c = 0; c + 1 < C; ++c)
                if (cells.find(r * C + c) != cells.find(r * C + c + 1)) { multiRow = true; break; }
        for (int c = 0; c < C && !multiCol; ++c)
            for (int r = 0; r + 1 < R; ++r)
                if (cells.find(r * C + c) != cells.find((r + 1) * C + c)) { multiCol = true; break; }
        if (regions.size() < 3 || !multiRow || !multiCol) continue;
        const fz_rect gridBox{xs.front(), ys.front(), xs.back(), ys.back()};
        const float span = 0.15f * std::min(gridBox.x1 - gridBox.x0, gridBox.y1 - gridBox.y0);
        int art = 0, longArt = 0;
        for (const auto& a : ev.artwork) {
            const fz_point c{(a.x0 + a.x1) * 0.5f, (a.y0 + a.y1) * 0.5f};
            if (!fz_is_point_inside_rect(c, gridBox)) continue;
            ++art;
            longArt += std::max(a.x1 - a.x0, a.y1 - a.y0) >= span;
        }
        if (art >= 3 || longArt >= 2) continue;

        PdfTable table;
        table.source = "ruled";
        table.bbox = {xs.front(), ys.front(), xs.back(), ys.back()};
        std::vector<int> keptBefore(R + 1, 0);
        for (int r = 0; r < R; ++r) {
            bool real = false;
            for (int c = 0; c < C && !real; ++c) {
                const Region& g = regions[cells.find(r * C + c)];
                real = g.r0 == r && g.c0 == c;
            }
            keptBefore[r + 1] = keptBefore[r] + (real ? 1 : 0);
        }
        for (int r = 0; r < R; ++r) {
            if (keptBefore[r + 1] == keptBefore[r]) continue;
            std::vector<PdfTableCell> row;
            for (int c = 0; c < C; ++c) {
                const Region& g = regions[cells.find(r * C + c)];
                PdfTableCell cell;
                cell.placeholder = g.r0 != r || g.c0 != c;
                if (!cell.placeholder) {
                    cell.rect = {xs[g.c0], ys[g.r0], xs[g.c1 + 1], ys[g.r1 + 1]};
                    cell.colspan = g.c1 - g.c0 + 1;
                    cell.rowspan = std::max(1, keptBefore[g.r1 + 1] - keptBefore[g.r0]);
                }
                row.push_back(cell);
            }
            table.rows.push_back(std::move(row));
        }
        if (table.rows.size() >= 2) out->push_back(std::move(table));
    }
}

struct TextRow {
    float baseline = 0;
    std::vector<fz_rect> segments;
    int wide = 1;
};

std::vector<TextRow> textRows(const PdfTableEvidence& ev, fz_rect box, float* em) {
    std::vector<const PdfGlyph*> gs;
    std::vector<float> sizes;
    for (const auto& g : ev.textGlyphs) {
        if (g.ucs <= 32) continue;
        const fz_point c{(g.box.x0 + g.box.x1) * 0.5f, (g.box.y0 + g.box.y1) * 0.5f};
        if (!fz_is_point_inside_rect(c, box)) continue;
        gs.push_back(&g);
        sizes.push_back((g.box.y1 - g.box.y0) / 1.05f);
    }
    std::vector<TextRow> rows;
    if (gs.empty()) return rows;
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    *em = std::max(1.0f, sizes[sizes.size() / 2]);
    std::sort(gs.begin(), gs.end(), [](const PdfGlyph* a, const PdfGlyph* b) { return a->y < b->y; });
    std::vector<std::vector<const PdfGlyph*>> clusters;
    for (const PdfGlyph* g : gs) {
        if (clusters.empty() || g->y - clusters.back().front()->y > 0.35f * *em) clusters.emplace_back();
        clusters.back().push_back(g);
    }
    for (size_t k = 0; k < clusters.size() && clusters.size() > 1;) {
        if (clusters[k].size() > 3) { ++k; continue; }
        const float y = clusters[k].front()->y;
        int best = -1;
        float bestD = 0.7f * *em;
        for (int j : {(int)k - 1, (int)k + 1}) {
            if (j < 0 || j >= (int)clusters.size()) continue;
            const float d = std::fabs(clusters[j].front()->y - y);
            if (d <= bestD && clusters[j].size() > 3) { bestD = d; best = j; }
        }
        if (best < 0) { ++k; continue; }
        clusters[best].insert(clusters[best].end(), clusters[k].begin(), clusters[k].end());
        std::sort(clusters[best].begin(), clusters[best].end(),
                  [](const PdfGlyph* a, const PdfGlyph* b) { return a->y < b->y; });
        clusters.erase(clusters.begin() + (long)k);
    }
    for (auto& cl : clusters) {
        std::sort(cl.begin(), cl.end(), [](const PdfGlyph* a, const PdfGlyph* b) { return a->box.x0 < b->box.x0; });
        TextRow row;
        row.baseline = cl[cl.size() / 2]->y;
        for (const PdfGlyph* g : cl) {
            if (!row.segments.empty() && g->box.x0 - row.segments.back().x1 < 0.45f * *em)
                row.segments.back() = fz_union_rect(row.segments.back(), g->box);
            else
                row.segments.push_back(g->box);
        }
        for (size_t k = 1; k < row.segments.size(); ++k)
            row.wide += row.segments[k].x0 - row.segments[k - 1].x1 >= *em;
        rows.push_back(std::move(row));
    }
    return rows;
}

void booktabsTables(const PdfTableEvidence& ev, fz_rect page, const std::vector<PdfTable>& existing,
                    std::vector<PdfTable>* out, int* candidates) {
    std::vector<Seg> hs;
    for (const auto& r : ev.rules) {
        if (r.x1 - r.x0 < 30.0f || r.x1 - r.x0 < 3 * (r.y1 - r.y0)) continue;
        hs.push_back({(r.y0 + r.y1) * 0.5f, std::max(r.x0, page.x0), std::min(r.x1, page.x1)});
    }
    hs = mergeSegments(std::move(hs));
    std::vector<std::vector<Seg>> groups;
    for (const auto& s : hs) {
        bool placed = false;
        for (auto& g : groups)
            if (std::fabs(g[0].a - s.a) <= 3.0f && std::fabs(g[0].b - s.b) <= 3.0f) {
                g.push_back(s);
                placed = true;
                break;
            }
        if (!placed) groups.push_back({s});
    }
    for (auto& g : groups) {
        if (g.size() < 2) continue;
        std::sort(g.begin(), g.end(), [](const Seg& l, const Seg& r) { return l.pos < r.pos; });
        const float x0 = g[0].a, x1 = g[0].b;
        struct Band { float top, bottom; std::vector<TextRow> rows; float em = 10; bool table = false; };
        std::vector<Band> bands;
        for (size_t k = 0; k + 1 < g.size(); ++k) {
            Band b;
            b.top = g[k].pos;
            b.bottom = g[k + 1].pos;
            if (b.bottom - b.top < 4.0f) continue;
            b.rows = textRows(ev, {x0 - 2, b.top + 0.5f, x1 + 2, b.bottom - 0.5f}, &b.em);
            int split = 0;
            for (const auto& row : b.rows) split += row.wide >= 2;
            b.table = !b.rows.empty() && split * 2 >= (int)b.rows.size();
            bands.push_back(std::move(b));
        }
        for (size_t k = 0; k < bands.size();) {
            if (!bands[k].table) { ++k; continue; }
            size_t end = k;
            while (end < bands.size() && bands[end].table) ++end;
            ++*candidates;
            const size_t body = end - k >= 2 ? k + 1 : k;
            const float em = bands[body].em;
            const fz_rect box{x0, bands[k].top, x1, bands[end - 1].bottom};
            bool clash = false;
            for (const auto& t : existing) clash |= overlapShare(box, t.bbox) > 0.3f;
            for (const auto& a : ev.artwork) {
                const fz_point c{(a.x0 + a.x1) * 0.5f, (a.y0 + a.y1) * 0.5f};
                clash |= fz_is_point_inside_rect(c, box);
            }
            const size_t first = k;
            k = end;
            auto reject = [&](const char* why) {
                if (std::getenv("HC_LAYOUT_DEBUG"))
                    std::fprintf(stderr, "  booktabs rejected %s %.0f,%.0f,%.0f,%.0f\n", why, box.x0, box.y0, box.x1, box.y1);
            };
            if (clash) { reject("overlap or artwork"); continue; }
            const int steps = std::max(1, (int)std::ceil((x1 - x0) * 2));
            std::vector<int> cover(steps + 1, 0);
            float lo = 1e9f, hi = -1e9f;
            std::vector<TextRow> bodyRows;
            for (size_t j = body; j < end; ++j)
                bodyRows.insert(bodyRows.end(), bands[j].rows.begin(), bands[j].rows.end());
            int splitRows = 0;
            for (const auto& row : bodyRows) {
                if (row.segments.size() < 2) continue;
                ++splitRows;
                for (const auto& s : row.segments) {
                    lo = std::min(lo, s.x0);
                    hi = std::max(hi, s.x1);
                    for (int i = std::max(0, (int)((s.x0 - x0) * 2)); i <= std::min(steps, (int)((s.x1 - x0) * 2)); ++i)
                        ++cover[i];
                }
            }
            const int allowed = (int)(0.15f * splitRows);
            std::vector<float> cuts;
            int runStart = -1;
            for (int i = 0; i <= steps; ++i) {
                const float x = x0 + i * 0.5f;
                const bool open = cover[i] <= allowed && x > lo && x < hi;
                if (open && runStart < 0) runStart = i;
                if ((!open || i == steps) && runStart >= 0) {
                    const float w = (i - runStart) * 0.5f;
                    if (w >= 0.5f * em) cuts.push_back(x0 + (runStart + i) * 0.25f);
                    runStart = -1;
                }
            }
            const int C = (int)cuts.size() + 1;
            if (C < 2 || C > 20) { reject("columns"); continue; }
            std::vector<float> colX{x0};
            for (float c : cuts) colX.push_back(c);
            colX.push_back(x1);
            auto columnOf = [&](float x) {
                int c = 0;
                while (c + 1 < C && x > colX[c + 1]) ++c;
                return c;
            };
            struct RowRef { const TextRow* row; bool header; };
            std::vector<RowRef> rows;
            for (size_t j = first; j < end; ++j)
                for (const auto& row : bands[j].rows) rows.push_back({&row, j < body});
            if (rows.size() < 2) { reject("rows"); continue; }
            PdfTable table;
            table.source = "booktabs";
            table.bbox = box;
            int multi = 0, filledCells = 0;
            float chars = 0;
            for (size_t r = 0; r < rows.size(); ++r) {
                const float top = r == 0 ? box.y0 : (rows[r - 1].row->baseline + rows[r].row->baseline) * 0.5f;
                const float bottom = r + 1 == rows.size() ? box.y1
                                   : (rows[r].row->baseline + rows[r + 1].row->baseline) * 0.5f;
                std::vector<PdfTableCell> cells(C);
                std::vector<int> spanEnd(C, -1);
                std::vector<float> textX0(C, 1e9f), textX1(C, -1e9f);
                for (const auto& s : rows[r].row->segments) {
                    int a = -1, z = -1;
                    const float need = std::max(0.5f * em, 0.25f * (s.x1 - s.x0));
                    for (int c = 0; c < C; ++c) {
                        const float overlap = std::min(s.x1, colX[c + 1]) - std::max(s.x0, colX[c]);
                        if (overlap < need && !(overlap > 0 && colX[c + 1] - colX[c] < need)) continue;
                        if (a < 0) a = c;
                        z = c;
                    }
                    if (a < 0) a = z = columnOf((s.x0 + s.x1) * 0.5f);
                    spanEnd[a] = std::max(spanEnd[a], z);
                    textX0[a] = std::min(textX0[a], s.x0);
                    textX1[a] = std::max(textX1[a], s.x1);
                    chars += (s.x1 - s.x0) / (0.5f * em);
                }
                int filled = 0;
                for (int c = 0; c < C;) {
                    PdfTableCell& cell = cells[c];
                    cell.header = rows[r].header;
                    const int z = std::max(c, spanEnd[c]);
                    cell.colspan = z - c + 1;
                    cell.rect = {std::min(colX[c], textX0[c] - 0.5f), top,
                                 std::max(colX[z + 1], textX1[c] + 0.5f), bottom};
                    filled += spanEnd[c] >= 0;
                    for (int k2 = c + 1; k2 <= z; ++k2) cells[k2].placeholder = true;
                    c = z + 1;
                }
                filledCells += filled;
                multi += filled >= 2;
                table.rows.push_back(std::move(cells));
            }
            if (multi * 10 < (int)rows.size() * 6) { reject("sparse rows"); continue; }
            if (filledCells == 0 || chars / filledCells >= 40) { reject("long cells"); continue; }
            out->push_back(std::move(table));
        }
    }
}

std::string pathKey(const std::vector<int>& path) {
    std::string key;
    for (int v : path) { key += std::to_string(v); key += ','; }
    return key;
}

pdf_obj* structKid(fz_context* ctx, pdf_obj* node, int idx) {
    pdf_obj* k = pdf_dict_get(ctx, node, PDF_NAME(K));
    if (pdf_is_array(ctx, k)) return pdf_array_get(ctx, k, idx);
    return idx == 0 ? k : nullptr;
}

int structKidCount(fz_context* ctx, pdf_obj* node) {
    pdf_obj* k = pdf_dict_get(ctx, node, PDF_NAME(K));
    if (pdf_is_array(ctx, k)) return pdf_array_len(ctx, k);
    return k ? 1 : 0;
}

int spanAttribute(fz_context* ctx, pdf_obj* elem, const char* name) {
    pdf_obj* a = pdf_dict_get(ctx, elem, PDF_NAME(A));
    auto look = [&](pdf_obj* d) {
        pdf_obj* v = pdf_is_dict(ctx, d) ? pdf_dict_gets(ctx, d, name) : nullptr;
        return pdf_is_int(ctx, v) ? pdf_to_int(ctx, v) : 0;
    };
    if (pdf_is_array(ctx, a)) {
        for (int i = 0; i < pdf_array_len(ctx, a); ++i)
            if (int v = look(pdf_array_get(ctx, a, i))) return v;
        return 1;
    }
    const int v = look(a);
    return v > 0 ? v : 1;
}

struct CellBox { fz_rect box = fz_empty_rect; bool th = false; };

bool treeTable(fz_context* ctx, pdf_document* pdf, const std::vector<std::pair<int, int>>& path,
               const std::map<std::string, CellBox>& boxes, PdfTable* out) {
    bool ok = false;
    fz_var(ok);
    fz_try(ctx) {
        pdf_obj* node = pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/StructTreeRoot");
        pdf_obj* roleMap = pdf_dict_get(ctx, node, PDF_NAME(RoleMap));
        for (const auto& step : path) {
            node = structKid(ctx, node, step.second);
            if (!pdf_is_dict(ctx, node)) { node = nullptr; break; }
        }
        if (node && pdf_structure_type(ctx, roleMap, pdf_dict_get(ctx, node, PDF_NAME(S)))
                == FZ_STRUCTURE_TABLE) {
            struct TagCell { std::string key; bool th; int colspan, rowspan; };
            std::vector<std::vector<TagCell>> rows;
            std::vector<int> rel;
            std::function<void(pdf_obj*)> visit = [&](pdf_obj* n) {
                const int count = structKidCount(ctx, n);
                for (int j = 0; j < count; ++j) {
                    pdf_obj* kid = structKid(ctx, n, j);
                    if (!pdf_is_dict(ctx, kid) || !pdf_dict_get(ctx, kid, PDF_NAME(S))) continue;
                    const fz_structure type =
                        pdf_structure_type(ctx, roleMap, pdf_dict_get(ctx, kid, PDF_NAME(S)));
                    rel.push_back(j);
                    if (type == FZ_STRUCTURE_TR) {
                        std::vector<TagCell> row;
                        const int cells = structKidCount(ctx, kid);
                        for (int k = 0; k < cells; ++k) {
                            pdf_obj* cell = structKid(ctx, kid, k);
                            if (!pdf_is_dict(ctx, cell)) continue;
                            const fz_structure ct =
                                pdf_structure_type(ctx, roleMap, pdf_dict_get(ctx, cell, PDF_NAME(S)));
                            if (ct != FZ_STRUCTURE_TD && ct != FZ_STRUCTURE_TH) continue;
                            rel.push_back(k);
                            row.push_back({pathKey(rel), ct == FZ_STRUCTURE_TH,
                                           std::clamp(spanAttribute(ctx, cell, "ColSpan"), 1, 64),
                                           std::clamp(spanAttribute(ctx, cell, "RowSpan"), 1, 512)});
                            rel.pop_back();
                        }
                        rows.push_back(std::move(row));
                    } else if (type == FZ_STRUCTURE_THEAD || type == FZ_STRUCTURE_TBODY ||
                               type == FZ_STRUCTURE_TFOOT) {
                        visit(kid);
                    }
                    rel.pop_back();
                }
            };
            visit(node);
            std::vector<std::vector<int>> slot;
            std::vector<std::vector<const TagCell*>> starts;
            int cols = 0;
            for (size_t r = 0; r < rows.size(); ++r) {
                int c = 0;
                for (const auto& cell : rows[r]) {
                    auto at = [&](size_t rr, int cc) -> int& {
                        if (slot.size() <= rr) { slot.resize(rr + 1); starts.resize(rr + 1); }
                        if ((int)slot[rr].size() <= cc) {
                            slot[rr].resize(cc + 1, -1);
                            starts[rr].resize(cc + 1, nullptr);
                        }
                        return slot[rr][cc];
                    };
                    while (at(r, c) != -1) ++c;
                    for (int dr = 0; dr < cell.rowspan; ++dr)
                        for (int dc = 0; dc < cell.colspan; ++dc)
                            at(r + dr, c + dc) = (dr == 0 && dc == 0) ? 0 : -2;
                    starts[r][c] = &cell;
                    c += cell.colspan;
                    cols = std::max(cols, c);
                }
            }
            const size_t gridRows = std::min(slot.size(), rows.size());
            if (gridRows >= 2 && cols >= 2) {
                out->source = "tagged";
                out->bbox = fz_empty_rect;
                for (size_t r = 0; r < gridRows; ++r) {
                    bool allTh = !rows[r].empty();
                    for (const auto& cell : rows[r]) allTh &= cell.th;
                    std::vector<PdfTableCell> row(cols);
                    for (int c = 0; c < cols; ++c) {
                        PdfTableCell& cell = row[c];
                        cell.header = allTh;
                        const TagCell* tc = c < (int)starts[r].size() ? starts[r][c] : nullptr;
                        cell.placeholder = tc == nullptr;
                        if (!tc) continue;
                        cell.colspan = std::min(tc->colspan, cols - c);
                        cell.rowspan = std::min(tc->rowspan, (int)(gridRows - r));
                        auto it = boxes.find(tc->key);
                        if (it == boxes.end()) continue;
                        cell.rect = fz_expand_rect(it->second.box, 0.8f);
                        out->bbox = fz_is_empty_rect(out->bbox) ? it->second.box
                                                                : fz_union_rect(out->bbox, it->second.box);
                    }
                    out->rows.push_back(std::move(row));
                }
                ok = !fz_is_empty_rect(out->bbox);
            }
        }
    } fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

void taggedTables(const PdfTableEvidence& ev, fz_context* ctx, fz_document* doc,
                  std::vector<PdfTable>* out, int* fromTree) {
    std::map<int, std::map<std::string, CellBox>> boxes;
    for (const auto& g : ev.glyphs) {
        CellBox& b = boxes[g.table][pathKey(g.cellPath)];
        b.box = fz_is_empty_rect(b.box) ? g.box : fz_union_rect(b.box, g.box);
        b.th = g.th;
    }
    pdf_document* pdf = ctx && doc ? pdf_specifics(ctx, doc) : nullptr;
    for (const auto& entry : boxes) {
        const int id = entry.first;
        PdfTable table;
        if (pdf && id < (int)ev.tablePaths.size() &&
            treeTable(ctx, pdf, ev.tablePaths[id], entry.second, &table)) {
            ++*fromTree;
            table.path = ev.tablePaths[id];
            out->push_back(std::move(table));
            continue;
        }
        std::map<std::vector<int>, std::map<int, const CellBox*>> rows;
        std::vector<int> cellIdx;
        std::map<std::string, std::vector<int>> keyPath;
        for (const auto& g : ev.glyphs)
            if (g.table == id) keyPath.emplace(pathKey(g.cellPath), g.cellPath);
        for (const auto& kp : keyPath) {
            const auto& cp = kp.second;
            if (cp.empty()) continue;
            std::vector<int> rowKey(cp.begin(), cp.end() - 1);
            rows[rowKey][cp.back()] = &entry.second.at(kp.first);
            cellIdx.push_back(cp.back());
        }
        std::sort(cellIdx.begin(), cellIdx.end());
        cellIdx.erase(std::unique(cellIdx.begin(), cellIdx.end()), cellIdx.end());
        if (rows.size() < 2 || cellIdx.size() < 2) continue;
        table.source = "tagged";
        table.bbox = fz_empty_rect;
        for (const auto& row : rows) {
            bool allTh = true;
            for (const auto& cell : row.second) allTh &= cell.second->th;
            std::vector<PdfTableCell> outRow(cellIdx.size());
            for (size_t c = 0; c < cellIdx.size(); ++c) {
                outRow[c].header = allTh;
                auto it = row.second.find(cellIdx[c]);
                if (it == row.second.end()) continue;
                outRow[c].rect = fz_expand_rect(it->second->box, 0.8f);
                table.bbox = fz_is_empty_rect(table.bbox) ? it->second->box
                                                          : fz_union_rect(table.bbox, it->second->box);
            }
            table.rows.push_back(std::move(outRow));
        }
        if (id < (int)ev.tablePaths.size()) table.path = ev.tablePaths[id];
        out->push_back(std::move(table));
    }
}


int structIndex(fz_context* ctx, pdf_obj* elem) {
    pdf_obj* p = pdf_dict_get(ctx, elem, PDF_NAME(P));
    if (!p) return 0;
    pdf_obj* k = pdf_dict_get(ctx, p, PDF_NAME(K));
    const int n = pdf_array_len(ctx, k);
    if (n == 0) return pdf_objcmp(ctx, k, elem) == 0 ? 0 : -1;
    for (int i = 0; i < n; ++i)
        if (pdf_objcmp(ctx, pdf_array_get(ctx, k, i), elem) == 0) return i;
    return -1;
}

PdfStructPath elementPath(fz_context* ctx, pdf_obj* roleMap, pdf_obj* root, pdf_obj* elem) {
    PdfStructPath rev;
    for (int guard = 0; guard < 128 && pdf_is_dict(ctx, elem) && pdf_objcmp(ctx, elem, root); ++guard) {
        const fz_structure t = pdf_structure_type(ctx, roleMap, pdf_dict_get(ctx, elem, PDF_NAME(S)));
        if (t != FZ_STRUCTURE_INVALID) rev.emplace_back((int)t, structIndex(ctx, elem));
        elem = pdf_dict_get(ctx, elem, PDF_NAME(P));
    }
    return PdfStructPath(rev.rbegin(), rev.rend());
}

std::map<PdfStructPath, int> directMcids(fz_context* ctx, fz_document* doc, int pageNumber) {
    std::map<PdfStructPath, int> out;
    pdf_document* pdf = ctx && doc ? pdf_specifics(ctx, doc) : nullptr;
    if (!pdf) return out;
    fz_try(ctx) {
        pdf_obj* root = pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/StructTreeRoot");
        pdf_obj* roleMap = pdf_dict_get(ctx, root, PDF_NAME(RoleMap));
        pdf_obj* page = pdf_lookup_page_obj(ctx, pdf, pageNumber);
        pdf_obj* sp = pdf_dict_get(ctx, page, PDF_NAME(StructParents));
        if (root && pdf_is_int(ctx, sp)) {
            pdf_obj* mcids = pdf_lookup_number(ctx, pdf_dict_get(ctx, root, PDF_NAME(ParentTree)),
                                               pdf_to_int(ctx, sp));
            std::map<int, PdfStructPath> byNum;
            for (int i = 0; i < pdf_array_len(ctx, mcids); ++i) {
                pdf_obj* elem = pdf_array_get(ctx, mcids, i);
                if (!pdf_is_dict(ctx, elem)) continue;
                const int num = pdf_to_num(ctx, elem);
                auto it = num > 0 ? byNum.find(num) : byNum.end();
                if (it == byNum.end()) {
                    PdfStructPath path = elementPath(ctx, roleMap, root, elem);
                    if (num <= 0) { ++out[path]; continue; }
                    it = byNum.emplace(num, std::move(path)).first;
                }
                ++out[it->second];
            }
        }
    } fz_catch(ctx) {
        out.clear();
    }
    return out;
}

bool isInlineStructure(int t) {
    switch (t) {
    case FZ_STRUCTURE_SPAN: case FZ_STRUCTURE_QUOTE: case FZ_STRUCTURE_REFERENCE:
    case FZ_STRUCTURE_BIBENTRY: case FZ_STRUCTURE_CODE: case FZ_STRUCTURE_LINK:
    case FZ_STRUCTURE_ANNOT: case FZ_STRUCTURE_EM: case FZ_STRUCTURE_STRONG:
    case FZ_STRUCTURE_RUBY: case FZ_STRUCTURE_RB: case FZ_STRUCTURE_RT: case FZ_STRUCTURE_RP:
    case FZ_STRUCTURE_WARICHU: case FZ_STRUCTURE_WT: case FZ_STRUCTURE_WP:
    case FZ_STRUCTURE_FORMULA: case FZ_STRUCTURE_FORM: case FZ_STRUCTURE_SUB:
        return true;
    default:
        return false;
    }
}

int resolveElement(PdfTableEvidence* ev, const PdfStructPath& owner, bool* label) {
    *label = false;
    if (owner.empty()) return -1;
    for (const auto& step : owner)
        if (step.first == FZ_STRUCTURE_ARTIFACT) return -1;
    int end = -1;
    for (int i = (int)owner.size() - 1; i >= 0; --i) {
        const int t = owner[i].first;
        if (isInlineStructure(t)) continue;
        if (t == FZ_STRUCTURE_LABEL) { *label = true; continue; }
        if (t == FZ_STRUCTURE_LISTBODY) continue;
        if (t != FZ_STRUCTURE_LISTITEM) {
            int j = i - 1;
            while (j >= 0 && isInlineStructure(owner[j].first)) --j;
            if (j >= 0 && (owner[j].first == FZ_STRUCTURE_LISTBODY ||
                           owner[j].first == FZ_STRUCTURE_LABEL))
                continue;
        }
        end = i;
        break;
    }
    if (end < 0) {
        end = (int)owner.size() - 1;
        *label = false;
    }
    PdfStructPath path(owner.begin(), owner.begin() + end + 1);
    if (owner[end].first != FZ_STRUCTURE_LISTITEM) *label = false;
    auto found = ev->elementIds.find(path);
    if (found != ev->elementIds.end()) return found->second;
    PdfStructElement e;
    e.type = owner[end].first;
    int lists = 0;
    for (const auto& step : path) {
        lists += step.first == FZ_STRUCTURE_LIST;
        e.inTable |= step.first == FZ_STRUCTURE_TABLE;
    }
    e.listDepth = std::max(0, lists - 1);
    e.path = std::move(path);
    const int id = (int)ev->elements.size();
    ev->elementIds.emplace(e.path, id);
    ev->elements.push_back(std::move(e));
    return id;
}

void recordCellGlyph(PdfTableEvidence* ev, const PdfStructPath& st, const PdfGlyph& glyph) {
    int t = -1;
    for (int i = (int)st.size() - 1; i >= 0; --i)
        if (st[i].first == FZ_STRUCTURE_TABLE) { t = i; break; }
    if (t < 0 || glyph.ucs <= 32) return;
    int r = -1, c = -1;
    for (int i = t + 1; i < (int)st.size(); ++i) {
        if (r < 0 && st[i].first == FZ_STRUCTURE_TR) r = i;
        else if (r >= 0 && (st[i].first == FZ_STRUCTURE_TD || st[i].first == FZ_STRUCTURE_TH)) {
            c = i;
            break;
        }
    }
    if (r < 0 || c < 0) return;
    std::string key;
    for (int i = 0; i <= t; ++i) {
        key += std::to_string(st[i].first);
        key += ':';
        key += std::to_string(st[i].second);
        key += '/';
    }
    auto found = ev->tableIds.find(key);
    const int table = found != ev->tableIds.end() ? found->second : (int)ev->tableIds.size();
    if (found == ev->tableIds.end()) {
        ev->tableIds.emplace(key, table);
        ev->tablePaths.emplace_back(st.begin(), st.begin() + t + 1);
    }
    PdfTaggedGlyph g;
    g.table = table;
    for (int i = t + 1; i <= c; ++i) g.cellPath.push_back(st[i].second);
    g.rowDepth = r - t;
    g.th = st[c].first == FZ_STRUCTURE_TH;
    g.box = glyph.box;
    ev->glyphs.push_back(std::move(g));
}

bool isPrefix(const PdfStructPath& prefix, const PdfStructPath& path) {
    return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

}

fz_device* newPdfTableEvidenceDevice(fz_context* ctx, fz_device* target, PdfTableEvidence* evidence) {
    auto* d = fz_new_derived_device(ctx, EvidenceDevice);
    d->super.fill_path = dFillPath;
    d->super.stroke_path = dStrokePath;
    d->super.clip_path = dClipPath;
    d->super.clip_stroke_path = dClipStrokePath;
    d->super.fill_text = dFillText;
    d->super.stroke_text = dStrokeText;
    d->super.clip_text = dClipText;
    d->super.clip_stroke_text = dClipStrokeText;
    d->super.ignore_text = dIgnoreText;
    d->super.fill_shade = dFillShade;
    d->super.fill_image = dFillImage;
    d->super.fill_image_mask = dFillImageMask;
    d->super.clip_image_mask = dClipImageMask;
    d->super.pop_clip = dPopClip;
    d->super.begin_mask = dBeginMask;
    d->super.end_mask = dEndMask;
    d->super.begin_group = dBeginGroup;
    d->super.end_group = dEndGroup;
    d->super.begin_tile = dBeginTile;
    d->super.end_tile = dEndTile;
    d->super.render_flags = dRenderFlags;
    d->super.set_default_colorspaces = dDefaultColorspaces;
    d->super.begin_layer = dBeginLayer;
    d->super.end_layer = dEndLayer;
    d->super.begin_structure = dBeginStructure;
    d->super.end_structure = dEndStructure;
    d->super.begin_metatext = dBeginMetatext;
    d->super.end_metatext = dEndMetatext;
    d->super.hints = target->hints;
    d->super.flags = target->flags;
    d->target = target;
    d->ev = evidence;
    return &d->super;
}

std::vector<PdfTable> findPdfTables(const PdfTableEvidence& evidence, fz_rect page,
                                    fz_context* ctx, fz_document* doc, std::string* debug) {
    std::vector<PdfTable> tagged, ruled;
    int fromTree = 0;
    taggedTables(evidence, ctx, doc, &tagged, &fromTree);
    int hCount = 0, vCount = 0;
    ruledTables(evidence, page, &ruled, &hCount, &vCount);
    std::vector<PdfTable> out = tagged;
    int ruledKept = 0;
    for (auto& t : ruled) {
        bool covered = false;
        for (const auto& g : tagged) covered |= overlapShare(t.bbox, g.bbox) > 0.3f;
        if (covered) continue;
        out.push_back(std::move(t));
        ++ruledKept;
    }
    std::vector<PdfTable> booktabs;
    int booktabsCandidates = 0;
    booktabsTables(evidence, page, out, &booktabs, &booktabsCandidates);
    for (auto& t : booktabs) out.push_back(std::move(t));
    std::sort(out.begin(), out.end(), [](const PdfTable& a, const PdfTable& b) {
        return a.bbox.y0 < b.bbox.y0;
    });
    if (debug) {
        char msg[240];
        std::snprintf(msg, sizeof(msg),
            "rules=%zu art=%zu h=%d v=%d structure=%d taggedGlyphs=%zu tagged=%zu (tree %d) ruled=%d (of %zu) "
            "booktabs=%zu (of %d)",
            evidence.rules.size(), evidence.artwork.size(), hCount, vCount, evidence.structureCalls,
            evidence.glyphs.size(), tagged.size(), fromTree, ruledKept, ruled.size(), booktabs.size(),
            booktabsCandidates);
        *debug = msg;
    }
    return out;
}

std::vector<fz_rect> findPdfFigures(const PdfTableEvidence& ev, fz_rect page,
                                    const std::vector<fz_rect>& seeds,
                                    const std::vector<fz_rect>& exclude, float bodySize) {
    std::vector<fz_rect> out;
    std::vector<fz_rect> items = ev.artwork;
    std::vector<int> score;
    for (const auto& r : ev.artwork)
        score.push_back(std::max(r.x1 - r.x0, r.y1 - r.y0) >= 3 * bodySize ? 2 : 1);
    for (const auto& r : seeds) { items.push_back(r); score.push_back(3); }
    const float near = std::max(4.0f, 0.6f * bodySize);
    if (ev.rules.size() >= 12) {
        UnionFind ru((int)ev.rules.size());
        std::vector<fz_rect> rr;
        for (const auto& r : ev.rules) rr.push_back(fz_expand_rect({r.x0, r.y0, r.x1, r.y1}, 0.5f));
        for (size_t a = 0; a < rr.size(); ++a)
            for (size_t b = a + 1; b < rr.size(); ++b)
                if (!fz_is_empty_rect(fz_intersect_rect(fz_expand_rect(rr[a], near), rr[b])))
                    ru.unite((int)a, (int)b);
        std::map<int, std::pair<int, fz_rect>> clusters;
        for (size_t a = 0; a < rr.size(); ++a) {
            auto& c = clusters[ru.find((int)a)];
            c.second = c.first++ == 0 ? rr[a] : fz_union_rect(c.second, rr[a]);
        }
        for (const auto& c : clusters) {
            if (c.second.first < 12) continue;
            const fz_rect r = c.second.second;
            if (r.x1 - r.x0 > 0.9f * (page.x1 - page.x0) || r.y1 - r.y0 > 0.9f * (page.y1 - page.y0)) continue;
            items.push_back(r);
            score.push_back(3);
        }
    }
    if (items.empty()) return out;
    UnionFind uf((int)items.size());
    for (size_t a = 0; a < items.size(); ++a)
        for (size_t b = a + 1; b < items.size(); ++b)
            if (!fz_is_empty_rect(fz_intersect_rect(fz_expand_rect(items[a], near), items[b])))
                uf.unite((int)a, (int)b);
    std::map<int, std::pair<int, fz_rect>> groups;
    for (size_t a = 0; a < items.size(); ++a) {
        auto& g = groups[uf.find((int)a)];
        g.second = g.first == 0 ? items[a] : fz_union_rect(g.second, items[a]);
        g.first += score[a];
    }
    std::vector<fz_rect> regions;
    for (const auto& entry : groups) {
        fz_rect r = entry.second.second;
        if (entry.second.first < 3) continue;
        if (r.x1 - r.x0 < 4 * bodySize || r.y1 - r.y0 < 2 * bodySize) continue;
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& rule : ev.rules) {
                const fz_rect rr = fz_expand_rect({rule.x0, rule.y0, rule.x1, rule.y1}, 0.5f);
                if (fz_is_empty_rect(fz_intersect_rect(fz_expand_rect(r, std::max(4.0f, bodySize)), rr)))
                    continue;
                if ((rr.x1 - rr.x0) > 1.5f * (r.x1 - r.x0) + 20 || (rr.y1 - rr.y0) > 1.5f * (r.y1 - r.y0) + 20)
                    continue;
                r = fz_union_rect(r, rr);
            }
        r = fz_intersect_rect(r, page);
        bool table = false;
        for (const auto& t : exclude) table |= overlapShare(r, t) > 0.3f;
        if (!table && !fz_is_empty_rect(r)) regions.push_back(r);
    }
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t a = 0; a < regions.size() && !merged; ++a)
            for (size_t b = a + 1; b < regions.size(); ++b)
                if (!fz_is_empty_rect(fz_intersect_rect(fz_expand_rect(regions[a], 2.0f), regions[b]))) {
                    regions[a] = fz_union_rect(regions[a], regions[b]);
                    regions.erase(regions.begin() + (long)b);
                    merged = true;
                    break;
                }
    }
    return regions;
}

void preparePdfStructure(fz_context* ctx, fz_document* doc) {
    pdf_document* pdf = ctx && doc ? pdf_specifics(ctx, doc) : nullptr;
    if (!pdf) return;
    fz_try(ctx) {
        pdf_obj* root = pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/StructTreeRoot");
        if (pdf_is_dict(ctx, root)) {
            pdf_obj* roleMap = pdf_dict_get(ctx, root, PDF_NAME(RoleMap));
            if (!pdf_is_dict(ctx, roleMap)) {
                roleMap = pdf_dict_put_dict(ctx, root, PDF_NAME(RoleMap), 4);
            }
            for (int i = 0; i < pdf_dict_len(ctx, roleMap); ++i)
                if (pdf_name_eq(ctx, pdf_dict_get_val(ctx, roleMap, i), PDF_NAME(L)))
                    pdf_dict_put(ctx, roleMap, pdf_dict_get_key(ctx, roleMap, i), PDF_NAME(List));
            if (!pdf_dict_get(ctx, roleMap, PDF_NAME(L)))
                pdf_dict_put(ctx, roleMap, PDF_NAME(L), PDF_NAME(List));
        }
    } fz_catch(ctx) {
        fz_report_error(ctx);
    }
}

void resolvePdfStructure(PdfTableEvidence* ev, fz_context* ctx, fz_document* doc, int pageNumber) {
    ev->elements.clear();
    ev->elementIds.clear();
    ev->glyphs.clear();
    ev->tableIds.clear();
    ev->tablePaths.clear();
    ev->artifactGlyphs = 0;
    ev->staleAssigned = 0;
    const size_t nseg = ev->segments.size();
    std::vector<fz_rect> segBox(nseg, fz_empty_rect);
    auto grow = [&](int s, fz_rect r) {
        if (s < 0 || s >= (int)nseg) return;
        segBox[s] = fz_is_empty_rect(segBox[s]) ? r : fz_union_rect(segBox[s], r);
    };
    std::vector<float> heights;
    for (const auto& g : ev->textGlyphs) {
        grow(g.segment, g.box);
        if (g.ucs > 32) heights.push_back(g.box.y1 - g.box.y0);
    }
    for (const auto& im : ev->images) grow(im.segment, im.box);
    float lineH = 12.0f;
    if (!heights.empty()) {
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        lineH = std::max(2.0f, heights[heights.size() / 2]);
    }

    std::vector<int> owner(nseg, -1);
    std::map<PdfStructPath, int> fresh;
    bool stale = false;
    for (size_t s = 0; s < nseg; ++s) {
        const PdfTextSegment& seg = ev->segments[s];
        if (seg.fresh) {
            owner[s] = seg.path;
            ++fresh[ev->paths[seg.path]];
        } else if (!ev->paths[seg.path].empty() && !fz_is_empty_rect(segBox[s])) {
            stale = true;
        }
    }
    if (stale) {
        const auto direct = directMcids(ctx, doc, pageNumber);
        std::map<PdfStructPath, int> used;
        for (size_t s = 0; s < nseg; ++s) {
            const PdfTextSegment& seg = ev->segments[s];
            const PdfStructPath& cand = ev->paths[seg.path];
            if (seg.fresh || cand.empty() || fz_is_empty_rect(segBox[s])) continue;
            auto d = direct.find(cand);
            if (d == direct.end()) continue;
            auto f = fresh.find(cand);
            int& u = used[cand];
            if (d->second - (f != fresh.end() ? f->second : 0) - u <= 0) continue;
            fz_rect near = fz_empty_rect;
            for (size_t o = 0; o < nseg; ++o) {
                if (!ev->segments[o].fresh || fz_is_empty_rect(segBox[o])) continue;
                if (!isPrefix(cand, ev->paths[ev->segments[o].path])) continue;
                near = fz_is_empty_rect(near) ? segBox[o] : fz_union_rect(near, segBox[o]);
            }
            if (fz_is_empty_rect(near) ||
                fz_is_empty_rect(fz_intersect_rect(segBox[s], fz_expand_rect(near, 2.5f * lineH))))
                continue;
            owner[s] = seg.path;
            ev->segments[s].assigned = true;
            ++u;
            ++ev->staleAssigned;
        }
    }

    std::vector<int> element(nseg, -1);
    std::vector<char> label(nseg, 0);
    for (size_t s = 0; s < nseg; ++s) {
        if (owner[s] < 0) continue;
        bool l = false;
        element[s] = resolveElement(ev, ev->paths[owner[s]], &l);
        label[s] = l;
    }
    for (size_t s = 0; s < nseg; ++s) {
        if (element[s] < 0) continue;
        PdfStructPath& order = ev->elements[element[s]].order;
        const PdfStructPath& o = ev->paths[owner[s]];
        if (order.empty() || pdfStructPathLess(o, order)) order = o;
    }
    for (auto& g : ev->textGlyphs) {
        const int s = g.segment;
        if (s < 0 || s >= (int)nseg || element[s] < 0) {
            g.element = -1;
            ev->artifactGlyphs += g.ucs > 32;
            continue;
        }
        g.element = element[s];
        g.label = label[s];
        recordCellGlyph(ev, ev->paths[owner[s]], g);
    }
    for (auto& im : ev->images) {
        const int s = im.segment;
        im.element = s >= 0 && s < (int)nseg ? element[s] : -1;
        if (im.element >= 0) im.order = ev->paths[owner[s]];
    }
}

bool pdfStructPathLess(const PdfStructPath& a, const PdfStructPath& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i].second != b[i].second) return a[i].second < b[i].second;
        if (a[i].first != b[i].first) return a[i].first < b[i].first;
    }
    return a.size() < b.size();
}

namespace {
uint64_t gridKey(int qx, int qy) { return ((uint64_t)(uint32_t)qx << 32) | (uint32_t)qy; }
}

PdfGlyphLookup::PdfGlyphLookup(const PdfTableEvidence& evidence) : ev_(evidence) {
    for (size_t i = 0; i < ev_.textGlyphs.size(); ++i) {
        const PdfGlyph& g = ev_.textGlyphs[i];
        grid_[gridKey((int)std::floor(g.x * 4), (int)std::floor(g.y * 4))].push_back((int)i);
    }
}

int PdfGlyphLookup::find(fz_point origin, int ucs) const {
    const int qx = (int)std::floor(origin.x * 4), qy = (int)std::floor(origin.y * 4);
    int best = -1;
    float bestScore = 1e9f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            auto it = grid_.find(gridKey(qx + dx, qy + dy));
            if (it == grid_.end()) continue;
            for (int i : it->second) {
                const PdfGlyph& g = ev_.textGlyphs[i];
                const float d = std::fabs(g.x - origin.x) + std::fabs(g.y - origin.y);
                if (d > 0.35f) continue;
                const float score = d + (g.ucs == ucs ? 0.0f : 1.0f);
                if (score < bestScore) { bestScore = score; best = i; }
            }
        }
    return best;
}

}
