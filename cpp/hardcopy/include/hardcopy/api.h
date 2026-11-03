#ifndef HARDCOPY_API_H_
#define HARDCOPY_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INK_OK                  = 0,
    INK_ERR_OPEN_INPUT      = 1,
    INK_ERR_PARSE           = 2,
    INK_ERR_LAYOUT          = 3,
    INK_ERR_RENDER          = 4,
    INK_ERR_ENCODE          = 5,
    INK_ERR_WRITE_OUTPUT    = 6,
    INK_ERR_INVALID_OPTIONS = 7,
    INK_ERR_CANCELLED       = 8,
    INK_ERR_INTERNAL        = 99
} ink_status_t;

typedef enum {
    INK_STAGE_PARSE   = 0,
    INK_STAGE_LAYOUT  = 1,
    INK_STAGE_RENDER  = 2,
    INK_STAGE_PACKAGE = 3,
    INK_STAGE_DONE    = 4,
    INK_STAGE_OCR     = 5
} ink_stage_t;

typedef enum {
    INK_LOG_DEBUG = 0,
    INK_LOG_INFO  = 1,
    INK_LOG_WARN  = 2,
    INK_LOG_ERROR = 3
} ink_log_level_t;

typedef void (*ink_progress_cb)(const char* job_id,
                                int          stage,
                                int          percent,
                                void*        userdata);

typedef void (*ink_log_cb)(int          level,
                           const char*  tag,
                           const char*  msg,
                           void*        userdata);

ink_status_t ink_convert(const char*      input_path,
                         const char*      output_path,
                         const char*      options_json,
                         const char*      job_id,
                         ink_progress_cb  progress,
                         void*            progress_userdata,
                         ink_log_cb       log,
                         void*            log_userdata);

ink_status_t ink_extract(const char*      input_path,
                         const char*      options_json,
                         const char*      out_dir,
                         const char*      job_id,
                         ink_progress_cb  progress,
                         void*            progress_userdata,
                         ink_log_cb       log,
                         void*            log_userdata);

ink_status_t ink_export_pdf_annotations(const char* input_path,
                                        const char* output_path,
                                        const char* annotation_packet,
                                        const char* author,
                                        float       marker_opacity,
                                        ink_log_cb  log,
                                        void*       log_userdata);

ink_status_t ink_export_pdf_annotations_mapped(const char* input_path,
                                                const char* output_path,
                                                const char* annotation_packet,
                                                const char* author,
                                                float       marker_opacity,
                                                const int*  ink_color_map,
                                                ink_log_cb  log,
                                                void*       log_userdata);

float* ink_pdf_text_boxes(const char* pdf_path, int page, int* out_count);

uint8_t* ink_pwg_encode_rgba(const uint8_t* rgba,
                             size_t         stride,
                             int            width,
                             int            rows,
                             int            color,
                             size_t*        out_size);

uint8_t* ink_jpeg_encode_rgba_gray(const uint8_t* rgba,
                                   size_t         stride,
                                   int            width,
                                   int            height,
                                   int            quality,
                                   size_t*        out_size);

void ink_free(void* ptr);

void ink_cancel(const char* job_id);

const char* ink_version(void);

#ifdef __cplusplus
}
#endif

#endif
