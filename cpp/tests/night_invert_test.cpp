#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "render/contrast_lut.h"

namespace {

void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void testDisabledLeavesPixelsUnchanged() {
    std::vector<uint8_t> pixels = {0, 64, 128, 192, 255};
    hardcopy::applyNightInvert(pixels.data(), pixels.size(), false);
    expect(pixels[0] == 0 && pixels[4] == 255,
           "disabled invert must not touch pixels");
}

void testEnabledInvertsBlackInkOnWhitePage() {
    std::vector<uint8_t> pixels = {255, 255, 0, 32, 255};
    hardcopy::applyNightInvert(pixels.data(), pixels.size(), true);
    expect(pixels[0] == 0 && pixels[1] == 0,
           "white page must become black");
    expect(pixels[2] == 255,
           "black ink must become white");
    expect(pixels[3] == 223,
           "gray anti-alias coverage must invert around 255");
}

void testNullAndEmptyAreSafe() {
    hardcopy::applyNightInvert(nullptr, 8, true);
    uint8_t pixel = 40;
    hardcopy::applyNightInvert(&pixel, 0, true);
    expect(pixel == 40, "empty invert must not write");
}

std::vector<uint8_t> solid(int w, int h, uint8_t v) {
    return std::vector<uint8_t>((size_t)w * h, v);
}

void testDarkPlateKeepsDesignedBlackBackground() {
    const int w = 80, h = 80;
    auto pixels = solid(w, h, 255);
    for (int y = 10; y < 70; ++y)
        for (int x = 10; x < 70; ++x)
            pixels[(size_t)y * w + x] = 32;
    pixels[(size_t)40 * w + 40] = 255;
    hardcopy::applyNightInvert(pixels.data(), w, h, true);
    expect(pixels[0] == 0, "page around the plate must invert to black");
    expect(pixels[(size_t)10 * w + 10] == 32,
           "already-dark plate must not invert");
    expect(pixels[(size_t)40 * w + 40] == 255,
           "white ink on a dark plate must stay white");
}

void testSmallDarkGlyphsStillInvert() {
    const int w = 64, h = 64;
    auto pixels = solid(w, h, 255);
    for (int y = 20; y < 28; ++y)
        for (int x = 20; x < 28; ++x)
            pixels[(size_t)y * w + x] = 0;
    hardcopy::applyNightInvert(pixels.data(), w, h, true);
    expect(pixels[0] == 0, "white page must invert");
    expect(pixels[(size_t)20 * w + 20] == 255,
           "small glyph-sized dark blobs must still invert");
}

void testGrayUnderlineBecomesPureWhite() {
    const int w = 48, h = 24;
    auto pixels = solid(w, h, 255);
    for (int x = 6; x < 42; ++x)
        pixels[(size_t)12 * w + x] = 128;
    hardcopy::applyNightInvert(pixels.data(), w, h, true);
    expect(pixels[0] == 0, "page must invert to black");
    expect(pixels[(size_t)12 * w + 20] == 255,
           "anti-aliased underline must snap to pure white");
    expect(pixels[(size_t)11 * w + 20] == 0,
           "pixels above the underline must stay page background");
}

void testTwoRowGrayUnderlineBecomesPureWhite() {
    const int w = 48, h = 24;
    auto pixels = solid(w, h, 255);
    for (int x = 6; x < 42; ++x) {
        pixels[(size_t)12 * w + x] = 128;
        pixels[(size_t)13 * w + x] = 128;
    }
    hardcopy::applyNightInvert(pixels.data(), w, h, true);
    expect(pixels[(size_t)12 * w + 20] == 255 &&
               pixels[(size_t)13 * w + 20] == 255,
           "half-pixel MuPDF underline spanning two rows must become white");
}

void testGrayLineInsideDarkPlateIsUnchanged() {
    const int w = 80, h = 80;
    auto pixels = solid(w, h, 255);
    for (int y = 10; y < 70; ++y)
        for (int x = 10; x < 70; ++x)
            pixels[(size_t)y * w + x] = 32;
    for (int x = 20; x < 60; ++x)
        pixels[(size_t)50 * w + x] = 200;
    hardcopy::applyNightInvert(pixels.data(), w, h, true);
    expect(pixels[(size_t)50 * w + 30] == 200,
           "light line art on a dark plate must not be forced white or inverted");
}

}

int main() {
    testDisabledLeavesPixelsUnchanged();
    testEnabledInvertsBlackInkOnWhitePage();
    testNullAndEmptyAreSafe();
    testDarkPlateKeepsDesignedBlackBackground();
    testSmallDarkGlyphsStillInvert();
    testGrayUnderlineBecomesPureWhite();
    testTwoRowGrayUnderlineBecomesPureWhite();
    testGrayLineInsideDarkPlateIsUnchanged();
    std::cout << "night invert regression passed\n";
    return 0;
}
