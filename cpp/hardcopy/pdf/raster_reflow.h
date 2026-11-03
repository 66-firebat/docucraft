#pragma once
#include "../pipeline.h"

namespace hardcopy {
ink_status_t runPdfRasterReflow(const std::filesystem::path& input,
    const std::filesystem::path& output, const Options& opts,
    const char* jobId, ink_progress_cb progress, void* user, Logger* log);
}
