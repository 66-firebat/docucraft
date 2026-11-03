#include <cstdio>
#include <random>
#include <vector>

#include "render/pdf_bitmap_processor.h"

using hardcopy::Bitmap8;
using hardcopy::ImageRect;
using hardcopy::PdfBaseLayer;
using hardcopy::PdfBitmapOptions;
using hardcopy::processPdfBitmap;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL %s\n", what); ++failures; }
}

Bitmap8 blank(int w, int h, uint8_t v) {
    Bitmap8 b; b.width = w; b.height = h; b.pixels.assign((size_t)w * h, v);
    return b;
}

uint8_t& at(Bitmap8& b, int x, int y) { return b.pixels[(size_t)y * b.width + x]; }

void whiteBaseMatchesSingleLayer() {
    std::mt19937 rng(7);
    const double gammas[] = {1.5, 2.25, 3.0};
    const double contrasts[] = {0.7, 1.0, 1.6};
    const double strokes[] = {0.0, 0.04, 0.1};
    const int watermarks[] = {0, 200};
    int cases = 0;
    for (double g : gammas) for (double c : contrasts) for (double s : strokes) for (int wm : watermarks) {
        const int w = 37 + (int)(rng() % 40), h = 23 + (int)(rng() % 30);
        Bitmap8 page = blank(w, h, 255);
        for (auto& p : page.pixels) {
            const unsigned r = rng() % 10;
            p = r < 2 ? (uint8_t)(rng() % 256) : r < 4 ? (uint8_t)(230 + rng() % 26) : 255;
        }
        PdfBitmapOptions o;
        o.trim = false; o.gamma = g; o.contrast = c; o.strokeExpand = s; o.watermarkThreshold = wm;
        o.imageContrast = 0.35;
        Bitmap8 single = page, layered = page;
        processPdfBitmap(&single, o);
        const Bitmap8 white = blank(w, h, 255);
        PdfBaseLayer base;
        base.pixels = &white;
        base.images.push_back({3, 3, w / 2, h / 2});
        processPdfBitmap(&layered, o, nullptr, &base);
        expect(single.pixels == layered.pixels, "white base reproduces the single-layer result");
        ++cases;
    }
    std::printf("white base: %d cases\n", cases);
}

void grayBoxKeepsItsGray() {
    const int w = 40, h = 30;
    Bitmap8 back = blank(w, h, 255);
    for (int y = 5; y < 25; ++y) for (int x = 5; x < 35; ++x) at(back, x, y) = 220;
    Bitmap8 page = back;
    at(page, 10, 10) = 0;
    at(page, 11, 10) = 110;
    at(page, 2, 2) = 110;
    PdfBitmapOptions o;
    o.trim = false; o.gamma = 3.0;
    PdfBaseLayer base; base.pixels = &back;
    Bitmap8 single = page;
    processPdfBitmap(&single, o);
    processPdfBitmap(&page, o, nullptr, &base);
    expect(at(page, 20, 20) == 220, "box background keeps its gray");
    expect(at(single, 20, 20) < 200, "single layer darkens the box (reference)");
    expect(at(page, 10, 10) == 0, "glyph core stays black");
    expect(at(page, 11, 10) < 60, "glyph edge on the box darkens");
    expect(at(page, 2, 2) == at(single, 2, 2), "ink on paper matches the single layer");
    expect(at(page, 0, 0) == 255, "paper stays white");
}

