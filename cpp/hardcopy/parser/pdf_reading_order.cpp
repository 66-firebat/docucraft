#include "pdf_reading_order.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace hardcopy {
namespace {

float boxHeight(const ReadingBox& b) { return std::max(0.5f, b.y1 - b.y0); }
float boxWidth(const ReadingBox& b) { return std::max(0.0f, b.x1 - b.x0); }

struct OrderCtx {
    const std::vector<ReadingBox>& boxes;
    float minGutter;
    ReadingOrder* out;
};

void emitLeaf(OrderCtx& c, std::vector<int> ids) {
    if (ids.empty()) return;
    const auto& bx = c.boxes;
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return bx[a].y0 < bx[b].y0; });
    for (size_t i = 0; i < ids.size();) {
        size_t j = i + 1;
        while (j < ids.size()) {
            const ReadingBox& a = bx[ids[i]];
            const ReadingBox& b = bx[ids[j]];
            const float overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            if (overlap < 0.5f * std::min(boxHeight(a), boxHeight(b))) break;
            ++j;
        }
        std::stable_sort(ids.begin() + (long)i, ids.begin() + (long)j,
                         [&](int a, int b) { return bx[a].x0 < bx[b].x0; });
        i = j;
    }
    const int region = (int)c.out->regions.size();
    ReadingBox bounds = bx[ids[0]];
    for (int id : ids) {
        const ReadingBox& b = bx[id];
        bounds.x0 = std::min(bounds.x0, b.x0); bounds.y0 = std::min(bounds.y0, b.y0);
        bounds.x1 = std::max(bounds.x1, b.x1); bounds.y1 = std::max(bounds.y1, b.y1);
        c.out->order.push_back(id);
        c.out->region[id] = region;
    }
    c.out->regions.push_back(bounds);
}

float typicalWidth(const OrderCtx& c, std::vector<int> ids) {
    if (ids.empty()) return 0;
    std::vector<float> w;
    for (int id : ids) w.push_back(boxWidth(c.boxes[id]));
    const size_t k = std::min(w.size() - 1, w.size() * 3 / 4);
    std::nth_element(w.begin(), w.begin() + (long)k, w.end());
    return w[k];
}

bool findGutter(const OrderCtx& c, const std::vector<int>& ids, float* gutter) {
    float x0 = 1e9f, x1 = -1e9f, total = 0;
    for (int id : ids) {
        x0 = std::min(x0, c.boxes[id].x0);
        x1 = std::max(x1, c.boxes[id].x1);
        total += boxHeight(c.boxes[id]);
    }
    const float width = x1 - x0;
    if (width < 3 * c.minGutter) return false;
    struct Candidate { float x, cross, corridor; };
    std::vector<Candidate> candidates;
    float lastL = 0, lastR = 0;
    const float step = std::max(0.5f, width / 600.0f);
    for (float x = x0 + 0.2f * width; x <= x0 + 0.8f * width; x += step) {
        float cross = 0, left = 0, right = 0, lmax = -1e9f, rmin = 1e9f;
        for (int id : ids) {
            const ReadingBox& b = c.boxes[id];
            if (b.x1 <= x) { left += boxHeight(b); lmax = std::max(lmax, b.x1); }
            else if (b.x0 >= x) { right += boxHeight(b); rmin = std::min(rmin, b.x0); }
            else cross += boxHeight(b);
        }
        const float corridor = rmin - lmax;
        if (corridor < c.minGutter || left < 0.1f * total || right < 0.1f * total) continue;
        if (cross > 0.4f * total) continue;
        if (!candidates.empty() && lmax == lastL && rmin == lastR) continue;
        lastL = lmax;
        lastR = rmin;
        candidates.push_back({x, cross, corridor});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (std::fabs(a.cross - b.cross) > 0.01f) return a.cross < b.cross;
        return a.corridor > b.corridor;
    });
    const float minColumn = std::max(2 * c.minGutter, 0.15f * width);
    for (const Candidate& cand : candidates) {
        std::vector<int> left, right, cross;
        for (int id : ids) {
            const ReadingBox& b = c.boxes[id];
            if (b.x1 <= cand.x) left.push_back(id);
            else if (b.x0 >= cand.x) right.push_back(id);
            else cross.push_back(id);
        }
        if (typicalWidth(c, left) < minColumn || typicalWidth(c, right) < minColumn) continue;
        bool beside = false;
        for (int k : cross) {
            const ReadingBox& a = c.boxes[k];
            for (const auto* side : {&left, &right})
                for (int id : *side) {
                    const ReadingBox& b = c.boxes[id];
                    const float overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
                    beside |= overlap > 0.3f * std::min(boxHeight(a), boxHeight(b));
                }
            if (beside) break;
        }
        if (beside) continue;
        *gutter = cand.x;
        return true;
    }
    return false;
}

