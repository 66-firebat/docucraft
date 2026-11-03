#include "parser/pdf_reading_order.h"

#include <cstdio>
#include <string>

using namespace hardcopy;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

void column(std::vector<ReadingBox>* boxes, std::vector<std::string>* names, const char* tag,
            float x0, float x1, float y, int n) {
    for (int i = 0; i < n; ++i) {
        boxes->push_back({x0, y + 12.0f * i, x1, y + 12.0f * i + 10});
        names->push_back(std::string(tag) + std::to_string(i));
    }
}

std::string sequence(const ReadingOrder& o, const std::vector<std::string>& names) {
    std::string s;
    for (int id : o.order) { s += names[id]; s += ' '; }
    return s;
}

ReadingLine line(float x0, float x1, float baseline, int region = 0, bool end = false) {
    ReadingLine l;
    l.x0 = x0;
    l.x1 = x1;
    l.baseline = baseline;
    l.size = 10;
    l.region = region;
    l.sentenceEnd = end;
    l.firstWordW = 25;
    return l;
}
}

int main() {
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        b.push_back({50, 40, 550, 60});
        n.push_back("T");
        column(&b, &n, "L", 50, 290, 100, 3);
        column(&b, &n, "R", 310, 550, 100, 3);
        check(sequence(readingOrder(b, 9), n) == "T L0 L1 L2 R0 R1 R2 ", "title + two columns");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        column(&b, &n, "L", 50, 290, 100, 3);
        column(&b, &n, "M", 50, 290, 160, 3);
        column(&b, &n, "R", 310, 550, 100, 3);
        column(&b, &n, "S", 310, 550, 160, 3);
        check(sequence(readingOrder(b, 9), n) == "L0 L1 L2 M0 M1 M2 R0 R1 R2 S0 S1 S2 ",
              "aligned paragraph gaps");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        column(&b, &n, "A", 50, 200, 100, 2);
        column(&b, &n, "B", 220, 370, 100, 2);
        column(&b, &n, "C", 390, 540, 100, 2);
        check(sequence(readingOrder(b, 9), n) == "A0 A1 B0 B1 C0 C1 ", "three columns");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        b.push_back({36, 222, 154, 242});
        n.push_back("H");
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 3; ++col) {
                const float x = 54.0f + 252 * col, y = 247.0f + 16 * r;
                b.push_back({x, y, x + (r % 2 ? 40.0f : 190.0f), y + 11});
                n.push_back(std::string(1, (char)('a' + col)) + std::to_string(r));
            }
        check(sequence(readingOrder(b, 9), n) == "H a0 a1 a2 a3 b0 b1 b2 b3 c0 c1 c2 c3 ",
              "three list columns");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        column(&b, &n, "L", 50, 290, 100, 2);
        column(&b, &n, "R", 310, 550, 100, 2);
        b.push_back({60, 140, 540, 300});
        n.push_back("F");
        column(&b, &n, "l", 50, 290, 320, 2);
        column(&b, &n, "r", 310, 550, 320, 2);
        const ReadingOrder o = readingOrder(b, 9);
        check(sequence(o, n) == "L0 L1 R0 R1 F l0 l1 r0 r1 ", "bands around a wide figure");
        check(o.region[0] != o.region[2] && o.region[0] == o.region[1], "column regions");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        column(&b, &n, "P", 50, 550, 100, 3);
        b.push_back({50, 136, 200, 146});
        n.push_back("S");
        column(&b, &n, "Q", 50, 550, 148, 2);
        const ReadingOrder o = readingOrder(b, 9);
        check(sequence(o, n) == "P0 P1 P2 S Q0 Q1 ", "single column");
        check(o.regions.size() == 1, "single region");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        for (int r = 0; r < 3; ++r) {
            b.push_back({50, 100.0f + 12 * r, 90, 110.0f + 12 * r});
            n.push_back("a" + std::to_string(r));
            b.push_back({300, 100.0f + 12 * r, 330, 110.0f + 12 * r});
            n.push_back("b" + std::to_string(r));
        }
        check(sequence(readingOrder(b, 9), n) == "a0 b0 a1 b1 a2 b2 ", "table cells by rows");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        column(&b, &n, "L", 50, 290, 100, 2);
        column(&b, &n, "R", 310, 550, 100, 2);
        column(&b, &n, "A", 50, 200, 200, 2);
        column(&b, &n, "B", 220, 370, 200, 2);
        column(&b, &n, "C", 390, 550, 200, 2);
        check(sequence(readingOrder(b, 9), n) == "L0 L1 R0 R1 A0 A1 B0 B1 C0 C1 ",
              "two then three columns");
    }

    std::vector<ReadingParagraph> paras;
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 300, 112), line(50, 180, 124, 0, true),
                                      line(62, 300, 136), line(50, 300, 148)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 0, 1, 1}), "indent and short last line");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 300, 112, 0, true),
                                      line(50, 300, 136), line(50, 300, 148)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 1, 1}), "gap");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(62, 300, 112), line(62, 300, 124),
                                      line(50, 300, 136), line(62, 200, 148, 0, true)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 0, 1, 1}), "hanging indent");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 300, 112, 0, true),
                                      line(50, 300, 124), line(62, 200, 136, 0, true),
                                      line(50, 300, 148), line(62, 300, 160)};
        l[2].itemTextX = 62;
        l[4].itemTextX = 62;
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 1, 1, 2, 2}), "list items");
        check(paras.size() == 3 && !paras[0].item && paras[1].item && paras[2].item, "item flags");
    }
    {
        std::vector<ReadingLine> l = {line(50, 290, 100), line(50, 290, 112), line(50, 290, 124)};
        ReadingLine foot = line(50, 200, 700, 0, true);
        foot.size = 7;
        l.push_back(foot);
        l.push_back(line(310, 550, 100, 1));
        l.push_back(line(310, 550, 112, 1, true));
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 0, 1, 0, 0}), "resume across column and footnote");
    }
    {
        ReadingLine h = line(50, 170, 100);
        h.size = 20;
        ReadingLine n = line(50, 230, 300, 1);
        n.size = 20;
        std::vector<ReadingLine> l = {h, n};
        const auto p = segmentLines(l, &paras);
        check(p[0] != p[1], "headings stay apart");
    }
    {
        std::vector<ReadingLine> l = {line(50, 290, 100), line(50, 290, 112, 0, true),
                                      line(310, 550, 100, 1), line(310, 550, 112, 1)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 1, 1}), "sentence end at column change");
    }
    {
        std::vector<ReadingLine> l = {line(50, 290, 100), line(50, 280, 112, 0, true),
                                      line(50, 300, 124), line(50, 120, 136, 0, true),
                                      line(50, 300, 148)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 0, 0, 1}), "ragged right");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 80), line(120, 230, 100), line(130, 220, 112),
                                      line(50, 300, 136), line(50, 300, 148)};
        const auto p = segmentLines(l, &paras);
        check(p[1] == p[2] && p[1] != p[0] && p[3] != p[2], "centered title");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 120, 112, 0, true),
                                      line(130, 300, 112), line(50, 300, 124), line(50, 300, 136)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 0, 0, 0}), "split visual line");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 300, 112), line(50, 80, 107),
                                      line(55, 62, 117), line(90, 300, 124), line(50, 300, 136)};
        const auto p = segmentLines(l, &paras);
        check(p == std::vector<int>({0, 0, 0, 0, 0, 0}), "fraction pieces");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 300, 112, 0, true),
                                      line(50, 90, 128), line(92, 300, 128), line(50, 300, 140)};
        l[2].bold = true;
        const auto p = segmentLines(l, &paras);
        check(p[2] != p[1] && p[3] == p[2] && p[4] == p[2], "run-in title");
    }
    {
        std::vector<ReadingBox> b;
        std::vector<std::string> n;
        b.push_back({150, 40, 450, 56});
        n.push_back("T");
        b.push_back({100, 70, 200, 80});
        n.push_back("A1");
        b.push_back({380, 70, 480, 80});
        n.push_back("A2");
        column(&b, &n, "L", 50, 290, 110, 20);
        column(&b, &n, "R", 310, 550, 110, 20);
        const std::string seq = sequence(readingOrder(b, 9), n);
        check(seq.compare(0, 12, "T A1 A2 L0 L") == 0, "authors before columns");
    }
    {
        std::vector<ReadingLine> l = {line(50, 300, 100), line(50, 200, 112, 0, true),
                                      line(120, 300, 128), line(50, 300, 144), line(50, 300, 156)};
        const auto p = segmentLines(l, &paras);
        check(p[2] != p[1] && p[3] != p[2] && p[3] == p[4], "display formula");
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("pdf_reading_order_test: all passed\n");
    return 0;
}
