#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwrite.h>
#endif

#include "hardcopy/api.h"
#include "font_listing.h"
#include "hardcopy/presets.h"
#if HARDCOPY_HAS_TEXT_MODE
#include "../hardcopy/pipeline.h"
#include "../hardcopy/logger.h"
#endif
#if HARDCOPY_HAS_PRINT
#include "../hardcopy/print/print_layout.h"
#endif

namespace {

#if HARDCOPY_HAS_TEXT_MODE
#ifdef _WIN32
std::string wcharToUtf8(const wchar_t* wstr, int len = -1) {
    if (!wstr) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &out[0], n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring utf8ToWchar(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::string getFilePathFromFace(IDWriteFontFace* face) {
    UINT32 fileCount = 0;
    face->GetFiles(&fileCount, nullptr);
    if (fileCount == 0) return {};

    std::vector<IDWriteFontFile*> files(fileCount);
    face->GetFiles(&fileCount, files.data());

    std::string result;
    if (files[0]) {
        const void* refKey = nullptr;
        UINT32 refKeySize = 0;
        files[0]->GetReferenceKey(&refKey, &refKeySize);

        IDWriteFontFileLoader* loader = nullptr;
        files[0]->GetLoader(&loader);
        if (loader) {
            IDWriteLocalFontFileLoader* localLoader = nullptr;
            HRESULT hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                reinterpret_cast<void**>(&localLoader));
            if (SUCCEEDED(hr) && localLoader) {
                UINT32 pathLen = 0;
                localLoader->GetFilePathLengthFromKey(refKey, refKeySize, &pathLen);
                std::wstring wpath(pathLen + 1, L'\0');
                localLoader->GetFilePathFromKey(refKey, refKeySize, &wpath[0], pathLen + 1);
                wpath.resize(pathLen);
                result = wcharToUtf8(wpath.c_str());
                localLoader->Release();
            }
            loader->Release();
        }
    }
    for (auto f : files) if (f) f->Release();
    return result;
}

std::wstring getLocalizedString(IDWriteLocalizedStrings* names, UINT32 idx = 0) {
    if (!names) return {};
    UINT32 len = 0;
    names->GetStringLength(idx, &len);
    std::wstring s(len + 1, L'\0');
    names->GetString(idx, &s[0], len + 1);
    s.resize(len);
    return s;
}

struct DWriteFontMatch {
    std::string filePath;
    IDWriteFont* font = nullptr;
    IDWriteFontFace* face = nullptr;
};

DWriteFontMatch findDWriteFont(IDWriteFactory* factory,
                                IDWriteFontCollection* collection,
                                const std::string& name) {
    std::wstring wName = utf8ToWchar(name);
    DWriteFontMatch result;

    UINT32 familyIdx = 0;
    BOOL exists = FALSE;
    collection->FindFamilyName(wName.c_str(), &familyIdx, &exists);
    if (exists) {
        IDWriteFontFamily* ff = nullptr;
        collection->GetFontFamily(familyIdx, &ff);
        if (ff) {
            IDWriteFont* font = nullptr;
            ff->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_REGULAR,
                DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
            if (font) {
                IDWriteFontFace* face = nullptr;
                font->CreateFontFace(&face);
                if (face) {
                    result.filePath = getFilePathFromFace(face);
                    result.font = font; font->AddRef();
                    result.face = face; face->AddRef();
                    face->Release();
                }
                font->Release();
            }
            ff->Release();
            if (result.face) return result;
        }
    }

    UINT32 familyCount = collection->GetFontFamilyCount();
    for (UINT32 fi = 0; fi < familyCount; ++fi) {
        IDWriteFontFamily* ff = nullptr;
        collection->GetFontFamily(fi, &ff);
        if (!ff) continue;

        UINT32 fontCount = ff->GetFontCount();
        for (UINT32 fonti = 0; fonti < fontCount; ++fonti) {
            IDWriteFont* font = nullptr;
            ff->GetFont(fonti, &font);
            if (!font) continue;

            bool matched = false;

            IDWriteLocalizedStrings* fullNames = nullptr;
            BOOL hasName = FALSE;
            font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_FULL_NAME,
                                          &fullNames, &hasName);
            if (hasName && fullNames) {
                UINT32 cnt = fullNames->GetCount();
                for (UINT32 ni = 0; ni < cnt; ++ni) {
                    if (getLocalizedString(fullNames, ni) == wName) {
                        matched = true; break;
                    }
                }
                fullNames->Release();
            }

            if (!matched) {
                IDWriteLocalizedStrings* psNames = nullptr;
                font->GetInformationalStrings(
                    DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &psNames, &hasName);
                if (hasName && psNames) {
                    UINT32 cnt = psNames->GetCount();
                    for (UINT32 ni = 0; ni < cnt; ++ni) {
                        if (getLocalizedString(psNames, ni) == wName) {
                            matched = true; break;
                        }
                    }
                    psNames->Release();
                }
            }

            if (matched) {
                IDWriteFontFace* face = nullptr;
                font->CreateFontFace(&face);
                if (face) {
                    result.filePath = getFilePathFromFace(face);
                    result.font = font; font->AddRef();
                    result.face = face; face->AddRef();
                    face->Release();
                }
                font->Release();
                ff->Release();
                return result;
            }
            font->Release();
        }
        ff->Release();
    }
    return result;
}

std::string resolveFontFamilyDWrite(const std::string& family) {
    IDWriteFactory* factory = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory));
    if (FAILED(hr) || !factory) return {};

    IDWriteFontCollection* collection = nullptr;
    hr = factory->GetSystemFontCollection(&collection, TRUE);
    if (FAILED(hr) || !collection) { factory->Release(); return {}; }

    auto match = findDWriteFont(factory, collection, family);
    std::string path = match.filePath;

    if (match.face) match.face->Release();
    if (match.font) match.font->Release();
    collection->Release(); factory->Release();
    return path;
}

