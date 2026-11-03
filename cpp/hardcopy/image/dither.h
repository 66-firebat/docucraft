#ifndef HARDCOPY_IMAGE_DITHER_H_
#define HARDCOPY_IMAGE_DITHER_H_

#include "../render/page_raster.h"

namespace hardcopy {

enum class DitherAlgo { None, Bayer, FloydSteinberg };

void ditherBayer(Bitmap8* bm, int levels = 16);
void ditherFloydSteinberg(Bitmap8* bm, int levels = 16);

}

#endif
