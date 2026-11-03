#ifndef HARDCOPY_DOCUMENT_H_
#define HARDCOPY_DOCUMENT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "layout/line_box.h"

namespace hardcopy {

enum class InlineKind : uint8_t {
    Text,
    Emphasis,
    Strong,
    Code,
    Superscript,
    Subscript,
    Ruby,
    LineBreak,
};

struct InlineRun {
    InlineKind kind = InlineKind::Text;
    std::string text;
    int32_t sourceByteOffset = 0;
    int8_t cssFontRole = -1;
    char cssFamilyKey[24] = {};
    float cssFontSizeEm = -1.0f;
    std::string linkTarget;
};

enum class BlockKind : uint8_t {
    Paragraph,
    Heading,
    UnorderedItem,
    OrderedItem,
    CodeBlock,
    MathBlock,
    ThematicBreak,
    PageBreak,
    Blockquote,
    Image,
    Table,
};

struct TableCell {
    std::vector<InlineRun> runs;
    int colspan = 1;
    int rowspan = 1;
    bool isHeader = false;
};

struct TableRow {
    std::vector<TableCell> cells;
};

struct ImageData {
    std::vector<uint8_t> rawBytes;
    int width = 0;
    int height = 0;
    std::string altText;
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int level = 0;
    int listDepth = 0;
    int quoteDepth = 0;
    int imageIndex = -1;
    bool imageFullPage = false;
    bool imageInline = false;
    std::vector<InlineRun> runs;
    std::vector<TableRow> rows;
    std::vector<TableRow> rowsDst;
    uint32_t pairId = 0;
    int8_t   pairSide = -1;

    float cssLineHeightMul  = -1.0f;
    float cssMarginTopEm    = -1.0f;
    float cssMarginBottomEm = -1.0f;
    float cssTextIndentEm   = -1.0f;
    int8_t cssFontRole      = -1;
    char cssFamilyKey[24]   = {};
    float cssFontSizeEm     = -1.0f;
    int8_t cssTextAlign     = -1;
    uint32_t boxId          = 0;
    uint8_t  boxGray        = 0;
    std::vector<std::string> anchorIds;
    int sourceSpineIndex = -1;
    std::string sourcePath;
};

struct EmbeddedFontData {
    std::vector<uint8_t> bytes;
    std::vector<std::string> familyKeys;
    std::string sourcePath;
    bool bold = false;
    bool italic = false;
};

struct Document {
    std::string title;
    std::string author;
    std::vector<Block> blocks;
    std::vector<ImageData> images;
    std::vector<EmbeddedFontData> embeddedFonts;

    bool  hasCssPageMargin = false;
    float cssPageMarginEm[4] = {0, 0, 0, 0};

    std::vector<HeadingAnchor> importedOutline;

    struct EpubTocEntry {
        std::string title;
        int level = 1;
        std::string targetPath;
        std::string fragment;
        int sourceSpineIndex = -1;
    };
    std::vector<EpubTocEntry> epubToc;
};

}

#endif
