#ifndef HARDCOPY_PARSER_TEXT_NORMALIZER_H_
#define HARDCOPY_PARSER_TEXT_NORMALIZER_H_

#include "../document.h"

namespace hardcopy {

void normalizeSourceParagraphIndents(Document* doc);

void normalizeCompactText(Document* doc);

}

#endif
