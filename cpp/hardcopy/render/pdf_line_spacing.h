#ifndef HARDCOPY_RENDER_PDF_LINE_SPACING_H_
#define HARDCOPY_RENDER_PDF_LINE_SPACING_H_

#include <cstdint>
#include <string>
#include <vector>

namespace hardcopy {

struct SpacingGlyph {
    float x = 0;
    float advance = 0;
    float baseline = 0;
    float size = 0;
};

struct SpacingInput {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<SpacingGlyph> glyphs;
    double lineHeight = 1.6;
    double paraSpacingEm = 0.0;
};

enum class SpacingPieceKind { Line, Heading, Block };

struct SpacingPiece {
    int y0 = 0, y1 = 0;
    int gapBefore = 0;
    SpacingPieceKind kind = SpacingPieceKind::Block;
    bool paragraphStart = false;
};

struct SpacingPlan {
    std::vector<SpacingPiece> pieces;
    int leadGap = 0;
};

SpacingPlan planLineSpacing(const SpacingInput& in, std::string* debug = nullptr);

const char* spacingPieceKindName(SpacingPieceKind kind);

}

#endif
