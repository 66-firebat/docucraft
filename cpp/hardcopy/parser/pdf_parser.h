#ifndef HARDCOPY_PARSER_PDF_PARSER_H_
#define HARDCOPY_PARSER_PDF_PARSER_H_

#include <filesystem>

#include "../document.h"
#include "../logger.h"
#include "../ocr/pdf_ocr.h"

namespace hardcopy {

bool parsePdf(const std::filesystem::path& path, Document* out, Logger* log,
              const PdfOcrText* ocr = nullptr, bool ignoreText = false);

}

#endif
