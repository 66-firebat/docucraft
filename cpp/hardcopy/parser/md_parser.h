#ifndef HARDCOPY_PARSER_MD_PARSER_H_
#define HARDCOPY_PARSER_MD_PARSER_H_

#include "../document.h"

#include <filesystem>

namespace hardcopy {

bool parseMarkdown(const std::filesystem::path& path, Document* out);
bool parseMarkdownFromString(const std::string& src, Document* out);

}

#endif
