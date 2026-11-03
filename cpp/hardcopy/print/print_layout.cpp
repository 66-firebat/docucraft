#include "print_layout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "../encode/jpeg.h"
#include "../image/image_decoder.h"
#include "../render/page_raster.h"

namespace hardcopy {
namespace print {

namespace fs = std::filesystem;

struct SizeMm { double w, h; };

static SizeMm paperSizeMm(Paper p) {
    switch (p) {
        case Paper::A4: return {210.0, 297.0};
        case Paper::A5: return {148.0, 210.0};
        case Paper::A6: return {105.0, 148.0};
    }
    return {210.0, 297.0};
}

static int mmToPx(double mm, int dpi) {
    return (int)(mm / 25.4 * dpi + 0.5);
}

struct CellRect { int x, y, w, h; };

struct SheetSpec {
    int wPx, hPx;
    double wPt, hPt;
    std::vector<CellRect> cells;
    std::vector<std::array<int, 4>> dividers;
};

static SheetSpec makeSheetSpec(const PrintOptions& o) {
    SheetSpec s;
    SizeMm mm = paperSizeMm(o.paper);
    if (o.layout == Layout::TwoUp) { mm = paperSizeMm(Paper::A4); std::swap(mm.w, mm.h); }
    if (o.layout == Layout::FourUp) mm = paperSizeMm(Paper::A4);

    s.wPx = mmToPx(mm.w, o.dpi);
    s.hPx = mmToPx(mm.h, o.dpi);
    s.wPt = mm.w * 72.0 / 25.4;
    s.hPt = mm.h * 72.0 / 25.4;

    const int m = mmToPx(o.marginMm, o.dpi);
    const int hm = m / 2;

    switch (o.layout) {
        case Layout::Single:
            s.cells.push_back({m, m, s.wPx - 2 * m, s.hPx - 2 * m});
            break;
        case Layout::TwoUp: {
            const int midX = s.wPx / 2;
            s.cells.push_back({m, m, midX - hm - m, s.hPx - 2 * m});
            s.cells.push_back({midX + hm, m, s.wPx - m - (midX + hm), s.hPx - 2 * m});
            s.dividers.push_back({midX, m, midX, s.hPx - m});
            break;
        }
        case Layout::FourUp: {
            const int midX = s.wPx / 2;
            const int midY = s.hPx / 2;
            const int cw1 = midX - hm - m;
            const int cw2 = s.wPx - m - (midX + hm);
            const int ch1 = midY - hm - m;
            const int ch2 = s.hPx - m - (midY + hm);
            s.cells.push_back({m,          m,          cw1, ch1});
            s.cells.push_back({midX + hm,  m,          cw2, ch1});
            s.cells.push_back({m,          midY + hm,  cw1, ch2});
            s.cells.push_back({midX + hm,  midY + hm,  cw2, ch2});
            s.dividers.push_back({midX, m, midX, s.hPx - m});
            s.dividers.push_back({m, midY, s.wPx - m, midY});
            break;
        }
    }
    return s;
}

static bool loadGray(const fs::path& path, Bitmap8* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open: " + path.string();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty() || !decodeToGray(bytes.data(), bytes.size(), out)) {
        if (err) *err = "decode failed: " + path.string();
        return false;
    }
    return true;
}

static void blitFit(const Bitmap8& src, Bitmap8* dst, const CellRect& cell) {
    if (cell.w <= 0 || cell.h <= 0) return;
    Bitmap8 fitted;
    scaleToFit(src, cell.w, cell.h, &fitted);
    for (int y = 0; y < cell.h; ++y) {
        std::memcpy(&dst->pixels[(size_t)(cell.y + y) * dst->width + cell.x],
                    &fitted.pixels[(size_t)y * fitted.width],
                    (size_t)cell.w);
    }
}

static void drawLine(Bitmap8* bm, int x0, int y0, int x1, int y1, uint8_t gray) {
    if (x0 == x1) {
        for (int y = std::max(0, y0); y <= std::min(bm->height - 1, y1); ++y)
            bm->pixels[(size_t)y * bm->width + x0] = gray;
    } else if (y0 == y1) {
        for (int x = std::max(0, x0); x <= std::min(bm->width - 1, x1); ++x)
            bm->pixels[(size_t)y0 * bm->width + x] = gray;
    }
}

struct SheetJpeg {
    std::vector<uint8_t> jpeg;
    int wPx, hPx;
};

static bool writePdf(const std::vector<SheetJpeg>& sheets,
                     double wPt, double hPt,
                     const fs::path& outPath, std::string* err) {
    FILE* fp = nullptr;
#ifdef _WIN32
    fp = _wfopen(outPath.wstring().c_str(), L"wb");
#else
    fp = std::fopen(outPath.string().c_str(), "wb");
#endif
    if (!fp) {
        if (err) *err = "cannot create: " + outPath.string();
        return false;
    }

    std::vector<long> offsets;
    long pos = 0;
    auto out = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        pos += std::vfprintf(fp, fmt, ap);
        va_end(ap);
    };
    auto beginObj = [&](int id) {
        if ((int)offsets.size() < id) offsets.resize(id, 0);
        offsets[id - 1] = pos;
        out("%d 0 obj\n", id);
    };

