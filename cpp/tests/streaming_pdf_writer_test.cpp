#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include "logger.h"
#include "pdf/streaming_pdf_writer.h"

using namespace hardcopy;

namespace {

struct StderrLogger : Logger {
    void log(LogLevel, const char* tag, const char* msg) override {
        std::fprintf(stderr, "[%s] %s\n", tag, msg);
    }
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

PlacedGlyph glyph(float x, float y, float adv, float px, const char* utf8, bool rotated = false) {
    PlacedGlyph g{};
    g.x = x; g.y = y; g.advance = adv; g.fontPx = px;
    g.sourceUtf8 = utf8; g.rotated = rotated;
    return g;
}

std::vector<PlacedGlyph> samplePage(float y0, int lines) {
    std::vector<PlacedGlyph> out;
    const float px = 40.0f;
    for (int l = 0; l < lines; ++l) {
        float x = 100.0f, y = y0 + l * 60.0f;
        const char* cjk[] = {"中", "文", "，", "测", "试", "。"};
        for (const char* c : cjk) { out.push_back(glyph(x, y, px, px, c)); x += px; }
        out.push_back(glyph(x, y, px * 0.5f, px, " ")); x += px * 0.5f;
        const char* latin = "e";
        out.push_back(glyph(x, y, px * 0.5f, px, latin)); x += px * 0.5f;
        out.push_back(glyph(x, y, px * 0.8f, px, "f"));
        out.push_back(glyph(x + px * 0.8f, y, 0.0f, px, "f"));
        out.push_back(glyph(x + px * 0.8f, y, 0.0f, px, "i"));
        x += px * 0.8f;
        for (const char* c : {"c", "i", "e", "n", "t"}) {
            out.push_back(glyph(x, y, px * 0.5f, px, c)); x += px * 0.5f;
        }
    }
    float ry = 300.0f;
    for (const char* c : {"T", "h", "e"}) {
        out.push_back(glyph(1800.0f, ry, px * 0.5f, px, c, true)); ry += px * 0.5f;
    }
    return out;
}

}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s tiny.jpg out.pdf\n", argv[0]); return 2; }
    std::ifstream jf(argv[1], std::ios::binary);
    std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(jf)), {});
    if (jpeg.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    StderrLogger log;
    const int pages = 10, lines = 40;
    StreamingPdfWriter pdf(argv[2], 1920, 2560, &log);
    check(pdf.open(), "open");
    const auto glyphs = samplePage(200.0f, lines);
    const auto t0 = std::chrono::steady_clock::now();
    for (int p = 0; p < pages; ++p)
        check(pdf.addJpegPage(jpeg.data(), jpeg.size(), glyphs) == p, "addJpegPage");
    const auto t1 = std::chrono::steady_clock::now();
    check(pdf.close(), "close");
    const auto t2 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "%d pages x %zu glyphs: pages %lld ms, close %lld ms\n",
        pages, glyphs.size(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());

    std::ifstream of(argv[2], std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(of)), {});
    check(out.find("/FontFile2") != std::string::npos, "embedded FontFile2");
    check(out.find("/Subtype/CIDFontType2") != std::string::npos, "CIDFontType2");
    check(out.find("/Encoding/Identity-H") != std::string::npos, "Identity-H");

    std::smatch m;
    check(std::regex_search(out, m, std::regex("/W\\[1\\[([^\\]]*)\\]\\]")), "/W array");
    std::vector<int> widths;
    if (m.size() > 1) {
        std::string s = m[1];
        for (size_t i = 0; i < s.size();) {
            while (i < s.size() && s[i] == ' ') ++i;
            if (i >= s.size()) break;
            widths.push_back(std::atoi(s.c_str() + i));
            while (i < s.size() && s[i] != ' ') ++i;
        }
    }
    check(widths.size() >= 12, "width count");
    if (widths.size() >= 12) {
        check(widths[0] == 1000, "CJK width 1000");
        check(widths[6] == 500, "space width 500");
        bool ligature = false;
        for (int w : widths) if (w == 267) ligature = true;
        check(ligature, "ffi split into 267-unit thirds");
    }

    size_t tj = 0;
    for (size_t pos = 0; (pos = out.find("] TJ", pos)) != std::string::npos; ++pos) ++tj;
    check(tj == (size_t)pages * (lines + 1), "one TJ per line");
    check(out.find("0 -1 1 0 1800 2260 Tm") != std::string::npos, "rotated Tm");

    check(out.find("beginbfchar") != std::string::npos, "ToUnicode present");

    std::fprintf(stderr, failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