size_t headCut(const OrderCtx& c, const std::vector<int>& byY, float lineH) {
    const auto& bx = c.boxes;
    float top = 1e9f, bottom = -1e9f;
    for (int id : byY) { top = std::min(top, bx[id].y0); bottom = std::max(bottom, bx[id].y1); }
    float reach = bx[byY[0]].y1;
    for (size_t k = 1; k < byY.size(); ++k) {
        const ReadingBox& b = bx[byY[k]];
        if (b.y0 > top + 0.4f * (bottom - top)) break;
        if (b.y0 - reach >= 2.0f * lineH) return k;
        reach = std::max(reach, b.y1);
    }
    return 0;
}

void orderRegion(OrderCtx& c, std::vector<int> ids, int depth) {
    if (ids.size() <= 1 || depth > 8) { emitLeaf(c, std::move(ids)); return; }
    const auto& bx = c.boxes;
    if (depth == 0) {
        std::vector<int> byY = ids;
        std::stable_sort(byY.begin(), byY.end(), [&](int a, int b) { return bx[a].y0 < bx[b].y0; });
        std::vector<float> heights;
        for (int id : ids) heights.push_back(boxHeight(bx[id]));
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        const size_t cut = headCut(c, byY, heights[heights.size() / 2]);
        if (cut > 0) {
            orderRegion(c, std::vector<int>(byY.begin(), byY.begin() + (long)cut), depth + 1);
            orderRegion(c, std::vector<int>(byY.begin() + (long)cut, byY.end()), depth + 1);
            return;
        }
    }
    float gutter = 0;
    if (findGutter(c, ids, &gutter)) {
        std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return bx[a].y0 < bx[b].y0; });
        std::vector<int> run;
        bool runCross = false;
        auto flush = [&] {
            if (run.empty()) return;
            if (runCross) {
                orderRegion(c, run, depth + 1);
            } else {
                std::vector<int> left, right;
                for (int id : run) (bx[id].x1 <= gutter ? left : right).push_back(id);
                orderRegion(c, left, depth + 1);
                orderRegion(c, right, depth + 1);
            }
            run.clear();
        };
        for (int id : ids) {
            const bool cross = bx[id].x0 < gutter && bx[id].x1 > gutter;
            if (!run.empty() && cross != runCross) flush();
            runCross = cross;
            run.push_back(id);
        }
        flush();
        return;
    }
    std::vector<int> byY = ids;
    std::stable_sort(byY.begin(), byY.end(), [&](int a, int b) { return bx[a].y0 < bx[b].y0; });
    std::vector<float> heights;
    for (int id : ids) heights.push_back(boxHeight(bx[id]));
    std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
    const float lineH = heights[heights.size() / 2];
    float reach = bx[byY[0]].y1, bestGap = 0;
    size_t cut = 0;
    for (size_t k = 1; k < byY.size(); ++k) {
        const float gap = bx[byY[k]].y0 - reach;
        if (gap > bestGap) { bestGap = gap; cut = k; }
        reach = std::max(reach, bx[byY[k]].y1);
    }
    if (cut == 0 || bestGap < lineH) { emitLeaf(c, std::move(ids)); return; }
    orderRegion(c, std::vector<int>(byY.begin(), byY.begin() + (long)cut), depth + 1);
    orderRegion(c, std::vector<int>(byY.begin() + (long)cut, byY.end()), depth + 1);
}

}

ReadingOrder readingOrder(const std::vector<ReadingBox>& boxes, float minGutter) {
    ReadingOrder out;
    out.region.assign(boxes.size(), 0);
    if (boxes.empty()) return out;
    OrderCtx c{boxes, std::max(1.0f, minGutter), &out};
    std::vector<int> ids(boxes.size());
    for (size_t i = 0; i < ids.size(); ++i) ids[i] = (int)i;
    orderRegion(c, std::move(ids), 0);
    return out;
}

