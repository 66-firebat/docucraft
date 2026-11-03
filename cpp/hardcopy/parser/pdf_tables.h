#ifndef HARDCOPY_PARSER_PDF_TABLES_H_
#define HARDCOPY_PARSER_PDF_TABLES_H_

extern "C" {
#include <mupdf/fitz.h>
}

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hardcopy {

struct PdfRule { float x0, y0, x1, y1; };

using PdfStructPath = std::vector<std::pair<int, int>>;

struct PdfTaggedGlyph {
    int table = -1;
    std::vector<int> cellPath;
    int rowDepth = 0;
    bool th = false;
    fz_rect box{};
};

struct PdfTextSegment {
    bool fresh = false;
    int path = -1;
    bool assigned = false;
};

struct PdfGlyph {
    float x = 0, y = 0;
    int ucs = 0;
    int segment = -1;
    fz_rect box{};
    int element = -1;
    bool label = false;
};

struct PdfImageMark {
    fz_rect box{};
    int segment = -1;
    int element = -1;
    PdfStructPath order;
};

struct PdfStructElement {
    PdfStructPath path;
    PdfStructPath order;
    int type = -1;
    int listDepth = 0;
    bool inTable = false;
};

struct PdfTableEvidence {
    std::vector<PdfRule> rules;
    std::vector<fz_rect> artwork;
    std::vector<PdfTaggedGlyph> glyphs;
    std::map<std::string, int> tableIds;
    std::vector<PdfStructPath> tablePaths;
    int structureCalls = 0;

    std::vector<PdfStructPath> paths;
    std::map<PdfStructPath, int> pathIds;
    std::vector<PdfTextSegment> segments;
    std::vector<PdfGlyph> textGlyphs;
    std::vector<PdfImageMark> images;
    std::vector<PdfStructElement> elements;
    std::map<PdfStructPath, int> elementIds;
    int artifactGlyphs = 0;
    int staleAssigned = 0;
    int textChars = 0;
    int invisibleChars = 0;

    PdfStructPath stack;
    bool fresh = false;
    bool newSegment = true;
};

void preparePdfStructure(fz_context* ctx, fz_document* doc);

fz_device* newPdfTableEvidenceDevice(fz_context* ctx, fz_device* target,
                                     PdfTableEvidence* evidence);

void resolvePdfStructure(PdfTableEvidence* evidence, fz_context* ctx, fz_document* doc,
                         int pageNumber);

bool pdfStructPathLess(const PdfStructPath& a, const PdfStructPath& b);

class PdfGlyphLookup {
public:
    explicit PdfGlyphLookup(const PdfTableEvidence& evidence);
    int find(fz_point origin, int ucs) const;
private:
    const PdfTableEvidence& ev_;
    std::unordered_map<uint64_t, std::vector<int>> grid_;
};

struct PdfTableCell {
    fz_rect rect{};
    bool header = false;
    bool placeholder = false;
    int colspan = 1;
    int rowspan = 1;
};

struct PdfTable {
    fz_rect bbox{};
    std::vector<std::vector<PdfTableCell>> rows;
    const char* source = "";
    PdfStructPath path;
};

std::vector<fz_rect> findPdfFigures(const PdfTableEvidence& evidence, fz_rect page,
                                    const std::vector<fz_rect>& seeds,
                                    const std::vector<fz_rect>& exclude, float bodySize);

std::vector<PdfTable> findPdfTables(const PdfTableEvidence& evidence, fz_rect page,
                                    fz_context* ctx, fz_document* doc,
                                    std::string* debug = nullptr);

}

#endif
