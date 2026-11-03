#include "print/pwg.h"
#include <cstdlib>
#include <cstring>
#include <vector>
#include "include/hardcopy/api.h"

#include <filesystem>
#include <mutex>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cancel.h"
#include "logger.h"
#include "options.h"
#include "pipeline.h"
#ifdef HARDCOPY_HAS_JPEG
#include "encode/jpeg.h"
#endif
#ifdef HARDCOPY_HAS_PDFIN
#include "pdf/annotation_exporter.h"
#include "pdf/text_boxes.h"
#endif

namespace {

constexpr const char* kVersion = "0.1.0-m9";

struct CallbackLogger : public hardcopy::Logger {
    ink_log_cb cb;
    void* ud;
    CallbackLogger(ink_log_cb c, void* u) : cb(c), ud(u) {}
    void log(hardcopy::LogLevel lvl, const char* tag, const char* msg) override {
        if (!cb) return;
        cb((int)lvl, tag, msg, ud);
    }
};

std::mutex& cancelMutex() { static std::mutex m; return m; }
std::set<std::string>& cancelledJobs() { static std::set<std::string> s; return s; }
thread_local const char* t_currentJob = nullptr;

struct CancelScope {
    std::string id;
    explicit CancelScope(const char* jobId) : id(jobId) { t_currentJob = id.c_str(); }
    ~CancelScope() {
        t_currentJob = nullptr;
        std::lock_guard<std::mutex> lock(cancelMutex());
        cancelledJobs().erase(id);
    }
};

std::filesystem::path pathFromUtf8(const char* s) {
#ifdef _WIN32
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::filesystem::path(s);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return std::filesystem::path(std::move(w));
#else
    return std::filesystem::path(s);
#endif
}

}

namespace hardcopy {
bool jobCancelled(const char* jobId) {
    if (!jobId) return false;
    std::lock_guard<std::mutex> lock(cancelMutex());
    return cancelledJobs().count(jobId) > 0;
}
bool currentJobCancelled() { return jobCancelled(t_currentJob); }
}

