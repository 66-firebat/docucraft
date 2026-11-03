#ifndef HARDCOPY_PARSER_MOBI_PARSER_H_
#define HARDCOPY_PARSER_MOBI_PARSER_H_

#include <filesystem>

#include "../comic.h"
#include "../logger.h"

namespace hardcopy {

bool parseMobi(const std::filesystem::path& path, ComicDocument* doc, Logger* log);

}

#endif
