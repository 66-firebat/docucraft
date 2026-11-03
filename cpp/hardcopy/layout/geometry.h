#ifndef HARDCOPY_LAYOUT_GEOMETRY_H_
#define HARDCOPY_LAYOUT_GEOMETRY_H_

#include "../options.h"

namespace hardcopy {

struct PageGeometry {
    int width;
    int height;
    int marginTop;
    int marginRight;
    int marginBottom;
    int marginLeft;

    int contentWidth()  const { return width  - marginLeft - marginRight; }
    int contentHeight() const { return height - marginTop  - marginBottom; }
};

inline PageGeometry geometryFromOptions(const Options& o) {
    return PageGeometry{
        o.pageWidth, o.pageHeight,
        o.marginTop, o.marginRight, o.marginBottom, o.marginLeft
    };
}

inline float imageDeviceScale(const PageGeometry& g) {
    float s = (float)g.width / 720.0f;
    if (s < 1.0f) s = 1.0f;
    if (s > 3.0f) s = 3.0f;
    return s;
}

inline float fitImageScale(float w, float h, float boxW, float boxH,
                           float deviceScale) {
    if (w <= 0 || h <= 0) return 0.0f;
    float s = deviceScale;
    if (w * s > boxW) s = boxW / w;
    if (h * s > boxH) s = boxH / h;
    return s > 0 ? s : 0.0f;
}

}

#endif
