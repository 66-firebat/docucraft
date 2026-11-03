#ifndef HARDCOPY_IMAGE_IMAGE_DECODER_H_
#define HARDCOPY_IMAGE_IMAGE_DECODER_H_

#include "../render/page_raster.h"

namespace hardcopy {

bool decodeToGray(const uint8_t* data, size_t len, Bitmap8* out);

bool decodeToGrayOnWhite(const uint8_t* data, size_t len, Bitmap8* out);

bool isWebpImage(const uint8_t* data, size_t len);

bool imageInfo(const uint8_t* data, size_t len, int* width, int* height);

void scaleToFit(const Bitmap8& src, int targetW, int targetH, Bitmap8* out);

}

#endif
