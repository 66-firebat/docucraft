#ifndef HARDCOPY_OCR_PPOCR_H_
#define HARDCOPY_OCR_PPOCR_H_

#include <memory>
#include <string>
#include <vector>

#include "render/contrast_lut.h"
#include "render/page_raster.h"

namespace hardcopy {

struct Logger;

struct OcrGlyph {
    std::string text;
    float x0 = 0, x1 = 0;
};

struct OcrTextLine {
    float baseline = 0;
    float fontPx = 0;
    std::vector<OcrGlyph> glyphs;
};

struct OcrStats {
    int lines = 0;
    int chars = 0;
    long long detectMs = 0;
    long long recognizeMs = 0;
};

class PpOcr {
public:
    static std::shared_ptr<PpOcr> create(Logger* log, int threads);
    ~PpOcr();

    void recognize(const Bitmap8& page, const ImageRect& region, int threads,
                   std::vector<OcrTextLine>* out, OcrStats* stats) const;

private:
    PpOcr();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool ocrAvailable();

}

#endif
