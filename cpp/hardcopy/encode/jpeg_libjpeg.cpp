#include "jpeg_libjpeg.h"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <jpeglib.h>

typedef void* backing_store_ptr;
typedef JMETHOD(long, j_custmem_init_ptr, (j_common_ptr cinfo));
typedef JMETHOD(void, j_custmem_term_ptr, (j_common_ptr cinfo));
typedef JMETHOD(long, j_custmem_avail_ptr, (j_common_ptr cinfo));
typedef JMETHOD(void *, j_custmem_get_small_ptr, (j_common_ptr cinfo, size_t size));
typedef JMETHOD(void, j_custmem_free_small_ptr, (j_common_ptr cinfo, void *object, size_t size));
typedef JMETHOD(void *, j_cust_mem_get_large_ptr, (j_common_ptr cinfo, size_t size));
typedef JMETHOD(void, j_custmem_free_large_ptr, (j_common_ptr cinfo, void *object, size_t size));
typedef JMETHOD(void, j_custmem_open_backing_store_ptr,
                (j_common_ptr cinfo, backing_store_ptr info, long total_bytes_needed));
typedef struct {
    j_custmem_init_ptr j_mem_init;
    j_custmem_term_ptr j_mem_term;
    j_custmem_avail_ptr j_mem_avail;
    j_custmem_get_small_ptr j_mem_get_small;
    j_custmem_free_small_ptr j_mem_free_small;
    j_cust_mem_get_large_ptr j_mem_get_large;
    j_custmem_free_large_ptr j_mem_free_large;
    j_custmem_open_backing_store_ptr j_mem_open_backing_store;
    void *priv;
} jpeg_cust_mem_data;
jpeg_cust_mem_data *jpeg_cust_mem_init(jpeg_cust_mem_data *custm, void *priv,
                                       j_custmem_init_ptr init,
                                       j_custmem_term_ptr term,
                                       j_custmem_avail_ptr avail,
                                       j_custmem_get_small_ptr get_small,
                                       j_custmem_free_small_ptr free_small,
                                       j_cust_mem_get_large_ptr get_large,
                                       j_custmem_free_large_ptr free_large,
                                       j_custmem_open_backing_store_ptr open_backing_store);
}

namespace hardcopy {
namespace detail {
namespace {

void* jpegAlloc(j_common_ptr, size_t size) { return std::malloc(size); }
void jpegFree(j_common_ptr, void* object, size_t) { std::free(object); }

struct ErrorManager {
    jpeg_error_mgr pub;
    std::jmp_buf jump;
};

void errorExit(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<ErrorManager*>(cinfo->err)->jump, 1);
}
void outputMessage(j_common_ptr) {}

struct VectorDestination {
    jpeg_destination_mgr pub;
    std::vector<uint8_t>* out;
};

void initDestination(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestination*>(cinfo->dest);
    dest->pub.next_output_byte = dest->out->data();
    dest->pub.free_in_buffer = dest->out->size();
}

boolean emptyOutputBuffer(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestination*>(cinfo->dest);
    const size_t used = dest->out->size();
    bool grown = true;
    try {
        dest->out->resize(used * 2);
    } catch (...) {
        grown = false;
    }
    if (!grown) cinfo->err->error_exit(reinterpret_cast<j_common_ptr>(cinfo));
    dest->pub.next_output_byte = dest->out->data() + used;
    dest->pub.free_in_buffer = dest->out->size() - used;
    return TRUE;
}

void termDestination(j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDestination*>(cinfo->dest);
    dest->out->resize(dest->out->size() - dest->pub.free_in_buffer);
}

}

bool encodeGrayBaseline(const uint8_t* pixels, int width, int height, int stride,
                        int quality, std::vector<uint8_t>* out) {
    if (!pixels || !out || width <= 0 || height <= 0 || stride < width) return false;
    try {
        out->assign(std::max<size_t>(64 * 1024, (size_t)width * height / 4), 0);
    } catch (...) {
        return false;
    }

    jpeg_compress_struct cinfo;
    ErrorManager err;
    jpeg_cust_mem_data mem;
    VectorDestination dest;
    std::memset(&cinfo, 0, sizeof(cinfo));
    if (!jpeg_cust_mem_init(&mem, nullptr, nullptr, nullptr, nullptr,
                            jpegAlloc, jpegFree, jpegAlloc, jpegFree, nullptr))
        return false;
    cinfo.client_data = &mem;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = errorExit;
    err.pub.output_message = outputMessage;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        out->clear();
        return false;
    }
    jpeg_create_compress(&cinfo);

    dest.pub.init_destination = initDestination;
    dest.pub.empty_output_buffer = emptyOutputBuffer;
    dest.pub.term_destination = termDestination;
    dest.out = out;
    cinfo.dest = &dest.pub;

    cinfo.image_width = (JDIMENSION)width;
    cinfo.image_height = (JDIMENSION)height;
    cinfo.input_components = 1;
    cinfo.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::max(1, std::min(quality, 100)), TRUE);
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_compress(&cinfo, TRUE);
    JSAMPROW rows[16];
    while (cinfo.next_scanline < cinfo.image_height) {
        const JDIMENSION y = cinfo.next_scanline;
        const JDIMENSION n = std::min<JDIMENSION>(16, cinfo.image_height - y);
        for (JDIMENSION i = 0; i < n; ++i)
            rows[i] = const_cast<JSAMPROW>(pixels + (size_t)(y + i) * (size_t)stride);
        jpeg_write_scanlines(&cinfo, rows, n);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return !out->empty();
}

}
}
