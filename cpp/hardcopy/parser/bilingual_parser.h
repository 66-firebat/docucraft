#ifndef HARDCOPY_PARSER_BILINGUAL_PARSER_H_
#define HARDCOPY_PARSER_BILINGUAL_PARSER_H_

#include <filesystem>
#include <string>

#include "../document.h"
#include "../logger.h"

namespace hardcopy {

bool parseBilingual(const std::filesystem::path& path, Document* out, Logger* log);

bool parseBilingualText(const std::string& json, const std::filesystem::path& baseDir,
                        Document* out, Logger* log);

}

#endif