std::string resolveFontFamilyRegistry(const std::string& family) {
    const char* regPath = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};

    char fontsDir[MAX_PATH] = {0};
    HRESULT hr = SHGetFolderPathA(nullptr, CSIDL_FONTS, nullptr, 0, fontsDir);
    if (FAILED(hr)) {
        char winDir[MAX_PATH];
        GetWindowsDirectoryA(winDir, MAX_PATH);
        std::snprintf(fontsDir, MAX_PATH, "%s\\Fonts", winDir);
    }

    std::string familyLower = family;
    std::transform(familyLower.begin(), familyLower.end(), familyLower.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::string bestPath;
    bool bestIsRegular = false;

    DWORD index = 0;
    char valueName[512];
    BYTE valueData[512];
    DWORD nameLen, dataLen, dataType;

    while (true) {
        nameLen = sizeof(valueName);
        dataLen = sizeof(valueData);
        if (RegEnumValueA(hKey, index++, valueName, &nameLen, nullptr,
                          &dataType, valueData, &dataLen) != ERROR_SUCCESS)
            break;
        if (dataType != REG_SZ) continue;

        std::string entryName(valueName);
        std::string entryLower = entryName;
        std::transform(entryLower.begin(), entryLower.end(), entryLower.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        if (entryLower.find(familyLower) == std::string::npos) continue;

        std::string fileName((const char*)valueData);
        std::string fullPath;
        if (fileName.size() > 2 && fileName[1] == ':') {
            fullPath = fileName;
        } else {
            fullPath = std::string(fontsDir) + "\\" + fileName;
        }

        bool isRegular = (entryLower.find("regular") != std::string::npos) ||
                         (entryLower.find("(truetype)") != std::string::npos &&
                          entryLower.find("bold") == std::string::npos &&
                          entryLower.find("italic") == std::string::npos);

        if (bestPath.empty() || (isRegular && !bestIsRegular)) {
            bestPath = fullPath;
            bestIsRegular = isRegular;
        }
    }
    RegCloseKey(hKey);
    return bestPath;
}

std::vector<uint8_t> extractFontDataGDI(const std::string& faceName) {
    HDC hdc = GetDC(nullptr);
    if (!hdc) return {};

    HFONT hFont = CreateFontA(
        48, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        faceName.c_str());
    if (!hFont) { ReleaseDC(nullptr, hdc); return {}; }

    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

    DWORD size = GetFontData(hdc, 0, 0, nullptr, 0);
    if (size == GDI_ERROR || size == 0) {
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        ReleaseDC(nullptr, hdc);
        return {};
    }

    std::vector<uint8_t> data(size);
    DWORD got = GetFontData(hdc, 0, 0, data.data(), size);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    ReleaseDC(nullptr, hdc);

    if (got == GDI_ERROR) return {};
    std::fprintf(stderr, "extracted %u bytes via GDI GetFontData\n", (unsigned)got);
    return data;
}

std::string findGDIFaceName(const std::string& query) {
    struct Ctx {
        std::string query;
        std::string result;
    };
    Ctx ctx;
    ctx.query = query;
    for (char& c : ctx.query) c = tolower(c);

    HDC hdc = GetDC(nullptr);
    EnumFontFamiliesA(hdc, nullptr,
        [](const LOGFONTA* lf, const TEXTMETRICA*, DWORD, LPARAM lp) -> int {
            auto* ctx = reinterpret_cast<Ctx*>(lp);
            std::string name = lf->lfFaceName;
            std::string lower = name;
            for (char& c : lower) c = tolower(c);
            std::string queryClean, nameClean;
            for (char c : ctx->query) if (c != ' ' && c != '-') queryClean += c;
            for (char c : lower) if (c != ' ' && c != '-') nameClean += c;
            if (nameClean == queryClean || lower.find(ctx->query) != std::string::npos) {
                if (name[0] != '@') {
                    ctx->result = name;
                    return 0;
                }
            }
            return 1;
        }, (LPARAM)&ctx);
    ReleaseDC(nullptr, hdc);
    return ctx.result;
}

std::vector<uint8_t> extractFontDataDWrite(const std::string& family) {
    IDWriteFactory* factory = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory));
    if (FAILED(hr) || !factory) return {};

    IDWriteFontCollection* collection = nullptr;
    hr = factory->GetSystemFontCollection(&collection, TRUE);
    if (FAILED(hr) || !collection) { factory->Release(); return {}; }

    auto match = findDWriteFont(factory, collection, family);
    IDWriteFontFace* face = match.face;
    if (!face) {
        collection->Release(); factory->Release();
        return {};
    }

    struct TableTag { UINT32 tag; };

    static const UINT32 knownTags[] = {
        0x636D6170,
        0x676C7966,
        0x68656164,
        0x68686561,
        0x686D7478,
        0x6C6F6361,
        0x6D617870,
        0x6E616D65,
        0x706F7374,
        0x4F532F32,
        0x63767420,
        0x6670676D,
        0x70726570,
        0x47535542,
        0x47504F53,
        0x47444546,
        0x6B65726E,
        0x76686561,
        0x766D7478,
    };

    struct TableEntry {
        UINT32 tag;
        std::vector<uint8_t> data;
    };
    std::vector<TableEntry> tables;

    for (UINT32 tag : knownTags) {
        const void* tableData = nullptr;
        UINT32 tableSize = 0;
        void* tableCtx = nullptr;
        BOOL tableExists = FALSE;
        hr = face->TryGetFontTable(tag, &tableData, &tableSize, &tableCtx, &tableExists);
        if (SUCCEEDED(hr) && tableExists && tableData && tableSize > 0) {
            TableEntry e;
            e.tag = tag;
            e.data.assign((const uint8_t*)tableData, (const uint8_t*)tableData + tableSize);
            tables.push_back(std::move(e));
            face->ReleaseFontTable(tableCtx);
        }
    }

    face->Release();
    if (match.font) match.font->Release();
    collection->Release(); factory->Release();

    if (tables.empty()) return {};

    UINT16 numTables = (UINT16)tables.size();
    UINT16 entrySelector = 0;
    UINT16 power = 1;
    while (power * 2 <= numTables) { power *= 2; entrySelector++; }
    UINT16 searchRange = power * 16;
    UINT16 rangeShift = numTables * 16 - searchRange;

    UINT32 headerSize = 12 + numTables * 16;
    UINT32 offset = headerSize;
    for (auto& t : tables) {
        offset = (offset + 3) & ~3u;
        offset += (UINT32)t.data.size();
    }

    std::vector<uint8_t> result(offset, 0);
    auto write16 = [&](size_t pos, UINT16 v) {
        result[pos] = (uint8_t)(v >> 8);
        result[pos+1] = (uint8_t)v;
    };
    auto write32 = [&](size_t pos, UINT32 v) {
        result[pos]   = (uint8_t)(v >> 24);
        result[pos+1] = (uint8_t)(v >> 16);
        result[pos+2] = (uint8_t)(v >> 8);
        result[pos+3] = (uint8_t)v;
    };

    write32(0, 0x00010000);
    write16(4, numTables);
    write16(6, searchRange);
    write16(8, entrySelector);
    write16(10, rangeShift);

    UINT32 dataOffset = headerSize;
    for (size_t i = 0; i < tables.size(); ++i) {
        dataOffset = (dataOffset + 3) & ~3u;
        size_t recPos = 12 + i * 16;
        write32(recPos, tables[i].tag);
        UINT32 checksum = 0;
        const uint8_t* d = tables[i].data.data();
        size_t len = tables[i].data.size();
        size_t nLongs = (len + 3) / 4;
        for (size_t j = 0; j < nLongs; ++j) {
            UINT32 val = 0;
            for (int b = 0; b < 4; ++b) {
                val <<= 8;
                size_t idx2 = j * 4 + b;
                if (idx2 < len) val |= d[idx2];
            }
            checksum += val;
        }
        write32(recPos + 4, checksum);
        write32(recPos + 8, dataOffset);
        write32(recPos + 12, (UINT32)len);
        std::memcpy(&result[dataOffset], d, len);
        dataOffset += (UINT32)len;
    }

    std::fprintf(stderr, "extracted %u tables (%zu bytes) from DirectWrite\n",
                 numTables, result.size());
    return result;
}

std::string resolveFontFamily(const std::string& family) {
    std::string path = resolveFontFamilyDWrite(family);
    if (!path.empty()) return path;
    return resolveFontFamilyRegistry(family);
}
#else
std::string resolveFontFamily(const std::string&) {
    return {};
}
#endif
#endif

const char* stageName(int s) {
    switch (s) {
        case INK_STAGE_PARSE:   return "parse";
        case INK_STAGE_LAYOUT:  return "layout";
        case INK_STAGE_RENDER:  return "render";
        case INK_STAGE_PACKAGE: return "package";
        case INK_STAGE_DONE:    return "done";
        case INK_STAGE_OCR:     return "ocr";
    }
    return "?";
}

