#ifndef HARDCOPY_ENCODE_FLATE_H_
#define HARDCOPY_ENCODE_FLATE_H_

#include <cstdint>
#include <vector>

#include "../render/page_raster.h"

namespace hardcopy {

bool encodeFlateGray4(const Bitmap8& src, std::vector<uint8_t>* out);

const char* flateBackendName();

}

#endif