namespace {

struct RegionMetrics {
    float left = 0, right = 0, pitch = 0;
};

float median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

bool similarSize(float a, float b) { return std::fabs(a - b) <= 0.07f * std::max(a, b); }

}

std::vector<int> segmentLines(const std::vector<ReadingLine>& lines,
                              std::vector<ReadingParagraph>* paragraphs) {
    const size_t n = lines.size();
    std::vector<int> paraOf(n, -1);
    paragraphs->clear();
    if (n == 0) return paraOf;

    std::map<int, std::vector<size_t>> byRegion;
    for (size_t i = 0; i < n; ++i) byRegion[lines[i].region].push_back(i);
    std::map<int, RegionMetrics> metrics;
    std::map<int, std::vector<float>> pitchBySize;
    for (const auto& entry : byRegion) {
        const auto& idx = entry.second;
        RegionMetrics m;
        std::map<int, int> lefts;
        float minX = 1e9f;
        std::vector<float> rights, pitches;
        for (size_t k = 0; k < idx.size(); ++k) {
            const ReadingLine& l = lines[idx[k]];
            ++lefts[(int)std::lround(l.x0)];
            minX = std::min(minX, l.x0);
            rights.push_back(l.x1);
            if (k > 0) {
                const ReadingLine& p = lines[idx[k - 1]];
                const float d = l.baseline - p.baseline;
                if (similarSize(l.size, p.size) && d > 0.5f * l.size && d < 3.0f * l.size) {
                    pitches.push_back(d);
                    pitchBySize[(int)std::lround(l.size)].push_back(d);
                }
            }
        }
        int bestCount = 0;
        m.left = minX;
        for (const auto& lc : lefts)
            if (lc.second > bestCount && lc.second >= 2) { bestCount = lc.second; m.left = (float)lc.first; }
        std::sort(rights.begin(), rights.end());
        m.right = rights.size() >= 5 ? rights[(rights.size() * 85) / 100] : rights.back();
        m.pitch = pitches.size() >= 3 ? median(pitches) : 0;
        metrics[entry.first] = m;
    }
    std::map<int, int> sizeCount;
    for (const auto& l : lines) ++sizeCount[(int)std::lround(l.size * 2)];
    int bodyKey = 0, bodyCount = 0;
    for (const auto& sc : sizeCount)
        if (sc.second > bodyCount) { bodyCount = sc.second; bodyKey = sc.first; }
    const float bodySize = bodyKey / 2.0f;
    auto pitchFor = [&](const ReadingLine& l) {
        const RegionMetrics& m = metrics[l.region];
        if (m.pitch > 0) return m.pitch;
        auto it = pitchBySize.find((int)std::lround(l.size));
        return it != pitchBySize.end() && !it->second.empty() ? median(it->second) : 0.0f;
    };
    auto centered = [&](const ReadingLine& l) {
        const RegionMetrics& m = metrics[l.region];
        const float em = std::max(1.0f, l.size);
        const float lm = l.x0 - m.left, rm = m.right - l.x1;
        return lm > 2 * em && rm > 2 * em && std::fabs(lm - rm) < 1.5f * em;
    };

    struct Para { size_t last = 0; int lines = 0; float contX = -1; };
    std::vector<Para> paras;
    auto start = [&](size_t i, bool item) {
        paraOf[i] = (int)paras.size();
        Para p;
        p.last = i;
        p.lines = 1;
        if (item) p.contX = lines[i].itemTextX - metrics[lines[i].region].left;
        paras.push_back(p);
        paragraphs->push_back(ReadingParagraph{item});
    };
    auto join = [&](size_t i, int para) {
        paraOf[i] = para;
        Para& p = paras[para];
        if (p.lines == 1 && p.contX < 0) p.contX = lines[i].x0 - metrics[lines[i].region].left;
        p.last = i;
        ++p.lines;
    };

    start(0, lines[0].itemTextX >= 0);
    ReadingLine prev = lines[0];
    std::vector<size_t> fragments;
    for (size_t i = 1; i < n; ++i) {
        ReadingLine l = lines[i];
        const ReadingLine piece = lines[i - 1];
        auto advance = [&](bool sameLine) {
            if (sameLine) {
                prev.x0 = std::min(prev.x0, l.x0);
                prev.x1 = std::max(prev.x1, l.x1);
                prev.sentenceEnd = l.sentenceEnd;
                prev.bold = prev.bold && l.bold;
            } else {
                prev = l;
            }
        };
        const RegionMetrics& m = metrics[l.region];
        const float em = std::max(1.0f, l.size);
        if (l.itemTextX >= 0) {
            const bool opens = !l.itemOrdered || l.region != prev.region || prev.sentenceEnd ||
                               (*paragraphs)[paraOf[i - 1]].item ||
                               prev.x1 + l.firstWordW + 0.3f * em < m.right;
            if (opens) { start(i, true); advance(false); continue; }
        }
        if (l.region != prev.region) {
            size_t q = i;
            for (size_t k = i; k-- > 0 && i - k <= 40;) {
                if (similarSize(lines[k].size, l.size)) { q = k; break; }
            }
            bool resume = false;
            if (q < i && similarSize(l.size, bodySize)) {
                const ReadingLine& last = lines[q];
                const int para = paraOf[q];
                const RegionMetrics& lm = metrics[last.region];
                resume = paras[para].last == q && !last.sentenceEnd &&
                         last.x1 - last.x0 >= 15 * std::max(1.0f, last.size) &&
                         last.x1 >= lm.right - 2 * std::max(1.0f, last.size) &&
                         last.bold == l.bold && l.x0 <= m.left + 0.5f * em && !centered(l) &&
                         !centered(last);
                if (resume) { join(i, para); advance(false); continue; }
            }
            start(i, false);
            advance(false);
            continue;
        }
        const int cur = paraOf[i - 1];
        Para& open = paras[cur];
        const float step = l.baseline - prev.baseline;
        const float pitch = pitchFor(l);
        const bool lineGoesOn = i + 1 < n && std::fabs(lines[i + 1].baseline - l.baseline) < 0.3f * em &&
                                lines[i + 1].x0 >= l.x1 - 1.0f;
        const bool small = l.size < 0.9f * prev.size;
        if (pitch > 0 && l.x1 - l.x0 <= 6 * em && step > -1.5f * em && step < 1.5f * pitch &&
            !lineGoesOn && l.bold == prev.bold && (small || std::fabs(step) < 0.75f * pitch) &&
            std::fabs(step - std::round(step / pitch) * pitch) > 0.25f * pitch) {
            paraOf[i] = cur;
            open.last = i;
            fragments.push_back(i);
            continue;
        }
        for (size_t k : fragments)
            if (std::fabs(lines[k].baseline - l.baseline) < 0.75f * em && lines[k].x1 <= l.x0 + 1.0f)
                l.x0 = std::min(l.x0, lines[k].x0);
        fragments.clear();
        if (std::fabs(l.baseline - piece.baseline) < 0.3f * em && l.x0 >= piece.x1 - 1.0f) {
            paraOf[i] = cur;
            open.last = i;
            advance(true);
            continue;
        }
        if (!similarSize(l.size, prev.size) || l.bold != prev.bold) { start(i, false); advance(false); continue; }
        if (step < 0.3f * em || (pitch > 0 && step > 1.4f * pitch)) { start(i, false); advance(false); continue; }
        if (prev.sentenceEnd && prev.x1 + l.firstWordW + 0.3f * em < m.right) {
            start(i, false);
            advance(false);
            continue;
        }
        if (prev.x0 > m.left + 3 * em && l.x0 <= m.left + 0.5f * em) { start(i, false); advance(false); continue; }
        const bool cl = centered(l), cp = centered(prev);
        if (cl || cp) {
            if (cl && cp) join(i, cur);
            else start(i, false);
            advance(false);
            continue;
        }
        if (open.contX >= 0) {
            const float x = l.x0 - m.left;
            if (x > open.contX + 0.8f * em || x < open.contX - 0.8f * em) start(i, false);
            else join(i, cur);
            advance(false);
            continue;
        }
        if (prev.x1 < m.left + 0.6f * (m.right - m.left) &&
            (prev.x0 <= m.left + 0.5f * em || l.x0 > m.left + 0.8f * em)) {
            start(i, false);
            advance(false);
            continue;
        }
        join(i, cur);
        advance(false);
    }
    return paraOf;
}

}