void onProgress(const char* jobId, int stage, int percent, void*) {
    std::fprintf(stderr, "[%s] %s %d%%\n", jobId, stageName(stage), percent);
}

void onLog(int level, const char* tag, const char* msg, void*) {
    const char* lvl = "I";
    switch (level) {
        case 0: lvl = "D"; break;
        case 1: lvl = "I"; break;
        case 2: lvl = "W"; break;
        case 3: lvl = "E"; break;
    }
    std::fprintf(stderr, "  [%s/%s] %s\n", lvl, tag ? tag : "?", msg ? msg : "");
}

struct Args {
    std::string input;
    std::string output;
    std::string font;
    std::string fontFamily;
    std::string fontFallback;
    std::string fontFallbackFamily = "AR UDJingxihei";
    std::string fontBold;
    std::string fontItalic;
    std::string fontMono;
    std::string fontSans;
    std::string fontSansFamily;
    std::string fontKai;
    std::string fontKaiFamily;
    int    pageWidth   = 1920;
    int    pageHeight  = 2560;
    double marginU       = -1.0;
    double marginLeftU   = -1.0;
    double marginRightU  = -1.0;
    double marginTopU    = -1.0;
    double marginBottomU = -1.0;
    int    marginLeftPx   = -1;
    int    marginRightPx  = -1;
    int    marginTopPx    = -1;
    int    marginBottomPx = -1;
    double lineHeight  = -1.0;
    double paragraphSpacingEm = -1.0;
    double indentChars = -1.0;
    std::string preset;
    std::string epubEngine = "auto";
    std::string pdfMode    = "raster";
    std::string pageCodec  = "auto";
    double fontSize    = 22.0;
    int    quality     = 90;
    int    threads     = 0;
    bool   vertical    = false;
    bool   splitLandscape = false;
    bool   bilingual = false;
    std::string bilingualLayout = "stacked";
    std::string extractDir;
    bool   reversePages   = false;
    bool   whiteText      = false;
    bool   embedBookmarks = true;
    bool   syntheticBold  = true;
    bool   footerProgress = false;
    std::string backgroundPath;
    std::string pageImageDir;
    std::string zhConvert = "none";
    bool   drawRuling     = true;
    std::string rulingStyle = "dotted";
    double contrastBoost  = 0.10;
    double imageContrastBoost = 0.0;
    double strokeExpand   = 0.0;
    bool   pdfTrim = true;
    bool   pdfAutoOrientation = true;
    bool   pdfOcr = false;
    bool   pdfIgnoreText = false;
    int pdfHeaderCutPercent = -1;
    int pdfFooterCutPercent = -1;
    int pdfMaxOutputPages = 0;
    double pdfWatermarkThreshold = 0.0;
    double pdfGamma = 0.0;
    double pdfContrast = 1.0;
    double pdfStroke = 0.0;
    double pdfImageContrast = 0.0;
    double pdfFillTone = 0.0;
    int    pdfColumns = 0;
    std::string pdfPreset;
    double pdfLineHeight = -1.0;
    double pdfParaSpacingEm = -1.0;
#if HARDCOPY_HAS_TEXT_MODE
    bool   comic          = false;
#else
    bool   comic          = true;
#endif
    int    ditherAlgo     = 0;
    int    ditherLevels   = 16;
    int    comicResizeW   = 0;
    int    comicResizeH   = 0;
    int    comicTone      = 0;
    bool   comicSkipBlank = true;
    int    comicMargin    = 0;
    int    comicSpread    = 0;

    bool        printMode   = false;
    std::string paper       = "A4";
    std::string printLayout = "single";
    int         printDpi    = 300;

    std::string annotationPacket;
    std::string annotationAuthor = "Hardcopy CLI";
    double      markerOpacity = 0.38;
};

void usage() {
#if HARDCOPY_HAS_TEXT_MODE
    std::fputs(
        "usage: hardcopy-cli --input <path> --output <path>\n"
        "                   (--font <ttf> | --font-family <name>)\n"
        "                   [--font-bold <p>] [--font-italic <p>] [--font-mono <p>]\n"
        "                   [--font-sans <p>|--font-sans-family <name>]\n"
        "                   [--font-kai <p>|--font-kai-family <name>]\n"
        "                   [--vertical] [--split-landscape] [--reverse-pages]\n"
        "                   [--bilingual] (side-by-side source/translation; forced for .bilingual.json)\n"
        "                   [--bilingual-layout stacked|columns] (default stacked: translation below\n"
        "                    each source block; columns: source left, translation right)\n"
        "                   [--page-width N] [--page-height N]\n"
        "                   [--font-size N] [--quality N] [--threads N]\n"
        "                   [--preset note|publish|web|fresh]\n"
        "                   [--epub-engine auto|mupdf|legacy]\n"
        "                   [--page-codec auto|jpeg|flate]\n"
        "                   [--pdf-reflow|--pdf-raster]\n"
        "                   [--pdf-trim|--pdf-no-trim]\n"
        "                   [--pdf-orientation auto|keep] (auto: landscape sources give landscape pages)\n"
        "                   [--pdf-ocr] (recognize text on pages without a text layer)\n"
        "                   [--pdf-ignore-text] (drop the source text layer; OCR every page with --pdf-ocr)\n"
        "                   [--pdf-header-cut -1..40] [--pdf-footer-cut -1..40]\n"
        "                   [--pdf-max-output-pages N] (0=all)\n"
        "                   [--pdf-gamma 1.5..3] [--pdf-contrast 0.5..2]\n"
        "                   [--pdf-watermark 0|128..254] [--pdf-stroke 0..0.1]\n"
        "                   [--pdf-image-contrast -1|0..1] (pictures; 0 = original tones, -1 = no split)\n"
        "                   [--pdf-fill-tone -1..1] (filled boxes: -1 white, 0 original, 1 darker)\n"
        "                   [--pdf-columns off|auto]\n"
        "                   [--pdf-preset web|publish|fresh|note] (page-image line spacing)\n"
        "                   [--pdf-line-height F] [--pdf-para-spacing F(em)] (0 = source spacing)\n"
        "                   [--line-height F] [--paragraph-spacing F(em)]\n"
        "                   [--indent F(chars)]\n"
        "                   [--margin U] [--margin-left U] [--margin-right U]\n"
        "                   [--margin-top U] [--margin-bottom U]\n"
        "                   [--white-text] [--no-bookmarks]\n"
        "                   [--zh-convert none|s2t|t2s]\n"
        "                   [--page-images <dir>]  (keep each page bitmap as JPEG)\n"
        "                   [--comic] [--dither bayer|fs] [--dither-levels N]\n"
        "                   [--resize WxH] [--comic-tone -10..10] [--comic-keep-blank]\n"
        "                   [--comic-margin 0..25] [--comic-spread off|ltr|rtl]\n"
        "note print mode:   --print --input <image-dir> --output <pdf>\n"
        "                   [--paper A4|A5|A6] [--print-layout single|2up|4up]\n"
        "                   [--print-dpi N] [--print-quality N]\n"
        "annotation mode:   --annotation-packet <hcan> --input <pdf> --output <pdf>\n"
        "                   [--annotation-author <name>] [--marker-opacity F]\n"
        "extract mode:      --extract <dir> --input <txt|md|epub|pdf> [--pdf-ocr]\n"
        "                   (translation source: <dir>/source.json + img_N.*)\n",
        stderr);
#else
    std::fputs(
        "usage: hardcopy-cli --input <path> --output <path>\n"
        "                   [--dither bayer|fs] [--dither-levels N]\n"
        "                   [--resize WxH] [--comic-tone -10..10] [--comic-keep-blank]\n"
        "                   [--comic-margin 0..25] [--comic-spread off|ltr|rtl]\n"
        "note print mode:   --print --input <image-dir> --output <pdf>\n"
        "                   [--paper A4|A5|A6] [--print-layout single|2up|4up]\n"
        "                   [--print-dpi N] [--print-quality N]\n"
        "annotation mode:   --annotation-packet <hcan> --input <pdf> --output <pdf>\n"
        "  (text mode disabled; comic-only build)\n",
        stderr);
#endif
#ifdef _WIN32
    std::fputs(
        "font inspection:  --list-fonts [filter]\n"
        "                   --list-fonts-gdi [filter]\n",
        stderr);
#endif
}

