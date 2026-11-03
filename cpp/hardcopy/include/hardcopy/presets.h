#ifndef HARDCOPY_PRESETS_H
#define HARDCOPY_PRESETS_H

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hardcopy {

struct ReadingPreset {
    const char* name;
    const char* displayName;
    float lineHeight;
    float paraSpacingEm;
    float indentChars;
    float mL, mR, mT, mB;
};

inline constexpr ReadingPreset kReadingPresets[] = {
    { "web",     "Web novel (dense)",   1.40f, 0.50f, 2.0f, 11.4f, 11.4f, 4.0f, 4.0f },
    { "publish", "Publish (book-like)", 1.80f, 1.00f, 2.0f, 10.2f, 10.2f, 4.0f, 4.0f },
    { "fresh",   "Fresh (airy)",        1.90f, 1.20f, 2.0f, 12.6f, 12.6f, 4.0f, 4.0f },
    { "note",    "Note (document + notes)", 1.55f, 0.40f, 2.0f, 10.0f, 10.0f, 4.0f, 10.0f },
};
inline constexpr int kReadingPresetCount =
    (int)(sizeof(kReadingPresets) / sizeof(kReadingPresets[0]));

inline constexpr double kPortraitPageWidthU = 192.0;
inline constexpr double kPortraitPageHeightU = 256.0;
inline constexpr double kMinHorizontalMarginU = 13.4;
inline constexpr double kMinTopBottomMarginU = 12.0;

inline double pageWidthU(int pageWidth, int pageHeight) {
    return pageWidth > pageHeight ? kPortraitPageHeightU : kPortraitPageWidthU;
}

inline double pageHeightU(int pageWidth, int pageHeight) {
    return pageWidth > pageHeight ? kPortraitPageWidthU : kPortraitPageHeightU;
}

inline int horizontalMarginUToPx(double u, int pageWidth, int pageHeight) {
    return (int)std::lround(u * pageWidth / pageWidthU(pageWidth, pageHeight));
}

inline int verticalMarginUToPx(double u, int pageWidth, int pageHeight) {
    return (int)std::lround(u * pageHeight / pageHeightU(pageWidth, pageHeight));
}

inline double horizontalMarginPxToU(int px, int pageWidth, int pageHeight) {
    return px * pageWidthU(pageWidth, pageHeight) / std::max(1, pageWidth);
}

inline double verticalMarginPxToU(int px, int pageWidth, int pageHeight) {
    return px * pageHeightU(pageWidth, pageHeight) / std::max(1, pageHeight);
}

struct ResolvedPresetMargins {
    int left, right, top, bottom;
};

inline constexpr double kNoteDocumentRatio = 0.58;

inline int noteDividerPosition(int extent) {
    if (extent <= 1) return 0;
    const int position = (int)(extent * kNoteDocumentRatio + 0.5);
    return std::clamp(position, 1, extent - 1);
}

inline ResolvedPresetMargins resolveReadingPresetMargins(
        const ReadingPreset& preset, int pageWidth, int pageHeight) {
    const int minH = std::max(1,
        horizontalMarginUToPx(kMinHorizontalMarginU, pageWidth, pageHeight));
    const int minV = std::max(1,
        verticalMarginUToPx(kMinTopBottomMarginU, pageWidth, pageHeight));
    ResolvedPresetMargins out {
        std::max(minH, horizontalMarginUToPx(preset.mL, pageWidth, pageHeight)),
        std::max(minH, horizontalMarginUToPx(preset.mR, pageWidth, pageHeight)),
        std::max(minV, verticalMarginUToPx(preset.mT, pageWidth, pageHeight)),
        std::max(minV, verticalMarginUToPx(preset.mB, pageWidth, pageHeight)),
    };
    if (std::strcmp(preset.name, "note") == 0) {
        if (pageHeight >= pageWidth)
            out.bottom = pageHeight - noteDividerPosition(pageHeight);
        else
            out.right = pageWidth - noteDividerPosition(pageWidth);
    }
    return out;
}

inline const ReadingPreset* findReadingPreset(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < kReadingPresetCount; ++i)
        if (std::strcmp(name, kReadingPresets[i].name) == 0)
            return &kReadingPresets[i];
    return nullptr;
}

}

#endif
