#include "annotation_exporter.h"

#ifdef HARDCOPY_HAS_PDFIN

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "../logger.h"

namespace hardcopy {
namespace {

constexpr uint32_t kPacketVersion = 3;
constexpr uint32_t kOldestPacketVersion = 1;
constexpr uint32_t kMaxPages = 100000;
constexpr uint32_t kMaxAnnotationsPerPage = 1000000;
constexpr uint32_t kMaxSegmentsPerAnnotation = 100000;
constexpr uint32_t kMaxContoursPerAnnotation = 100000;
constexpr uint32_t kMaxPointsPerSegment = 10000000;
constexpr uint32_t kMaxStringBytes = 1024 * 1024;

struct PacketPoint { float x = 0; float y = 0; };
struct PacketStroke {
    int penType = 0;
    uint32_t rgb = 0;
    float thicknessPx = 1.0f;
    std::string uuid;
    std::vector<std::vector<PacketPoint>> segments;
    std::vector<std::vector<PacketPoint>> contours;
};
struct PacketPage {
    int pageIndex = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::vector<PacketStroke> strokes;
};
struct PacketTextMark {
    int pageIndex = 0;
    int type = 0;
    uint32_t rgb = 0;
    float opacity = 1;
    std::string uuid;
    std::string contents;
    std::vector<fz_quad> quads;
};

class PacketReader {
public:
    explicit PacketReader(const std::filesystem::path& path)
        : in_(path, std::ios::binary) {}
    bool good() const { return in_.good(); }