bool parseArgs(int argc, char** argv, Args* out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](std::string* dst) -> bool {
            if (i + 1 >= argc) return false;
            *dst = argv[++i];
            return true;
        };
        auto needI = [&](int* dst) -> bool {
            if (i + 1 >= argc) return false;
            *dst = std::atoi(argv[++i]);
            return true;
        };
        auto needD = [&](double* dst) -> bool {
            if (i + 1 >= argc) return false;
            *dst = std::atof(argv[++i]);
            return true;
        };

        if      (a == "--input")          { if (!need(&out->input))     return false; }
        else if (a == "--output")         { if (!need(&out->output))    return false; }
        else if (a == "--annotation-packet") { if (!need(&out->annotationPacket)) return false; }
        else if (a == "--annotation-author") { if (!need(&out->annotationAuthor)) return false; }
        else if (a == "--marker-opacity") { if (!needD(&out->markerOpacity)) return false; }
#if HARDCOPY_HAS_TEXT_MODE
        else if (a == "--font")           { if (!need(&out->font))      return false; }
        else if (a == "--font-family")    { if (!need(&out->fontFamily)) return false; }
        else if (a == "--font-fallback")  { if (!need(&out->fontFallback)) return false; }
        else if (a == "--font-fallback-family") { if (!need(&out->fontFallbackFamily)) return false; }
        else if (a == "--font-bold")      { if (!need(&out->fontBold))  return false; }
        else if (a == "--font-italic")    { if (!need(&out->fontItalic)) return false; }
        else if (a == "--font-mono")      { if (!need(&out->fontMono))  return false; }
        else if (a == "--font-sans")      { if (!need(&out->fontSans)) return false; }
        else if (a == "--font-sans-family") { if (!need(&out->fontSansFamily)) return false; }
        else if (a == "--font-kai")       { if (!need(&out->fontKai)) return false; }
        else if (a == "--font-kai-family") { if (!need(&out->fontKaiFamily)) return false; }
        else if (a == "--page-width")     { if (!needI(&out->pageWidth))  return false; }
        else if (a == "--page-height")    { if (!needI(&out->pageHeight)) return false; }
        else if (a == "--margin")         { if (!needD(&out->marginU)) return false; }
        else if (a == "--margin-left")    { if (!needD(&out->marginLeftU)) return false; }
        else if (a == "--margin-right")   { if (!needD(&out->marginRightU)) return false; }
        else if (a == "--margin-top")     { if (!needD(&out->marginTopU)) return false; }
        else if (a == "--margin-bottom")  { if (!needD(&out->marginBottomU)) return false; }
        else if (a == "--margin-left-px")   { if (!needI(&out->marginLeftPx)) return false; }
        else if (a == "--margin-right-px")  { if (!needI(&out->marginRightPx)) return false; }
        else if (a == "--margin-top-px")    { if (!needI(&out->marginTopPx)) return false; }
        else if (a == "--margin-bottom-px") { if (!needI(&out->marginBottomPx)) return false; }
        else if (a == "--line-height")    { if (!needD(&out->lineHeight)) return false; }
        else if (a == "--paragraph-spacing") { if (!needD(&out->paragraphSpacingEm)) return false; }
        else if (a == "--indent")         { if (!needD(&out->indentChars)) return false; }
        else if (a == "--preset")         { if (!need(&out->preset)) return false; }
        else if (a == "--epub-engine") {
            std::string val;
            if (!need(&val)) return false;
            if (val != "auto" && val != "mupdf" && val != "legacy") {
                std::fprintf(stderr, "unknown epub engine: %s (use auto|mupdf|legacy)\n", val.c_str());
                return false;
            }
            out->epubEngine = val;
        }
        else if (a == "--page-codec") {
            std::string val;
            if (!need(&val)) return false;
            if (val != "auto" && val != "jpeg" && val != "flate") {
                std::fprintf(stderr, "unknown page codec: %s (use auto|jpeg|flate)\n", val.c_str());
                return false;
            }
            out->pageCodec = val;
        }
        else if (a == "--font-size")      { if (!needD(&out->fontSize))   return false; }
        else if (a == "--quality")        { if (!needI(&out->quality))    return false; }
        else if (a == "--threads")        { if (!needI(&out->threads))    return false; }
        else if (a == "--vertical")       { out->vertical = true; }
        else if (a == "--split-landscape"){ out->splitLandscape = true; }
        else if (a == "--bilingual")      { out->bilingual = true; }
        else if (a == "--bilingual-layout") {
            if (!need(&out->bilingualLayout)) return false;
            if (out->bilingualLayout != "columns" && out->bilingualLayout != "stacked") {
                std::fprintf(stderr, "--bilingual-layout expects stacked or columns\n");
                return false;
            }
        }
        else if (a == "--extract")        { if (!need(&out->extractDir)) return false; }
        else if (a == "--reverse-pages")  { out->reversePages = true; }
        else if (a == "--white-text")     { out->whiteText = true; }
        else if (a == "--no-bookmarks")   { out->embedBookmarks = false; }
        else if (a == "--bold")           { out->syntheticBold = true; }
        else if (a == "--no-bold")        { out->syntheticBold = false; }
        else if (a == "--footer")         { out->footerProgress = true; }
        else if (a == "--background")     { if (!need(&out->backgroundPath)) return false; }
        else if (a == "--page-images")    { if (!need(&out->pageImageDir)) return false; }
        else if (a == "--zh-convert") {
            std::string val;
            if (!need(&val)) return false;
            if (val != "none" && val != "s2t" && val != "t2s") {
                std::fprintf(stderr, "unknown zh-convert: %s (use none|s2t|t2s)\n", val.c_str());
                return false;
            }
            out->zhConvert = val;
        }
        else if (a == "--no-ruling")     { out->drawRuling = false; }
        else if (a == "--ruling-style") {
            if (!need(&out->rulingStyle)) return false;
            if (out->rulingStyle != "none" && out->rulingStyle != "dotted"
                && out->rulingStyle != "dashed") return false;
            out->drawRuling = out->rulingStyle != "none";
        }
        else if (a == "--contrast")       { if (!needD(&out->contrastBoost)) return false; }
        else if (a == "--image-contrast") { if (!needD(&out->imageContrastBoost)) return false; }
        else if (a == "--stroke")         { if (!needD(&out->strokeExpand)) return false; }
        else if (a == "--pdf-header-cut") { if (!needI(&out->pdfHeaderCutPercent)) return false; }
        else if (a == "--pdf-footer-cut") { if (!needI(&out->pdfFooterCutPercent)) return false; }
        else if (a == "--pdf-max-output-pages") { if (!needI(&out->pdfMaxOutputPages)) return false; }
        else if (a == "--pdf-trim")      { out->pdfTrim = true; }
        else if (a == "--pdf-no-trim")   { out->pdfTrim = false; }
        else if (a == "--pdf-ocr")       { out->pdfOcr = true; }
        else if (a == "--pdf-ignore-text") { out->pdfIgnoreText = true; }
        else if (a == "--pdf-orientation" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v != "auto" && v != "keep") { std::fprintf(stderr, "--pdf-orientation expects auto|keep\n"); return false; }
            out->pdfAutoOrientation = v == "auto";
        }
        else if (a == "--pdf-watermark") { if (!needD(&out->pdfWatermarkThreshold)) return false; }
        else if (a == "--pdf-gamma")    { if (!needD(&out->pdfGamma)) return false; }
        else if (a == "--pdf-contrast") { if (!needD(&out->pdfContrast)) return false; }
        else if (a == "--pdf-stroke")   { if (!needD(&out->pdfStroke)) return false; }
        else if (a == "--pdf-image-contrast") { if (!needD(&out->pdfImageContrast)) return false; }
        else if (a == "--pdf-fill-tone") { if (!needD(&out->pdfFillTone)) return false; }
        else if (a == "--pdf-preset") {
            if (!need(&out->pdfPreset)) return false;
            if (!hardcopy::findReadingPreset(out->pdfPreset.c_str())) {
                std::fprintf(stderr, "--pdf-preset expects web|publish|fresh|note\n");
                return false;
            }
        }
        else if (a == "--pdf-line-height")  { if (!needD(&out->pdfLineHeight)) return false; }
        else if (a == "--pdf-para-spacing") { if (!needD(&out->pdfParaSpacingEm)) return false; }
        else if (a == "--pdf-columns") {
            std::string v;
            if (!need(&v)) return false;
            if (v == "auto") out->pdfColumns = 1;
            else if (v == "off") out->pdfColumns = 0;
            else { std::fprintf(stderr, "--pdf-columns expects auto|off\n"); return false; }
        }
        else if (a == "--comic")         { out->comic = true; }
        else if (a == "--pdf-raster")    { out->pdfMode = "raster"; }
        else if (a == "--pdf-reflow")    { out->pdfMode = "reflow"; }
