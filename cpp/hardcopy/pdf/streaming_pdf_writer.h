#ifndef HARDCOPY_PDF_STREAMING_PDF_WRITER_H_
#define HARDCOPY_PDF_STREAMING_PDF_WRITER_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "../layout/line_box.h"

namespace hardcopy {

class Logger;

struct PdfLink {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int targetPage0 = -1;
    std::string uri;
    bool hasTargetPoint = false;
    float targetX = 0.0f;
    float targetY = 0.0f;
};

class StreamingPdfWriter {
public:
    StreamingPdfWriter(const std::filesystem::path& outPath,
                       int pageWidthPx, int pageHeightPx,
                       Logger* log = nullptr);
    ~StreamingPdfWriter();

    StreamingPdfWriter(const StreamingPdfWriter&) = delete;
    StreamingPdfWriter& operator=(const StreamingPdfWriter&) = delete;

    bool open();

    void setMetadata(const std::string& title, const std::string& author);

    void setFileId(const std::string& hex) { fileIdHex_ = hex; }

    enum class ImageCodec { Jpeg, FlateGray4 };

    int addJpegPage(const uint8_t* jpegData, std::size_t jpegSize,
                    const std::vector<PlacedGlyph>& glyphs = {},
                    const std::vector<PdfLink>& links = {},
                    ImageCodec codec = ImageCodec::Jpeg);

    int addJpegSpread(const uint8_t* leftJpeg, std::size_t leftSize,
                      const uint8_t* rightJpeg, std::size_t rightSize,
                      int leafW, int leafH,
                      const std::vector<PlacedGlyph>& glyphs = {},
                      const std::vector<PdfLink>& links = {},
                      ImageCodec leftCodec = ImageCodec::Jpeg,
                      ImageCodec rightCodec = ImageCodec::Jpeg);

    bool addLinks(int pageIndex0, const std::vector<PdfLink>& links);

    bool addOutline(const std::string& title, int level, int pageIndex0);

    bool close();

    int pageCount() const { return (int)pageObjIds_.size(); }

    struct OutlineEntry {
        std::string title;
        int         level;
        int         pageIndex0;
    };
    const std::vector<OutlineEntry>& outline() const { return outlines_; }

private:
    struct PageRecord {
        int pageId = 0;
        int imageLeftId = 0;
        int imageRightId = 0;
        int contentId = 0;
        bool spread = false;
        bool hasText = false;
        std::vector<PdfLink> links;
    };

    int  allocObjId_();
    void beginObject_(int id);
    void endObject_();

    void writeBytes_(const void* data, std::size_t n);
    void writeStr_(const char* s);
    void writeStr_(const std::string& s) { writeStr_(s.c_str()); }
    void writeFmt_(const char* fmt, ...);

    bool appendTextContent_(const std::vector<PlacedGlyph>& glyphs,
                            std::string& out);
    uint16_t cidFor_(uint32_t codepoint, int widthUnits);

    std::filesystem::path  outPath_;
    std::filesystem::path  tempPath_;
    int                    pageW_;
    int                    pageH_;
    Logger*                log_;

    std::FILE*             fp_ = nullptr;
    uint64_t               cursor_ = 0;
    int                    nextId_ = 1;

    std::vector<uint64_t>  offsets_;

    int                    catalogId_ = 0;
    int                    pagesTreeId_ = 0;
    int                    outlinesId_ = 0;
    int                    infoId_ = 0;

    std::vector<int>           pageObjIds_;
    std::vector<PageRecord>    pageRecords_;
    std::vector<OutlineEntry>  outlines_;

    std::string  metaTitle_;
    std::string  metaAuthor_;
    std::string  fileIdHex_;

    int fontDescObjId_   = 0;
    int cidFontObjId_    = 0;
    int toUnicodeObjId_  = 0;
    int type0FontObjId_  = 0;

    struct TextCid {
        uint32_t codepoint;
        uint16_t width;
    };
    std::unordered_map<uint64_t, uint16_t> cidByKey_;
    std::vector<TextCid> cids_;
    std::unordered_map<uint32_t, uint16_t> cidByCodepoint_;
    bool cidSpaceFullWarned_ = false;

    std::string textScratch_;
};

}

#endif
