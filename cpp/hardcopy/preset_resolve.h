#ifndef HARDCOPY_PRESET_RESOLVE_H
#define HARDCOPY_PRESET_RESOLVE_H

#include <string>

#include "options.h"

namespace hardcopy {

struct OptionOverrides {
    double marginTopU = -1.0, marginRightU = -1.0;
    double marginBottomU = -1.0, marginLeftU = -1.0;
    int    marginTopPx = -1, marginRightPx = -1;
    int    marginBottomPx = -1, marginLeftPx = -1;
    double lineHeightMul        = -1.0;
    double paragraphSpacingPx   = -1.0;
    double firstLineIndentChars = -1.0;
};

bool resolvePresetOverrides(const OptionOverrides& ov, Options* io,
                            std::string* errMsg);

}

#endif
