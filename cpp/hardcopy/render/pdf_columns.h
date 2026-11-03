#ifndef HARDCOPY_RENDER_PDF_COLUMNS_H_
#define HARDCOPY_RENDER_PDF_COLUMNS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace hardcopy {

enum class ColumnRegionKind { Full, Left, Right };

struct ColumnRect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

struct ColumnRegion {
    ColumnRegionKind kind = ColumnRegionKind::Full;
    ColumnRect rect;
    float fitWidth = 0;
};

struct ColumnDetectInput {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    ColumnRect body;
    std::vector<ColumnRect> lines;
    float pxPerPt = 1.0f;
    bool hasGutterHint = false;
    ColumnRect gutterHint;
};

std::vector<ColumnRegion> detectColumnRegions(const ColumnDetectInput& in,
                                              std::string* debug = nullptr,
                                              ColumnRect* gutterOut = nullptr);

const char* columnRegionKindName(ColumnRegionKind kind);

}

#endif
