#include "pdf_columns.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace hardcopy {
namespace {

constexpr int kInkLevel = 200;

enum RowState : uint8_t { kBlank = 0, kSplit = 1, kSpan = 2 };
enum BandType : uint8_t { kBandSplit = 1, kBandSpan = 2, kBandTwo = 3, kBandFull = 4 };

struct Run {
    int y0 = 0, y1 = 0;
    uint8_t type = kBandSplit;
    int rowsL = 0, rowsR = 0;
    int fullL = 0, fullR = 0;
};

int clampi(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void say(std::string* debug, const char* fmt, ...) {
    if (!debug) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    *debug = buf;
}

}

const char* columnRegionKindName(ColumnRegionKind kind) {
    switch (kind) {
        case ColumnRegionKind::Left: return "left";
        case ColumnRegionKind::Right: return "right";
        default: return "full";
    }
}

std::vector<ColumnRegion> detectColumnRegions(const ColumnDetectInput& in,
                                              std::string* debug,
                                              ColumnRect* gutterOut) {
    std::vector<ColumnRegion> out;
    if (!in.pixels || in.width < 16 || in.height < 16) { say(debug, "single: no raster"); return out; }

    const int W = in.width;
    const int bx0 = clampi((int)std::floor(in.body.x0), 0, W - 1);
    const int bx1 = clampi((int)std::ceil(in.body.x1), bx0 + 1, W);
    const int by0 = clampi((int)std::floor(in.body.y0), 0, in.height - 1);
    const int by1 = clampi((int)std::ceil(in.body.y1), by0 + 1, in.height);
    const int bw = bx1 - bx0, bh = by1 - by0;
    if (bw < 40 || bh < 40) { say(debug, "single: body too small"); return out; }
    const float pt = std::max(0.25f, in.pxPerPt);

    struct Box { int x0, y0, x1, y1; };
    std::vector<Box> lines;
    for (const auto& l : in.lines) {
        Box b{clampi((int)std::floor(l.x0) - bx0, 0, bw), clampi((int)std::floor(l.y0) - by0, 0, bh),
              clampi((int)std::ceil(l.x1) - bx0, 0, bw), clampi((int)std::ceil(l.y1) - by0, 0, bh)};
        if (b.x1 <= b.x0 || b.y1 <= b.y0 || b.y1 - b.y0 > bh / 8) continue;
        lines.push_back(b);
    }

    float u = pt;
    {
        std::vector<int> heights;
        for (const auto& b : lines)
            if (b.x1 - b.x0 > b.y1 - b.y0) heights.push_back(b.y1 - b.y0);
        float lineBox = 0;
        if (heights.size() >= 8) {
            std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
            lineBox = (float)heights[heights.size() / 2];
        } else {
            std::vector<int> runs;
            for (int y = 0, run = 0; y <= bh; ++y) {
                bool ink = false;
                if (y < bh) {
                    const uint8_t* row = in.pixels + (size_t)(by0 + y) * W + bx0;
                    for (int x = 0; x < bw && !ink; ++x) ink = row[x] < kInkLevel;
                }
                if (ink) { ++run; continue; }
                if (run >= 2) runs.push_back(run);
                run = 0;
            }
            if (runs.size() >= 8) {
                std::nth_element(runs.begin(), runs.begin() + runs.size() / 2, runs.end());
                lineBox = runs[runs.size() / 2] * 1.2f;
            }
        }
        u = std::min(std::max(pt, lineBox / 12.0f), 8.0f * pt);
    }

    const int bridge = std::max(2, (int)std::lround(4.5f * u));
    std::vector<uint8_t> cov((size_t)bw * bh, 0);
    for (int y = 0; y < bh; ++y) {
        const uint8_t* row = in.pixels + (size_t)(by0 + y) * W + bx0;
        uint8_t* c = &cov[(size_t)y * bw];
        int last = -1;
        for (int x = 0; x < bw; ++x) {
            if (row[x] >= kInkLevel) continue;
            if (last >= 0 && x - last - 1 <= bridge) std::fill(c + last + 1, c + x, uint8_t(1));
            c[x] = 1;
            last = x;
        }
    }
    for (const auto& b : lines)
        for (int y = b.y0; y < b.y1; ++y)
            std::fill(&cov[(size_t)y * bw + b.x0], &cov[(size_t)y * bw + b.x1], uint8_t(1));

    std::vector<int> colCount(bw, 0);
    int inkRows = 0;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* c = &cov[(size_t)y * bw];
        bool any = false;
        for (int x = 0; x < bw; ++x) if (c[x]) { ++colCount[x]; any = true; }
        inkRows += any;
    }
    if (inkRows < (int)(20 * u)) { say(debug, "single: little ink rows=%d", inkRows); return out; }

