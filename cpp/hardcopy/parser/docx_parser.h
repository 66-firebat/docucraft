#ifndef HARDCOPY_PARSER_DOCX_PARSER_H_
#define HARDCOPY_PARSER_DOCX_PARSER_H_

#include <filesystem>

#include "../document.h"
#include "../logger.h"

namespace hardcopy {

bool parseDocx(const std::filesystem::path& path, Document* out, Logger* log);

}

#endif
