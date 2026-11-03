#ifndef HARDCOPY_PARSER_TXT_PARSER_H_
#define HARDCOPY_PARSER_TXT_PARSER_H_

#include "../document.h"

#include <filesystem>

namespace hardcopy {

enum class TxtMode {
    Normal,
    Poetry,
    Web,
};

bool parseTxt(const std::filesystem::path& path, Document* out,
              TxtMode mode = TxtMode::Normal);
int plainTextChapterLevel(const std::string& trimmedLine);

bool parseTxtFromString(const std::string& src, Document* out,
                        TxtMode mode = TxtMode::Normal);

}

#endif
