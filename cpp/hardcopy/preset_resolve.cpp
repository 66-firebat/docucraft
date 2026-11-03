#include "preset_resolve.h"

#include <algorithm>
#include <cmath>

#include "include/hardcopy/presets.h"

namespace hardcopy {

bool resolvePresetOverrides(const OptionOverrides& ov, Options* io,
                            std::string* errMsg) {
    if (!io) return false;

    auto horizontal = [&](double u, int px, int fallback) {
        if (u >= 0.0) return horizontalMarginUToPx(u, io->pageWidth, io->pageHeight);
        return px >= 0 ? px : fallback;
    };
    auto vertical = [&](double u, int px, int fallback) {
        if (u >= 0.0) return verticalMarginUToPx(u, io->pageWidth, io->pageHeight);
        return px >= 0 ? px : fallback;
    };
    io->marginTop = vertical(ov.marginTopU, ov.marginTopPx, io->marginTop);
    io->marginRight = horizontal(ov.marginRightU, ov.marginRightPx, io->marginRight);
    io->marginBottom = vertical(ov.marginBottomU, ov.marginBottomPx, io->marginBottom);
    io->marginLeft = horizontal(ov.marginLeftU, ov.marginLeftPx, io->marginLeft);
    if (ov.lineHeightMul        >= 0.0) io->lineHeightMul        = ov.lineHeightMul;
    if (ov.firstLineIndentChars >= 0.0) io->firstLineIndentChars = ov.firstLineIndentChars;
    if (ov.paragraphSpacingPx   >= 0.0) io->paragraphSpacing     = ov.paragraphSpacingPx;

    const int minHorizontal = std::max(1,
        horizontalMarginUToPx(kMinHorizontalMarginU, io->pageWidth, io->pageHeight));
    const int minVertical = std::max(1,
        verticalMarginUToPx(kMinTopBottomMarginU, io->pageWidth, io->pageHeight));

    const bool explicitLeft = ov.marginLeftU >= 0.0 || ov.marginLeftPx >= 0;
    const bool explicitRight = ov.marginRightU >= 0.0 || ov.marginRightPx >= 0;
    const bool explicitTop = ov.marginTopU >= 0.0 || ov.marginTopPx >= 0;
    const bool explicitBottom = ov.marginBottomU >= 0.0 || ov.marginBottomPx >= 0;

    if (io->preset.empty()) {
        io->marginTop = std::max(minVertical, io->marginTop);
        io->marginRight = std::max(minHorizontal, io->marginRight);
        io->marginBottom = std::max(minVertical, io->marginBottom);
        io->marginLeft = std::max(minHorizontal, io->marginLeft);
        return true;
    }

    const ReadingPreset* p = findReadingPreset(io->preset.c_str());
    if (!p) {
        if (errMsg) *errMsg = "unknown preset: " + io->preset
                            + " (use note|publish|web|fresh)";
        return false;
    }

    if (ov.lineHeightMul        < 0.0) io->lineHeightMul        = p->lineHeight;
    if (ov.firstLineIndentChars < 0.0) io->firstLineIndentChars = p->indentChars;
    if (ov.paragraphSpacingPx   < 0.0)
        io->paragraphSpacing = (double)p->paraSpacingEm * io->fontSize;

    const ResolvedPresetMargins margins =
        resolveReadingPresetMargins(*p, io->pageWidth, io->pageHeight);
    if (!explicitLeft) io->marginLeft = margins.left;
    if (!explicitRight) io->marginRight = margins.right;
    if (!explicitTop) io->marginTop = margins.top;
    if (!explicitBottom) io->marginBottom = margins.bottom;

    io->marginTop = std::max(minVertical, io->marginTop);
    io->marginRight = std::max(minHorizontal, io->marginRight);
    io->marginBottom = std::max(minVertical, io->marginBottom);
    io->marginLeft = std::max(minHorizontal, io->marginLeft);
    return true;
}

}
