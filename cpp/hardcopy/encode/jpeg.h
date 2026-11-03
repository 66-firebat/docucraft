#ifndef HARDCOPY_ENCODE_JPEG_H_
#define HARDCOPY_ENCODE_JPEG_H_

#include <cstdint>
#include <vector>

#include "../render/page_raster.h"

namespace hardcopy {

bool encodeJpegGray(const Bitmap8& src, int quality, std::vector<uint8_t>* out);

}

#endif