#endif
        else if (a == "--print")         { out->printMode = true; }
        else if (a == "--paper")         { if (!need(&out->paper)) return false; }
        else if (a == "--print-layout")  { if (!need(&out->printLayout)) return false; }
        else if (a == "--print-dpi")     { if (!needI(&out->printDpi)) return false; }
        else if (a == "--print-quality") { if (!needI(&out->quality)) return false; }
        else if (a == "--dither") {
            std::string val;
            if (!need(&val)) return false;
            if (val == "bayer")     out->ditherAlgo = 1;
            else if (val == "fs")   out->ditherAlgo = 2;
            else { std::fprintf(stderr, "unknown dither: %s (use bayer or fs)\n", val.c_str()); return false; }
        }
        else if (a == "--dither-levels") { if (!needI(&out->ditherLevels)) return false; }
        else if (a == "--comic-tone")    { if (!needI(&out->comicTone)) return false; }
        else if (a == "--comic-keep-blank") { out->comicSkipBlank = false; }
        else if (a == "--comic-margin")  { if (!needI(&out->comicMargin)) return false; }
        else if (a == "--comic-spread")  {
            if (i + 1 >= argc) return false;
            const std::string v = argv[++i];
            if (v == "off") out->comicSpread = 0;
            else if (v == "ltr") out->comicSpread = 1;
            else if (v == "rtl") out->comicSpread = 2;
            else return false;
        }
        else if (a == "--resize") {
            std::string val;
            if (!need(&val)) return false;
            auto xpos = val.find('x');
            if (xpos == std::string::npos) xpos = val.find('X');
            if (xpos == std::string::npos) { std::fprintf(stderr, "resize format: WxH\n"); return false; }
            out->comicResizeW = std::atoi(val.substr(0, xpos).c_str());
            out->comicResizeH = std::atoi(val.substr(xpos + 1).c_str());
        }
        else if (a == "-h" || a == "--help") { return false; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
    }
#if HARDCOPY_HAS_TEXT_MODE
    if (!out->preset.empty()) {
        const hardcopy::ReadingPreset* p = hardcopy::findReadingPreset(out->preset.c_str());
        if (!p) {
            std::fprintf(stderr, "unknown preset: %s (use note|publish|web|fresh)\n",
                         out->preset.c_str());
            return false;
        }
        if (out->lineHeight < 0.0)         out->lineHeight = p->lineHeight;
        if (out->paragraphSpacingEm < 0.0) out->paragraphSpacingEm = p->paraSpacingEm;
        if (out->indentChars < 0.0)        out->indentChars = p->indentChars;
        const hardcopy::ResolvedPresetMargins margins =
            hardcopy::resolveReadingPresetMargins(*p, out->pageWidth, out->pageHeight);
        if (out->marginU < 0.0 && out->marginLeftU < 0.0 && out->marginLeftPx < 0)
            out->marginLeftU = hardcopy::horizontalMarginPxToU(
                margins.left, out->pageWidth, out->pageHeight);
        if (out->marginU < 0.0 && out->marginRightU < 0.0 && out->marginRightPx < 0)
            out->marginRightU = hardcopy::horizontalMarginPxToU(
                margins.right, out->pageWidth, out->pageHeight);
        if (out->marginU < 0.0 && out->marginTopU < 0.0 && out->marginTopPx < 0)
            out->marginTopU = hardcopy::verticalMarginPxToU(
                margins.top, out->pageWidth, out->pageHeight);
        if (out->marginU < 0.0 && out->marginBottomU < 0.0 && out->marginBottomPx < 0)
            out->marginBottomU = hardcopy::verticalMarginPxToU(
                margins.bottom, out->pageWidth, out->pageHeight);
        std::fprintf(stderr,
            "preset %s: line-height %.2f, para-spacing %.2fem, indent %.1f, "
            "margins L%.3gu R%.3gu T%.3gu B%.3gu\n",
            p->name, out->lineHeight, out->paragraphSpacingEm, out->indentChars,
            out->marginLeftU, out->marginRightU, out->marginTopU, out->marginBottomU);
    }
    auto defaultHorizontal = [&](double* u, int px) {
        if (*u < 0.0 && px < 0)
            *u = out->marginU >= 0.0 ? out->marginU : hardcopy::kMinHorizontalMarginU;
    };
    auto defaultVertical = [&](double* u, int px) {
        if (*u < 0.0 && px < 0)
            *u = out->marginU >= 0.0 ? out->marginU : hardcopy::kMinTopBottomMarginU;
    };
    defaultHorizontal(&out->marginLeftU, out->marginLeftPx);
    defaultHorizontal(&out->marginRightU, out->marginRightPx);
    defaultVertical(&out->marginTopU, out->marginTopPx);
    defaultVertical(&out->marginBottomU, out->marginBottomPx);
    if (out->font.empty() && !out->fontFamily.empty()) {
        out->font = resolveFontFamily(out->fontFamily);
        if (!out->font.empty()) {
            std::fprintf(stderr, "resolved font family '%s' -> %s\n", out->fontFamily.c_str(), out->font.c_str());
        }
    }
    auto resolveOptional = [](std::string* path, const std::string& family,
                              const char* label) {
        if (!path->empty() || family.empty()) return;
        *path = resolveFontFamily(family);
        if (!path->empty())
            std::fprintf(stderr, "resolved %s family '%s' -> %s\n",
                         label, family.c_str(), path->c_str());
        else
            std::fprintf(stderr, "warning: could not resolve %s family '%s'\n",
                         label, family.c_str());
    };
    resolveOptional(&out->fontFallback, out->fontFallbackFamily, "system fallback");
    if (out->fontFallback.empty()) {
        for (const char* family : {"Microsoft YaHei", "SimSun", "Arial Unicode MS"}) {
            out->fontFallback = resolveFontFamily(family);
            if (!out->fontFallback.empty()) {
                std::fprintf(stderr, "resolved system fallback '%s' -> %s\n",
                             family, out->fontFallback.c_str());
                break;
            }
        }
    }
    resolveOptional(&out->fontSans, out->fontSansFamily, "sans");
    resolveOptional(&out->fontKai, out->fontKaiFamily, "kai");
    if (!out->extractDir.empty()) return !out->input.empty();
    if (out->input.empty() || out->output.empty()) return false;
    if (out->printMode) return true;
    if (out->comic) return true;
    if (out->pdfMode == "raster") return true;
    return !out->font.empty() || !out->fontFamily.empty();