    const int n = (int)sheets.size();
    auto pageId = [](int i) { return 5 + i * 3; };

    out("%%PDF-1.4\n");

    beginObj(1);
    out("<</Type/Catalog/Pages 2 0 R>>\nendobj\n");

    beginObj(2);
    out("<</Type/Pages/Count %d/Kids[", n);
    for (int i = 0; i < n; ++i) out("%d 0 R ", pageId(i));
    out("]>>\nendobj\n");

    for (int i = 0; i < n; ++i) {
        const SheetJpeg& sj = sheets[i];
        const int imgId = 3 + i * 3, cntId = 4 + i * 3, pgId = pageId(i);

        beginObj(imgId);
        out("<</Type/XObject/Subtype/Image/Width %d/Height %d"
            "/ColorSpace/DeviceGray/BitsPerComponent 8"
            "/Filter/DCTDecode/Length %zu>>\nstream\n", sj.wPx, sj.hPx, sj.jpeg.size());
        std::fwrite(sj.jpeg.data(), 1, sj.jpeg.size(), fp);
        pos += (long)sj.jpeg.size();
        out("\nendstream\nendobj\n");

        char content[128];
        int clen = std::snprintf(content, sizeof(content),
                                 "q\n%.2f 0 0 %.2f 0 0 cm\n/Im0 Do\nQ\n", wPt, hPt);
        beginObj(cntId);
        out("<</Length %d>>\nstream\n%s\nendstream\nendobj\n", clen, content);

        beginObj(pgId);
        out("<</Type/Page/Parent 2 0 R/MediaBox[0 0 %.2f %.2f]"
            "/Resources<</XObject<</Im0 %d 0 R>>>>"
            "/Contents %d 0 R>>\nendobj\n", wPt, hPt, imgId, cntId);
    }

    const long xrefPos = pos;
    const int total = (int)offsets.size();
    out("xref\n0 %d\n0000000000 65535 f \n", total + 1);
    for (int i = 0; i < total; ++i) out("%010ld 00000 n \n", offsets[i]);
    out("trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%ld\n%%%%EOF\n",
        total + 1, xrefPos);

    std::fclose(fp);
    return true;
}

bool buildPrintPdf(const std::vector<fs::path>& imagePaths,
                   const PrintOptions& opts,
                   const fs::path& outPdf,
                   std::string* err) {
    if (imagePaths.empty()) {
        if (err) *err = "no input images";
        return false;
    }
    const SheetSpec spec = makeSheetSpec(opts);
    const int perSheet = (int)spec.cells.size();

    std::vector<SheetJpeg> sheets;
    size_t idx = 0;
    while (idx < imagePaths.size()) {
        Bitmap8 sheet;
        sheet.width  = spec.wPx;
        sheet.height = spec.hPx;
        sheet.pixels.assign((size_t)spec.wPx * spec.hPx, 255);

        for (int c = 0; c < perSheet && idx < imagePaths.size(); ++c, ++idx) {
            Bitmap8 page;
            if (!loadGray(imagePaths[idx], &page, err)) return false;
            blitFit(page, &sheet, spec.cells[c]);
        }
        if (opts.cutMarks) {
            for (const auto& d : spec.dividers)
                drawLine(&sheet, d[0], d[1], d[2], d[3], 150);
        }

        SheetJpeg sj;
        sj.wPx = sheet.width;
        sj.hPx = sheet.height;
        if (!encodeJpegGray(sheet, opts.jpegQuality, &sj.jpeg)) {
            if (err) *err = "jpeg encode failed";
            return false;
        }
        sheets.push_back(std::move(sj));
    }

    return writePdf(sheets, spec.wPt, spec.hPt, outPdf, err);
}

std::vector<fs::path> collectImages(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool parsePaper(const std::string& s, Paper* out) {
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    if (v == "A4") { *out = Paper::A4; return true; }
    if (v == "A5") { *out = Paper::A5; return true; }
    if (v == "A6") { *out = Paper::A6; return true; }
    return false;
}

bool parseLayout(const std::string& s, Layout* out) {
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (v == "single") { *out = Layout::Single; return true; }
    if (v == "2up")    { *out = Layout::TwoUp;  return true; }
    if (v == "4up")    { *out = Layout::FourUp; return true; }
    return false;
}

}
}
