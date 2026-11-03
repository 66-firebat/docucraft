#ifndef HARDCOPY_PIPELINE_H_
#define HARDCOPY_PIPELINE_H_

#include <filesystem>

#include "include/hardcopy/api.h"
#include "logger.h"
#include "options.h"

namespace hardcopy {

ink_status_t runPipeline(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         const Options& opts,
                         const char* jobId,
                         ink_progress_cb progressCb,
                         void* progressUserdata,
                         Logger* log);

ink_status_t runExtract(const std::filesystem::path& input,
                        const std::filesystem::path& outDir,
                        const Options& opts,
                        const char* jobId,
                        ink_progress_cb progressCb,
                        void* progressUserdata,
                        Logger* log);

}

#endif
