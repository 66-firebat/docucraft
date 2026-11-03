#ifndef HARDCOPY_SOURCE_EXPORT_H_
#define HARDCOPY_SOURCE_EXPORT_H_

#include <filesystem>
#include <string>
#include <vector>

#include "document.h"
#include "logger.h"

namespace hardcopy {

bool writeSourceExport(const Document& doc, const std::string& format,
                       const std::filesystem::path& outDir, Logger* log);

void joinSplitParagraphs(Document* doc);

std::string runsPlainText(const std::vector<InlineRun>& runs, size_t first = 0);

const char* imageFileExtension(const std::vector<uint8_t>& bytes);

}

#endif
