#ifndef HARDCOPY_PARSER_EPUB_PARSER_H_
#define HARDCOPY_PARSER_EPUB_PARSER_H_

#include <filesystem>

#include "../comic.h"
#include "../document.h"
#include "../logger.h"

namespace hardcopy {

bool parseEpub(const std::filesystem::path& path, Document* out, Logger* log);

bool parseEpubComic(const std::filesystem::path& path, ComicDocument* doc, Logger* log);

}

#endif
