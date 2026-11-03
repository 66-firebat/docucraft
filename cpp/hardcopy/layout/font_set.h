#ifndef HARDCOPY_LAYOUT_FONT_SET_H_
#define HARDCOPY_LAYOUT_FONT_SET_H_

#include <array>
#include <string>
#include <vector>

#include "line_box.h"

namespace hardcopy {

class FontFace;

struct FamilyFontSlots {
    std::string key;
    FontSlot normal = FontSlot::Regular;
    FontSlot bold   = FontSlot::Regular;
};

struct FontSet {
    FontFace* regular  = nullptr;
    FontFace* bold     = nullptr;
    FontFace* mono     = nullptr;
    FontFace* fallback = nullptr;
    FontFace* sans     = nullptr;
    FontFace* kai      = nullptr;
    std::array<FontFace*, kMaxEmbeddedFonts> embedded{};
    std::vector<FamilyFontSlots> families;

    FontFace* faceFor(FontSlot slot) const {
        if (isEmbeddedSlot(slot)) {
            FontFace* f = embedded[(size_t)embeddedIndex(slot)];
            return f ? f : regular;
        }
        switch (slot) {
            case FontSlot::Bold:     return bold ? bold : regular;
            case FontSlot::Mono:     return mono ? mono : regular;
            case FontSlot::Fallback: return fallback ? fallback : regular;
            case FontSlot::Sans:     return sans ? sans : (bold ? bold : regular);
            case FontSlot::Kai:      return kai ? kai : regular;
            default:                 return regular;
        }
    }

    bool findFamilySlot(const char* key, bool wantBold, FontSlot* out) const {
        if (!key || !*key || !out) return false;
        for (const auto& f : families) {
            if (f.key == key) {
                *out = wantBold ? f.bold : f.normal;
                return true;
            }
        }
        return false;
    }
};

}

#endif
