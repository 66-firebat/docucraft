#include <cstdlib>
#include <iostream>
#include <vector>
#include "render/ruling_pattern.h"

void expect(bool ok, const char* message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}

int main() {
    constexpr int w = 80, h = 20;
    std::vector<uint8_t> dots(w * h, 255), dashes(w * h, 255);
    hardcopy::paintRulingPattern(dots.data(), w, h, 4, 76, 8, 4, 6, 175, true, false);
    hardcopy::paintRulingPattern(dashes.data(), w, h, 4, 76, 8, 4, 6, 175, true, true);
    expect(dots != dashes, "Dotted and dashed outputs must differ");
    int dotCount = 0, dashCount = 0;
    for (int x = 0; x < w; ++x) {
        dotCount += dots[9 * w + x] == 175;
        dashCount += dashes[9 * w + x] == 175;
        expect(dots[7 * w + x] == 255 && dashes[7 * w + x] == 255,
               "Guide must preserve the text area above it");
    }
    expect(dotCount == 28, "Round dots retain their original diameter and gray");
    expect(dashCount == 52, "Dashes form longer marks with visible gaps");
    expect(dots[8 * w + 8] == 255 && dots[9 * w + 8] == 175,
           "Legacy round dot corners stay rounded");
    auto squares = std::vector<uint8_t>(w * h, 255);
    hardcopy::paintRulingPattern(squares.data(), w, h, 0, w, 8, 2, 5, 219, false, false);
    expect(squares[8 * w] == 219 && squares[9 * w] == 219,
           "EPUB square dots retain their shape and lighter gray");
    auto guarded = std::vector<uint8_t>(w * h + 2, 87);
    std::fill(guarded.begin() + 1, guarded.end() - 1, 255);
    hardcopy::paintRulingPattern(guarded.data() + 1, w, h, -20, w + 20, 19, 4, 6, 175, true, true);
    expect(guarded.front() == 87 && guarded.back() == 87,
           "Clipped marks must stay inside the framebuffer");
    expect(std::all_of(guarded.begin() + 1, guarded.end() - 1,
                      [](uint8_t v) { return v == 255; }),
           "A mark crossing the bottom edge is omitted");
    hardcopy::paintRulingPattern(guarded.data() + 1, w, h, -20, w + 20, 8, 4, 6, 175, true, true);
    expect(guarded.front() == 87 && guarded.back() == 87,
           "Horizontal clipping must preserve guard bytes");
    std::cout << "ruling_pattern_test: PASS\n";
}
