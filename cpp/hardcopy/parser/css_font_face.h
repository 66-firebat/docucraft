#ifndef HARDCOPY_PARSER_CSS_FONT_FACE_H_
#define HARDCOPY_PARSER_CSS_FONT_FACE_H_

#include <string>
#include <vector>

#include "font_alias.h"

namespace hardcopy {

struct FontFaceDecl {
    std::string family;
    std::vector<std::string> urls;
    std::vector<std::string> localNames;
    bool bold = false;
    bool italic = false;
};

void collectFontFaces(const std::string& css, std::vector<FontFaceDecl>* out);

std::string fileStemKey(const std::string& zipEntryName);

}

#endif
