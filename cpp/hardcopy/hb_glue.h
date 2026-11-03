#pragma once

#ifdef HARDCOPY_HB_MUPDF
namespace hardcopy {
void hbMupdfLock();
void hbMupdfUnlock();
}
#endif

namespace hardcopy {

struct HbGuard {
    HbGuard() {
#ifdef HARDCOPY_HB_MUPDF
        hbMupdfLock();
#endif
    }
    ~HbGuard() {
#ifdef HARDCOPY_HB_MUPDF
        hbMupdfUnlock();
#endif
    }
    HbGuard(const HbGuard&) = delete;
    HbGuard& operator=(const HbGuard&) = delete;
};

}
