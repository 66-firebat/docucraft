#ifndef HARDCOPY_ENCODE_JPEG_LIBJPEG_H_
#define HARDCOPY_ENCODE_JPEG_LIBJPEG_H_

#include <cstdint>
#include <vector>

namespace hardcopy {
namespace detail {

bool encodeGrayBaseline(const uint8_t* pixels, int width, int height, int stride,
                        int quality, std::vector<uint8_t>* out);

}
}

#endif
