#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "render/pdf_columns.h"

namespace {

using hardcopy::ColumnDetectInput;
using hardcopy::ColumnRect;
using hardcopy::ColumnRegion;
using hardcopy::ColumnRegionKind;

void expect(bool value, const std::string& message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

struct Page {
    int w = 720, h = 932;
    std::vector<uint8_t> px = std::vector<uint8_t>((size_t)720 * 932, 255);
    std::vector<ColumnRect> lines;

    void fill(int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) px[(size_t)y * w + x] = 0;
    }
    void textLine(int x0, int x1, int y) {
        for (int x = x0; x < x1; x += 33) fill(x, y, std::min(x + 30, x1), y + 8);
        lines.push_back({(float)x0, (float)y - 2, (float)x1, (float)y + 10});
    }
    void paragraph(int x0, int x1, int y0, int y1) {
        for (int y = y0; y + 8 <= y1; y += 14) textLine(x0, x1, y);
    }
    ColumnDetectInput input(int bx0, int by0, int bx1, int by1) const {
        ColumnDetectInput in;
        in.pixels = px.data();
        in.width = w;
        in.height = h;
        in.body = {(float)bx0, (float)by0, (float)bx1, (float)by1};
        in.lines = lines;
        in.pxPerPt = 720.0f / 612.0f;
        return in;
    }
};

const char* kinds(const std::vector<ColumnRegion>& r) {
    static std::string s;
    s.clear();
    for (const auto& x : r) s += hardcopy::columnRegionKindName(x.kind), s += ' ';
    return s.c_str();
}

void titleAndTwoColumns() {
    Page p;
    p.textLine(200, 520, 80);
    p.textLine(150, 570, 100);
    p.paragraph(60, 350, 140, 860);
    p.paragraph(370, 660, 140, 860);
    std::string why;
    const auto r = hardcopy::detectColumnRegions(p.input(60, 70, 660, 870), &why);
    expect(r.size() == 3, std::string("title + two columns gives 3 regions: ") + kinds(r) + why);
    expect(r[0].kind == ColumnRegionKind::Full, "title band is full width");
    expect(r[1].kind == ColumnRegionKind::Left && r[2].kind == ColumnRegionKind::Right,
           "left column precedes right column");
    expect(r[1].fitWidth == r[2].fitWidth, "both columns share one scale");
    expect(r[1].rect.x1 <= 361 && r[2].rect.x0 >= 359, "columns split at the gutter");
    expect(r[1].rect.y0 > r[0].rect.y1 - 1, "columns start below the title");
}

void singleColumn() {
    Page p;
    p.paragraph(60, 660, 80, 860);
    std::string why;
    const auto r = hardcopy::detectColumnRegions(p.input(60, 70, 660, 870), &why);
    expect(r.empty(), "single-column text keeps the whole body: " + why);
}

void subfiguresStayWhole() {
    Page p;
    p.fill(60, 80, 340, 300);
    p.fill(380, 80, 660, 300);
    p.textLine(150, 250, 310);
    p.textLine(470, 570, 310);
    p.textLine(60, 660, 330);
    p.paragraph(60, 350, 370, 860);
    p.paragraph(370, 660, 370, 860);
    std::string why;
    const auto r = hardcopy::detectColumnRegions(p.input(60, 70, 660, 870), &why);
    expect(r.size() == 3, std::string("figure band + two columns: ") + kinds(r) + why);
    expect(r[0].kind == ColumnRegionKind::Full && r[0].rect.y0 <= 80 && r[0].rect.y1 >= 340,
           "side-by-side subfigures and caption stay one full-width region");
}

void stampSliverIgnored() {
    Page p;
    p.fill(14, 250, 26, 650);
    p.paragraph(60, 350, 80, 860);
    p.paragraph(370, 660, 80, 860);
    std::string why;
    const auto r = hardcopy::detectColumnRegions(p.input(14, 70, 660, 870), &why);
    expect(r.size() == 2, std::string("two columns despite the stamp: ") + kinds(r) + why);
    expect(r[0].rect.x0 >= 59, "left column excludes the margin stamp");
}

void gutterHintFindsColumnsBelowWideFigure() {
    Page p;
    p.fill(60, 80, 660, 560);
    p.textLine(60, 660, 570);
    p.paragraph(60, 350, 600, 860);
    p.paragraph(370, 660, 600, 860);
    auto in = p.input(60, 70, 660, 870);
    in.hasGutterHint = true;
    in.gutterHint = {352, 0, 368, 0};
    std::string why;
    ColumnRect gutter{};
    const auto r = hardcopy::detectColumnRegions(in, &why, &gutter);
    expect(r.size() == 3, std::string("hinted gutter: full figure then two columns: ") + kinds(r) + why);
    expect(r[0].kind == ColumnRegionKind::Full && r[1].kind == ColumnRegionKind::Left,
           "figure first, then left column");
    expect(gutter.x0 >= 350 && gutter.x1 <= 370, "reports the gutter for the next page");
}

}

int main() {
    titleAndTwoColumns();
    singleColumn();
    subfiguresStayWhole();
    stampSliverIgnored();
    gutterHintFindsColumnsBelowWideFigure();
    std::cout << "pdf columns regression passed\n";
    return 0;
}