    std::vector<std::pair<int, int>> xr;
    for (int x = 0; x < bw;) {
        while (x < bw && !colCount[x]) ++x;
        if (x >= bw) break;
        const int s = x;
        while (x < bw && colCount[x]) ++x;
        xr.push_back({s, x});
    }
    const int sliverW = std::max(3, (int)(0.035f * bw));
    const int sliverGap = std::max(3, (int)(0.02f * bw));
    while (xr.size() >= 2 && xr.front().second - xr.front().first < sliverW &&
           xr[1].first - xr.front().second > sliverGap) xr.erase(xr.begin());
    while (xr.size() >= 2 && xr.back().second - xr.back().first < sliverW &&
           xr.back().first - xr[xr.size() - 2].second > sliverGap) xr.pop_back();
    if (xr.empty()) { say(debug, "single: empty"); return out; }
    const int cx0 = xr.front().first, cx1 = xr.back().second, cw = cx1 - cx0;

    auto leastCovered = [&](int x0, int x1, int target, int* best) {
        int m = INT_MAX;
        *best = -1;
        for (int x = x0; x < x1; ++x) {
            if (colCount[x] < m || (colCount[x] == m && std::abs(x - target) < std::abs(*best - target))) {
                m = colCount[x]; *best = x;
            }
        }
        return m;
    };
    int m = INT_MAX, best = -1, lo = cx0, hi = cx1;
    bool fromHint = false;
    if (in.hasGutterHint) {
        const int hx0 = clampi((int)std::floor(in.gutterHint.x0) - bx0, cx0, cx1);
        const int hx1 = clampi((int)std::ceil(in.gutterHint.x1) - bx0, cx0, cx1);
        if (hx1 > hx0 && hx0 - cx0 >= cw / 5 && cx1 - hx1 >= cw / 5) {
            const int hm = leastCovered(hx0, hx1, (hx0 + hx1) / 2, &best);
            const int clearNeeded = std::max((int)std::lround(24.0f * u), inkRows * 15 / 100);
            if (best >= 0 && inkRows - hm >= clearNeeded) {
                m = hm; lo = std::max(cx0, hx0 - 2); hi = std::min(cx1, hx1 + 2); fromHint = true;
            }
        }
    }
    if (!fromHint) {
        m = leastCovered(cx0 + (int)(0.3f * cw), cx0 + (int)(0.7f * cw), cx0 + cw / 2, &best);
        if (best < 0 || m > inkRows * 7 / 10) { say(debug, "single: no gutter min=%d rows=%d", m, inkRows); return out; }
    }
    const int tol = std::max(2, inkRows / 50);
    int ga = best, gb = best + 1;
    if (fromHint) {
        const int tolHint = std::max(tol, inkRows / 10);
        ga = lo; gb = hi;
        while (ga < gb && colCount[ga] > m + tolHint) ++ga;
        while (gb > ga && colCount[gb - 1] > m + tolHint) --gb;
    } else {
        while (ga > lo && colCount[ga - 1] <= m + tol) --ga;
        while (gb < hi && colCount[gb] <= m + tol) ++gb;
    }
    const int minGutter = std::max(4, (int)std::lround(5.0f * u));
    if (gb - ga < minGutter || ga - cx0 < cw / 5 || cx1 - gb < cw / 5) {
        say(debug, "single: gutter %d..%d%s rejected", ga + bx0, gb + bx0, fromHint ? "(hint)" : "");
        return out;
    }
    const int gmid = (ga + gb) / 2;

    std::vector<uint8_t> spanLine(bh, 0);
    for (const auto& b : lines)
        if (b.x0 < ga && b.x1 > gb)
            for (int y = b.y0; y < b.y1; ++y) spanLine[y] = 1;
    std::vector<uint8_t> state(bh, kBlank), hasL(bh, 0), hasR(bh, 0);
    for (int y = 0; y < bh; ++y) {
        const uint8_t* c = &cov[(size_t)y * bw];
        hasL[y] = std::any_of(c + cx0, c + ga, [](uint8_t v) { return v != 0; });
        hasR[y] = std::any_of(c + gb, c + cx1, [](uint8_t v) { return v != 0; });
        if (c[gmid] || spanLine[y]) state[y] = kSpan;
        else if (hasL[y] || hasR[y]) state[y] = kSplit;
    }

    std::vector<Run> runs;
    for (int y = 0; y < bh; ++y) {
        if (state[y] == kBlank) continue;
        const uint8_t type = state[y] == kSpan ? kBandSpan : kBandSplit;
        if (runs.empty() || runs.back().type != type) runs.push_back({y, y + 1, type});
        Run& r = runs.back();
        r.y1 = y + 1;
        if (type == kBandSplit) { r.rowsL += hasL[y]; r.rowsR += hasR[y]; }
    }
    auto merge = [&]() {
        std::vector<Run> merged;
        for (const auto& r : runs) {
            if (!merged.empty() && merged.back().type == r.type) {
                Run& p = merged.back();
                p.y1 = r.y1;
                p.rowsL += r.rowsL; p.rowsR += r.rowsR;
                p.fullL += r.fullL; p.fullR += r.fullR;
            } else {
                merged.push_back(r);
            }
        }
        runs.swap(merged);
    };