#else
    if (out->input.empty() || out->output.empty()) return false;
    return true;
#endif
}

std::string buildOptionsJson(const Args& a) {
    auto esc = [](const std::string& s) {
        std::string o;
        o.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '"' || c == '\\') o.push_back('\\');
            if (c == '\n') { o += "\\n"; continue; }
            if (c == '\r') { o += "\\r"; continue; }
            o.push_back(c);
        }
        return o;
    };
    std::ostringstream j;
    j.setf(std::ios::fixed);
    j.precision(3);
    j << "{\"fontPath\":\"" << esc(a.font)
      << "\",\"fontPathBold\":\"" << esc(a.fontBold)
      << "\",\"fontPathItalic\":\"" << esc(a.fontItalic)
      << "\",\"fontPathMono\":\"" << esc(a.fontMono)
      << "\",\"fontPathSans\":\"" << esc(a.fontSans)
      << "\",\"fontPathKai\":\"" << esc(a.fontKai)
      << "\",\"fontPathFallback\":\"" << esc(a.fontFallback) << "\""
      << ",\"pageWidth\":" << a.pageWidth
      << ",\"pageHeight\":" << a.pageHeight;
    auto addMargin = [&](const char* uKey, double u, const char* pxKey, int px) {
        if (u >= 0.0) j << ",\"" << uKey << "\":" << u;
        else if (px >= 0) j << ",\"" << pxKey << "\":" << px;
    };
    addMargin("marginTopU", a.marginTopU, "marginTop", a.marginTopPx);
    addMargin("marginRightU", a.marginRightU, "marginRight", a.marginRightPx);
    addMargin("marginBottomU", a.marginBottomU, "marginBottom", a.marginBottomPx);
    addMargin("marginLeftU", a.marginLeftU, "marginLeft", a.marginLeftPx);
    j << ",\"pdfTrim\":" << (a.pdfTrim ? "true" : "false")
      << ",\"pdfAutoOrientation\":" << (a.pdfAutoOrientation ? "true" : "false")
      << ",\"pdfOcr\":" << (a.pdfOcr ? "true" : "false")
      << ",\"pdfIgnoreText\":" << (a.pdfIgnoreText ? "true" : "false")
      << ",\"pdfHeaderCutPercent\":" << a.pdfHeaderCutPercent
      << ",\"pdfFooterCutPercent\":" << a.pdfFooterCutPercent
      << ",\"pdfMaxOutputPages\":" << a.pdfMaxOutputPages
      << ",\"pdfWatermarkThreshold\":" << (int)std::lround(a.pdfWatermarkThreshold)
      << ",\"pdfGamma\":" << a.pdfGamma
      << ",\"pdfContrast\":" << a.pdfContrast
      << ",\"pdfStroke\":" << a.pdfStroke
      << ",\"pdfImageContrast\":" << a.pdfImageContrast
      << ",\"pdfFillTone\":" << a.pdfFillTone
      << ",\"pdfColumns\":" << a.pdfColumns;
    if (!a.pdfPreset.empty()) j << ",\"pdfPreset\":\"" << esc(a.pdfPreset) << "\"";
    if (a.pdfLineHeight >= 0.0) j << ",\"pdfLineHeight\":" << a.pdfLineHeight;
    if (a.pdfParaSpacingEm >= 0.0) j << ",\"pdfParaSpacing\":" << a.pdfParaSpacingEm;
    j << ",\"fontSize\":" << a.fontSize;
    if (a.lineHeight > 0.0)
        j << ",\"lineHeightMul\":" << a.lineHeight;
    if (a.paragraphSpacingEm >= 0.0)
        j << ",\"paragraphSpacing\":" << a.paragraphSpacingEm * a.fontSize;
    if (a.indentChars >= 0.0)
        j << ",\"firstLineIndentChars\":" << a.indentChars;
    j << ",\"jpegQuality\":" << a.quality
      << ",\"threadCount\":" << a.threads
      << ",\"orientation\":\"" << (a.vertical ? "vertical-rtl" : "horizontal") << "\""
      << ",\"splitLandscape\":" << (a.splitLandscape ? "true" : "false")
      << ",\"bilingual\":" << (a.bilingual ? "true" : "false")
      << ",\"bilingualLayout\":\"" << a.bilingualLayout << "\""
      << ",\"reversePages\":" << (a.reversePages ? "true" : "false")
      << ",\"embedTextLayer\":true"
      << ",\"whiteText\":" << (a.whiteText ? "true" : "false")
      << ",\"embedBookmarks\":" << (a.embedBookmarks ? "true" : "false")
      << ",\"syntheticBold\":" << (a.syntheticBold ? "true" : "false")
      << ",\"footerProgress\":" << (a.footerProgress ? "true" : "false");
    if (!a.backgroundPath.empty())
        j << ",\"backgroundPath\":\"" << esc(a.backgroundPath) << "\"";
    if (!a.pageImageDir.empty())
        j << ",\"pageImageDir\":\"" << esc(a.pageImageDir) << "\"";
    j << ",\"drawRuling\":" << (a.drawRuling ? "true" : "false")
      << ",\"rulingStyle\":\"" << a.rulingStyle << "\""
      << ",\"contrastBoost\":" << a.contrastBoost
      << ",\"imageContrastBoost\":" << a.imageContrastBoost
      << ",\"strokeExpand\":" << a.strokeExpand
      << ",\"comicMode\":" << (a.comic ? "true" : "false")
      << ",\"ditherAlgo\":" << a.ditherAlgo
      << ",\"ditherLevels\":" << a.ditherLevels
      << ",\"comicResizeW\":" << a.comicResizeW
      << ",\"comicResizeH\":" << a.comicResizeH
      << ",\"comicTone\":" << a.comicTone
      << ",\"comicSkipBlank\":" << (a.comicSkipBlank ? "true" : "false")
      << ",\"comicMarginPercent\":" << a.comicMargin
      << ",\"comicSpread\":" << a.comicSpread
      << ",\"epubEngine\":\"" << esc(a.epubEngine)
      << "\",\"pdfMode\":\"" << esc(a.pdfMode)
      << "\",\"pageCodec\":\"" << esc(a.pageCodec)
      << "\",\"zhConvert\":\"" << esc(a.zhConvert)
      << "\",\"preset\":\"" << esc(a.preset) << "\"}";
    return j.str();
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Ptrs;
    if (wargv && wargc > 0) {
        utf8Args.reserve(wargc);
        utf8Ptrs.reserve(wargc);
        for (int i = 0; i < wargc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
            utf8Args.push_back(std::move(s));
        }
        for (auto& s : utf8Args) utf8Ptrs.push_back(s.data());
        argc = wargc;
        argv = utf8Ptrs.data();
        LocalFree(wargv);
    }