void fillToneMovesTheBoxOnly() {
    const int w = 40, h = 30;
    Bitmap8 back = blank(w, h, 255);
    for (int y = 5; y < 25; ++y) for (int x = 5; x < 35; ++x) at(back, x, y) = 200;
    for (int y = 5; y < 25; ++y) for (int x = 20; x < 35; ++x) at(back, x, y) = 120;
    Bitmap8 page = back;
    at(page, 10, 10) = 0;
    PdfBaseLayer base; base.pixels = &back; base.images.push_back({20, 5, 35, 25});
    PdfBitmapOptions o;
    o.trim = false; o.gamma = 2.0;
    uint8_t lut[256];
    o.fillTone = -1.0;
    Bitmap8 faded = page;
    processPdfBitmap(&faded, o, nullptr, &base);
    expect(at(faded, 12, 20) == 255, "fillTone -1 removes the box");
    expect(at(faded, 10, 10) == 0, "text on a removed box stays black");
    expect(at(faded, 30, 20) == 120, "picture ignores fillTone");
    o.fillTone = 1.0;
    Bitmap8 darker = page;
    processPdfBitmap(&darker, o, nullptr, &base);
    hardcopy::buildFillToneLut(1.0, lut);
    expect(at(darker, 12, 20) == lut[200] && lut[200] < 130, "fillTone 1 darkens the box with gamma 3");
    expect(at(darker, 30, 20) == 120 && at(darker, 0, 0) == 255, "picture and paper unchanged");
    hardcopy::buildFillToneLut(-0.5, lut);
    expect(lut[255] == 255 && lut[0] == 128 && lut[155] == 205, "fillTone -0.5 halves the darkness");
}

void inkSettingsLeaveBaseAlone() {
    const int w = 80, h = 60;
    Bitmap8 back = blank(w, h, 255);
    for (int y = 4; y < 28; ++y) for (int x = 4; x < 36; ++x) at(back, x, y) = 200;
    for (int y = 32; y < 56; ++y) for (int x = 4; x < 36; ++x) at(back, x, y) = 235;
    for (int y = 4; y < 56; ++y) for (int x = 44; x < 76; ++x)
        at(back, x, y) = y < 30 ? (uint8_t)((x * 7 + y * 3) & 255) : 230;
    Bitmap8 page = back;
    std::vector<uint8_t> near((size_t)w * h, 0);
    auto ink = [&](int x, int y, uint8_t v) {
        at(page, x, y) = std::min(at(page, x, y), v);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < w && ny >= 0 && ny < h) near[(size_t)ny * w + nx] = 1;
        }
    };
    for (int x = 8; x < 30; x += 3) { ink(x, 10, 0); ink(x, 40, 60); ink(x + 1, 50, 170); }
    for (int x = 48; x < 70; x += 4) { ink(x, 12, 20); ink(x, 45, 120); }
    ink(2, 58, 0); ink(40, 2, 140);
    PdfBaseLayer base; base.pixels = &back; base.images.push_back({44, 4, 76, 56});
    int cases = 0, bad = 0;
    for (double g : {1.5, 3.0}) for (double c : {0.5, 1.0, 2.0}) for (int wm : {0, 128, 200})
    for (double st : {0.0, 0.1}) for (double ft : {-1.0, 0.0, 0.6}) for (double ic : {0.0, 0.3}) {
        PdfBitmapOptions o;
        o.trim = false; o.gamma = g; o.contrast = c; o.watermarkThreshold = wm;
        o.strokeExpand = st; o.fillTone = ft; o.imageContrast = ic;
        uint8_t fill[256], pic[256];
        hardcopy::buildFillToneLut(ft, fill);
        hardcopy::buildContrastLut(ic, pic);
        Bitmap8 out = page;
        processPdfBitmap(&out, o, nullptr, &base);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            if (near[(size_t)y * w + x]) continue;
            const bool inPicture = x >= 42 && x < 78 && y >= 2 && y < 58;
            const uint8_t want = inPicture ? pic[at(back, x, y)] : fill[at(back, x, y)];
            if (at(out, x, y) != want && bad++ < 5)
                std::printf("  g=%.1f c=%.1f wm=%d st=%.1f ft=%.1f ic=%.1f (%d,%d): %d want %d\n",
                            g, c, wm, st, ft, ic, x, y, at(out, x, y), want);
        }
        ++cases;
    }
    std::printf("ink settings vs base: %d cases\n", cases);
    expect(bad == 0, "ink settings leave paper, boxes and pictures alone");
}

