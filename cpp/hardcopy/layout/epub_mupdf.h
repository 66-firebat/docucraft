#ifndef HARDCOPY_LAYOUT_EPUB_MUPDF_H_
#define HARDCOPY_LAYOUT_EPUB_MUPDF_H_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../layout/line_box.h"
#include "../logger.h"
#include "../options.h"
#include "../render/page_raster.h"
#include "../render/contrast_lut.h"
#include "../pdf/streaming_pdf_writer.h"

namespace hardcopy {

class MupdfEpubEngine {
public:
    MupdfEpubEngine();
    ~MupdfEpubEngine();

    bool open(const std::filesystem::path& epubPath, const Options& opts,
              Logger* log);
    void close();

    int pageCount() const;

    void* hookSystemFont(void* ctx, int bold);
    void* hookCjkFont(void* ctx);
    void* hookFallbackFont(void* ctx, int script, int bold);

    bool renderPage(int pageIndex0, Bitmap8* out,
                    std::vector<ImageRect>* imageRects = nullptr);

    class PageDisplayList {
    public:
        ~PageDisplayList();
        bool isReady() const { return list_ != nullptr; }

    private:
        friend class MupdfEpubEngine;
        friend class PageRenderer;
        explicit PageDisplayList(MupdfEpubEngine* engine);
        MupdfEpubEngine* engine_ = nullptr;
        void* list_ = nullptr;
    };

    class PageRenderer {
    public:
        ~PageRenderer();
        bool isReady() const { return ctx_ != nullptr; }
        bool renderPage(const PageDisplayList& page, Bitmap8* out,
                        std::vector<ImageRect>* imageRects = nullptr);

    private:
        friend class MupdfEpubEngine;
        explicit PageRenderer(MupdfEpubEngine* engine);
        MupdfEpubEngine* engine_ = nullptr;
        void* ctx_ = nullptr;
    };

    std::shared_ptr<PageDisplayList> preparePage(int pageIndex0,
                                                 std::vector<PlacedGlyph>* glyphs = nullptr);
    std::unique_ptr<PageRenderer> createPageRenderer();

    void textLayer(int pageIndex0, std::vector<PlacedGlyph>* out);

    void links(int pageIndex0, std::vector<PdfLink>* out);

    const std::vector<HeadingAnchor>& outline() const;

    std::string title;
    std::string author;

private:
    bool renderPageInContext(void* ctx, int pageIndex0, Bitmap8* out,
                             std::vector<ImageRect>* imageRects);
    void textLayerInContext(void* ctx, int pageIndex0,
                            std::vector<PlacedGlyph>* out);
    void textLayerFromDisplayListInContext(void* ctx, void* list,
                                           std::vector<PlacedGlyph>* out);
    void imageRectsFromStextPage(void* ctx, void* stextPage,
                                 std::vector<ImageRect>* out);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
