#ifndef HARDCOPY_PDF_TEXT_BOXES_H_
#define HARDCOPY_PDF_TEXT_BOXES_H_

#include <vector>

namespace hardcopy {

std::vector<float> pdfTextLineBoxes(const char* utf8Path, int page);

}

#endif
