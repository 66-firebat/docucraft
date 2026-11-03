#ifndef HARDCOPY_OPTIONS_H_
#define HARDCOPY_OPTIONS_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hardcopy {

enum class Orientation {
    HorizontalLtr,
    VerticalRtl,
};

enum class EpubEngine {
    Auto = 0,
    Mupdf,
    Legacy,
};

enum class PdfMode {
    Reflow = 0,
    Raster,
};

enum class PageCodec {
    Auto = 0,
    Jpeg,
    Flate,
};

enum class ZhConvert {
    None = 0,
    S2T,
    T2S,
};

struct Options {
    std::string fontPath;
    std::string fontPathBold;
    std::string fontPathItalic;
    std::string fontPathMono;
    std::string fontPathSans;
    std::string fontPathKai;

    std::string fontPathFallback;

    std::vector<uint8_t> fontData;
    std::vector<uint8_t> fontDataBold;
    std::vector<uint8_t> fontDataItalic;
    std::vector<uint8_t> fontDataMono;
    std::vector<uint8_t> fontDataSans;
    std::vector<uint8_t> fontDataKai;
    std::vector<uint8_t> fontDataFallback;

    int pageWidth    = 1920;
    int pageHeight   = 2560;
    int marginTop    = 120;
    int marginRight  = 134;
    int marginBottom = 120;
    int marginLeft   = 134;

    double fontSize       = 22.0;
    double lineHeightMul  = 1.6;
    double headingScale[6] = {2.0, 1.7, 1.45, 1.25, 1.1, 1.0};
    double paragraphSpacing = 6.0;
    double firstLineIndentChars = 0.0;

    Orientation orientation = Orientation::HorizontalLtr;
    bool        splitLandscape = false;
    bool        reversePages   = false;
    bool        bilingual      = false;
    std::string bilingualLayout = "stacked";
    bool bilingualSideBySide() const { return bilingualLayout == "columns"; }

    int  jpegQuality = 90;
    int  threadCount = 0;

    bool embedTextLayer = true;
    bool embedBookmarks = true;

    bool drawRuling     = true;
    std::string rulingStyle = "dotted";
    bool whiteText      = false;
    bool syntheticBold  = true;
    double contrastBoost = 0.08;
    double imageContrastBoost = 0.0;
    double strokeExpand  = 0.0;

    bool pdfTrim = true;
    bool pdfAutoOrientation = true;
    bool pdfOcr = false;
    bool pdfIgnoreText = false;
    std::string ocrCacheDir;
    int pdfHeaderCutPercent = -1;
    int pdfFooterCutPercent = -1;
    int pdfMaxOutputPages = 0;
    int pdfPreviewSourcePage = -1;
    int pdfWatermarkThreshold = 0;
    double pdfGamma = 0.0;
    double pdfContrast = 1.0;
    double pdfStroke = 0.0;
    double pdfImageContrast = 0.0;
    double pdfFillTone = 0.0;
    int pdfColumns = 0;
    double pdfLineHeight = 0.0;
    double pdfParaSpacingEm = 0.0;

    bool        footerProgress = false;
    std::string backgroundPath;

    bool comicMode      = false;

    std::string preset;

    EpubEngine epubEngine = EpubEngine::Auto;
    PdfMode    pdfMode    = PdfMode::Raster;
    PageCodec  pageCodec  = PageCodec::Auto;
    ZhConvert  zhConvert  = ZhConvert::None;

    int  ditherAlgo     = 0;
    int  ditherLevels   = 16;

    int  comicResizeW   = 0;
    int  comicResizeH   = 0;

    int  comicTone      = 0;
    bool comicSkipBlank = true;
    int  comicMarginPercent = 0;
    int  comicSpread    = 0;

    std::string sourceIdentity;

    std::string pageImageDir;
};

inline int outputPageIndex0(int pageIndex0, int pageCount, bool reversePages) {
    if (!reversePages || pageCount <= 0) return pageIndex0;
    return pageCount - 1 - pageIndex0;
}

bool parseOptions(const std::string& json, Options* out, std::string* errMsg);

inline double footerFontPx(const Options& o) {
    return std::max(18.0, o.fontSize * 0.70);
}
inline int footerReservePx(const Options& o) {
    return o.footerProgress ? (int)std::ceil(footerFontPx(o) * 2.4) : 0;
}
inline int footerBottomInsetPx(const Options& o) {
    const double logicalPageHeight = o.pageWidth > o.pageHeight ? 192.0 : 256.0;
    return (int)std::lround(4.0 * o.pageHeight / logicalPageHeight);
}
inline int footerHorizontalInsetPx(const Options& o) {
    const double logicalPageWidth = o.pageWidth > o.pageHeight ? 256.0 : 192.0;
    return (int)std::lround(6.0 * o.pageWidth / logicalPageWidth);
}
inline int footerContentBottomInsetPx(const Options& o) {
    if (!o.footerProgress) return o.marginBottom;
    return std::max(o.marginBottom, footerBottomInsetPx(o) + footerReservePx(o));
}

}

#endif
