#include "jpeg.h"

#include <cstring>
#include <limits>

#if HARDCOPY_HAS_PDFIN
#if HARDCOPY_JPEG_DIRECT
#include "jpeg_libjpeg.h"
#endif
extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/fitz/write-pixmap.h>
}
#elif HARDCOPY_HAS_JPEG
#include <turbojpeg.h>
#endif

namespace hardcopy {

bool encodeJpegGray(const Bitmap8& src, int quality, std::vector<uint8_t>* out) {
    if (!out || src.pixels.empty() || src.width <= 0 || src.height <= 0) return false;
    const size_t width = static_cast<size_t>(src.width);
    const size_t height = static_cast<size_t>(src.height);
    if (width > std::numeric_limits<size_t>::max() / height ||
        src.pixels.size() < width * height) return false;
    out->clear();

#if HARDCOPY_HAS_PDFIN
#if HARDCOPY_JPEG_DIRECT
    if (detail::encodeGrayBaseline(src.pixels.data(), src.width, src.height,
                                   src.width, quality, out))
        return true;
    out->clear();
#endif
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;

    fz_pixmap* pix = nullptr;
    fz_buffer* jpeg = nullptr;
    bool ok = false;
    fz_var(pix);
    fz_var(jpeg);
    fz_var(ok);
    fz_try(ctx) {
        pix = fz_new_pixmap(ctx, fz_device_gray(ctx), src.width, src.height,
                            nullptr, 0);
        for (int y = 0; y < src.height; ++y) {
            std::memcpy(pix->samples + (ptrdiff_t)y * pix->stride,
                        src.pixels.data() + (size_t)y * src.width,
                        (size_t)src.width);
        }
        jpeg = fz_new_buffer_from_pixmap_as_jpeg(
            ctx, pix, fz_default_color_params, quality, 0);
        unsigned char* data = nullptr;
        size_t size = fz_buffer_storage(ctx, jpeg, &data);
        if (data && size > 0) {
            try {
                out->assign(data, data + size);
                ok = true;
            } catch (...) {
                out->clear();
                ok = false;
            }
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, jpeg);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) {
        out->clear();
        ok = false;
    }
    fz_drop_context(ctx);
    return ok;
#elif HARDCOPY_HAS_JPEG
    tjhandle h = tjInitCompress();
    if (!h) return false;

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    int rc = tjCompress2(h,
                         src.pixels.data(),
                         src.width, src.width, src.height,
                         TJPF_GRAY,
                         &jpegBuf, &jpegSize,
                         TJSAMP_GRAY,
                         quality,
                         TJFLAG_FASTDCT);
    if (rc == 0 && jpegBuf && jpegSize > 0)
        out->assign(jpegBuf, jpegBuf + jpegSize);
    if (jpegBuf) tjFree(jpegBuf);
    tjDestroy(h);
    return rc == 0;
#else
    (void)quality;
    return false;
#endif
}

}
