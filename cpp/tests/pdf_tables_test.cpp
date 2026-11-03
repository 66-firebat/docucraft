#include "parser/pdf_tables.h"

#include <cstdio>

using namespace hardcopy;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
const fz_rect kPage{0, 0, 600, 800};

void hline(PdfTableEvidence& ev, float y, float x0, float x1) { ev.rules.push_back({x0, y - 0.25f, x1, y + 0.25f}); }
void vline(PdfTableEvidence& ev, float x, float y0, float y1) { ev.rules.push_back({x - 0.25f, y0, x + 0.25f, y1}); }

void grid(PdfTableEvidence& ev, const std::vector<float>& xs, const std::vector<float>& ys) {
    for (float y : ys) hline(ev, y, xs.front(), xs.back());
    for (float x : xs) vline(ev, x, ys.front(), ys.back());
}

int realCells(const std::vector<PdfTableCell>& row) {
    int n = 0;
    for (const auto& c : row) n += !c.placeholder;
    return n;
}
}

int main() {
    {
        PdfTableEvidence ev;
        grid(ev, {100, 200, 300, 400}, {100, 120, 140, 160});
        const auto t = findPdfTables(ev, kPage, nullptr, nullptr);
        check(t.size() == 1, "one table");
        if (t.size() == 1) {
            check(t[0].rows.size() == 3 && t[0].rows[0].size() == 3, "3x3");
            check(t[0].rows[1][1].rect.x0 == 200 && t[0].rows[1][1].rect.y0 == 120, "cell rect");
        }
    }
    {
        PdfTableEvidence ev;
        for (float y : {100.f, 120.f, 140.f, 160.f}) hline(ev, y, 100, 400);
        vline(ev, 100, 100, 160);
        vline(ev, 400, 100, 160);
        vline(ev, 200, 100, 160);
        vline(ev, 300, 120, 160);
        const auto t = findPdfTables(ev, kPage, nullptr, nullptr);
        check(t.size() == 1 && t[0].rows.size() == 3, "span table");
        if (t.size() == 1 && t[0].rows.size() == 3) {
            check(t[0].rows[0].size() == 3 && realCells(t[0].rows[0]) == 2, "header keeps 3 slots, 2 cells");
            check(t[0].rows[0][1].rect.x1 == 400, "spanning cell covers both columns");
            check(t[0].rows[0][1].colspan == 2 && t[0].rows[0][1].rowspan == 1, "colspan 2");
            check(t[0].rows[0][2].placeholder, "covered slot is a placeholder");
            check(realCells(t[0].rows[1]) == 3, "body row has 3 cells");
        }
    }
    {
        PdfTableEvidence ev;
        for (float x : {100.f, 200.f, 300.f}) vline(ev, x, 100, 160);
        hline(ev, 100, 100, 300);
        hline(ev, 120, 100, 300);
        hline(ev, 140, 200, 300);
        hline(ev, 160, 100, 300);
        const auto t = findPdfTables(ev, kPage, nullptr, nullptr);
        check(t.size() == 1 && t[0].rows.size() == 3, "rowspan table");
        if (t.size() == 1 && t[0].rows.size() == 3) {
            check(t[0].rows[1][0].rowspan == 2 && !t[0].rows[1][0].placeholder, "rowspan 2");
            check(t[0].rows[2][0].placeholder, "covered by the row span");
        }
    }
    {
        PdfTableEvidence ev;
        for (float y : {100.f, 120.f, 140.f}) hline(ev, y, 100, 400);
        vline(ev, 200, 100, 140);
        vline(ev, 300, 100, 140);
        const auto t = findPdfTables(ev, kPage, nullptr, nullptr);
        check(t.size() == 1 && t[0].rows.size() == 2 && t[0].rows[0].size() == 3, "open 2x3 table");
    }
    {
        PdfTableEvidence ev;
        for (float y : {100.f, 400.f, 700.f}) hline(ev, y, 50, 550);
        vline(ev, 300, 100, 700);
        check(findPdfTables(ev, kPage, nullptr, nullptr).empty(), "single divider rejected");
    }
    {
        PdfTableEvidence ev;
        grid(ev, {100, 150, 200, 250, 300}, {100, 150, 200, 250, 300});
        ev.artwork.push_back({110, 120, 290, 280});
        ev.artwork.push_back({110, 140, 290, 260});
        check(findPdfTables(ev, kPage, nullptr, nullptr).empty(), "plot rejected");
    }
    {
        PdfTableEvidence ev;
        grid(ev, {100, 200, 300, 400}, {100, 120, 140, 160});
        ev.artwork.push_back({100, 100, 200, 120});
        check(findPdfTables(ev, kPage, nullptr, nullptr).size() == 1, "split header kept");
    }
    {
        PdfTableEvidence ev;
        grid(ev, {100, 400}, {100, 300});
        hline(ev, 500, 100, 400);
        vline(ev, 50, 400, 700);
        check(findPdfTables(ev, kPage, nullptr, nullptr).empty(), "frame rejected");
    }
    {
        PdfTableEvidence ev;
        grid(ev, {100, 200, 300}, {100, 120, 140});
        hline(ev, 101, 100, 300);
        const auto t = findPdfTables(ev, kPage, nullptr, nullptr);
        check(t.size() == 1 && t[0].rows.size() == 2, "double rule merged");
    }
    {
        PdfTableEvidence ev;
        ev.artwork.push_back({110, 120, 290, 280});
        ev.artwork.push_back({110, 140, 290, 260});
        ev.rules.push_back({100, 290, 300, 290});
        ev.rules.push_back({100, 100, 100, 290});
        ev.artwork.push_back({400, 600, 404, 606});
        const auto f = findPdfFigures(ev, kPage, {}, {}, 10);
        check(f.size() == 1, "one plot");
        if (f.size() == 1) check(f[0].x0 <= 100.5f && f[0].y1 >= 289.5f, "axes join the plot");
        check(findPdfFigures(ev, kPage, {}, {fz_rect{100, 100, 300, 300}}, 10).empty(), "table wins");
        PdfTableEvidence d;
        d.artwork.push_back({150, 131, 156, 137});
        const std::vector<fz_rect> boxes = {{120, 100, 190, 130}, {120, 138, 190, 168}};
        const auto g = findPdfFigures(d, kPage, boxes, {}, 10);
        check(g.size() == 1 && g[0].y0 <= 100 && g[0].y1 >= 168, "diagram from boxes");
        PdfTableEvidence c;
        for (int k = 0; k < 4; ++k) {
            const float y = 100.0f + 30 * k;
            c.rules.push_back({120, y, 190, y});
            c.rules.push_back({120, y + 20, 190, y + 20});
            c.rules.push_back({120, y, 120, y + 20});
            c.rules.push_back({190, y, 190, y + 20});
            if (k < 3) c.rules.push_back({155, y + 20, 155, y + 30});
        }
        check(findPdfFigures(c, kPage, {}, {}, 10).size() == 1, "stroked box diagram");
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("pdf_tables_test: all passed\n");
    return 0;
}
