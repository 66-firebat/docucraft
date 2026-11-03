#include "render/pdf_line_spacing.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace hardcopy;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct Page {
    int w, h;
    std::vector<uint8_t> px;
    std::vector<SpacingGlyph> glyphs;
    Page(int width, int height) : w(width), h(height), px((size_t)width * height, 255) {}
    void ink(int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) px[(size_t)y * w + x] = 0;
    }
    void line(int x0, int x1, float baseline, float size, bool withGlyphs = true) {
        ink(x0, (int)std::lround(baseline - 0.75f * size), x1, (int)std::lround(baseline + 0.2f * size));
        if (!withGlyphs) return;
        for (float x = (float)x0; x + size <= (float)x1 + 0.5f; x += size)
            glyphs.push_back({x, size, baseline, size});
    }
    SpacingInput input(double lh, double para) const {
        SpacingInput in;
        in.pixels = px.data(); in.width = w; in.height = h; in.stride = w;
        in.glyphs = glyphs; in.lineHeight = lh; in.paraSpacingEm = para;
        return in;
    }
};

bool coversInk(const Page& p, const SpacingPlan& plan) {
    for (size_t k = 1; k < plan.pieces.size(); ++k)
        if (plan.pieces[k].y0 < plan.pieces[k - 1].y1) return false;
    for (int y = 0; y < p.h; ++y) {
        bool ink = false;
        for (int x = 0; x < p.w && !ink; ++x) ink = p.px[(size_t)y * p.w + x] < 235;
        if (!ink) continue;
        bool in = false;
        for (const auto& piece : plan.pieces) in |= y >= piece.y0 && y < piece.y1;
        if (!in) return false;
    }
    return true;
}

float outputPitch(const SpacingPlan& plan, size_t k, float baseA, float baseB) {
    const auto& a = plan.pieces[k - 1];
    const auto& b = plan.pieces[k];
    return (a.y1 - baseA) + b.gapBefore + (baseB - b.y0);
}
}

