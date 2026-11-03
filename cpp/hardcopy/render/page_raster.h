#ifndef HARDCOPY_RENDER_PAGE_RASTER_H_
#define HARDCOPY_RENDER_PAGE_RASTER_H_

#include <cstdint>
#include <vector>

#include "../document.h"
#include "../layout/font_set.h"
#include "../layout/line_box.h"
#include "../logger.h"
#include "../options.h"
#include "font_face.h"

namespace hardcopy {

struct Bitmap8 {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    void clearWhite() {
        std::fill(pixels.begin(), pixels.end(), uint8_t(255));
    }
};

struct RasterStageUs {
    uint64_t clear  = 0;
    uint64_t decor  = 0;
    uint64_t glyphs = 0;
    uint64_t images = 0;
    uint64_t tone   = 0;
    uint64_t stroke = 0;
};

class PageRaster {
public:
    PageRaster(const Options& opts, const FontSet& fonts, Logger* log);

    void setStageStats(RasterStageUs* stats) { stats_ = stats; }

    bool render(const PageGlyphs& page, Bitmap8* out,
                const std::vector<ImageData>* images = nullptr,
                int totalPages = 0);

private:
    const Bitmap8* background();

    const Options& opts_;
    FontSet   fonts_;
    Logger*   log_;
    RasterStageUs* stats_ = nullptr;
    bool      bgTried_ = false;
    bool      bgReady_ = false;
    Bitmap8   bg_;
};

void applyStrokeExpand(Bitmap8& bm, double strokeExpand, bool lightInk = false);

struct FooterStrip {
    std::vector<FontFace::Glyph> glyphs;
    int width = 0;
};

FooterStrip renderFooterStrip(const std::string& utf8, double fontPx,
                              bool embolden, FontFace* regular,
                              FontFace* fallback);

void drawFooterStrips(Bitmap8& bm, const Options& opts,
                      const FooterStrip& left, const FooterStrip& right);

void drawFooterProgress(Bitmap8& bm, const Options& opts, FontFace* regular,
                        FontFace* fallback, int pageNumber, int totalPages);

bool loadPageBackground(const Options& opts, Logger* log, Bitmap8* out);

}

#endif