    bool bytes(void* dst, size_t size) {
        if (size == 0) return true;
        in_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
        return !!in_;
    }
    bool u32(uint32_t* value) {
        uint8_t b[4];
        if (!bytes(b, 4)) return false;
        *value = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
                 (uint32_t(b[2]) << 8) | uint32_t(b[3]);
        return true;
    }
    bool i32(int32_t* value) {
        uint32_t u = 0;
        if (!u32(&u)) return false;
        *value = static_cast<int32_t>(u);
        return true;
    }
    bool f32(float* value) {
        uint32_t u = 0;
        if (!u32(&u)) return false;
        static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
        std::memcpy(value, &u, sizeof(float));
        return true;
    }
    bool string(std::string* value) {
        uint32_t size = 0;
        if (!u32(&size) || size > kMaxStringBytes) return false;
        value->resize(size);
        return bytes(value->data(), size);
    }

private:
    std::ifstream in_;
};

bool readPacket(const std::filesystem::path& path,
                std::vector<PacketPage>* pages,
                std::vector<PacketTextMark>* textMarks,
                bool* replaceTextMarks,
                std::string* error) {
    PacketReader r(path);
    if (!r.good()) {
        if (error) *error = "cannot open annotation packet";
        return false;
    }
    char magic[4];
    uint32_t version = 0, pageCount = 0;
    if (!r.bytes(magic, sizeof(magic)) || std::memcmp(magic, "HCAN", 4) != 0 ||
        !r.u32(&version) || version < kOldestPacketVersion || version > kPacketVersion ||
        !r.u32(&pageCount) || pageCount > kMaxPages) {
        if (error) *error = "invalid HCAN header";
        return false;
    }

    pages->clear();
    pages->reserve(pageCount);
    for (uint32_t p = 0; p < pageCount; ++p) {
        int32_t pageIndex = 0, pixelW = 0, pixelH = 0;
        uint32_t annotationCount = 0;
        if (!r.i32(&pageIndex) || !r.i32(&pixelW) || !r.i32(&pixelH) ||
            !r.u32(&annotationCount) || annotationCount > kMaxAnnotationsPerPage ||
            pixelW <= 0 || pixelH <= 0) {
            if (error) *error = "invalid HCAN page record";
            return false;
        }
        PacketPage page;
        page.pageIndex = pageIndex;
        page.pixelWidth = pixelW;
        page.pixelHeight = pixelH;
        page.strokes.reserve(annotationCount);

        for (uint32_t a = 0; a < annotationCount; ++a) {
            int32_t penType = 0;
            uint32_t rgb = 0;
            float thickness = 0;
            uint32_t segmentCount = 0;
            PacketStroke stroke;
            if (!r.i32(&penType) || !r.u32(&rgb) || !r.f32(&thickness) ||
                !r.string(&stroke.uuid) || !r.u32(&segmentCount) ||
                segmentCount > kMaxSegmentsPerAnnotation) {
                if (error) *error = "invalid HCAN annotation record";
                return false;
            }
            stroke.penType = penType;
            stroke.rgb = rgb & 0x00FFFFFFu;
            stroke.thicknessPx = std::max(0.25f, thickness);
            stroke.segments.reserve(segmentCount);
            for (uint32_t s = 0; s < segmentCount; ++s) {
                uint32_t pointCount = 0;
                if (!r.u32(&pointCount) || pointCount > kMaxPointsPerSegment) {
                    if (error) *error = "invalid HCAN point count";
                    return false;
                }
                std::vector<PacketPoint> segment;
                segment.resize(pointCount);
                for (PacketPoint& point : segment) {
                    if (!r.f32(&point.x) || !r.f32(&point.y)) {
                        if (error) *error = "truncated HCAN point data";
                        return false;
                    }
                }
                if (!segment.empty()) stroke.segments.push_back(std::move(segment));
            }
            if (version >= 2) {
                uint32_t contourCount = 0;
                if (!r.u32(&contourCount) || contourCount > kMaxContoursPerAnnotation) {
                    if (error) *error = "invalid HCAN contour count";
                    return false;
                }
                stroke.contours.reserve(contourCount);
                for (uint32_t c = 0; c < contourCount; ++c) {
                    uint32_t pointCount = 0;
                    if (!r.u32(&pointCount) || pointCount > kMaxPointsPerSegment) {
                        if (error) *error = "invalid HCAN contour point count";
                        return false;
                    }
                    std::vector<PacketPoint> contour;
                    contour.resize(pointCount);
                    for (PacketPoint& point : contour) {
                        if (!r.f32(&point.x) || !r.f32(&point.y)) {
                            if (error) *error = "truncated HCAN contour data";
                            return false;
                        }
                    }
                    if (!contour.empty()) stroke.contours.push_back(std::move(contour));
                }
            }
            if (!stroke.segments.empty() || !stroke.contours.empty())
                page.strokes.push_back(std::move(stroke));
        }
        pages->push_back(std::move(page));
    }
    if (version >= 3) {
        uint32_t replace = 0, count = 0;
        if (!r.u32(&replace) || replace > 1 || !r.u32(&count) || count > kMaxAnnotationsPerPage) {
            if (error) *error = "invalid HCAN text markup header";
            return false;
        }
        *replaceTextMarks = replace != 0;
        for (uint32_t i = 0; i < count; ++i) {
            int32_t pageIndex = 0, type = 0;
            uint32_t quadCount = 0;
            PacketTextMark mark;
            if (!r.i32(&pageIndex) || pageIndex < 0 || !r.i32(&type) || type < 0 || type > 1 ||
                !r.u32(&mark.rgb) || !r.f32(&mark.opacity) || !std::isfinite(mark.opacity) ||
                mark.opacity < 0 || mark.opacity > 1 || !r.string(&mark.uuid) ||
                !r.string(&mark.contents) || !r.u32(&quadCount) || quadCount > kMaxSegmentsPerAnnotation) {
                if (error) *error = "invalid HCAN text markup";
                return false;
            }
            mark.pageIndex = pageIndex;
            mark.type = type;
            for (uint32_t q = 0; q < quadCount; ++q) {
                float x0, y0, x1, y1;
                if (!r.f32(&x0) || !r.f32(&y0) || !r.f32(&x1) || !r.f32(&y1) ||
                    !std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) ||
                    x1 <= x0 || y1 <= y0) {
                    if (error) *error = "invalid HCAN text rectangle";
                    return false;
                }
                mark.quads.push_back(fz_make_quad(x0, y0, x1, y0, x0, y1, x1, y1));
            }
            if (!mark.quads.empty()) textMarks->push_back(std::move(mark));
        }
    }
    return true;
}

void logMsg(Logger* log, LogLevel level, const std::string& message) {
    if (log) log->log(level, "pdf-annot", message.c_str());
}

fz_point mapPoint(const PacketPoint& p, int pixelW, int pixelH, fz_rect bounds,
                  bool clampToPage = true) {
    float nx = p.x / std::max(1, pixelW);
    float ny = p.y / std::max(1, pixelH);
    if (clampToPage) {
        nx = std::clamp(nx, 0.0f, 1.0f);
        ny = std::clamp(ny, 0.0f, 1.0f);
    }
    return fz_make_point(bounds.x0 + nx * (bounds.x1 - bounds.x0),
                         bounds.y0 + ny * (bounds.y1 - bounds.y0));
}

using MappedContours = std::vector<std::vector<fz_point>>;

MappedContours mapContours(const PacketStroke& stroke, int pixelW, int pixelH,
                           fz_rect pageBounds) {
    MappedContours mapped;
    mapped.reserve(stroke.contours.size());
    for (const auto& source : stroke.contours) {
        if (source.empty()) continue;
        std::vector<fz_point> contour;
        contour.reserve(source.size());
        for (const PacketPoint& point : source) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
            contour.push_back(mapPoint(point, pixelW, pixelH, pageBounds, false));
        }
        if (!contour.empty()) mapped.push_back(std::move(contour));
    }
    return mapped;
}

