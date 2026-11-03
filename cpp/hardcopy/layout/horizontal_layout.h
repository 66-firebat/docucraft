#ifndef HARDCOPY_LAYOUT_HORIZONTAL_LAYOUT_H_
#define HARDCOPY_LAYOUT_HORIZONTAL_LAYOUT_H_

#include <memory>
#include <vector>

#include "../document.h"
#include "../logger.h"
#include "../options.h"
#include "font_set.h"
#include "line_box.h"

namespace hardcopy {

class FontFace;

struct LayoutResult {
    std::vector<PageGlyphs>     pages;
    std::vector<HeadingAnchor>  headings;
    std::vector<int>            blockPageIndex;
};

class HorizontalLayout {
public:
    HorizontalLayout(const Options& opts, const FontSet& fonts, Logger* log);
    ~HorizontalLayout();
    HorizontalLayout(const HorizontalLayout&) = delete;
    HorizontalLayout& operator=(const HorizontalLayout&) = delete;

    LayoutResult run(const Document& doc);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