#endif

#ifdef _WIN32
    if (argc >= 2 && (std::strcmp(argv[1], "--list-fonts") == 0
                   || std::strcmp(argv[1], "--list-fonts-gdi") == 0)) {
        const std::string filter = argc >= 3 ? argv[2] : "";
        if (std::strcmp(argv[1], "--list-fonts-gdi") == 0)
            return printGdiFonts(filter, stdout);
        return printDWriteFonts(filter, stdout);
    }
#endif

    Args args;
    if (!parseArgs(argc, argv, &args)) {
        usage();
        return 2;
    }

    std::fprintf(stderr, "hardcopy-cli %s\n", ink_version());
    std::fprintf(stderr, "input:  %s\n", args.input.c_str());
    std::fprintf(stderr, "output: %s\n", args.output.c_str());
    if (!args.annotationPacket.empty()) {
        std::fprintf(stderr, "mode:   editable annotations (HCAN)\n");
        const ink_status_t status = ink_export_pdf_annotations(
            args.input.c_str(), args.output.c_str(), args.annotationPacket.c_str(),
            args.annotationAuthor.c_str(), static_cast<float>(args.markerOpacity),
            &onLog, nullptr);
        if (status != INK_OK) {
            std::fprintf(stderr, "annotation export failed: %d\n", static_cast<int>(status));
            return static_cast<int>(status);
        }
        std::fprintf(stderr, "annotation PDF written: %s\n", args.output.c_str());
        return 0;
    }
    if (!args.extractDir.empty()) {
        std::fprintf(stderr, "mode:   extract -> %s\n", args.extractDir.c_str());
        const std::string opts = buildOptionsJson(args);
        const ink_status_t st = ink_extract(args.input.c_str(), opts.c_str(),
                                            args.extractDir.c_str(), "cli",
                                            &onProgress, nullptr, &onLog, nullptr);
        if (st != INK_OK) {
            std::fprintf(stderr, "extract failed: %d\n", (int)st);
            return (int)st;
        }
        return 0;
    }
    const int minMarginHPx = hardcopy::horizontalMarginUToPx(
        hardcopy::kMinHorizontalMarginU, args.pageWidth, args.pageHeight);
    const int minMarginVPx = hardcopy::verticalMarginUToPx(
        hardcopy::kMinTopBottomMarginU, args.pageWidth, args.pageHeight);
    auto effectiveHorizontalMarginPx = [&](double u, int legacyPx) {
        const int px = u >= 0.0
            ? hardcopy::horizontalMarginUToPx(u, args.pageWidth, args.pageHeight)
            : legacyPx;
        return std::max(minMarginHPx, px);
    };
    auto effectiveVerticalMarginPx = [&](double u, int legacyPx) {
        const int px = u >= 0.0
            ? hardcopy::verticalMarginUToPx(u, args.pageWidth, args.pageHeight)
            : legacyPx;
        return std::max(minMarginVPx, px);
    };
    std::fprintf(
        stderr,
        "margins: L%.3gu R%.3gu T%.3gu B%.3gu -> L%d R%d T%d B%d render px\n",
        args.marginLeftU, args.marginRightU, args.marginTopU, args.marginBottomU,
        effectiveHorizontalMarginPx(args.marginLeftU, args.marginLeftPx),
        effectiveHorizontalMarginPx(args.marginRightU, args.marginRightPx),
        effectiveVerticalMarginPx(args.marginTopU, args.marginTopPx),
        effectiveVerticalMarginPx(args.marginBottomU, args.marginBottomPx));
    if (args.comic && !args.printMode) std::fprintf(stderr, "mode:   comic\n");

