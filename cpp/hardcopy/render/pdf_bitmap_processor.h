#ifndef HARDCOPY_RENDER_PDF_BITMAP_PROCESSOR_H_
#define HARDCOPY_RENDER_PDF_BITMAP_PROCESSOR_H_

#include <vector>

#include "contrast_lut.h"
#include "page_raster.h"

namespace hardcopy {

struct PdfBitmapOptions {
    bool trim = true;
    int watermarkThreshold = 0;
    double gamma = 1.0;
    double contrast = 1.0;
    double strokeExpand = 0.0;
    double imageContrast = 0.0;
    double fillTone = 0.0;
};

void buildFillToneLut(double fillTone, uint8_t (&lut)[256]);

struct PdfBaseLayer {
    const Bitmap8* pixels = nullptr;
    std::vector<ImageRect> images;
};

void processPdfBitmap(Bitmap8* bitmap, const PdfBitmapOptions& options,
                      Logger* log = nullptr,
                      const PdfBaseLayer* base = nullptr);

}

#endif
