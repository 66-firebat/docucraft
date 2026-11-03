#include "text_boxes.h"

extern "C" {
#include <mupdf/fitz.h>
}

namespace hardcopy {

std::vector<float> pdfTextLineBoxes(const char* utf8Path, int page) {
    std::vector<float> boxes;
    if (!utf8Path || page < 0) return boxes;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return boxes;
    fz_document* doc = nullptr;
    fz_page* pg = nullptr;
    fz_stext_page* text = nullptr;
    fz_var(doc);
    fz_var(pg);
    fz_var(text);
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = fz_open_document(ctx, utf8Path);
        if (page < fz_count_pages(ctx, doc)) {
            pg = fz_load_page(ctx, doc, page);
            const fz_rect bounds = fz_bound_page(ctx, pg);
            const float w = bounds.x1 - bounds.x0;
            const float h = bounds.y1 - bounds.y0;
            if (w > 0 && h > 0) {
                text = fz_new_stext_page_from_page(ctx, pg, nullptr);
                for (fz_stext_block* b = text->first_block; b; b = b->next) {
                    if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (fz_stext_line* line = b->u.t.first_line; line; line = line->next) {
                        const fz_rect r = line->bbox;
                        if (!(r.x1 > r.x0 && r.y1 > r.y0)) continue;
                        bool ink = false;
                        for (fz_stext_char* c = line->first_char; c && !ink; c = c->next)
                            ink = c->c > ' ';
                        if (!ink) continue;
                        boxes.push_back((r.x0 - bounds.x0) / w);
                        boxes.push_back((r.y0 - bounds.y0) / h);
                        boxes.push_back((r.x1 - bounds.x0) / w);
                        boxes.push_back((r.y1 - bounds.y0) / h);
                    }
                }
            }
        }
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, text);
        fz_drop_page(ctx, pg);
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        boxes.clear();
    }
    fz_drop_context(ctx);
    return boxes;
}

}
