#ifndef HARDCOPY_PDF_ANNOTATION_EXPORTER_H_
#define HARDCOPY_PDF_ANNOTATION_EXPORTER_H_

#include <filesystem>

#include "../include/hardcopy/api.h"

namespace hardcopy {
class Logger;

ink_status_t exportPdfAnnotations(const std::filesystem::path& inputPdf,
                                  const std::filesystem::path& outputPdf,
                                  const std::filesystem::path& packetPath,
                                  const char* author,
                                  float markerOpacity,
                                  const int* inkColorMap,
                                  Logger* log);

}

#endif
