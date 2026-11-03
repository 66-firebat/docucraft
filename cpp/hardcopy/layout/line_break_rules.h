#ifndef HARDCOPY_LAYOUT_LINE_BREAK_RULES_H_
#define HARDCOPY_LAYOUT_LINE_BREAK_RULES_H_

#include <cstdint>

namespace hardcopy::linebreak {

inline bool isLineStartForbidden(uint32_t cp) {
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case '%': case ')': case ']': case '}':
        case 0x3001: case 0x3002:
        case 0xFF0C: case 0xFF0E:
        case 0xFF01: case 0xFF1F:
        case 0xFF1B: case 0xFF1A:
        case 0xFF09: case 0xFF3D: case 0xFF5D:
        case 0x300D: case 0x300F:
        case 0x300B: case 0x3009:
        case 0x3011: case 0x3015: case 0x3017: case 0x3019:
        case 0x301B:
        case 0x2019: case 0x201D:
        case 0x2026: case 0x2025:
        case 0x2014: case 0x2015:
        case 0x30FB: case 0x00B7:
        case 0xFF05:
        case 0x30FC:
        case 0x3005:
        case 0x309D: case 0x309E:
        case 0x30FD: case 0x30FE:
        case 0x309B: case 0x309C:
            return true;
        default:
            break;
    }
    if (cp >= 0x3041 && cp <= 0x3049) return true;
    if (cp == 0x3063 || (cp >= 0x3083 && cp <= 0x3087) || cp == 0x308E)
        return true;
    if (cp >= 0x30A1 && cp <= 0x30A9) return true;
    if (cp == 0x30C3 || (cp >= 0x30E3 && cp <= 0x30E7) || cp == 0x30EE)
        return true;
    return false;
}

inline bool isLineEndForbidden(uint32_t cp) {
    switch (cp) {
        case '(': case '[': case '{':
        case 0xFF08: case 0xFF3B: case 0xFF5B:
        case 0x300C: case 0x300E:
        case 0x300A: case 0x3008:
        case 0x3010:
        case 0x3014: case 0x3016: case 0x3018: case 0x301A:
        case 0x301D:
        case 0x2018: case 0x201C:
            return true;
        default:
            return false;
    }
}

inline bool isHangingPunctuation(uint32_t cp) {
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case '%': case ')': case ']': case '}':
        case 0x3001: case 0x3002:
        case 0xFF0C: case 0xFF0E: case 0xFF01: case 0xFF1F:
        case 0xFF1B: case 0xFF1A: case 0xFF05:
        case 0xFF09: case 0xFF3D: case 0xFF5D:
        case 0x300D: case 0x300F: case 0x300B: case 0x3009:
        case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301B:
        case 0x2019: case 0x201D:
        case 0x2026: case 0x2025:
            return true;
        default:
            return false;
    }
}

inline bool canBreakBetween(bool leftAllowsBreak, uint32_t leftCp,
                            uint32_t rightCp) {
    return leftAllowsBreak
        && !isLineEndForbidden(leftCp)
        && !isLineStartForbidden(rightCp);
}

}

#endif
