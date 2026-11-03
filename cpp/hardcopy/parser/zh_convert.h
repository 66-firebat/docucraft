#ifndef HARDCOPY_PARSER_ZH_CONVERT_H_
#define HARDCOPY_PARSER_ZH_CONVERT_H_

#include <string>

#include "../document.h"
#include "../options.h"

namespace hardcopy {

std::string convertZhUtf8(const std::string& utf8, ZhConvert mode);
void convertDocumentZh(Document* doc, ZhConvert mode);

}

#endif