#if HARDCOPY_HAS_PRINT
    if (args.printMode) {
        namespace print = hardcopy::print;
        print::PrintOptions po;
        if (!print::parsePaper(args.paper, &po.paper)) {
            std::fprintf(stderr, "unknown paper: %s (use A4/A5/A6)\n", args.paper.c_str());
            return 2;
        }
        if (!print::parseLayout(args.printLayout, &po.layout)) {
            std::fprintf(stderr, "unknown print layout: %s (use single/2up/4up)\n",
                         args.printLayout.c_str());
            return 2;
        }
        po.dpi = args.printDpi > 0 ? args.printDpi : 300;
        po.jpegQuality = args.quality;

        std::filesystem::path in = std::filesystem::u8path(args.input);
        std::filesystem::path outp = std::filesystem::u8path(args.output);
        std::vector<std::filesystem::path> images;
        if (std::filesystem::is_directory(in)) {
            images = print::collectImages(in);
        } else {
            images.push_back(in);
        }
        std::fprintf(stderr, "mode:   print (%s %s, %d pages @ %ddpi)\n",
                     args.paper.c_str(), args.printLayout.c_str(),
                     (int)images.size(), po.dpi);
        std::string err;
        if (!print::buildPrintPdf(images, po, outp, &err)) {
            std::fprintf(stderr, "print layout failed: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "PROGRESS stage=4 percent=100\n");
        std::fprintf(stderr, "print PDF written: %s\n", args.output.c_str());
        return 0;
    }
#else
    if (args.printMode) {
        std::fprintf(stderr, "print mode disabled in this build\n");
        return 2;
    }
#endif

    if (args.comic) {
        std::string opts = buildOptionsJson(args);
        ink_status_t st = ink_convert(args.input.c_str(),
                                      args.output.c_str(),
                                      opts.c_str(),
                                      "cli",
                                      &onProgress, nullptr,
                                      &onLog,      nullptr);
        if (st != INK_OK) {
            std::fprintf(stderr, "convert failed: %d\n", (int)st);
            return (int)st;
        }
        return 0;
    }

#if !HARDCOPY_HAS_TEXT_MODE
    std::fprintf(stderr, "text mode disabled (comic-only build)\n");
    return 2;
#else

#ifdef _WIN32
    std::vector<uint8_t> memoryFontData;
    if (args.font.empty() && !args.fontFamily.empty()) {
        namespace fs = std::filesystem;
        char appdata[MAX_PATH];
        SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata);
        fs::path cacheDir = fs::path(appdata) / "hardcopy" / "font_cache";
        std::string safeName = args.fontFamily;
        for (char& c : safeName) {
            if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*'
                || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        }
        fs::path cachePath = cacheDir / (safeName + ".ttf");

        if (fs::exists(cachePath)) {
            FILE* f = fopen(cachePath.string().c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    memoryFontData.resize(sz);
                    fread(memoryFontData.data(), 1, sz, f);
                    std::fprintf(stderr, "loaded cached font: %s (%ld bytes)\n",
                                 cachePath.string().c_str(), sz);
                }
                fclose(f);
            }
        }

        if (memoryFontData.empty()) {
            std::string gdiFace = findGDIFaceName(args.fontFamily);
            if (!gdiFace.empty()) {
                std::fprintf(stderr, "found GDI face: %s\n", gdiFace.c_str());
                memoryFontData = extractFontDataGDI(gdiFace);
            }
            if (memoryFontData.empty()) {
                memoryFontData = extractFontDataDWrite(args.fontFamily);
            }
            if (!memoryFontData.empty()) {
                fs::create_directories(cacheDir);
                FILE* f = fopen(cachePath.string().c_str(), "wb");
                if (f) {
                    fwrite(memoryFontData.data(), 1, memoryFontData.size(), f);
                    fclose(f);
                    std::fprintf(stderr, "cached font: %s (%zu bytes)\n",
                                 cachePath.string().c_str(), memoryFontData.size());
                }
            }
        }

        if (memoryFontData.empty()) {
            std::fprintf(stderr, "could not find or extract font for family: %s\n",
                         args.fontFamily.c_str());
            return 2;
        }
        hardcopy::Options opts;
        opts.fontData = std::move(memoryFontData);
        if (!args.fontFallback.empty()) opts.fontPathFallback = args.fontFallback;
        if (!args.fontBold.empty())     opts.fontPathBold = args.fontBold;
        if (!args.fontItalic.empty())   opts.fontPathItalic = args.fontItalic;
        if (!args.fontMono.empty())     opts.fontPathMono = args.fontMono;
        if (!args.fontSans.empty())     opts.fontPathSans = args.fontSans;
        if (!args.fontKai.empty())      opts.fontPathKai = args.fontKai;
        opts.pageWidth = args.pageWidth;
        opts.pageHeight = args.pageHeight;
        const int minH = hardcopy::horizontalMarginUToPx(
            hardcopy::kMinHorizontalMarginU, args.pageWidth, args.pageHeight);
        const int minV = hardcopy::verticalMarginUToPx(
            hardcopy::kMinTopBottomMarginU, args.pageWidth, args.pageHeight);
        auto hPx = [&](double u, int legacyPx) {
            const int px = u >= 0.0
                ? hardcopy::horizontalMarginUToPx(u, args.pageWidth, args.pageHeight)
                : legacyPx;
            return std::max(minH, px);
        };
        auto vPx = [&](double u, int legacyPx) {
            const int px = u >= 0.0
                ? hardcopy::verticalMarginUToPx(u, args.pageWidth, args.pageHeight)
                : legacyPx;
            return std::max(minV, px);
        };
        opts.marginLeft = hPx(args.marginLeftU, args.marginLeftPx);
        opts.marginRight = hPx(args.marginRightU, args.marginRightPx);
        opts.marginTop = vPx(args.marginTopU, args.marginTopPx);
        opts.marginBottom = vPx(args.marginBottomU, args.marginBottomPx);
        opts.fontSize = args.fontSize;
        opts.jpegQuality = args.quality;
        opts.threadCount = args.threads;
        opts.orientation = args.vertical ? hardcopy::Orientation::VerticalRtl
                                         : hardcopy::Orientation::HorizontalLtr;
        opts.splitLandscape = args.splitLandscape;
        opts.bilingual = args.bilingual;
        opts.bilingualLayout = args.bilingualLayout;
        opts.reversePages = args.reversePages;
        opts.embedTextLayer = true;
        opts.whiteText = args.whiteText;
        opts.embedBookmarks = args.embedBookmarks;
        opts.syntheticBold = args.syntheticBold;
        opts.footerProgress = args.footerProgress;
        opts.backgroundPath = args.backgroundPath;
        opts.pageImageDir = args.pageImageDir;
        opts.drawRuling = args.drawRuling;
        opts.rulingStyle = args.rulingStyle;
        opts.contrastBoost = args.contrastBoost;
        opts.imageContrastBoost = args.imageContrastBoost;
        opts.strokeExpand = args.strokeExpand;
        opts.pdfTrim = args.pdfTrim;
        opts.pdfAutoOrientation = args.pdfAutoOrientation;
        opts.pdfOcr = args.pdfOcr;
        opts.pdfIgnoreText = args.pdfIgnoreText;
        opts.pdfHeaderCutPercent = args.pdfHeaderCutPercent;
        opts.pdfFooterCutPercent = args.pdfFooterCutPercent;
        opts.pdfMaxOutputPages = args.pdfMaxOutputPages;
        opts.pdfWatermarkThreshold = (int)std::lround(args.pdfWatermarkThreshold);
        opts.pdfGamma = args.pdfGamma;
        opts.pdfContrast = args.pdfContrast;
        opts.pdfStroke = args.pdfStroke;
        opts.pdfImageContrast = args.pdfImageContrast;
        opts.pdfFillTone = args.pdfFillTone;
        opts.pdfColumns = args.pdfColumns;
        if (const auto* p = hardcopy::findReadingPreset(args.pdfPreset.c_str())) {
            opts.pdfLineHeight = p->lineHeight;
            opts.pdfParaSpacingEm = p->paraSpacingEm;
        }
        if (args.pdfLineHeight >= 0.0) opts.pdfLineHeight = args.pdfLineHeight;
        if (args.pdfParaSpacingEm >= 0.0) opts.pdfParaSpacingEm = args.pdfParaSpacingEm;
        if (args.epubEngine == "mupdf")       opts.epubEngine = hardcopy::EpubEngine::Mupdf;
        else if (args.epubEngine == "legacy") opts.epubEngine = hardcopy::EpubEngine::Legacy;
        if (args.pageCodec == "jpeg")         opts.pageCodec = hardcopy::PageCodec::Jpeg;
        else if (args.pageCodec == "flate")   opts.pageCodec = hardcopy::PageCodec::Flate;
        if (args.zhConvert == "s2t")          opts.zhConvert = hardcopy::ZhConvert::S2T;
        else if (args.zhConvert == "t2s")     opts.zhConvert = hardcopy::ZhConvert::T2S;

        struct StderrLogger : hardcopy::Logger {
            void log(hardcopy::LogLevel lv, const char* tag, const char* msg) override {
                const char* lvl = "I";
                switch (lv) {
                    case hardcopy::LogLevel::Debug: lvl = "D"; break;
                    case hardcopy::LogLevel::Info:  lvl = "I"; break;
                    case hardcopy::LogLevel::Warn:  lvl = "W"; break;
                    case hardcopy::LogLevel::Error: lvl = "E"; break;
                }
                std::fprintf(stderr, "  [%s/%s] %s\n", lvl, tag ? tag : "?", msg ? msg : "");
            }
        } logger;

        ink_status_t st = hardcopy::runPipeline(
            args.input, args.output, opts, "cli",
            &onProgress, nullptr, &logger);
        if (st != INK_OK) {
            std::fprintf(stderr, "convert failed: %d\n", (int)st);
            return (int)st;
        }
        return 0;
    }
#endif

    std::string opts = buildOptionsJson(args);
    ink_status_t st = ink_convert(args.input.c_str(),
                                  args.output.c_str(),
                                  opts.c_str(),
                                  "cli",
                                  &onProgress, nullptr,
                                  &onLog,      nullptr);
    if (st != INK_OK) {
        std::fprintf(stderr, "convert failed: %d\n", (int)st);
        return (int)st;
    }
    return 0;
#endif
}
