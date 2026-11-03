#ifndef HARDCOPY_UTIL_NATURAL_SORT_H_
#define HARDCOPY_UTIL_NATURAL_SORT_H_

#include <cctype>
#include <cstddef>
#include <string>

namespace hardcopy {
namespace natsort {

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

inline char lower(char c) {
    return (char)std::tolower((unsigned char)c);
}

inline int compareDigitRun(const std::string& a, size_t& ia,
                           const std::string& b, size_t& ib) {
    size_t za = ia, zb = ib;
    while (ia < a.size() && a[ia] == '0') ++ia;
    while (ib < b.size() && b[ib] == '0') ++ib;
    size_t zerosA = ia - za, zerosB = ib - zb;

    size_t sa = ia, sb = ib;
    while (ia < a.size() && isDigit(a[ia])) ++ia;
    while (ib < b.size() && isDigit(b[ib])) ++ib;
    size_t lenA = ia - sa, lenB = ib - sb;

    if (lenA != lenB) return lenA < lenB ? -1 : 1;
    for (size_t k = 0; k < lenA; ++k) {
        if (a[sa + k] != b[sb + k]) return a[sa + k] < b[sb + k] ? -1 : 1;
    }
    if (zerosA != zerosB) return zerosA > zerosB ? -1 : 1;
    return 0;
}

inline int compare(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        bool da = isDigit(a[ia]), db = isDigit(b[ib]);
        if (da && db) {
            int c = compareDigitRun(a, ia, b, ib);
            if (c != 0) return c;
            continue;
        }
        if (da != db) {
            return da ? -1 : 1;
        }
        char ca = a[ia], cb = b[ib];
        if (ca == '/' || ca == '\\') ca = 1;
        if (cb == '/' || cb == '\\') cb = 1;
        char la = lower(ca), lb = lower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        ++ia;
        ++ib;
    }
    if (ia < a.size()) return 1;
    if (ib < b.size()) return -1;
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

inline bool less(const std::string& a, const std::string& b) {
    return compare(a, b) < 0;
}

}
}

#endif
