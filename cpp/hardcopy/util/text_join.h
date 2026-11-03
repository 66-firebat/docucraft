#ifndef HARDCOPY_UTIL_TEXT_JOIN_H_
#define HARDCOPY_UTIL_TEXT_JOIN_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace hardcopy {
namespace textjoin {

inline uint32_t decodeAt(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    const uint8_t* p = (const uint8_t*)s.data();
    size_t n = s.size();
    uint8_t c = p[i];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && i + 1 < n)
        return ((uint32_t)(c & 0x1F) << 6) | (p[i + 1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && i + 2 < n)
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6)
             | (p[i + 2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && i + 3 < n)
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12)
             | ((uint32_t)(p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
    return 0;
}

inline uint32_t firstCodepoint(const std::string& s) { return decodeAt(s, 0); }

inline uint32_t lastCodepoint(const std::string& s) {
    if (s.empty()) return 0;
    const uint8_t* p = (const uint8_t*)s.data();
    size_t i = s.size() - 1;
    size_t guard = 0;
    while (i > 0 && (p[i] & 0xC0) == 0x80 && guard < 3) { --i; ++guard; }
    return decodeAt(s, i);
}

inline bool isWideScript(uint32_t cp) {
    return (cp >= 0x1100  && cp <= 0x11FF)
        || (cp >= 0x2E80  && cp <= 0x303F)
        || (cp >= 0x3040  && cp <= 0x30FF)
        || (cp >= 0x3100  && cp <= 0x312F)
        || (cp >= 0x3130  && cp <= 0x318F)
        || (cp >= 0x31A0  && cp <= 0x31BF)
        || (cp >= 0x31F0  && cp <= 0x31FF)
        || (cp >= 0x3400  && cp <= 0x4DBF)
        || (cp >= 0x4E00  && cp <= 0x9FFF)
        || (cp >= 0xA000  && cp <= 0xA4CF)
        || (cp >= 0xAC00  && cp <= 0xD7AF)
        || (cp >= 0xF900  && cp <= 0xFAFF)
        || (cp >= 0xFE10  && cp <= 0xFE1F)
        || (cp >= 0xFE30  && cp <= 0xFE4F)
        || (cp >= 0xFF00  && cp <= 0xFF60)
        || (cp >= 0xFFE0  && cp <= 0xFFE6)
        || (cp >= 0x20000 && cp <= 0x2FA1F);
}

inline bool needsSpace(const std::string& prev, const std::string& next) {
    if (prev.empty() || next.empty()) return false;
    uint32_t a = lastCodepoint(prev);
    uint32_t b = firstCodepoint(next);
    if (a == 0 || b == 0) return false;
    if (a == ' ' || a == '\t' || b == ' ' || b == '\t') return false;
    if (isWideScript(a) || isWideScript(b)) return false;
    auto isAsciiAlnum = [](uint32_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    const bool aPunct = a >= 0x2010 && a <= 0x2027;
    const bool bPunct = b >= 0x2010 && b <= 0x2027;
    if (aPunct && !isAsciiAlnum(b)) return false;
    if (bPunct && !isAsciiAlnum(a)) return false;
    if (a == '-' && b >= 'a' && b <= 'z') return false;
    return true;
}

inline void appendSoftWrap(std::string& buf, const std::string& next) {
    if (buf.empty()) { buf = next; return; }
    if (needsSpace(buf, next)) buf.push_back(' ');
    buf.append(next);
}

}
}

#endif
