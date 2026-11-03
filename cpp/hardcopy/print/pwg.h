#ifndef HARDCOPY_PRINT_PWG_H_
#define HARDCOPY_PRINT_PWG_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hardcopy {
namespace pwg {

void convertRgba8888(const uint8_t* src, size_t srcStride, int width, int rows,
                     bool color, std::vector<uint8_t>* dst);

void encodeRows(const uint8_t* data, int width, int rows, int unit,
                std::vector<uint8_t>* out);

bool decodeRows(const uint8_t* data, size_t size, int width, int rows, int unit,
                std::vector<uint8_t>* pixels, size_t* consumed = nullptr);

}
}

#endif
