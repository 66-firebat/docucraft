#ifndef HARDCOPY_PARSER_TEXT_ENCODING_H_
#define HARDCOPY_PARSER_TEXT_ENCODING_H_

#include <string>

namespace hardcopy {

std::string decodeTextToUtf8(const std::string& bytes,
                             std::string* encodingName = nullptr);

}

#endif
