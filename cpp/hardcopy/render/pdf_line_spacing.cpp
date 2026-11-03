#include "pdf_line_spacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hardcopy {
namespace {

constexpr uint8_t kInkLevel = 235;

struct Band {
    int y0 = 0, y1 = 0;
    int x0 = 0, x1 = 0;
    std::vector<int> glyphs;
    bool line = false;
    bool heading = false;
    float size = 0;
    float baseline = 0;
    float tx0 = 0, tx1 = 0;
};

float percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1,
        (size_t)std::floor(p * (double)(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

void absorb(Band& into, Band& from) {
    into.y0 = std::min(into.y0, from.y0);
    into.y1 = std::max(into.y1, from.y1);
    into.x0 = std::min(into.x0, from.x0);
    into.x1 = std::max(into.x1, from.x1);
    into.glyphs.insert(into.glyphs.end(), from.glyphs.begin(), from.glyphs.end());
}

}

const char* spacingPieceKindName(SpacingPieceKind kind) {
    switch (kind) {
    case SpacingPieceKind::Line: return "line";
    case SpacingPieceKind::Heading: return "heading";
    case SpacingPieceKind::Block: return "block";
    }
    return "block";
}

SpacingPlan planLineSpacing(const SpacingInput& in, std::string* debug) {
    SpacingPlan plan;
    auto say = [&](const char* msg) { if (debug) *debug = msg; };
    if (!in.pixels || in.width <= 0 || in.height <= 0) { say("empty region"); return plan; }
    const int stride = in.stride > 0 ? in.stride : in.width;

    std::vector<Band> bands;
    {
        std::vector<int> rowX0(in.height, -1), rowX1(in.height, -1);
        for (int y = 0; y < in.height; ++y) {
            const uint8_t* row = in.pixels + (size_t)y * stride;
            int x0 = 0;
            while (x0 < in.width && row[x0] >= kInkLevel) ++x0;
            if (x0 == in.width) continue;
            int x1 = in.width;
            while (x1 > x0 && row[x1 - 1] >= kInkLevel) --x1;
            rowX0[y] = x0;
            rowX1[y] = x1;
        }
        for (int y = 0; y < in.height;) {
            while (y < in.height && rowX0[y] < 0) ++y;
            if (y >= in.height) break;
            Band b;
            b.y0 = y; b.x0 = in.width; b.x1 = 0;
            while (y < in.height && rowX0[y] >= 0) {
                b.x0 = std::min(b.x0, rowX0[y]);
                b.x1 = std::max(b.x1, rowX1[y]);
                ++y;
            }
            b.y1 = y;
            bands.push_back(std::move(b));
        }
    }
    if (bands.empty()) { say("no ink"); return plan; }

    std::vector<float> allSizes;
    for (size_t i = 0; i < in.glyphs.size(); ++i) {
        const auto& g = in.glyphs[i];
        if (!(g.size > 0)) continue;
        const float y = g.baseline;
        auto it = std::upper_bound(bands.begin(), bands.end(), y,
            [](float v, const Band& b) { return v < (float)b.y1; });
        int best = -1;
        float bestDistance = 0.25f * g.size;
        for (auto c : {it, it == bands.begin() ? it : it - 1}) {
            if (c == bands.end()) continue;
            const float d = y < c->y0 ? c->y0 - y : (y >= c->y1 ? y - (c->y1 - 1) : 0.f);
            if (d <= bestDistance) { bestDistance = d; best = (int)(c - bands.begin()); }
        }
        if (best < 0) continue;
        bands[best].glyphs.push_back((int)i);
        allSizes.push_back(g.size);
    }
    const bool hasText = !allSizes.empty();

    float scanLineHeight = 0;
    if (!hasText) {
        std::vector<float> heights, wide;
        for (const auto& b : bands) {
            heights.push_back((float)(b.y1 - b.y0));
            if (b.x1 - b.x0 >= in.width / 4) wide.push_back((float)(b.y1 - b.y0));
        }
        scanLineHeight = percentile(wide.empty() ? heights : wide, 0.5);
    }
    const float ref = hasText ? percentile(allSizes, 0.5) : scanLineHeight;

    for (bool changed = true; changed && bands.size() > 1;) {
        changed = false;
        for (size_t i = 0; i < bands.size(); ++i) {
            const Band& b = bands[i];
            const float h = (float)(b.y1 - b.y0);
            const bool small = hasText ? (b.glyphs.empty() && h < 0.6f * ref)
                                       : h < 0.35f * scanLineHeight;
            if (!small) continue;
            const float up = i > 0 ? (float)(b.y0 - bands[i - 1].y1) : 1e9f;
            const float down = i + 1 < bands.size() ? (float)(bands[i + 1].y0 - b.y1) : 1e9f;
            const float limit = 0.4f * ref;
            if (std::min(up, down) > limit) continue;
            const size_t into = up <= down ? i - 1 : i + 1;
            absorb(bands[into], bands[i]);
            bands.erase(bands.begin() + (long)i);
            changed = true;
            break;
        }
    }

    for (auto& b : bands) {
        if (hasText) {
            if (b.glyphs.empty()) continue;
            std::vector<float> sizes, bases;
            for (int gi : b.glyphs) {
                sizes.push_back(in.glyphs[gi].size);
                bases.push_back(in.glyphs[gi].baseline);
            }
            b.size = percentile(sizes, 0.5);
            std::sort(bases.begin(), bases.end());
            const float tol = 0.25f * b.size;
            size_t bestStart = 0, bestCount = 0;
            for (size_t i = 0, j = 0; i < bases.size(); ++i) {
                while (bases[i] - bases[j] > tol) ++j;
                if (i - j + 1 > bestCount) { bestCount = i - j + 1; bestStart = j; }
            }
            b.baseline = bases[bestStart + bestCount / 2];
            const float share = (float)bestCount / (float)bases.size();
            b.line = share >= 0.8f && (float)(b.y1 - b.y0) <= 1.9f * b.size;
            b.tx0 = (float)in.width; b.tx1 = 0;
            for (int gi : b.glyphs) {
                const auto& g = in.glyphs[gi];
                if (std::fabs(g.baseline - b.baseline) > tol) continue;
                b.tx0 = std::min(b.tx0, g.x);
                b.tx1 = std::max(b.tx1, g.x + g.advance);
            }
        } else {
            const float h = (float)(b.y1 - b.y0);
            b.line = h >= 0.5f * scanLineHeight && h <= 1.5f * scanLineHeight &&
                     h <= 0.12f * (float)in.width;
            b.size = ref;
            b.baseline = (float)b.y0 + 0.8f * ref;
            b.tx0 = (float)b.x0; b.tx1 = (float)b.x1;
        }
    }

    std::vector<float> bodySizes;
    for (const auto& b : bands) {
        if (!b.line) continue;
        if (hasText) for (int gi : b.glyphs) bodySizes.push_back(in.glyphs[gi].size);
        else bodySizes.push_back(b.size);
    }
    if (bodySizes.empty()) {
        SpacingPiece only;
        only.y0 = bands.front().y0; only.y1 = bands.back().y1;
        plan.pieces.push_back(only);
        say("no text lines");
        return plan;
    }
    const float body = percentile(bodySizes, 0.5);
    for (auto& b : bands) b.heading = b.line && b.size > 1.18f * body;
    auto similar = [](const Band& a, const Band& b) {
        const float r = a.size / std::max(1e-3f, b.size);
        return r > 0.87f && r < 1.15f;
    };

    std::vector<float> pitches;
    for (size_t i = 1; i < bands.size(); ++i) {
        const Band& a = bands[i - 1];
        const Band& b = bands[i];
        if (!a.line || !b.line || a.heading || b.heading || !similar(a, b)) continue;
        const float em = 0.5f * (a.size + b.size);
        const float p = (b.baseline - a.baseline) / em;
        if (p > 0.9f && p < 2.6f) pitches.push_back(p);
    }
    const float pitchEm = pitches.empty() ? 1.2f : percentile(pitches, 0.5);

    std::vector<float> lefts, rights;
    for (const auto& b : bands) {
        if (!b.line || b.heading) continue;
        lefts.push_back(b.tx0);
        rights.push_back(b.tx1);
    }
    const float left = percentile(lefts, 0.1);
    const float right = percentile(rights, 0.9);
    int flush = 0;
    for (float r : rights) if (r >= right - 0.6f * body) ++flush;
    const bool justified = rights.size() >= 4 && flush >= 0.55 * (double)rights.size();

    struct Span { size_t first, last; };
    std::vector<Span> spans;
    for (size_t i = 0; i < bands.size(); ++i) {
        const bool joinBlock = !bands[i].line && !spans.empty() &&
            !bands[spans.back().last].line &&
            (float)(bands[i].y0 - bands[spans.back().last].y1) <= 1.5f * body;
        if (joinBlock) spans.back().last = i;
        else spans.push_back({i, i});
    }
    auto indentedStart = [&](size_t first, size_t last, const Band* prev) {
        const Band& b = bands[first];
        const float em = b.size;
        if (!b.line || b.heading) return false;
        if (!(b.tx0 > left + 0.6f * em && b.tx0 < left + 4.5f * em)) return false;
        if (prev && std::fabs(prev->tx0 - b.tx0) <= 0.3f * em) return false;
        if (last + 1 < bands.size()) {
            const Band& c = bands[last + 1];
            if (c.line && std::fabs(c.tx0 - b.tx0) <= 0.3f * em) return false;
        }
        return true;
    };

    const double L = in.lineHeight;
    const double PE = std::max(0.0, in.paraSpacingEm);
    const float lineDelta = (float)(L - pitchEm) * body;
    int lineCount = 0, paraCount = 0, headingCount = 0, blockCount = 0;
    long added = 0;
    std::vector<float> lineGaps;
    for (size_t k = 0; k < spans.size(); ++k) {
        const Band& B = bands[spans[k].first];
        SpacingPiece out;
        out.y0 = B.y0;
        out.y1 = bands[spans[k].last].y1;
        out.kind = !B.line ? SpacingPieceKind::Block
                 : B.heading ? SpacingPieceKind::Heading : SpacingPieceKind::Line;
        if (out.kind == SpacingPieceKind::Line) ++lineCount;
        else if (out.kind == SpacingPieceKind::Heading) ++headingCount;
        else ++blockCount;
        if (k == 0) { plan.pieces.push_back(out); continue; }
        const Band& A = bands[spans[k - 1].last];
        const float srcGap = (float)(out.y0 - plan.pieces.back().y1);
        float gap = srcGap;
        bool para = false, plainLine = false;
        if (A.line && B.line && !A.heading && !B.heading && similar(A, B)) {
            const float em = 0.5f * (A.size + B.size);
            const float pitch = B.baseline - A.baseline;
            if (pitch > 3.0f * pitchEm * em) {
                para = true;
                gap = std::max(srcGap, srcGap + (float)(L + PE) * em - pitch);
            } else {
                const bool indent = indentedStart(spans[k].first, spans[k].last, &A);
                const bool shortPrev = justified && A.tx1 < right - 2.0f * em;
                const bool spaced = pitch > (pitchEm + 0.35f) * em;
                para = indent || shortPrev || spaced;
                gap = srcGap + (float)(L + (para ? PE : 0.0)) * em - pitch;
                plainLine = !para;
            }
        } else if (A.line && B.line) {
            gap = srcGap + std::max(0.f, lineDelta);
            para = A.heading && !B.heading;
        } else {
            gap = srcGap + 0.5f * std::max(0.f, lineDelta);
        }
        const float minGap = std::min(srcGap, std::max(2.f, 0.12f * body));
        gap = std::max(gap, minGap);
        out.gapBefore = (int)std::lround(gap);
        out.paragraphStart = para;
        if (para) ++paraCount;
        if (plainLine) lineGaps.push_back(gap);
        added += out.gapBefore - (long)srcGap;
        plan.pieces.push_back(out);
    }
    plan.leadGap = lineGaps.empty()
        ? (int)std::lround(std::max(2.0, (L - 0.9) * body))
        : (int)std::lround(percentile(lineGaps, 0.5));
    if (indentedStart(spans[0].first, spans[0].last, nullptr)) {
        plan.pieces[0].paragraphStart = true;
        plan.leadGap += (int)std::lround(PE * bands[spans[0].first].size);
        ++paraCount;
    }

    for (size_t k = 0; k < plan.pieces.size(); ++k) {
        auto& p = plan.pieces[k];
        if (k == 0) { if (p.y0 > 0) --p.y0; continue; }
        auto& prev = plan.pieces[k - 1];
        if (p.y0 - prev.y1 >= 3) {
            ++prev.y1; --p.y0;
            p.gapBefore = std::max(0, p.gapBefore - 2);
        }
    }
    if (plan.pieces.back().y1 < in.height) ++plan.pieces.back().y1;
    plan.leadGap = std::max(0, plan.leadGap - 2);

    if (debug) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "body=%.1fpx srcPitch=%.2fem target=%.2fem para=%.2fem lines=%d paragraphs=%d "
            "headings=%d blocks=%d justified=%s scan=%s rowsAdded=%+ld leadGap=%d",
            body, pitchEm, L, PE, lineCount, paraCount, headingCount, blockCount,
            justified ? "yes" : "no", hasText ? "no" : "yes", added, plan.leadGap);
        *debug = msg;
    }
    return plan;
}

}
