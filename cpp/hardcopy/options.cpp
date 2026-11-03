#include "options.h"

#include <nlohmann/json.hpp>

#include "preset_resolve.h"
#include "include/hardcopy/presets.h"

namespace hardcopy {

namespace {

template <typename T>
void readIfPresent(const nlohmann::json& obj, const char* key, T& out) {
    auto it = obj.find(key);
    if (it != obj.end() && !it->is_null()) {
        try { out = it->get<T>(); } catch (...) {}
    }
}

}

bool parseOptions(const std::string& jsonStr, Options* out, std::string* errMsg) {
    if (!out) return false;
    if (jsonStr.empty()) {
        return resolvePresetOverrides(OptionOverrides{}, out, errMsg);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonStr);
    } catch (const std::exception& e) {
        if (errMsg) *errMsg = std::string("json parse: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (errMsg) *errMsg = "options json must be an object";
        return false;
    }

    readIfPresent(j, "fontPath",       out->fontPath);
    readIfPresent(j, "fontPathBold",   out->fontPathBold);
    readIfPresent(j, "fontPathItalic", out->fontPathItalic);
    readIfPresent(j, "fontPathMono",   out->fontPathMono);
    readIfPresent(j, "fontPathSans",   out->fontPathSans);
    readIfPresent(j, "fontPathKai",    out->fontPathKai);
    readIfPresent(j, "fontPathFallback", out->fontPathFallback);
    readIfPresent(j, "pageWidth",      out->pageWidth);
    readIfPresent(j, "pageHeight",     out->pageHeight);
    OptionOverrides ov;
    readIfPresent(j, "marginTopU",      ov.marginTopU);
    readIfPresent(j, "marginRightU",    ov.marginRightU);
    readIfPresent(j, "marginBottomU",   ov.marginBottomU);
    readIfPresent(j, "marginLeftU",     ov.marginLeftU);
    readIfPresent(j, "marginTop",       ov.marginTopPx);
    readIfPresent(j, "marginRight",     ov.marginRightPx);
    readIfPresent(j, "marginBottom",    ov.marginBottomPx);
    readIfPresent(j, "marginLeft",      ov.marginLeftPx);
    readIfPresent(j, "fontSize",       out->fontSize);
    readIfPresent(j, "lineHeightMul",  ov.lineHeightMul);
    readIfPresent(j, "paragraphSpacing", ov.paragraphSpacingPx);
    readIfPresent(j, "firstLineIndentChars", ov.firstLineIndentChars);
    readIfPresent(j, "jpegQuality",    out->jpegQuality);
    readIfPresent(j, "threadCount",    out->threadCount);
    out->embedTextLayer = true;
    readIfPresent(j, "embedBookmarks", out->embedBookmarks);
    readIfPresent(j, "splitLandscape", out->splitLandscape);
    readIfPresent(j, "reversePages",   out->reversePages);
    readIfPresent(j, "bilingual",      out->bilingual);
    readIfPresent(j, "bilingualLayout", out->bilingualLayout);
    if (out->bilingualLayout != "columns") out->bilingualLayout = "stacked";
    readIfPresent(j, "drawRuling",     out->drawRuling);
    readIfPresent(j, "rulingStyle",    out->rulingStyle);
    if (out->rulingStyle != "none" && out->rulingStyle != "dotted"
        && out->rulingStyle != "dashed") out->rulingStyle = "dotted";
    if (out->rulingStyle == "none") out->drawRuling = false;
    readIfPresent(j, "whiteText",      out->whiteText);
    readIfPresent(j, "syntheticBold",  out->syntheticBold);
    readIfPresent(j, "contrastBoost",  out->contrastBoost);
    readIfPresent(j, "imageContrastBoost", out->imageContrastBoost);
    readIfPresent(j, "strokeExpand",   out->strokeExpand);
    readIfPresent(j, "pdfTrim", out->pdfTrim);
    readIfPresent(j, "pdfAutoOrientation", out->pdfAutoOrientation);
    readIfPresent(j, "pdfOcr", out->pdfOcr);
    readIfPresent(j, "pdfIgnoreText", out->pdfIgnoreText);
    readIfPresent(j, "ocrCacheDir", out->ocrCacheDir);
    readIfPresent(j, "pdfHeaderCutPercent", out->pdfHeaderCutPercent);
    readIfPresent(j, "pdfFooterCutPercent", out->pdfFooterCutPercent);
    readIfPresent(j, "pdfMaxOutputPages", out->pdfMaxOutputPages);
    readIfPresent(j, "pdfPreviewSourcePage", out->pdfPreviewSourcePage);
    readIfPresent(j, "pdfWatermarkThreshold", out->pdfWatermarkThreshold);
    readIfPresent(j, "pdfGamma", out->pdfGamma);
    readIfPresent(j, "pdfContrast", out->pdfContrast);
    readIfPresent(j, "pdfStroke", out->pdfStroke);
    readIfPresent(j, "pdfImageContrast", out->pdfImageContrast);
    readIfPresent(j, "pdfFillTone", out->pdfFillTone);
    readIfPresent(j, "pdfColumns", out->pdfColumns);
    std::string pdfPreset;
    readIfPresent(j, "pdfPreset", pdfPreset);
    if (const ReadingPreset* p = findReadingPreset(pdfPreset.c_str())) {
        out->pdfLineHeight = p->lineHeight;
        out->pdfParaSpacingEm = p->paraSpacingEm;
    }
    readIfPresent(j, "pdfLineHeight", out->pdfLineHeight);
    readIfPresent(j, "pdfParaSpacing", out->pdfParaSpacingEm);
    readIfPresent(j, "footerProgress", out->footerProgress);
    readIfPresent(j, "backgroundPath", out->backgroundPath);
    readIfPresent(j, "comicMode",      out->comicMode);
    readIfPresent(j, "preset",         out->preset);
    readIfPresent(j, "ditherAlgo",     out->ditherAlgo);
    readIfPresent(j, "ditherLevels",   out->ditherLevels);
    readIfPresent(j, "comicResizeW",   out->comicResizeW);
    readIfPresent(j, "comicResizeH",   out->comicResizeH);
    readIfPresent(j, "comicTone",      out->comicTone);
    readIfPresent(j, "comicSkipBlank", out->comicSkipBlank);
    readIfPresent(j, "comicMarginPercent", out->comicMarginPercent);
    readIfPresent(j, "comicSpread",    out->comicSpread);
    if (out->comicMarginPercent < 0)  out->comicMarginPercent = 0;
    if (out->comicMarginPercent > 25) out->comicMarginPercent = 25;
    if (out->comicSpread < 0 || out->comicSpread > 2) out->comicSpread = 0;
    if (out->comicTone < -10) out->comicTone = -10;
    if (out->comicTone > 10)  out->comicTone = 10;
    readIfPresent(j, "sourceIdentity", out->sourceIdentity);
    readIfPresent(j, "pageImageDir",   out->pageImageDir);

    auto hsIt = j.find("headingScale");
    if (hsIt != j.end() && hsIt->is_array()) {
        for (size_t i = 0; i < hsIt->size() && i < 6; ++i) {
            try { out->headingScale[i] = (*hsIt)[i].get<double>(); } catch (...) {}
        }
    }

    auto pmIt = j.find("pdfMode");
    if (pmIt != j.end() && pmIt->is_string()) {
        const auto s = pmIt->get<std::string>();
        if (s == "raster")       out->pdfMode = PdfMode::Raster;
        else                     out->pdfMode = PdfMode::Reflow;
    }

    auto pcIt = j.find("pageCodec");
    if (pcIt != j.end() && pcIt->is_string()) {
        const auto s = pcIt->get<std::string>();
        if (s == "jpeg")        out->pageCodec = PageCodec::Jpeg;
        else if (s == "flate")  out->pageCodec = PageCodec::Flate;
        else                    out->pageCodec = PageCodec::Auto;
    }

    auto eeIt = j.find("epubEngine");
    if (eeIt != j.end() && eeIt->is_string()) {
        const auto s = eeIt->get<std::string>();
        if (s == "mupdf")       out->epubEngine = EpubEngine::Mupdf;
        else if (s == "legacy") out->epubEngine = EpubEngine::Legacy;
        else                    out->epubEngine = EpubEngine::Auto;
    }

    auto zcIt = j.find("zhConvert");
    if (zcIt != j.end() && zcIt->is_string()) {
        const auto s = zcIt->get<std::string>();
        if (s == "s2t" || s == "S2T")       out->zhConvert = ZhConvert::S2T;
        else if (s == "t2s" || s == "T2S")  out->zhConvert = ZhConvert::T2S;
        else                                out->zhConvert = ZhConvert::None;
    }

    auto orIt = j.find("orientation");
    if (orIt != j.end() && orIt->is_string()) {
        const auto s = orIt->get<std::string>();
        if (s == "vertical-rtl" || s == "verticalRtl" || s == "vertical") {
            out->orientation = Orientation::VerticalRtl;
        } else {
            out->orientation = Orientation::HorizontalLtr;
        }
    }

    return resolvePresetOverrides(ov, out, errMsg);
}

}