extern "C" {

const char* ink_version(void) {
    return kVersion;
}

ink_status_t ink_convert(const char*      input_path,
                         const char*      output_path,
                         const char*      options_json,
                         const char*      job_id,
                         ink_progress_cb  progress,
                         void*            progress_userdata,
                         ink_log_cb       log,
                         void*            log_userdata) {
    if (!input_path || !output_path || !job_id) return INK_ERR_INVALID_OPTIONS;

    CallbackLogger cbLog(log, log_userdata);
    CancelScope cancelScope(job_id);

    hardcopy::Options opts;
    std::string err;
    if (!hardcopy::parseOptions(options_json ? options_json : "", &opts, &err)) {
        cbLog.log(hardcopy::LogLevel::Error, "hardcopy", err.c_str());
        return INK_ERR_INVALID_OPTIONS;
    }

    return hardcopy::runPipeline(pathFromUtf8(input_path),
                                pathFromUtf8(output_path),
                                opts, job_id,
                                progress, progress_userdata,
                                &cbLog);
}


ink_status_t ink_extract(const char*      input_path,
                         const char*      options_json,
                         const char*      out_dir,
                         const char*      job_id,
                         ink_progress_cb  progress,
                         void*            progress_userdata,
                         ink_log_cb       log,
                         void*            log_userdata) {
    if (!input_path || !out_dir || !job_id) return INK_ERR_INVALID_OPTIONS;

    CallbackLogger cbLog(log, log_userdata);
    CancelScope cancelScope(job_id);

    hardcopy::Options opts;
    std::string err;
    if (!hardcopy::parseOptions(options_json ? options_json : "", &opts, &err)) {
        cbLog.log(hardcopy::LogLevel::Error, "hardcopy", err.c_str());
        return INK_ERR_INVALID_OPTIONS;
    }

    return hardcopy::runExtract(pathFromUtf8(input_path),
                               pathFromUtf8(out_dir),
                               opts, job_id,
                               progress, progress_userdata,
                               &cbLog);
}


void ink_cancel(const char* job_id) {
    if (!job_id) return;
    std::lock_guard<std::mutex> lock(cancelMutex());
    cancelledJobs().insert(job_id);
}


ink_status_t ink_export_pdf_annotations(const char* input_path,
                                        const char* output_path,
                                        const char* annotation_packet,
                                        const char* author,
                                        float marker_opacity,
                                        ink_log_cb log,
                                        void* log_userdata) {
    if (!input_path || !output_path || !annotation_packet)
        return INK_ERR_INVALID_OPTIONS;

    CallbackLogger cbLog(log, log_userdata);
#ifdef HARDCOPY_HAS_PDFIN
    return hardcopy::exportPdfAnnotations(pathFromUtf8(input_path),
                                          pathFromUtf8(output_path),
                                          pathFromUtf8(annotation_packet),
                                          author ? author : "Hardcopy",
                                          marker_opacity,
                                          nullptr,
                                          &cbLog);
#else
    cbLog.log(hardcopy::LogLevel::Error, "pdf-annot",
              "PDF input support is disabled");
    return INK_ERR_INVALID_OPTIONS;
#endif
}


ink_status_t ink_export_pdf_annotations_mapped(const char* input_path,
                                                const char* output_path,
                                                const char* annotation_packet,
                                                const char* author,
                                                float marker_opacity,
                                                const int* ink_color_map,
                                                ink_log_cb log,
                                                void* log_userdata) {
    if (!input_path || !output_path || !annotation_packet)
        return INK_ERR_INVALID_OPTIONS;

    CallbackLogger cbLog(log, log_userdata);
#ifdef HARDCOPY_HAS_PDFIN
    return hardcopy::exportPdfAnnotations(pathFromUtf8(input_path),
                                          pathFromUtf8(output_path),
                                          pathFromUtf8(annotation_packet),
                                          author ? author : "Hardcopy",
                                          marker_opacity,
                                          ink_color_map,
                                          &cbLog);
#else
    cbLog.log(hardcopy::LogLevel::Error, "pdf-annot",
              "PDF input support is disabled");
    return INK_ERR_INVALID_OPTIONS;
#endif
}

uint8_t* ink_pwg_encode_rgba(const uint8_t* rgba, size_t stride, int width, int rows,
                             int color, size_t* out_size) {
    if (!rgba || !out_size || width <= 0 || rows <= 0) return nullptr;
    std::vector<uint8_t> packed;
    hardcopy::pwg::convertRgba8888(rgba, stride, width, rows, color != 0, &packed);
    std::vector<uint8_t> encoded;
    hardcopy::pwg::encodeRows(packed.data(), width, rows, color != 0 ? 3 : 1, &encoded);
    auto* buf = static_cast<uint8_t*>(std::malloc(encoded.empty() ? 1 : encoded.size()));
    if (!buf) return nullptr;
    if (!encoded.empty()) std::memcpy(buf, encoded.data(), encoded.size());
    *out_size = encoded.size();
    return buf;
}

uint8_t* ink_jpeg_encode_rgba_gray(const uint8_t* rgba, size_t stride, int width, int height,
                                   int quality, size_t* out_size) {
    if (!rgba || !out_size || width <= 0 || height <= 0 ||
        stride < static_cast<size_t>(width) * 4) return nullptr;
#ifdef HARDCOPY_HAS_JPEG
    hardcopy::Bitmap8 gray;
    gray.width = width;
    gray.height = height;
    try {
        gray.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    } catch (...) {
        return nullptr;
    }
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(y) * stride;
        uint8_t* dst = gray.pixels.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x, src += 4) {
            const int luma = (77 * src[0] + 150 * src[1] + 29 * src[2] + 128) >> 8;
            const int v = luma + 255 - src[3];
            dst[x] = static_cast<uint8_t>(v > 255 ? 255 : v);
        }
    }
    std::vector<uint8_t> jpeg;
    if (!hardcopy::encodeJpegGray(gray, quality < 1 ? 1 : (quality > 100 ? 100 : quality), &jpeg) ||
        jpeg.empty()) return nullptr;
    auto* buf = static_cast<uint8_t*>(std::malloc(jpeg.size()));
    if (!buf) return nullptr;
    std::memcpy(buf, jpeg.data(), jpeg.size());
    *out_size = jpeg.size();
    return buf;
#else
    (void)quality;
    return nullptr;
#endif
}

float* ink_pdf_text_boxes(const char* pdf_path, int page, int* out_count) {
    if (out_count) *out_count = 0;
    if (!pdf_path || !out_count) return nullptr;
#ifdef HARDCOPY_HAS_PDFIN
    const std::vector<float> boxes = hardcopy::pdfTextLineBoxes(pdf_path, page);
    if (boxes.empty()) return nullptr;
    auto* buf = static_cast<float*>(std::malloc(boxes.size() * sizeof(float)));
    if (!buf) return nullptr;
    std::memcpy(buf, boxes.data(), boxes.size() * sizeof(float));
    *out_count = static_cast<int>(boxes.size() / 4);
    return buf;
#else
    (void)page;
    return nullptr;
#endif
}

void ink_free(void* ptr) {
    std::free(ptr);
}

}