void lowContrastKeepsPaperWhite() {
    const int w = 40, h = 30;
    Bitmap8 back = blank(w, h, 255);
    for (int y = 5; y < 25; ++y) for (int x = 5; x < 20; ++x) at(back, x, y) = 200;
    for (int y = 5; y < 25; ++y) for (int x = 25; x < 35; ++x) at(back, x, y) = 90;
    Bitmap8 page = back;
    at(page, 2, 2) = 0;
    at(page, 10, 10) = 0;
    PdfBitmapOptions o;
    o.trim = false; o.gamma = 1.5; o.contrast = 0.5;
    Bitmap8 single = page;
    const Bitmap8 white = blank(w, h, 255);
    PdfBaseLayer none; none.pixels = &white;
    processPdfBitmap(&single, o, nullptr, &none);
    expect(at(single, 0, 0) == 255, "contrast 0.5 keeps paper white");
    expect(at(single, 2, 2) == 128, "contrast 0.5 fades black to mid-gray");
    PdfBaseLayer base; base.pixels = &back; base.images.push_back({25, 5, 35, 25});
    Bitmap8 layered = page;
    processPdfBitmap(&layered, o, nullptr, &base);
    expect(at(layered, 0, 0) == 255 && at(layered, 12, 20) == 200 && at(layered, 30, 20) == 90,
           "contrast 0.5 leaves paper, box and picture alone");
    expect(at(layered, 10, 10) == 100, "ink on the box fades with it");
}

void picturesFollowImageContrast() {
    const int w = 64, h = 48;
    Bitmap8 back = blank(w, h, 255);
    for (int y = 8; y < 40; ++y) for (int x = 8; x < 56; ++x) at(back, x, y) = (uint8_t)((x * 4 + y) & 255);
    for (int y = 8; y < 40; ++y) at(back, 8, y) = 0;
    PdfBaseLayer base; base.pixels = &back; base.images.push_back({8, 8, 56, 40});
    PdfBitmapOptions o;
    o.trim = false; o.gamma = 3.0; o.strokeExpand = 0.1;
    Bitmap8 page = back;
    processPdfBitmap(&page, o, nullptr, &base);
    expect(page.pixels == back.pixels, "picture unchanged at imageContrast 0, no growth into paper");
    o.imageContrast = 0.25;
    uint8_t lut[256];
    hardcopy::buildContrastLut(o.imageContrast, lut);
    page = back;
    processPdfBitmap(&page, o, nullptr, &base);
    bool follows = true;
    for (int y = 8; y < 40; ++y) for (int x = 8; x < 56; ++x)
        follows = follows && at(page, x, y) == lut[at(back, x, y)];
    expect(follows, "picture follows buildContrastLut(imageContrast)");
    expect(at(page, 7, 20) == 255 && at(page, 4, 20) == 255, "paper beside the picture stays white");
}

void lightInkOnDarkBand() {
    const int w = 30, h = 12;
    Bitmap8 back = blank(w, h, 255);
    for (int y = 2; y < 10; ++y) for (int x = 0; x < w; ++x) at(back, x, y) = 40;
    Bitmap8 page = back;
    at(page, 10, 5) = 255;
    at(page, 11, 5) = 150;
    PdfBaseLayer base; base.pixels = &back;
    PdfBitmapOptions o;
    o.trim = false; o.gamma = 3.0; o.strokeExpand = 0.1;
    processPdfBitmap(&page, o, nullptr, &base);
    expect(at(page, 10, 5) == 255 && at(page, 11, 5) == 150, "light ink keeps its value");
    expect(at(page, 20, 5) == 40, "dark band keeps its gray");
}

}

int main() {
    whiteBaseMatchesSingleLayer();
    grayBoxKeepsItsGray();
    fillToneMovesTheBoxOnly();
    lowContrastKeepsPaperWhite();
    inkSettingsLeaveBaseAlone();
    picturesFollowImageContrast();
    lightInkOnDarkBand();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("ok\n");
    return 0;
}
