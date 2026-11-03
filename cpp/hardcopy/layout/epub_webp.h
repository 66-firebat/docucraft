#ifndef HARDCOPY_LAYOUT_EPUB_WEBP_H_
#define HARDCOPY_LAYOUT_EPUB_WEBP_H_

#include <filesystem>

extern "C" {
#include <mupdf/fitz.h>
}

namespace hardcopy {

struct Logger;

#if HARDCOPY_HAS_EPUB_WEBP
fz_document* openMupdfDocument(fz_context* ctx, const std::filesystem::path& path, Logger* log);
#else
inline fz_document* openMupdfDocument(fz_context* ctx, const std::filesystem::path& path, Logger*) {
    return fz_open_document(ctx, path.string().c_str());
}
#endif

}

#endif
