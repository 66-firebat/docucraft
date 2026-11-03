#include <cstdio>
#include <cstring>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "font_listing.h"

namespace {

struct EnumContext {
    std::string filter;
    FILE* output = stdout;
};

int CALLBACK enumProc(const LOGFONTA* lf, const TEXTMETRICA*, DWORD, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);
    std::string name = lf->lfFaceName;
    if (context->filter.empty()) {
        std::fprintf(context->output, "%s\n", name.c_str());
    } else {
        std::string lower = name;
        for (char& c : lower) c = tolower(c);
        std::string lf2 = context->filter;
        for (char& c : lf2) c = tolower(c);
        if (lower.find(lf2) != std::string::npos) {
            std::fprintf(context->output, "%s\n", name.c_str());
        }
    }
    return 1;
}

}

int printGdiFonts(const std::string& filter, FILE* output) {
    EnumContext context{filter, output ? output : stdout};
    HDC hdc = GetDC(nullptr);
    EnumFontFamiliesA(hdc, nullptr, enumProc, reinterpret_cast<LPARAM>(&context));
    ReleaseDC(nullptr, hdc);
    return 0;
}
