#include "pdf_comic_parser.h"

#include <cstdio>
#include <set>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace hardcopy {

namespace {

const char* directExt(int type) {
    switch (type) {
        case FZ_IMAGE_JPEG: return ".jpg";
        case FZ_IMAGE_PNG:  return ".png";
        default:            return nullptr;
    }
}

void appendImage(fz_context* ctx, pdf_document* pdf, pdf_obj* imgObj,
                 ComicDocument* doc, int* counter) {
    fz_image*  img = nullptr;
    fz_buffer* buf = nullptr;
    fz_pixmap* pix = nullptr;
    const char* ext = nullptr;

    fz_var(img);
    fz_var(buf);
    fz_var(pix);

    fz_try(ctx) {
        img = pdf_load_image(ctx, pdf, imgObj);

        fz_compressed_buffer* cb = fz_compressed_image_buffer(ctx, img);
        if (cb && cb->buffer && directExt(cb->params.type)) {
            ext = directExt(cb->params.type);
            buf = fz_keep_buffer(ctx, cb->buffer);
        } else {
            pix = fz_get_pixmap_from_image(ctx, img, nullptr, nullptr,
                                           nullptr, nullptr);
            buf = fz_new_buffer_from_pixmap_as_png(ctx, pix,
                                                   fz_default_color_params);
            ext = ".png";
        }

        unsigned char* bytes = nullptr;
        size_t len = fz_buffer_storage(ctx, buf, &bytes);
        if (bytes && len > 0) {
            ComicPage page;
            page.data.assign(bytes, bytes + len);
            char name[32];
            std::snprintf(name, sizeof(name), "%04d%s", ++(*counter), ext);
            page.filename = name;
            doc->pages.push_back(std::move(page));
        }
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
        fz_drop_buffer(ctx, buf);
        fz_drop_image(ctx, img);
    }
    fz_catch(ctx) {
    }
}

void walkResources(fz_context* ctx, pdf_document* pdf, pdf_obj* res,
                   ComicDocument* doc, int* counter, std::set<int>& seen,
                   int depth) {
    if (!res || depth > 16) return;

    pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    int n = pdf_dict_len(ctx, xobjs);
    for (int i = 0; i < n; ++i) {
        pdf_obj* xobj = pdf_dict_get_val(ctx, xobjs, i);
        if (!xobj) continue;

        int num = pdf_to_num(ctx, xobj);
        if (num) {
            if (seen.count(num)) continue;
            seen.insert(num);
        }

        pdf_obj* subtype = pdf_dict_get(ctx, xobj, PDF_NAME(Subtype));
        if (pdf_name_eq(ctx, subtype, PDF_NAME(Image))) {
            appendImage(ctx, pdf, xobj, doc, counter);
        } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Form))) {
            pdf_obj* sub = pdf_dict_get(ctx, xobj, PDF_NAME(Resources));
            walkResources(ctx, pdf, sub, doc, counter, seen, depth + 1);
        }
    }
}

}

bool parsePdfComic(const std::filesystem::path& path, ComicDocument* doc,
                   Logger* log) {
    if (!doc) return false;

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        if (log) log->log(LogLevel::Error, "pdf-comic", "fz_new_context failed");
        return false;
    }
    fz_register_document_handlers(ctx);

    fz_document* fzdoc = nullptr;
    bool ok = false;

    fz_var(fzdoc);
    fz_var(ok);

    fz_try(ctx) {
        fzdoc = fz_open_document(ctx, path.string().c_str());
        if (!fzdoc) fz_throw(ctx, FZ_ERROR_GENERIC, "open_document returned NULL");

        pdf_document* pdf = pdf_specifics(ctx, fzdoc);
        if (!pdf) fz_throw(ctx, FZ_ERROR_GENERIC, "not a PDF document");

        doc->title = path.stem().string();
        char meta[1024];
        int m = fz_lookup_metadata(ctx, fzdoc, FZ_META_INFO_TITLE,
                                   meta, sizeof(meta));
        if (m > 0) doc->title.assign(meta, (size_t)(m - 1));
        m = fz_lookup_metadata(ctx, fzdoc, FZ_META_INFO_AUTHOR,
                               meta, sizeof(meta));
        if (m > 0) doc->author.assign(meta, (size_t)(m - 1));

        int counter = 0;
        std::set<int> seen;
        int npages = pdf_count_pages(ctx, pdf);
        for (int i = 0; i < npages; ++i) {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdf, i);
            pdf_obj* res = pdf_dict_get_inheritable(ctx, pageObj,
                                                    PDF_NAME(Resources));
            walkResources(ctx, pdf, res, doc, &counter, seen, 0);
        }

        ok = !doc->pages.empty();
        if (!ok && log)
            log->log(LogLevel::Error, "pdf-comic",
                     "no embedded images found in PDF");
    }
    fz_catch(ctx) {
        if (log) log->log(LogLevel::Error, "pdf-comic", fz_caught_message(ctx));
        ok = false;
    }

    if (fzdoc) fz_drop_document(ctx, fzdoc);
    fz_drop_context(ctx);

    if (ok && log) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "extracted %zu images from PDF",
                      doc->pages.size());
        log->log(LogLevel::Info, "pdf-comic", buf);
    }
    return ok;
}

}
