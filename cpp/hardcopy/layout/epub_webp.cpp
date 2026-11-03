#include "epub_webp.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <webp/decode.h>

#include "../encode/jpeg.h"
#include "../logger.h"
#include "../render/page_raster.h"

namespace hardcopy {

namespace {

constexpr int kJpegQuality = 92;

std::string lowerExt(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

bool isWebpBytes(const unsigned char* d, size_t n) {
    return n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0;
}

bool mayBePicture(const std::string& name) {
    const std::string ext = lowerExt(name);
    return ext == "webp" || ext == "jpg" || ext == "jpeg" || ext == "png" ||
           ext == "gif" || ext == "bmp" || ext.empty();
}

bool entryIsWebp(fz_context* ctx, fz_archive* arch, const char* name) {
    unsigned char head[12];
    size_t n = 0;
    fz_stream* s = nullptr;
    fz_var(s);
    fz_try(ctx) {
        s = fz_open_archive_entry(ctx, arch, name);
        n = fz_read(ctx, s, head, sizeof(head));
    }
    fz_always(ctx) { fz_drop_stream(ctx, s); }
    fz_catch(ctx) { n = 0; }
    return isWebpBytes(head, n);
}

fz_buffer* reencodeWebp(fz_context* ctx, const unsigned char* data, size_t len) {
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(data, len, &features) != VP8_STATUS_OK) return nullptr;
    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA(data, len, &w, &h);
    if (!rgba) return nullptr;
    auto luma = [](const uint8_t* p) -> unsigned {
        return ((unsigned)p[0] * 77 + (unsigned)p[1] * 150 + (unsigned)p[2] * 29) >> 8;
    };

    if (!features.has_alpha) {
        Bitmap8 gray;
        gray.width = w;
        gray.height = h;
        gray.pixels.resize((size_t)w * h);
        for (size_t i = 0; i < gray.pixels.size(); ++i) gray.pixels[i] = (uint8_t)luma(rgba + i * 4);
        WebPFree(rgba);
        std::vector<uint8_t> jpeg;
        if (!encodeJpegGray(gray, kJpegQuality, &jpeg)) return nullptr;
        return fz_new_buffer_from_copied_data(ctx, jpeg.data(), jpeg.size());
    }

    fz_pixmap* pix = nullptr;
    fz_buffer* out = nullptr;
    fz_var(pix);
    fz_var(out);
    fz_try(ctx) {
        pix = fz_new_pixmap(ctx, fz_device_gray(ctx), w, h, nullptr, 1);
        unsigned char* samples = fz_pixmap_samples(ctx, pix);
        const int stride = fz_pixmap_stride(ctx, pix);
        for (int y = 0; y < h; ++y) {
            unsigned char* row = samples + (size_t)y * stride;
            const uint8_t* src = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                const unsigned a = src[x * 4 + 3];
                row[x * 2] = (unsigned char)((luma(src + x * 4) * a + 127) / 255);
                row[x * 2 + 1] = (unsigned char)a;
            }
        }
        out = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
        WebPFree(rgba);
    }
    fz_catch(ctx) {
        out = nullptr;
    }
    return out;
}

bool isEpub(const std::filesystem::path& path) {
    return lowerExt(path.filename().string()) == "epub";
}

}

fz_document* openMupdfDocument(fz_context* ctx, const std::filesystem::path& path, Logger* log) {
    if (!isEpub(path)) return fz_open_document(ctx, path.string().c_str());

    fz_archive* arch = nullptr;
    fz_buffer* copy = nullptr;
    fz_buffer* entry = nullptr;
    fz_buffer* converted = nullptr;
    fz_zip_writer* zip = nullptr;
    fz_stream* stm = nullptr;
    fz_document* doc = nullptr;
    int webp = 0;
    int reencoded = 0;
    int failed = 0;
    fz_var(arch);
    fz_var(copy);
    fz_var(entry);
    fz_var(converted);
    fz_var(zip);
    fz_var(stm);
    fz_var(doc);
    fz_var(webp);
    fz_var(reencoded);
    fz_var(failed);
    fz_try(ctx) {
        arch = fz_open_zip_archive(ctx, path.string().c_str());
        const int count = fz_count_archive_entries(ctx, arch);
        for (int i = 0; i < count && webp == 0; ++i) {
            const char* name = fz_list_archive_entry(ctx, arch, i);
            if (name && mayBePicture(name) && entryIsWebp(ctx, arch, name)) ++webp;
        }
        if (webp == 0) {
            doc = fz_open_document(ctx, path.string().c_str());
        } else {
            copy = fz_new_buffer(ctx, 1 << 20);
            zip = fz_new_zip_writer_with_output(ctx, fz_new_output_with_buffer(ctx, copy));
            for (int i = 0; i < count; ++i) {
                const char* name = fz_list_archive_entry(ctx, arch, i);
                if (!name) continue;
                entry = fz_read_archive_entry(ctx, arch, name);
                unsigned char* data = nullptr;
                const size_t len = fz_buffer_storage(ctx, entry, &data);
                fz_buffer* write = entry;
                if (isWebpBytes(data, len)) {
                    converted = reencodeWebp(ctx, data, len);
                    if (converted) {
                        write = converted;
                        ++reencoded;
                    } else {
                        ++failed;
                    }
                }
                fz_write_zip_entry(ctx, zip, name, write, std::strcmp(name, "mimetype") != 0);
                fz_drop_buffer(ctx, converted);
                converted = nullptr;
                fz_drop_buffer(ctx, entry);
                entry = nullptr;
            }
            fz_close_zip_writer(ctx, zip);
            stm = fz_open_buffer(ctx, copy);
            doc = fz_open_document_with_stream(ctx, "application/epub+zip", stm);
        }
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, stm);
        fz_drop_zip_writer(ctx, zip);
        fz_drop_buffer(ctx, converted);
        fz_drop_buffer(ctx, entry);
        fz_drop_buffer(ctx, copy);
        fz_drop_archive(ctx, arch);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    if (webp > 0 && log) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "epub webp: %d pictures re-encoded, %d undecodable",
                      reencoded, failed);
        log->log(LogLevel::Info, "epub-webp", msg);
    }
    return doc;
}

}