    const int spanMin = std::max(2, (int)std::lround(3.0f * u));
    for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].type != kBandSpan || runs[i].y1 - runs[i].y0 >= spanMin) continue;
        const bool prev = i > 0 && runs[i - 1].type == kBandSplit;
        const bool next = i + 1 < runs.size() && runs[i + 1].type == kBandSplit;
        if (prev && next) runs[i].type = kBandSplit;
    }
    merge();

    const bool haveText = !lines.empty();
    const int minSide = std::max(4, (int)std::lround(18.0f * u));
    const int minTwo = std::max(8, (int)std::lround(24.0f * u));
    const int fullWL = (ga - cx0) * 6 / 10, fullWR = (cx1 - gb) * 6 / 10;
    for (auto& r : runs) {
        if (r.type != kBandSplit) continue;
        for (const auto& b : lines) {
            const int cy = (b.y0 + b.y1) / 2;
            if (cy < r.y0 || cy >= r.y1) continue;
            if (b.x1 <= gmid && b.x1 - b.x0 >= fullWL) ++r.fullL;
            else if (b.x0 >= gmid && b.x1 - b.x0 >= fullWR) ++r.fullR;
        }
    }
    int twoBands = 0;
    for (auto& r : runs) {
        if (r.type == kBandSpan) { r.type = kBandFull; continue; }
        bool two = r.y1 - r.y0 >= minTwo && r.rowsL >= minSide && r.rowsR >= minSide;
        if (two && haveText) two = r.fullL >= 2 || r.fullR >= 2;
        r.type = two ? kBandTwo : kBandFull;
        twoBands += two;
    }
    merge();
    constexpr size_t kMaxBands = 12;
    if (twoBands && runs.size() > kMaxBands) {
        say(debug, "single: fragmented bands=%d gutter=%d..%d", (int)runs.size(), ga + bx0, gb + bx0);
        return out;
    }
    if (!twoBands) { say(debug, "single: no two-column band gutter=%d..%d", ga + bx0, gb + bx0); return out; }

    std::vector<int> gaps;
    for (int y = 1, run = 0; y < bh; ++y) {
        if (!hasL[y]) { ++run; continue; }
        if (run > 0 && hasL[y - run - 1]) gaps.push_back(run);
        run = 0;
    }
    int pad = std::max(1, (int)std::lround(2.0f * u));
    if (!gaps.empty()) {
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        pad = clampi((gaps[gaps.size() / 2] + 1) / 2, 1, (int)std::lround(8.0f * u));
    }

    auto inkRowsIn = [&](int x0, int x1, int y0, int y1, int* first, int* last) {
        *first = -1; *last = -1;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* c = &cov[(size_t)y * bw];
            if (std::any_of(c + x0, c + x1, [](uint8_t v) { return v != 0; })) {
                if (*first < 0) *first = y;
                *last = y;
            }
        }
        return *first >= 0;
    };
    const int padX = std::max(1, (int)std::lround(2.0f * u));
    const int leftX1 = std::min(gmid, ga + padX), rightX0 = std::max(gmid, gb - padX);
    const float fitTwo = (float)std::max(leftX1 - cx0, cx1 - rightX0);
    std::string bands;
    for (size_t i = 0; i < runs.size(); ++i) {
        const int top = i == 0 ? 0 : (runs[i - 1].y1 + runs[i].y0) / 2;
        const int bottom = i + 1 == runs.size() ? bh : (runs[i].y1 + runs[i + 1].y0) / 2;
        auto emit = [&](ColumnRegionKind kind, int x0, int x1, float fit) {
            int first, last;
            if (!inkRowsIn(x0, x1, top, bottom, &first, &last)) return;
            ColumnRegion region;
            region.kind = kind;
            region.rect = {(float)(bx0 + x0), (float)(by0 + std::max(top, first - pad)),
                           (float)(bx0 + x1), (float)(by0 + std::min(bottom, last + 1 + pad))};
            region.fitWidth = fit;
            out.push_back(region);
        };
        if (runs[i].type == kBandTwo) {
            emit(ColumnRegionKind::Left, cx0, leftX1, fitTwo);
            emit(ColumnRegionKind::Right, rightX0, cx1, fitTwo);
            bands += 'T';
        } else {
            emit(ColumnRegionKind::Full, cx0, cx1, (float)cw);
            bands += 'F';
        }
    }
    if (gutterOut) *gutterOut = {(float)(ga + bx0), (float)by0, (float)(gb + bx0), (float)by1};
    say(debug, "two-column gutter=%d..%d%s bands=%s regions=%d pad=%d", ga + bx0, gb + bx0,
        fromHint ? "(hint)" : "", bands.c_str(), (int)out.size(), pad);
    return out;
}

}
