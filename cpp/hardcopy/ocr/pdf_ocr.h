#ifndef HARDCOPY_OCR_PDF_OCR_H_
#define HARDCOPY_OCR_PDF_OCR_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <mupdf/fitz.h>

#include "logger.h"

namespace hardcopy {

struct PdfOcrChar {
    uint32_t c = 0;
    float x0 = 0, x1 = 0;
};

struct PdfOcrLine {
    float baseline = 0, size = 0;
    std::vector<PdfOcrChar> chars;
};

struct PdfOcrPage {
    std::vector<PdfOcrLine> lines;
};

using PdfOcrText = std::map<int, PdfOcrPage>;

struct PdfOcrRequest {
    bool compute = false;
    bool ignoreText = false;
    std::string identity;
    std::filesystem::path cacheDir;
    int threads = 1;
    std::vector<int> pages;
    const char* jobId = nullptr;
    std::function<void(int done, int total)> progress;
};

constexpr int kOcrMaxSourceChars = 10;

bool preparePdfOcr(fz_context* ctx, fz_document* doc, const PdfOcrRequest& request,
                   Logger* log, PdfOcrText* out);

bool preparePdfOcr(const std::filesystem::path& input, const PdfOcrRequest& request,
                   Logger* log, PdfOcrText* out);

std::filesystem::path pdfOcrCacheDir(const std::string& configured);

std::string pdfSourceIdentity(const std::filesystem::path& input, const std::string& sourceIdentity);

bool pdfOcrCacheExists(const std::filesystem::path& cacheDir, const std::string& identity);

int stextVisibleChars(const fz_stext_page* page);

void addOcrToStext(fz_context* ctx, fz_stext_page* page, const PdfOcrPage& ocr,
                   bool replaceText, bool dropCoveredImages);

int dropScanPageImages(fz_context* ctx, fz_stext_page* page, float invisibleShare);

}

#endif
