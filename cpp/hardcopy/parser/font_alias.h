#ifndef HARDCOPY_PARSER_FONT_ALIAS_H_
#define HARDCOPY_PARSER_FONT_ALIAS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace hardcopy {

enum class FontRole : int8_t {
    None  = -1,
    Serif = 0,
    Sans  = 1,
    Kai   = 2,
    Mono  = 3,
};

std::string normalizeFamilyKey(const std::string& family);

FontRole roleForFamily(const std::string& family);

FontRole roleFromNames(const std::vector<std::string>& names);

std::vector<std::string> splitFamilyList(const std::string& value);

}

#endif