fz_rect contourBounds(const MappedContours& contours) {
    fz_rect bounds = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (const auto& contour : contours) {
        for (const fz_point& point : contour) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
    }
    if (!std::isfinite(bounds.x0) || !std::isfinite(bounds.y0) ||
        !std::isfinite(bounds.x1) || !std::isfinite(bounds.y1)) {
        return fz_empty_rect;
    }
    if (bounds.x1 - bounds.x0 < 0.01f) {
        bounds.x0 -= 0.5f;
        bounds.x1 += 0.5f;
    }
    if (bounds.y1 - bounds.y0 < 0.01f) {
        bounds.y0 -= 0.5f;
        bounds.y1 += 0.5f;
    }
    return bounds;
}

float estimateContourWidth(const PacketStroke& stroke,
                           const MappedContours& contours,
                           int pixelW, int pixelH, fz_rect pageBounds,
                           float fallbackWidth) {
    double area = 0.0;
    for (const auto& contour : contours) {
        if (contour.size() < 3) continue;
        double twiceArea = 0.0;
        for (size_t i = 0; i < contour.size(); ++i) {
            const fz_point& a = contour[i];
            const fz_point& b = contour[(i + 1) % contour.size()];
            twiceArea += static_cast<double>(a.x) * b.y -
                         static_cast<double>(b.x) * a.y;
        }
        area += std::abs(twiceArea) * 0.5;
    }

    double centerlineLength = 0.0;
    for (const auto& segment : stroke.segments) {
        fz_point previous{};
        bool havePrevious = false;
        for (const PacketPoint& source : segment) {
            const fz_point point = mapPoint(source, pixelW, pixelH, pageBounds);
            if (havePrevious) {
                centerlineLength += std::hypot(
                    static_cast<double>(point.x - previous.x),
                    static_cast<double>(point.y - previous.y));
            }
            previous = point;
            havePrevious = true;
        }
    }
    if (area > 0.0 && centerlineLength > 0.01) {
        const float estimated = static_cast<float>(area / centerlineLength);
        if (std::isfinite(estimated) && estimated >= 0.25f) return estimated;
    }

    const fz_rect bounds = contourBounds(contours);
    const float dotWidth = std::min(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
    if (std::isfinite(dotWidth) && dotWidth >= 0.25f) return dotWidth;
    return std::max(0.25f, fallbackWidth);
}

void setContourAppearance(fz_context* ctx, pdf_page* page, pdf_annot* annot,
                          const MappedContours& contours, const float color[3],
                          float opacity) {
    if (contours.empty()) return;
    const fz_rect bounds = contourBounds(contours);
    fz_display_list* list = nullptr;
    fz_device* device = nullptr;
    fz_path* path = nullptr;
    fz_var(list);
    fz_var(device);
    fz_var(path);
    fz_try(ctx) {
        path = fz_new_path(ctx);
        for (const auto& contour : contours) {
            if (contour.empty()) continue;
            fz_moveto(ctx, path, contour[0].x, contour[0].y);
            for (size_t i = 1; i < contour.size(); ++i)
                fz_lineto(ctx, path, contour[i].x, contour[i].y);
            fz_closepath(ctx, path);
        }

        list = fz_new_display_list(ctx, bounds);
        device = fz_new_list_device(ctx, list);
        fz_fill_path(ctx, device, path, 1, fz_identity, fz_device_rgb(ctx),
                     color, opacity, fz_default_color_params);
        fz_close_device(ctx, device);
        fz_drop_device(ctx, device);
        device = nullptr;

        fz_matrix pageCtm;
        pdf_page_transform(ctx, page, nullptr, &pageCtm);
        const fz_rect pdfBounds = fz_transform_rect(bounds, fz_invert_matrix(pageCtm));
        pdf_dict_put_rect(ctx, pdf_annot_obj(ctx, annot), PDF_NAME(Rect), pdfBounds);
        pdf_set_annot_appearance_from_display_list(
            ctx, annot, "N", nullptr, fz_identity, list);
    }
    fz_always(ctx) {
        fz_drop_path(ctx, path);
        fz_drop_device(ctx, device);
        fz_drop_display_list(ctx, list);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

constexpr const char* kSupernoteIdPrefix = "hardcopy:sn:";

constexpr uint32_t kInkTargetColors[4] = {
    0x00000000u,
    0x002775A0u,
    0x004E3531u,
    0x0088979Bu,
};

int inkGrayBucket(uint32_t rgb) {
    const int r = static_cast<int>((rgb >> 16) & 0xff);
    const int g = static_cast<int>((rgb >> 8) & 0xff);
    const int b = static_cast<int>(rgb & 0xff);
    const int gray = (r * 77 + g * 150 + b * 29) >> 8;
    if (gray >= 230) return -1;
    if (gray >= 179) return 2;
    if (gray >= 78) return 1;
    return 0;
}

uint32_t mappedInkRgb(uint32_t rgb, const int* inkColorMap) {
    if (!inkColorMap) return rgb & 0x00ffffffu;
    const int bucket = inkGrayBucket(rgb);
    if (bucket < 0) return rgb & 0x00ffffffu;
    const int target = std::clamp(inkColorMap[bucket], 0, 3);
    if (target == 0) return rgb & 0x00ffffffu;
    return kInkTargetColors[target];
}

bool hasSupernoteIdPrefix(const char* value) {
    if (!value) return false;
    return std::strncmp(value, kSupernoteIdPrefix, std::strlen(kSupernoteIdPrefix)) == 0;
}

}

ink_status_t exportPdfAnnotations(const std::filesystem::path& inputPdf,
                                  const std::filesystem::path& outputPdf,
                                  const std::filesystem::path& packetPath,
                                  const char* author,
                                  float markerOpacity,
                                  const int* inkColorMap,
                                  Logger* log) {
    std::vector<PacketPage> packetPages;
    std::vector<PacketTextMark> textMarks;
    bool replaceTextMarks = false;
    std::string packetError;
    if (!readPacket(packetPath, &packetPages, &textMarks, &replaceTextMarks, &packetError)) {
        logMsg(log, LogLevel::Error, packetError);
        return INK_ERR_PARSE;
    }

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return INK_ERR_INTERNAL;
    fz_register_document_handlers(ctx);

    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    bool saved = false;
    markerOpacity = std::clamp(markerOpacity, 0.05f, 1.0f);

    fz_var(doc);
    fz_var(page);
    fz_try(ctx) {
        doc = pdf_open_document(ctx, inputPdf.string().c_str());
        const int pdfPages = pdf_count_pages(ctx, doc);
        int written = 0;
        int outlined = 0;
        int centerlineFallback = 0;
        int removed = 0;

        std::unordered_map<int, std::unordered_set<std::string>> currentIds;
        for (const PacketPage& srcPage : packetPages) {
            auto& ids = currentIds[srcPage.pageIndex];
            for (const PacketStroke& stroke : srcPage.strokes) {
                if (!stroke.uuid.empty()) ids.insert(stroke.uuid);
            }
        }
        for (int pageIndex = 0; pageIndex < pdfPages; ++pageIndex) {
            page = pdf_load_page(ctx, doc, pageIndex);
            const auto pageIdsIt = currentIds.find(pageIndex);
            for (pdf_annot* annot = pdf_first_annot(ctx, page); annot;) {
                pdf_annot* next = pdf_next_annot(ctx, annot);
                pdf_obj* nmKey = pdf_new_name(ctx, "NM");
                const char* nm = pdf_dict_get_text_string(ctx, pdf_annot_obj(ctx, annot), nmKey);
                const bool ours = hasSupernoteIdPrefix(nm);
                const bool oldTextMark = replaceTextMarks && nm && std::strncmp(nm, "hardcopy:text:", 14) == 0;
                const bool keep = ours && pageIdsIt != currentIds.end() &&
                    pageIdsIt->second.find(nm) != pageIdsIt->second.end() &&
                    pdf_annot_type(ctx, annot) == PDF_ANNOT_INK;
                pdf_drop_obj(ctx, nmKey);
                if ((ours && !keep) || oldTextMark) {
                    pdf_delete_annot(ctx, page, annot);
                    ++removed;
                }
                annot = next;
            }
            pdf_drop_page(ctx, page);
            page = nullptr;
        }

        for (const PacketPage& srcPage : packetPages) {
            if (srcPage.pageIndex < 0 || srcPage.pageIndex >= pdfPages) continue;
            page = pdf_load_page(ctx, doc, srcPage.pageIndex);
            const fz_rect bounds = pdf_bound_page(ctx, page, FZ_CROP_BOX);
            const float xScale = (bounds.x1 - bounds.x0) / std::max(1, srcPage.pixelWidth);
            const float yScale = (bounds.y1 - bounds.y0) / std::max(1, srcPage.pixelHeight);
            const float widthScale = std::max(0.01f, (xScale + yScale) * 0.5f);

            for (const PacketStroke& src : srcPage.strokes) {
                std::vector<int> counts;
                std::vector<fz_point> points;
                const auto& editablePaths = src.segments.empty() ? src.contours : src.segments;
                counts.reserve(editablePaths.size());
                size_t totalPoints = 0;
                for (const auto& seg : editablePaths) totalPoints += seg.size();
                points.reserve(totalPoints);
                for (const auto& seg : editablePaths) {
                    if (seg.empty()) continue;
                    counts.push_back(static_cast<int>(seg.size()));
                    for (const PacketPoint& p : seg)
                        points.push_back(mapPoint(p, srcPage.pixelWidth, srcPage.pixelHeight, bounds));
                }
                if (counts.empty() || points.empty()) continue;

                pdf_annot* annot = nullptr;
                if (!src.uuid.empty()) {
                    pdf_obj* nmKey = pdf_new_name(ctx, "NM");
                    for (pdf_annot* candidate = pdf_first_annot(ctx, page);
                         candidate; candidate = pdf_next_annot(ctx, candidate)) {
                        const char* nm = pdf_dict_get_text_string(
                            ctx, pdf_annot_obj(ctx, candidate), nmKey);
                        if (nm && src.uuid == nm && pdf_annot_type(ctx, candidate) == PDF_ANNOT_INK) {
                            annot = candidate;
                            break;
                        }
                    }
                    pdf_drop_obj(ctx, nmKey);
                }
                if (!annot) annot = pdf_create_annot(ctx, page, PDF_ANNOT_INK);
                pdf_set_annot_ink_list(ctx, annot, static_cast<int>(counts.size()),
                                       counts.data(), points.data());

                const uint32_t mappedRgb = mappedInkRgb(src.rgb, inkColorMap);
                const float color[3] = {
                    ((mappedRgb >> 16) & 0xff) / 255.0f,
                    ((mappedRgb >> 8) & 0xff) / 255.0f,
                    (mappedRgb & 0xff) / 255.0f,
                };
                const float opacity = src.penType == 11 ? markerOpacity : 1.0f;
                const MappedContours contours = mapContours(
                    src, srcPage.pixelWidth, srcPage.pixelHeight, bounds);
                const float nominalWidth = contours.empty()
                    ? std::max(0.25f, src.thicknessPx * widthScale)
                    : estimateContourWidth(
                        src, contours, srcPage.pixelWidth, srcPage.pixelHeight,
                        bounds, src.thicknessPx * widthScale);
                pdf_set_annot_color(ctx, annot, 3, color);
                pdf_set_annot_border_width(ctx, annot, nominalWidth);
                pdf_set_annot_opacity(ctx, annot, opacity);
                pdf_set_annot_flags(ctx, annot, PDF_ANNOT_IS_PRINT);
                if (author && *author) pdf_set_annot_author(ctx, annot, author);
                if (!src.uuid.empty()) {
                    pdf_obj* nmKey = pdf_new_name(ctx, "NM");
                    pdf_dict_put_text_string(ctx, pdf_annot_obj(ctx, annot),
                                             nmKey, src.uuid.c_str());
                    pdf_drop_obj(ctx, nmKey);
                }
                if (!contours.empty()) {
                    setContourAppearance(ctx, page, annot, contours, color, opacity);
                    ++outlined;
                } else {
                    pdf_update_annot(ctx, annot);
                    ++centerlineFallback;
                }
                ++written;
            }
            pdf_drop_page(ctx, page);
            page = nullptr;
        }

        for (const PacketTextMark& mark : textMarks) {
            if (mark.pageIndex >= pdfPages) fz_throw(ctx, FZ_ERROR_FORMAT, "text annotation page exceeds PDF page count");
            page = pdf_load_page(ctx, doc, mark.pageIndex);
            pdf_annot* annot = pdf_create_annot(ctx, page, mark.type == 0 ? PDF_ANNOT_HIGHLIGHT : PDF_ANNOT_UNDERLINE);
            pdf_set_annot_quad_points(ctx, annot, static_cast<int>(mark.quads.size()), mark.quads.data());
            const float color[3] = { ((mark.rgb >> 16) & 255) / 255.f,
                                     ((mark.rgb >> 8) & 255) / 255.f, (mark.rgb & 255) / 255.f };
            pdf_set_annot_color(ctx, annot, 3, color);
            pdf_set_annot_opacity(ctx, annot, mark.opacity);
            pdf_set_annot_flags(ctx, annot, PDF_ANNOT_IS_PRINT);
            if (author && *author) pdf_set_annot_author(ctx, annot, author);
            pdf_set_annot_contents(ctx, annot, mark.contents.c_str());
            pdf_obj* nmKey = pdf_new_name(ctx, "NM");
            pdf_dict_put_text_string(ctx, pdf_annot_obj(ctx, annot), nmKey, mark.uuid.c_str());
            pdf_drop_obj(ctx, nmKey);
            pdf_update_annot(ctx, annot);
            pdf_drop_page(ctx, page);
            page = nullptr;
        }

        pdf_write_options opts = pdf_default_write_options;
        opts.do_compress = 1;
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_appearance = 1;
        pdf_save_document(ctx, doc, outputPdf.string().c_str(), &opts);
        saved = true;
        logMsg(log, LogLevel::Info,
               "wrote " + std::to_string(written) + " editable ink annotations (" +
               std::to_string(outlined) + " native contour appearances, " +
               std::to_string(centerlineFallback) + " centerline fallbacks); removed " +
               std::to_string(removed) + " stale Hardcopy annotations; text markups=" + std::to_string(textMarks.size()));
    }
    fz_always(ctx) {
        if (page) pdf_drop_page(ctx, page);
        if (doc) pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        logMsg(log, LogLevel::Error, fz_caught_message(ctx));
        saved = false;
    }
    fz_drop_context(ctx);
    return saved ? INK_OK : INK_ERR_WRITE_OUTPUT;
}

}

#else

#include "../logger.h"
namespace hardcopy {
ink_status_t exportPdfAnnotations(const std::filesystem::path&,
                                  const std::filesystem::path&,
                                  const std::filesystem::path&,
                                  const char*, float, Logger* log) {
    if (log) log->log(LogLevel::Error, "pdf-annot", "PDF input support is disabled");
    return INK_ERR_INVALID_OPTIONS;
}
}

#endif
