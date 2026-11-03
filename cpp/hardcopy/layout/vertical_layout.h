#ifndef HARDCOPY_LAYOUT_VERTICAL_LAYOUT_H_
#define HARDCOPY_LAYOUT_VERTICAL_LAYOUT_H_

#include <memory>

#include "../document.h"
#include "../logger.h"
#include "../options.h"
#include "font_set.h"
#include "horizontal_layout.h"
#include "line_box.h"

namespace hardcopy {

class FontFace;

class VerticalLayout {
public:
    VerticalLayout(const Options& opts, const FontSet& fonts, Logger* log);
    ~VerticalLayout();
    VerticalLayout(const VerticalLayout&) = delete;
    VerticalLayout& operator=(const VerticalLayout&) = delete;

    LayoutResult run(const Document& doc);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