int main() {
    const float S = 30;

    {
        Page p(900, 600);
        std::vector<float> base;
        for (int i = 0; i < 10; ++i) {
            const float b = 40 + i * 1.2f * S;
            base.push_back(b);
            const int x0 = i == 5 ? 60 : 0;
            const int x1 = i == 4 ? 600 : 900;
            p.line(x0, x1, b, S);
        }
        std::string why;
        const auto plan = planLineSpacing(p.input(1.8, 1.0), &why);
        std::printf("body: %s\n", why.c_str());
        check(plan.pieces.size() == 10, "ten line pieces");
        check(coversInk(p, plan), "body ink covered");
        for (size_t k = 1; k < plan.pieces.size() && plan.pieces.size() == 10; ++k) {
            const float pitch = outputPitch(plan, k, base[k - 1], base[k]);
            const float want = (k == 5 ? 2.8f : 1.8f) * S;
            if (std::fabs(pitch - want) > 1.5f) {
                std::printf("  k=%zu pitch=%.1f want=%.1f\n", k, pitch, want);
                check(false, "body pitch follows preset");
            }
            check(plan.pieces[k].paragraphStart == (k == 5), "paragraph start only at line 5");
        }
    }

    {
        Page p(900, 800);
        std::vector<float> base;
        for (int i = 0; i < 8; ++i) { base.push_back(40 + i * 2.2f * S); p.line(0, 900, base.back(), S); }
        const auto plan = planLineSpacing(p.input(1.4, 0.0));
        check(plan.pieces.size() == 8, "eight loose lines");
        for (size_t k = 1; k < plan.pieces.size() && plan.pieces.size() == 8; ++k) {
            const float pitch = outputPitch(plan, k, base[k - 1], base[k]);
            check(std::fabs(pitch - 1.4f * S) <= 1.5f, "loose text tightened to 1.4em");
            check(plan.pieces[k].gapBefore >= 0, "gap non-negative");
        }
    }

    {
        Page p(900, 900);
        p.line(0, 900, 40, S);
        p.line(0, 900, 40 + 1.2f * S, S);
        p.ink(100, 120, 800, 300);
        p.ink(100, 330, 800, 500);
        p.line(0, 900, 560, S);
        p.line(0, 900, 560 + 1.2f * S, S);
        const auto plan = planLineSpacing(p.input(1.8, 0.0));
        check(coversInk(p, plan), "figure ink covered");
        int blocks = 0;
        for (const auto& piece : plan.pieces) {
            if (piece.kind != SpacingPieceKind::Block) continue;
            ++blocks;
            check(piece.y0 <= 120 && piece.y1 >= 500, "figure stays one piece");
        }
        check(blocks == 1, "one block");
    }

    {
        Page p(900, 1200);
        p.line(0, 900, 40, S);
        p.line(0, 900, 40 + 1.2f * S, S);
        p.ink(300, 150, 600, 200);
        p.ink(300, 900, 600, 1000);
        const auto plan = planLineSpacing(p.input(1.8, 0.0));
        int blocks = 0;
        for (const auto& piece : plan.pieces) blocks += piece.kind == SpacingPieceKind::Block;
        check(blocks == 2, "far-apart blocks are separate pieces");
        check(plan.pieces.back().gapBefore >= 690, "wide blank between blocks kept");
    }

    {
        Page p(900, 400);
        p.line(60, 900, 40, S);
        for (int i = 1; i < 5; ++i) p.line(0, 900, 40 + i * 1.2f * S, S);
        const auto plan = planLineSpacing(p.input(1.8, 1.0));
        check(!plan.pieces.empty() && plan.pieces[0].paragraphStart, "lead indent is a paragraph");
        check(plan.pieces.size() > 1 && plan.leadGap >= plan.pieces[1].gapBefore + 28,
              "lead gap carries paragraph spacing");
    }

    {
        Page p(900, 400);
        p.line(0, 900, 60, S);
        p.ink(200, 60 + (int)(1.2f * S) - 30, 220, 60 + (int)(1.2f * S) - 26);
        p.line(0, 900, 60 + 1.2f * S, S);
        const auto plan = planLineSpacing(p.input(1.8, 0.0));
        check(plan.pieces.size() == 2, "accent merged, two pieces");
        check(coversInk(p, plan), "accent covered");
    }

    {
        Page p(900, 400);
        p.line(0, 900, 60, S);
        p.line(0, 900, 60 + 0.95f * S, S);
        p.line(0, 900, 60 + 2.2f * S, S);
        const auto plan = planLineSpacing(p.input(1.8, 0.0));
        check(!plan.pieces.empty() && plan.pieces[0].kind == SpacingPieceKind::Block,
              "glued lines are a block");
        check(coversInk(p, plan), "glued covered");
    }

    {
        Page p(900, 700);
        std::vector<float> base;
        for (int i = 0; i < 8; ++i) {
            base.push_back(40 + i * 1.3f * S);
            p.line(i == 4 ? 60 : 0, 900, base.back(), S, false);
        }
        std::string why;
        const auto plan = planLineSpacing(p.input(1.8, 0.5), &why);
        std::printf("scan: %s\n", why.c_str());
        check(plan.pieces.size() == 8, "scan lines found");
        int paras = 0;
        for (const auto& piece : plan.pieces) paras += piece.paragraphStart;
        check(paras == 1, "scan paragraph from indent");
        check(coversInk(p, plan), "scan covered");
    }

    {
        Page p(900, 600);
        std::vector<float> base;
        const int x0s[] = {0, 45, 45, 45, 0, 45, 45};
        for (int i = 0; i < 7; ++i) { base.push_back(40 + i * 1.2f * S); p.line(x0s[i], 900, base.back(), S); }
        const auto plan = planLineSpacing(p.input(1.6, 1.0));
        int paras = 0;
        for (const auto& piece : plan.pieces) paras += piece.paragraphStart;
        check(paras == 0, "hanging indents are not paragraphs");
    }

    {
        Page p(100, 100);
        check(planLineSpacing(p.input(1.6, 0)).pieces.empty(), "blank region has no pieces");
        SpacingInput none;
        check(planLineSpacing(none).pieces.empty(), "null input");
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("pdf_line_spacing_test: all passed\n");
    return 0;
}
