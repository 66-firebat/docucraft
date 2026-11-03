#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "imgui_stdlib.h"
#include "companion_server.h"
#include "localsend_client.h"
#include "hardcopy/presets.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static FILE* g_traceFile = nullptr;

static void traceInit() {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string path = std::string(tmp) + "hardcopy-gui-trace.log";
    g_traceFile = fopen(path.c_str(), "w");
    if (g_traceFile) {
        std::fprintf(g_traceFile, "=== hardcopy-gui trace ===\n");
        std::fflush(g_traceFile);
    }
}

static void trace(const char* fmt, ...) {
    if (!g_traceFile) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_traceFile, "[%02d:%02d:%02d.%03d] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_traceFile, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_traceFile);
    std::fflush(g_traceFile);
}

struct ConvertState {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> fontListing{false};
    std::atomic<bool> externalParsing{false};
    std::atomic<int>  stage{0};
    std::atomic<int>  percent{0};
    std::atomic<int>  result{-1};

    std::mutex logMtx;
    std::vector<std::string> logLines;

    void addLog(const std::string& line) {
        std::lock_guard<std::mutex> lk(logMtx);
        logLines.push_back(line);
    }
    void reset() {
        running = false;
        done = false;
        fontListing = false;
        externalParsing = false;
        stage = 0;
        percent = 0;
        result = -1;
        std::lock_guard<std::mutex> lk(logMtx);
        logLines.clear();
    }
};

struct LocalSendState {
    std::atomic<bool> scanning{false};
    std::atomic<bool> sending{false};
    std::mutex mutex;
    std::vector<LocalSendPeer> peers;
    LocalSendPeer bound;
    bool hasBound = false;
    std::string manualIp;
    std::string message;
};

struct GuiParams {
    std::string inputPath;
    std::string outputPath;
    std::string fontFamily         = "FangSong";
    std::string fontSansFamily;
    std::string fontKaiFamily;
    std::string fontPath;
    std::string fontSansPath;
    std::string fontKaiPath;
    bool   listFontsOnly = false;
    int    fontSize       = 18;
    int    pageWidth      = 1920;
    int    pageHeight     = 2560;
    float  marginHorizontalU = 13.4f;
    float  marginVerticalU   = 12.0f;
    int    jpegQuality    = 90;

    int    presetIdx      = 0;
    float  lineHeight     = 1.6f;
    float  lineHeightV    = 1.8f;
    float  paraSpacing    = 0.5f;
    int    indentChars    = 2;

    bool   vertical       = false;
    bool   splitLandscape = false;
    bool   reversePages   = false;
    bool   whiteText      = false;
    bool   embedBookmarks = true;
    bool   syntheticBold  = true;
    bool   drawRuling     = true;
    int    rulingStyleIdx = 1;
    float  imageContrastBoost = 0.0f;
    bool   footerProgress = false;
    std::string backgroundPath;
    int    zhConvertIdx   = 0;
    float  contrastBoost  = 0.10f;
    float  strokeExpand   = 0.0f;
    bool   pdfTrim = true;
    float  pdfWatermarkThreshold = 0.0f;
    int    pdfDarkLevel = 0;
    int    pdfImageLevel = 0;
    int    pdfFillLevel = 0;
    bool   pdfColumns = false;
    bool   pdfOcr = false;
    bool   pdfIgnoreText = false;
    int    pdfSpacingIdx = 0;
    float  pdfContrast = 1.0f;
    float  pdfMarginHU = 13.4f;
    float  pdfMarginVU = 12.0f;
    bool   pdfWhiteText = false;
    bool   pdfSplitLandscape = false;
    int    pdfHeaderCut = -1;
    int    pdfFooterCut = -1;
    bool   pdfRaster      = true;

    bool   useMineru      = false;
    int    mineruBackend  = 0;
    int    mineruMethod   = 0;
    int    mineruLanguage = 0;
    bool   mineruFormula  = true;
    bool   mineruTable    = true;
#if HARDCOPY_HAS_TEXT_MODE
    bool   comic          = false;
#else
    bool   comic          = true;
#endif
    int    ditherAlgo     = 0;
    int    comicTone      = 0;
    bool   comicSkipBlank = true;
    int    comicMargin    = 0;
    int    comicSpread    = 0;
    bool   comicResize    = false;
    int    comicResizeW   = 1920;
    int    comicResizeH   = 2560;

    bool   printMode      = false;
    int    printPaperIdx  = 0;
    int    printLayoutIdx = 0;
    int    printDpi       = 300;
};

static const char* kPrintPapers[]  = { "A4", "A5", "A6" };
static const char* kPrintLayouts[] = { "single", "2up", "4up" };

static const int kPageSizes[][2] = {
    {1440, 1920}, {1920, 2560}, {2400, 3200},
};
static const char* kPageSizeNames[] = {
    "1440x1920", "1920x2560", "2400x3200",
};
static constexpr int kPageSizeCount =
    (int)(sizeof(kPageSizes) / sizeof(kPageSizes[0]));

static constexpr const wchar_t* kMineruExe =
    L"Y:\\envs\\mineru\\Scripts\\mineru.exe";
static const char* kMineruBackendLabels[] = {
    "Pipeline (general)", "VLM auto (local)", "Hybrid auto (local)",
};
static const char* kMineruBackendValues[] = {
    "pipeline", "vlm-auto-engine", "hybrid-auto-engine",
};
static const char* kMineruMethodLabels[] = {
    "Auto", "Text extraction", "OCR",
};
static const char* kMineruMethodValues[] = { "auto", "txt", "ocr" };
static const char* kMineruLanguageLabels[] = {
    "Chinese", "Traditional Chinese", "English", "Japanese", "Korean",
    "Chinese lite", "Chinese server", "Latin", "Arabic", "Cyrillic",
    "East Slavic", "Devanagari", "Tamil", "Telugu", "Georgian",
    "Thai", "Greek",
};
static const char* kMineruLanguageValues[] = {
    "ch", "chinese_cht", "en", "japan", "korean", "ch_lite",
    "ch_server", "latin", "arabic", "cyrillic", "east_slavic",
    "devanagari", "ta", "te", "ka", "th", "el",
};

static int qualityTier(int quality) {
    return std::clamp(((quality + 2) / 5) * 5, 75, 100);
}

static int qualityScaledDimension(int value, int quality) {
    const int q = qualityTier(quality);
    return (value * (q - 50) + 25) / 50;
}

static HWND        g_hWnd = nullptr;
static std::string g_droppedFile;
static NOTIFYICONDATAW g_trayIcon = {};
static constexpr UINT kTrayMessage = WM_APP + 42;
static constexpr UINT kTrayOpen = 40001;
static constexpr UINT kTrayExit = 40002;

static void installTrayIcon(HWND window) {
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = window;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;
    g_trayIcon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(g_trayIcon.szTip, L"Hardcopy Companion · 53421");
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
}

static void removeTrayIcon() {
    if (g_trayIcon.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayIcon = {};
}

static void showMainWindow(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

static std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

static std::string deriveOutputPath(const std::string& inputUtf8, bool comic = false) {
    namespace fs = std::filesystem;
    fs::path p(utf8ToWide(inputUtf8));
    const wchar_t* ext = comic ? L"_hardcopy.cbz" : L"_hardcopy.pdf";
    fs::path out = p.parent_path() / (p.stem().wstring() + ext);
    return wideToUtf8(out.wstring().c_str());
}

static std::wstring inputExtension(const std::string& inputUtf8) {
    std::wstring ext =
        std::filesystem::path(utf8ToWide(inputUtf8)).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return ext;
}

static bool isPdfInput(const std::string& inputUtf8) {
    return inputExtension(inputUtf8) == L".pdf";
}

static bool isEpubInput(const std::string& inputUtf8) {
    return inputExtension(inputUtf8) == L".epub";
}

static bool isTextOnlyInput(const std::string& inputUtf8) {
    const std::wstring ext = inputExtension(inputUtf8);
    return ext == L".txt" || ext == L".md" || ext == L".markdown";
}

static bool isComicOnlyInput(const std::string& inputUtf8) {
    const std::wstring ext = inputExtension(inputUtf8);
    return ext == L".cbz" || ext == L".zip" || ext == L".mobi"
        || ext == L".prc";
}

static bool isMineruSupportedInput(const std::string& inputUtf8) {
    std::wstring ext = std::filesystem::path(utf8ToWide(inputUtf8)).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    static const wchar_t* supported[] = {
        L".pdf", L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff",
        L".webp", L".docx", L".pptx", L".xlsx",
    };
    for (const wchar_t* value : supported)
        if (ext == value) return true;
    return false;
}

static std::string openFileDialog(HWND owner) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
#if HARDCOPY_HAS_TEXT_MODE
    ofn.lpstrFilter = L"All Supported\0*.txt;*.md;*.epub;*.pdf;*.docx;*.pptx;*.xlsx;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp;*.cbz;*.zip;*.mobi;*.prc\0"
                      L"Hardcopy text\0*.txt;*.md;*.epub;*.pdf\0"
                      L"MinerU documents\0*.pdf;*.docx;*.pptx;*.xlsx;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp\0"
                      L"Comic / Manga\0*.cbz;*.zip;*.mobi;*.prc;*.pdf\0All\0*.*\0";
#else
    ofn.lpstrFilter = L"Comic\0*.cbz;*.zip;*.mobi;*.prc;*.pdf\0All\0*.*\0";
#endif
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    BOOL ok = GetOpenFileNameW(&ofn);
    return ok ? wideToUtf8(path) : std::string();
}

static std::string openFontFileDialog(HWND owner) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"Font files\0*.ttf;*.otf;*.ttc\0TrueType\0*.ttf;*.ttc\0"
                      L"OpenType\0*.otf\0All\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    BOOL ok = GetOpenFileNameW(&ofn);
    return ok ? wideToUtf8(path) : std::string();
}

static std::string openImageFileDialog(HWND owner) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg\0All\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    BOOL ok = GetOpenFileNameW(&ofn);
    return ok ? wideToUtf8(path) : std::string();
}

static std::string openFolderDialog(HWND owner) {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select folder with note page images";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    std::string out;
    if (SHGetPathFromIDListW(pidl, path)) out = wideToUtf8(path);
    CoTaskMemFree(pidl);
    return out;
}

static std::string saveFileDialog(HWND owner, bool comic) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = comic
        ? L"Comic Book Archive\0*.cbz\0All\0*.*\0"
        : L"PDF\0*.pdf\0All\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = comic ? L"cbz" : L"pdf";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    BOOL ok = GetSaveFileNameW(&ofn);
    return ok ? wideToUtf8(path) : std::string();
}

static std::wstring locateCliExe() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::filesystem::path selfDir = std::filesystem::path(self).parent_path();

    const std::filesystem::path candidates[] = {
        selfDir / L"hardcopy-cli.exe",
        selfDir / L".." / L"cli" / L"hardcopy-cli.exe",
        selfDir.parent_path() / L"cli" / L"hardcopy-cli.exe",
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return std::filesystem::absolute(p, ec).wstring();
    }
    return L"hardcopy-cli.exe";
}

static std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;

    std::wstring out;
    out.push_back(L'"');
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

static std::wstring buildCommandLine(const std::wstring& exe,
                                     const GuiParams&    p) {
    std::wstring cmd = quoteArg(exe);

    auto add  = [&](const wchar_t* flag) {
        cmd.push_back(L' '); cmd.append(flag);
    };
    auto addS = [&](const wchar_t* flag, const std::string& val) {
        if (val.empty()) return;
        cmd.push_back(L' '); cmd.append(flag);
        cmd.push_back(L' '); cmd.append(quoteArg(utf8ToWide(val)));
    };
    auto addI = [&](const wchar_t* flag, int v) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", v);
        cmd.push_back(L' '); cmd.append(flag);
        cmd.push_back(L' '); cmd.append(buf);
    };
    auto addF = [&](const wchar_t* flag, float v) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.3f", v);
        cmd.push_back(L' '); cmd.append(flag);
        cmd.push_back(L' '); cmd.append(buf);
    };

    if (p.listFontsOnly) {
        add(L"--list-fonts");
        if (!p.fontFamily.empty()) {
            cmd.push_back(L' ');
            cmd.append(quoteArg(utf8ToWide(p.fontFamily)));
        }
        return cmd;
    }

    if (p.printMode) {
        add(L"--print");
        addS(L"--input",         p.inputPath);
        addS(L"--output",        p.outputPath);
        addS(L"--paper",         kPrintPapers[p.printPaperIdx]);
        addS(L"--print-layout",  kPrintLayouts[p.printLayoutIdx]);
        addI(L"--print-dpi",     p.printDpi);
        addI(L"--print-quality", p.jpegQuality);
        return cmd;
    }

    addS(L"--input",                 p.inputPath);
    addS(L"--output",                p.outputPath);
#if HARDCOPY_HAS_TEXT_MODE
    if (!p.fontPath.empty()) addS(L"--font", p.fontPath);
    else                     addS(L"--font-family", p.fontFamily);
    if (!p.fontSansPath.empty()) addS(L"--font-sans", p.fontSansPath);
    else                         addS(L"--font-sans-family", p.fontSansFamily);
    if (!p.fontKaiPath.empty()) addS(L"--font-kai", p.fontKaiPath);
    else                        addS(L"--font-kai-family", p.fontKaiFamily);
    const bool sourcePdfRaster = isPdfInput(p.inputPath) && p.pdfRaster;
    const int outputQuality = p.comic ? std::clamp(p.jpegQuality, 50, 100)
        : sourcePdfRaster ? 95 : qualityTier(p.jpegQuality);
    const float renderScale = sourcePdfRaster
        ? 1.0f : (float)(outputQuality - 50) / 50.0f;
    addF(L"--font-size",             (float)(p.fontSize * 2) * renderScale);
    const int outputPageWidth = sourcePdfRaster
        ? p.pageWidth : qualityScaledDimension(p.pageWidth, p.jpegQuality);
    const int outputPageHeight = sourcePdfRaster
        ? p.pageHeight : qualityScaledDimension(p.pageHeight, p.jpegQuality);
    addI(L"--page-width",            outputPageWidth);
    addI(L"--page-height",           outputPageHeight);
    bool notePreset = false;
    if (p.presetIdx > 0 && p.presetIdx <= hardcopy::kReadingPresetCount) {
        const hardcopy::ReadingPreset& selected =
            hardcopy::kReadingPresets[p.presetIdx - 1];
        notePreset = std::strcmp(selected.name, "note") == 0;
    }
    if (sourcePdfRaster) {
        addF(L"--margin-left", p.pdfMarginHU);
        addF(L"--margin-right", p.pdfMarginHU);
        addF(L"--margin-top", p.pdfMarginVU);
        addF(L"--margin-bottom", p.pdfMarginVU);
    } else if (notePreset) {
        addS(L"--preset", "note");
    } else {
        addF(L"--margin-left", p.marginHorizontalU);
        addF(L"--margin-right", p.marginHorizontalU);
        addF(L"--margin-top", p.marginVerticalU);
        addF(L"--margin-bottom", p.marginVerticalU);
    }
    addF(L"--line-height",        p.vertical ? p.lineHeightV : p.lineHeight);
    addF(L"--paragraph-spacing",  (p.drawRuling && !p.vertical) ? 0.0f : p.paraSpacing);
    addF(L"--indent", (float)p.indentChars);
    addI(L"--quality",               outputQuality);
    if (p.vertical)        add(L"--vertical");
    if (sourcePdfRaster ? p.pdfSplitLandscape : (p.splitLandscape && !p.vertical))
        add(L"--split-landscape");
    if (p.reversePages)    add(L"--reverse-pages");
    if (sourcePdfRaster ? p.pdfWhiteText : p.whiteText) add(L"--white-text");
    if (!p.embedBookmarks) add(L"--no-bookmarks");
    if (p.syntheticBold)   add(L"--bold");
    if (!p.drawRuling)     add(L"--no-ruling");
    else if (p.rulingStyleIdx == 2) addS(L"--ruling-style", "dashed");
    const bool textLayout = !sourcePdfRaster && !p.comic;
    if (textLayout && p.footerProgress) add(L"--footer");
    if (textLayout) addS(L"--background", p.backgroundPath);
    if (textLayout && p.zhConvertIdx == 1) addS(L"--zh-convert", "s2t");
    if (textLayout && p.zhConvertIdx == 2) addS(L"--zh-convert", "t2s");
    if (textLayout && p.imageContrastBoost > 0.0f) addF(L"--image-contrast", p.imageContrastBoost);
    addI(L"--pdf-header-cut", p.pdfHeaderCut);
    addI(L"--pdf-footer-cut", p.pdfFooterCut);
    addF(L"--contrast", p.contrastBoost);
    addF(L"--stroke", p.strokeExpand);
    if (p.pdfTrim) add(L"--pdf-trim"); else add(L"--pdf-no-trim");
    addF(L"--pdf-watermark", p.pdfWatermarkThreshold);
    addF(L"--pdf-gamma", 1.5f + 0.15f * p.pdfDarkLevel);
    addF(L"--pdf-stroke", 0.01f * p.pdfDarkLevel);
    addF(L"--pdf-image-contrast", p.pdfImageLevel / 20.0f);
    addF(L"--pdf-fill-tone", p.pdfFillLevel / 10.0f);
    if (p.pdfColumns)      addS(L"--pdf-columns", "auto");
    if (p.pdfOcr)          add(L"--pdf-ocr");
    if (p.pdfIgnoreText)   add(L"--pdf-ignore-text");
    {
        static const char* const kPdfSpacing[] = {"", "web", "publish", "fresh"};
        if (p.pdfSpacingIdx > 0 && p.pdfSpacingIdx < 4)
            addS(L"--pdf-preset", kPdfSpacing[p.pdfSpacingIdx]);
    }
    addF(L"--pdf-contrast", p.pdfContrast);
    if (p.pdfRaster)       add(L"--pdf-raster");
    else                   add(L"--pdf-reflow");
    if (p.comic)           add(L"--comic");
#endif
    if (p.comic && p.ditherAlgo == 1) addS(L"--dither", "bayer");
    if (p.comic && p.ditherAlgo == 2) addS(L"--dither", "fs");
    if (p.comic && p.comicTone != 0) addS(L"--comic-tone", std::to_string(p.comicTone));
    if (p.comic && !p.comicSkipBlank) add(L"--comic-keep-blank");
    if (p.comic && p.comicMargin > 0) addS(L"--comic-margin", std::to_string(p.comicMargin));
    if (p.comic && p.comicSpread == 1) addS(L"--comic-spread", "ltr");
    if (p.comic && p.comicSpread == 2) addS(L"--comic-spread", "rtl");
    if (p.comic && p.comicResize) {
        std::string rz = std::to_string(p.comicResizeW) + "x" + std::to_string(p.comicResizeH);
        addS(L"--resize", rz);
    }

    return cmd;
}

static int stageFromName(const std::string& name) {
    if (name == "parse")   return 0;
    if (name == "layout")  return 1;
    if (name == "render")  return 2;
    if (name == "package") return 3;
    if (name == "done")    return 4;
    return -1;
}

static bool parseProgress(const std::string& line, ConvertState* state) {
    if (line.empty() || line[0] != '[') return false;
    auto rb = line.find(']');
    if (rb == std::string::npos) return false;
    size_t i = rb + 1;
    while (i < line.size() && line[i] == ' ') ++i;
    auto sp = line.find(' ', i);
    if (sp == std::string::npos) return false;
    std::string stage = line.substr(i, sp - i);
    int s = stageFromName(stage);
    if (s < 0) return false;
    int pct = std::atoi(line.c_str() + sp + 1);
    state->stage.store(s);
    state->percent.store(pct);
    return true;
}

static std::wstring buildMineruCommandLine(const GuiParams& params,
                                            const std::filesystem::path& outputDir) {
    std::wstring cmd = quoteArg(kMineruExe);
    auto add = [&](const wchar_t* flag, const std::wstring& value) {
        cmd.push_back(L' '); cmd.append(flag);
        cmd.push_back(L' '); cmd.append(quoteArg(value));
    };
    add(L"-p", utf8ToWide(params.inputPath));
    add(L"-o", outputDir.wstring());
    add(L"-b", utf8ToWide(kMineruBackendValues[
        std::clamp(params.mineruBackend, 0, 2)]));
    add(L"-m", utf8ToWide(kMineruMethodValues[
        std::clamp(params.mineruMethod, 0, 2)]));
    add(L"-l", utf8ToWide(kMineruLanguageValues[
        std::clamp(params.mineruLanguage, 0,
                   (int)(sizeof(kMineruLanguageValues) /
                         sizeof(kMineruLanguageValues[0])) - 1)]));
    add(L"-f", params.mineruFormula ? L"true" : L"false");
    add(L"-t", params.mineruTable ? L"true" : L"false");
    return cmd;
}

static int runCapturedProcess(ConvertState* state, const std::wstring& cmdLine,
                              const char* label, bool hardcopyProgress) {
    state->addLog(std::string(label) + " cmdline: " +
                  wideToUtf8(cmdLine.c_str()));
    trace("%s cmdline=%s", label, wideToUtf8(cmdLine.c_str()).c_str());

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        state->addLog(std::string("ERROR: ") + label + " CreatePipe failed");
        return -1;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmdBuf = cmdLine;
    PROCESS_INFORMATION pi = {};
    BOOL created = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                  &si, &pi);
    CloseHandle(writePipe);
    if (!created) {
        DWORD err = GetLastError();
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "ERROR: %s CreateProcessW failed (err=0x%lx)",
                      label, (unsigned long)err);
        state->addLog(buf);
        CloseHandle(readPipe);
        return -1;
    }

    std::string buffer;
    char chunk[1024];
    DWORD got = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        buffer.append(chunk, got);
        for (;;) {
            auto nl = buffer.find('\n');
            if (nl == std::string::npos) break;
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            trace("%s> %s", label, line.c_str());
            if (hardcopyProgress) parseProgress(line, state);
            state->addLog(std::string(label) + "> " + line);
        }
    }
    if (!buffer.empty()) {
        if (hardcopyProgress) parseProgress(buffer, state);
        state->addLog(std::string(label) + "> " + buffer);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    trace("%s exit=%lu", label, (unsigned long)exitCode);
    return (int)exitCode;
}

static std::filesystem::path findMineruMarkdown(
        const std::filesystem::path& root,
        const std::filesystem::path& inputPath) {
    std::filesystem::path fallback;
    std::error_code ec;
    const std::wstring expected = inputPath.stem().wstring() + L".md";
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != L".md") continue;
        if (it->path().filename() == expected) return it->path();
        if (fallback.empty()) fallback = it->path();
    }
    return fallback;
}

#if HARDCOPY_HAS_TEXT_MODE
static void applyReadingPreset(GuiParams* p, const hardcopy::ReadingPreset& rp) {
    if (p->vertical)
        p->lineHeightV = std::min(2.0f, std::max(1.5f, rp.lineHeight));
    else
        p->lineHeight = rp.lineHeight;
    p->paraSpacing = rp.paraSpacingEm;
    p->indentChars = (int)rp.indentChars;
    p->marginHorizontalU = std::max(
        (float)hardcopy::kMinHorizontalMarginU, (rp.mL + rp.mR) * 0.5f);
    p->marginVerticalU = std::max(
        (float)hardcopy::kMinTopBottomMarginU, (rp.mT + rp.mB) * 0.5f);
}
#endif

static std::mutex g_pcSettingsMutex;
static GuiParams  g_pcSettings;
static bool       g_usePcSettings = false;

static std::wstring pcSettingsCommand(const CompanionServer::PhoneJob& job) {
    GuiParams p;
    {
        std::lock_guard<std::mutex> lock(g_pcSettingsMutex);
        p = g_pcSettings;
    }
    p.inputPath = job.input;
    p.outputPath = job.output;
    p.comic = job.comic;
    p.pdfRaster = job.pdfRaster;
    p.printMode = false;
    p.listFontsOnly = false;
    p.useMineru = false;
    std::wstring cmd = buildCommandLine(job.cli, p);
    wchar_t buf[96];
    swprintf(buf, 96, L" --page-width %d --page-height %d", job.pageWidth, job.pageHeight);
    cmd += buf;
    if (!job.comic && !job.pdfRaster) {
        const float px = p.fontSize * 2.0f * job.pageWidth / (float)std::max(1, p.pageWidth);
        swprintf(buf, 96, L" --font-size %.3f", px);
        cmd += buf;
    }
    return cmd;
}

static void runCliSubprocess(ConvertState* state, GuiParams params) {
    trace("runCliSubprocess: enter input='%s' output='%s'",
          params.inputPath.c_str(), params.outputPath.c_str());

    namespace fs = std::filesystem;
    fs::path mineruTemp;
    if (params.useMineru && !params.comic && !params.printMode &&
        !params.listFontsOnly) {
        auto failMineru = [&](const std::string& message, int code) {
            state->addLog("ERROR: " + message);
            if (!mineruTemp.empty())
                state->addLog("MinerU files kept at: " +
                              wideToUtf8(mineruTemp.wstring().c_str()));
            state->externalParsing.store(false);
            state->result.store(code);
            state->running.store(false);
            state->done.store(true);
        };
        if (!isMineruSupportedInput(params.inputPath)) {
            failMineru("MinerU supports PDF, images, DOCX, PPTX and XLSX", -2);
            return;
        }
        std::error_code ec;
        if (!fs::exists(fs::path(kMineruExe), ec)) {
            failMineru("local MinerU not found at Y:\\envs\\mineru\\Scripts\\mineru.exe", -3);
            return;
        }
        wchar_t tempBase[MAX_PATH] = {};
        if (!GetTempPathW(MAX_PATH, tempBase)) {
            failMineru("GetTempPathW failed", -4);
            return;
        }
        mineruTemp = fs::path(tempBase) /
            (L"hardcopy-mineru-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(mineruTemp, ec);
        if (ec) {
            failMineru("cannot create temporary output directory", -5);
            return;
        }

        state->externalParsing.store(true);
        state->stage.store(0);
        state->percent.store(0);
        state->addLog("MinerU parsing: " + params.inputPath);
        const int mineruExit = runCapturedProcess(
            state, buildMineruCommandLine(params, mineruTemp), "mineru", false);
        state->externalParsing.store(false);
        if (mineruExit != 0) {
            failMineru("MinerU exited with code " + std::to_string(mineruExit),
                       mineruExit);
            return;
        }

        const fs::path markdown = findMineruMarkdown(
            mineruTemp, fs::path(utf8ToWide(params.inputPath)));
        if (markdown.empty()) {
            failMineru("MinerU completed but produced no Markdown", -6);
            return;
        }
        params.inputPath = wideToUtf8(markdown.wstring().c_str());
        state->addLog("MinerU Markdown: " + params.inputPath);
        state->stage.store(0);
        state->percent.store(0);
    }

    std::wstring exe = locateCliExe();
    std::string exeUtf8 = wideToUtf8(exe.c_str());
    state->addLog("cli: " + exeUtf8);
    trace("runCliSubprocess: cli='%s'", exeUtf8.c_str());

    std::wstring cmdLine = buildCommandLine(exe, params);
    std::string cmdUtf8 = wideToUtf8(cmdLine.c_str());
    state->addLog("cmdline: " + cmdUtf8);
    trace("runCliSubprocess: cmdline=%s", cmdUtf8.c_str());

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        state->addLog("ERROR: CreatePipe failed");
        state->result.store(-1);
        state->running.store(false);
        state->done.store(true);
        if (!mineruTemp.empty())
            state->addLog("MinerU files kept at: " +
                          wideToUtf8(mineruTemp.wstring().c_str()));
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = writePipe;
    si.hStdError   = writePipe;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmdBuf = cmdLine;
    PROCESS_INFORMATION pi = {};
    BOOL created = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si, &pi);

    CloseHandle(writePipe);

    if (!created) {
        DWORD err = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "ERROR: CreateProcessW failed (err=0x%lx)",
                      (unsigned long)err);
        state->addLog(buf);
        CloseHandle(readPipe);
        state->result.store(-1);
        state->running.store(false);
        state->done.store(true);
        if (!mineruTemp.empty())
            state->addLog("MinerU files kept at: " +
                          wideToUtf8(mineruTemp.wstring().c_str()));
        return;
    }

    std::string buffer;
    char        chunk[1024];
    DWORD       got = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        buffer.append(chunk, got);
        for (;;) {
            auto nl = buffer.find('\n');
            if (nl == std::string::npos) break;
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            trace("cli> %s", line.c_str());
            parseProgress(line, state);
            state->addLog(line);
        }
    }
    if (!buffer.empty()) {
        trace("cli> (residual) %s", buffer.c_str());
        parseProgress(buffer, state);
        state->addLog(buffer);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    trace("runCliSubprocess: cli exit=%lu", (unsigned long)exitCode);
    state->result.store(static_cast<int>(exitCode));
    state->running.store(false);
    state->done.store(true);

    if (exitCode == 0 && params.listFontsOnly) {
        state->stage.store(4);
        state->percent.store(100);
        state->addLog("font listing complete");
    } else if (exitCode == 0) {
        state->stage.store(4);
        state->percent.store(100);
        state->addLog("done — " + params.outputPath);
    } else {
        state->addLog("cli exited with code " + std::to_string(exitCode));
    }

    if (!mineruTemp.empty()) {
        if (exitCode == 0) {
            std::error_code cleanupError;
            std::filesystem::remove_all(mineruTemp, cleanupError);
            if (cleanupError)
                state->addLog("warning: could not remove MinerU temporary files");
        } else {
            state->addLog("MinerU files kept at: " +
                          wideToUtf8(mineruTemp.wstring().c_str()));
        }
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case kTrayMessage:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            showMainWindow(hWnd);
            return 0;
        }
        if (lParam == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kTrayOpen, L"Open Hardcopy");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit Companion");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hWnd, nullptr);
            DestroyMenu(menu);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == kTrayOpen) {
            showMainWindow(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == kTrayExit) {
            removeTrayIcon();
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    case WM_SIZE:
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t wpath[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, wpath, MAX_PATH))
            g_droppedFile = wideToUtf8(wpath);
        DragFinish(hDrop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static const char* stageName(int s) {
    switch (s) {
        case 0: return "parse";
        case 1: return "layout";
        case 2: return "render";
        case 3: return "package";
        case 4: return "done";
    }
    return "?";
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    const wchar_t* cmd = GetCommandLineW();
    if (cmd && wcsstr(cmd, L"--setup-firewall")) {
        OleInitialize(nullptr);
        const bool ok = CompanionServer::installFirewallRules();
        OleUninitialize();
        return ok ? 0 : 1;
    }
    traceInit();
    float dpiScale = 1.0f;
    {
        HDC screenDC = GetDC(nullptr);
        int dpiX = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
        dpiScale = dpiX / 96.0f;
        if (dpiScale < 1.0f) dpiScale = 1.0f;
    }
    trace("WinMain: dpiScale=%.2f", dpiScale);

    HRESULT oleHr = OleInitialize(nullptr);
    trace("WinMain: OleInitialize hr=0x%lx", (unsigned long)oleHr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HardcopyGUI";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"HardcopyGUI",
        L"Hardcopy — 文档转换",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)(1080 * dpiScale), (int)(975 * dpiScale),
        nullptr, nullptr, hInstance, nullptr);
    trace("WinMain: window created hWnd=%p", (void*)g_hWnd);

    HDC hDC = GetDC(g_hWnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(hDC, &pfd);
    SetPixelFormat(hDC, pf, &pfd);
    HGLRC hRC = wglCreateContext(hDC);
    wglMakeCurrent(hDC, hRC);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    installTrayIcon(g_hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding  = 4.0f;
    style.GrabRounding   = 4.0f;
    style.WindowRounding = 0.0f;
    style.FramePadding   = ImVec2(6, 4);
    style.ItemSpacing    = ImVec2(8, 6);

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplOpenGL3_Init();

    {
        namespace fs = std::filesystem;
        const char* candidates[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\simhei.ttf",
            "C:\\Windows\\Fonts\\simsun.ttc",
        };
        for (auto fp : candidates) {
            if (fs::exists(fp)) {
                io.Fonts->AddFontFromFileTTF(fp, 20.0f * dpiScale, nullptr,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                break;
            }
        }
    }
    trace("WinMain: entering main loop");

    GuiParams    params;
#if HARDCOPY_HAS_TEXT_MODE
    for (int i = 0; i < hardcopy::kReadingPresetCount; ++i) {
        if (std::strcmp(hardcopy::kReadingPresets[i].name, "publish") == 0) {
            params.presetIdx = i + 1;
            applyReadingPreset(&params, hardcopy::kReadingPresets[i]);
            break;
        }
    }
#endif
    ConvertState convertState;
    std::thread  convertThread;
    LocalSendState localSendState;
    CompanionServer companionServer;
    std::thread scanThread;
    std::thread sendThread;
    localSendState.hasBound = loadBoundLocalSendPeer(localSendState.bound);
    companionServer.start();
    int          pageSizeIdx  = 1;
    bool         pageLandscape = false;

    bool appRunning = true;
    int  frameNum   = 0;
    while (appRunning) {
        if (frameNum < 3) trace("frame %d: pump messages", frameNum);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) appRunning = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!appRunning) break;
        if (!IsWindowVisible(g_hWnd)) {
            Sleep(100);
            continue;
        }

        if (!g_droppedFile.empty()) {
            params.inputPath = g_droppedFile;
            if (params.outputPath.empty())
                params.outputPath = deriveOutputPath(g_droppedFile, params.comic);
            g_droppedFile.clear();
        }

        if (frameNum < 3) trace("frame %d: NewFrame", frameNum);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (g_usePcSettings) {
            std::lock_guard<std::mutex> lock(g_pcSettingsMutex);
            g_pcSettings = params;
        }
        if (frameNum < 3) trace("frame %d: NewFrame done", frameNum);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("Hardcopy");
        ImGui::Separator();

        static std::wstring lastConversionExtension;
        const std::wstring currentConversionExtension =
            inputExtension(params.inputPath);
        if (!params.printMode && currentConversionExtension != lastConversionExtension) {
            if (isPdfInput(params.inputPath)) {
                params.comic = false;
                params.pdfRaster = true;
            } else if (isEpubInput(params.inputPath)
                       || isTextOnlyInput(params.inputPath)) {
                params.comic = false;
            } else if (isComicOnlyInput(params.inputPath)) {
                params.comic = true;
            }
            if (!params.inputPath.empty())
                params.outputPath = deriveOutputPath(params.inputPath, params.comic);
            lastConversionExtension = currentConversionExtension;
        }

        {
            bool wasComic = params.comic;
            bool wasPrint = params.printMode;
            bool wasPdfRaster = params.pdfRaster;
#if HARDCOPY_HAS_TEXT_MODE
            if (isPdfInput(params.inputPath)) {
                if (ImGui::RadioButton("Text reflow PDF",
                                       !params.comic && !params.printMode
                                       && !params.pdfRaster)) {
                    params.comic = false;
                    params.printMode = false;
                    params.pdfRaster = false;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Original-page image PDF",
                                       !params.comic && !params.printMode
                                       && params.pdfRaster)) {
                    params.comic = false;
                    params.printMode = false;
                    params.pdfRaster = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Comic / Manga CBZ",
                                       params.comic && !params.printMode)) {
                    params.comic = true;
                    params.printMode = false;
                }
            } else if (isEpubInput(params.inputPath)) {
                if (ImGui::RadioButton("Text reflow PDF",
                                       !params.comic && !params.printMode)) {
                    params.comic = false;
                    params.printMode = false;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Comic / Manga CBZ",
                                       params.comic && !params.printMode)) {
                    params.comic = true;
                    params.printMode = false;
                }
            } else if (isTextOnlyInput(params.inputPath)) {
                params.comic = false;
                ImGui::TextDisabled("Text reflow PDF");
            } else if (isComicOnlyInput(params.inputPath)) {
                params.comic = true;
                ImGui::TextDisabled("Comic / Manga CBZ");
            } else {
                if (ImGui::RadioButton("Text reflow PDF",
                                       !params.comic && !params.printMode)) {
                    params.comic = false;
                    params.printMode = false;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Comic / Manga CBZ",
                                       params.comic && !params.printMode)) {
                    params.comic = true;
                    params.printMode = false;
                }
            }
#else
            if (ImGui::RadioButton("Comic / Manga", !params.printMode))
                params.printMode = false;
#endif
            ImGui::SameLine();
            if (ImGui::RadioButton("Note Print", params.printMode))
                params.printMode = true;
            if (params.comic != wasComic || params.printMode != wasPrint
                || params.pdfRaster != wasPdfRaster) {
                if (!params.inputPath.empty() && !params.printMode)
                    params.outputPath = deriveOutputPath(params.inputPath, params.comic);
                else
                    params.outputPath.clear();
            }
            if (isPdfInput(params.inputPath) && !params.printMode) {
                if (params.comic)
                    ImGui::TextDisabled("Extract embedded manga images and package them as CBZ.");
                else if (params.pdfRaster)
                    ImGui::TextDisabled("Render every source page as an image PDF and preserve its text search layer.");
                else
                    ImGui::TextDisabled("Reflow text and inline images; fold scanned page images under a text layer.");
            }
        }
        ImGui::Separator();

        {
            float btnW = ImGui::CalcTextSize("Browse").x + style.FramePadding.x * 2;
            float inputW = ImGui::GetContentRegionAvail().x - btnW - style.ItemSpacing.x;

            ImGui::Text("Input");
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputText("##in", &params.inputPath);
            ImGui::SameLine();
            if (ImGui::Button("Browse##in")) {
                auto p = params.printMode ? openFolderDialog(g_hWnd)
                                          : openFileDialog(g_hWnd);
                if (!p.empty()) {
                    params.inputPath = p;
                    if (params.printMode) {
                        namespace fs = std::filesystem;
                        fs::path dir(utf8ToWide(p));
                        fs::path out = dir / (dir.filename().wstring() + L"_print.pdf");
                        params.outputPath = wideToUtf8(out.wstring().c_str());
                    } else {
                        params.outputPath = deriveOutputPath(p, params.comic);
                    }
                }
            }

            ImGui::Text("Output");
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputText("##out", &params.outputPath);
            ImGui::SameLine();
            if (ImGui::Button("Browse##out")) {
                auto p = saveFileDialog(g_hWnd, params.comic);
                if (!p.empty()) params.outputPath = p;
            }
        }
        ImGui::Separator();

        if (params.printMode) {
            if (ImGui::CollapsingHeader("Note Print", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* paperNames[]  = { "A4", "A5", "A6" };
                const char* layoutNames[] = { "Single page", "2-up (A4 landscape)", "4-up grid (A4)" };
                ImGui::Combo("Paper",  &params.printPaperIdx,  paperNames, 3);
                ImGui::Combo("Layout", &params.printLayoutIdx, layoutNames, 3);
                if (params.printLayoutIdx > 0)
                    ImGui::TextDisabled("2-up / 4-up always compose on A4 paper");
                ImGui::SliderInt("DPI",       &params.printDpi,    150, 600);
                ImGui::SliderInt("JPEG Q##p", &params.jpegQuality, 50, 100);
            }
        } else
#if HARDCOPY_HAS_TEXT_MODE
        if (!params.comic) {
            params.pageWidth  = pageLandscape ? kPageSizes[pageSizeIdx][1]
                                              : kPageSizes[pageSizeIdx][0];
            params.pageHeight = pageLandscape ? kPageSizes[pageSizeIdx][0]
                                              : kPageSizes[pageSizeIdx][1];
            if (ImGui::CollapsingHeader("External parser", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Parse with local MinerU", &params.useMineru);
                ImGui::TextDisabled("Y:\\envs\\mineru\\Scripts\\mineru.exe");
                if (params.useMineru) {
                    std::error_code mineruPathError;
                    if (!std::filesystem::exists(std::filesystem::path(kMineruExe),
                                                 mineruPathError)) {
                        ImGui::TextColored(ImVec4(0.8f, 0, 0, 1),
                                           "Local MinerU environment not found");
                    }
                    if (!params.inputPath.empty() &&
                        !isMineruSupportedInput(params.inputPath)) {
                        ImGui::TextColored(ImVec4(0.8f, 0, 0, 1),
                            "MinerU input must be PDF, image, DOCX, PPTX or XLSX");
                    }
                    ImGui::Combo("Backend", &params.mineruBackend,
                                 kMineruBackendLabels,
                                 (int)(sizeof(kMineruBackendLabels) /
                                       sizeof(kMineruBackendLabels[0])));
                    ImGui::Combo("PDF method", &params.mineruMethod,
                                 kMineruMethodLabels,
                                 (int)(sizeof(kMineruMethodLabels) /
                                       sizeof(kMineruMethodLabels[0])));
                    ImGui::Combo("Language", &params.mineruLanguage,
                                 kMineruLanguageLabels,
                                 (int)(sizeof(kMineruLanguageLabels) /
                                       sizeof(kMineruLanguageLabels[0])));
                    ImGui::Checkbox("Formula recognition", &params.mineruFormula);
                    ImGui::SameLine(240);
                    ImGui::Checkbox("Table recognition", &params.mineruTable);
                    ImGui::TextDisabled(
                        "MinerU produces Markdown and images; Hardcopy applies the settings below.");
                }
            }

            if (ImGui::CollapsingHeader("Font", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::InputText("Main family", &params.fontFamily);
                ImGui::InputText("Main font file", &params.fontPath);
                ImGui::SameLine();
                if (ImGui::Button("Browse##font-main")) {
                    auto p = openFontFileDialog(g_hWnd);
                    if (!p.empty()) params.fontPath = p;
                }
                ImGui::InputText("Sans family", &params.fontSansFamily);
                ImGui::InputText("Sans font file", &params.fontSansPath);
                ImGui::SameLine();
                if (ImGui::Button("Browse##font-sans")) {
                    auto p = openFontFileDialog(g_hWnd);
                    if (!p.empty()) params.fontSansPath = p;
                }
                ImGui::InputText("Kai family", &params.fontKaiFamily);
                ImGui::InputText("Kai font file", &params.fontKaiPath);
                ImGui::SameLine();
                if (ImGui::Button("Browse##font-kai")) {
                    auto p = openFontFileDialog(g_hWnd);
                    if (!p.empty()) params.fontKaiPath = p;
                }
                ImGui::TextDisabled("A selected font file overrides its family field. Embedded EPUB fonts still win.");
                bool fontScanDisabled = convertState.running.load();
                if (fontScanDisabled) ImGui::BeginDisabled();
                if (ImGui::Button("Inspect matching fonts")) {
                    convertState.reset();
                    convertState.fontListing.store(true);
                    convertState.running.store(true);
                    if (convertThread.joinable()) convertThread.join();
                    GuiParams probe = params;
                    probe.listFontsOnly = true;
                    convertThread = std::thread(runCliSubprocess, &convertState, probe);
                }
                if (fontScanDisabled) ImGui::EndDisabled();
                ImGui::SliderInt("Size", &params.fontSize, 18, 37);
                ImGui::SameLine();
                {
                    int px = params.fontSize * 2;
                    int cpl = std::max(1,
                        (params.pageWidth - hardcopy::horizontalMarginUToPx(
                            params.marginHorizontalU, params.pageWidth, params.pageHeight) * 2) / px);
                    ImGui::TextDisabled("(%dpx, ~%d chars/line)", px, cpl);
                }
            }

            if (ImGui::CollapsingHeader("Page", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::RadioButton("Portrait", !pageLandscape))
                    pageLandscape = false;
                ImGui::SameLine();
                if (ImGui::RadioButton("Landscape", pageLandscape))
                    pageLandscape = true;
                const bool pdfRasterPage = isPdfInput(params.inputPath) && params.pdfRaster;
                if (pdfRasterPage) {
                    ImGui::TextDisabled(
                        "PDF raster uses the target canvas; source detail follows the PDF page/DPI.");
                } else {
                    ImGui::TextDisabled("Screen resolution");
                    for (int i = 0; i < kPageSizeCount; ++i) {
                        if (i % 2 == 1) ImGui::SameLine();
                        if (ImGui::RadioButton(kPageSizeNames[i], pageSizeIdx == i))
                            pageSizeIdx = i;
                    }
                    const char* qualityNames[] = { "75%", "80%", "85%", "90%", "95%", "100%" };
                    int qualityIdx = std::clamp((params.jpegQuality - 75 + 2) / 5, 0, 5);
                    if (ImGui::Combo("Image quality", &qualityIdx, qualityNames, 6))
                        params.jpegQuality = 75 + qualityIdx * 5;
                }
            }

            if (isPdfInput(params.inputPath) && params.pdfRaster) {
                ImGui::SeparatorText("PDF raster processing");
                ImGui::Checkbox("Trim white border (auto)", &params.pdfTrim);
                ImGui::Checkbox("Paper columns (two-column reflow)", &params.pdfColumns);
                ImGui::Checkbox("Recognize text (OCR)", &params.pdfOcr);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Pages without a text layer get one from the built-in\n"
                                      "PP-OCRv5 model. Needs an ncnn build of the CLI;\n"
                                      "takes a few seconds per page.");
                ImGui::Checkbox("Ignore source text layer", &params.pdfIgnoreText);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("For scans with wrong OCR text: the embedded text is\n"
                                      "neither used nor copied. With OCR, every page is\n"
                                      "recognized again.");
                {
                    static const char* const kSpacingNames[] = {
                        "Original", "Tight (web)", "Book (publish)", "Open (fresh)"};
                    ImGui::Combo("Line spacing", &params.pdfSpacingIdx, kSpacingNames, 4);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Moves text lines apart or together to the preset's\n"
                                          "line height and paragraph gap. Glyphs and line\n"
                                          "breaks stay as in the PDF.");
                }
                ImGui::SliderInt("Dark enhancement", &params.pdfDarkLevel, 0, 10);
                ImGui::SliderInt("Image contrast", &params.pdfImageLevel, 0, 20);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Dark enhancement darkens text and lines only.\n"
                                      "This darkens pictures (0 = original).");
                ImGui::SliderInt("Background shade", &params.pdfFillLevel, -10, 10);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Filled boxes and bands: -10 removes them, 0 keeps\n"
                                      "their gray, +10 darkens them.");
                int watermarkLevel = params.pdfWatermarkThreshold <= 0
                    ? 0
                    : std::clamp((int)std::lround(
                        (256.0f - std::clamp(params.pdfWatermarkThreshold, 128.0f, 254.0f)) / 8.0f), 1, 16);
                if (ImGui::SliderInt("Watermark whitening", &watermarkLevel, 0, 16))
                    params.pdfWatermarkThreshold = watermarkLevel == 0
                        ? 0.0f : 256.0f - watermarkLevel * 8.0f;
                ImGui::SliderFloat("PDF contrast", &params.pdfContrast, 0.5f, 2.0f, "%.1f");
                {
                    auto snapMargin = [](const char* label, float* v, float lo) {
                        if (ImGui::SliderFloat(label, v, lo, 40.0f, "%.3gu"))
                            *v = lo + (int)((*v - lo) + 0.5f);
                    };
                    snapMargin("Margin horizontal##pdf", &params.pdfMarginHU,
                               (float)hardcopy::kMinHorizontalMarginU);
                    snapMargin("Margin vertical##pdf", &params.pdfMarginVU,
                               (float)hardcopy::kMinTopBottomMarginU);
                }
                ImGui::Checkbox("Night invert (black bg)##pdf", &params.pdfWhiteText);
                ImGui::SameLine(200);
                ImGui::Checkbox("Book spread##pdf", &params.pdfSplitLandscape);
                ImGui::SliderInt("Header cut %", &params.pdfHeaderCut, -1, 29,
                                 params.pdfHeaderCut < 0 ? "auto" : "%d%%");
                ImGui::SliderInt("Footer cut %", &params.pdfFooterCut, -1, 29,
                                 params.pdfFooterCut < 0 ? "auto" : "%d%%");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("auto: cut headers/footers repeated across pages;\n"
                                      "0 keeps them; 1..29 cuts that share of the page.");
            }

            if (isPdfInput(params.inputPath) && !params.pdfRaster) {
                ImGui::SeparatorText("PDF text source");
                ImGui::Checkbox("Recognize text (OCR)##reflow", &params.pdfOcr);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Pages without a text layer get one from the built-in\n"
                                      "PP-OCRv5 model; results are cached per file and\n"
                                      "shared with the raster mode.");
                ImGui::Checkbox("Ignore source text layer##reflow", &params.pdfIgnoreText);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("For scans with wrong OCR text: with OCR, every page\n"
                                      "is recognized again and page tiles the new text\n"
                                      "covers are dropped.");
            }

            if (ImGui::CollapsingHeader("Typography", ImGuiTreeNodeFlags_DefaultOpen)) {
                static const std::vector<const char*> presetLabels = [] {
                    std::vector<const char*> v{ "Custom" };
                    for (int i = 0; i < hardcopy::kReadingPresetCount; ++i)
                        v.push_back(hardcopy::kReadingPresets[i].displayName);
                    v.push_back("Vertical RTL (publish)");
                    return v;
                }();
                const int verticalEntry = (int)presetLabels.size() - 1;
                if (ImGui::Combo("Preset", &params.presetIdx,
                                 presetLabels.data(), (int)presetLabels.size())) {
                    const hardcopy::ReadingPreset* rp = nullptr;
                    if (params.presetIdx == verticalEntry) {
                        rp = hardcopy::findReadingPreset("publish");
                        params.vertical = true;
                    } else {
                        if (params.presetIdx > 0)
                            rp = &hardcopy::kReadingPresets[params.presetIdx - 1];
                        params.vertical = false;
                    }
                    if (rp) applyReadingPreset(&params, *rp);
                }

                bool edited = false;
                auto snapF = [](const char* label, float* v, float lo, float hi,
                                float step, const char* fmt) {
                    bool ch = ImGui::SliderFloat(label, v, lo, hi, fmt);
                    if (ch) *v = lo + (int)((*v - lo) / step + 0.5f) * step;
                    return ch;
                };
                if (!(isPdfInput(params.inputPath) && params.pdfRaster)) {
                    edited |= snapF("Margin horizontal", &params.marginHorizontalU,
                                    (float)hardcopy::kMinHorizontalMarginU, 40.0f, 1.0f, "%.3gu");
                    edited |= snapF("Margin vertical", &params.marginVerticalU,
                                    (float)hardcopy::kMinTopBottomMarginU, 40.0f, 1.0f, "%.3gu");
                }
                if (params.vertical)
                    edited |= snapF("Line height", &params.lineHeightV, 1.5f, 2.0f, 0.05f, "%.2f");
                else
                    edited |= snapF("Line height", &params.lineHeight, 1.0f, 2.5f, 0.25f, "%.2f");
                if (params.drawRuling && !params.vertical) {
                    float gridParaSpacing = 0.0f;
                    ImGui::BeginDisabled();
                    snapF("Para spacing", &gridParaSpacing,
                          0.0f, 2.0f, 0.25f, "follows line height");
                    ImGui::EndDisabled();
                } else {
                    edited |= snapF("Para spacing", &params.paraSpacing,
                                    0.0f, 2.0f, 0.25f, "%.2f em");
                }
                edited |= ImGui::SliderInt("Indent", &params.indentChars, 0, 3);
                if (edited && params.presetIdx > 0) params.presetIdx = 0;
            }

            if (ImGui::CollapsingHeader("Options", ImGuiTreeNodeFlags_DefaultOpen)) {
                const bool rasterOptions = isPdfInput(params.inputPath) && params.pdfRaster;
                if (params.vertical) {
                    ImGui::Checkbox("Reverse turn", &params.reversePages);
                } else if (!rasterOptions) {
                    ImGui::Checkbox("Book spread",  &params.splitLandscape);
                }

                if (!rasterOptions) {
                    ImGui::Checkbox("Night invert (black bg)", &params.whiteText);
                    ImGui::SameLine(200);
                }
                ImGui::Checkbox("Bookmarks",        &params.embedBookmarks);

                ImGui::Checkbox("Synthetic bold",   &params.syntheticBold);
                ImGui::SameLine(200);
                ImGui::Checkbox("Footer progress",  &params.footerProgress);

                {
                    static const char* const kRulingNames[] = { "None", "Dotted", "Dashed" };
                    params.rulingStyleIdx = params.drawRuling
                        ? std::clamp(params.rulingStyleIdx, 1, 2) : 0;
                    if (ImGui::Combo("Ruling lines", &params.rulingStyleIdx, kRulingNames, 3))
                        params.drawRuling = params.rulingStyleIdx != 0;
                    static const char* const kZhNames[] = {
                        "None", "Simplified -> Traditional", "Traditional -> Simplified" };
                    ImGui::Combo("Chinese conversion", &params.zhConvertIdx, kZhNames, 3);
                    ImGui::InputText("Background image", &params.backgroundPath);
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##bg")) {
                        auto bg = openImageFileDialog(g_hWnd);
                        if (!bg.empty()) params.backgroundPath = bg;
                    }
                }

                if (!isPdfInput(params.inputPath) || !params.pdfRaster) {
                    int contrastLevel = (int)std::lround(params.contrastBoost * 20.0f);
                    if (ImGui::SliderInt("Contrast", &contrastLevel, 0, 20))
                        params.contrastBoost = contrastLevel / 20.0f;
                    int strokeLevel = (int)std::lround(params.strokeExpand * 100.0f);
                    if (ImGui::SliderInt("Stroke", &strokeLevel, 0, 10))
                        params.strokeExpand = strokeLevel / 100.0f;
                    int imageLevel = (int)std::lround(params.imageContrastBoost * 20.0f);
                    if (ImGui::SliderInt("Image contrast##text", &imageLevel, 0, 20))
                        params.imageContrastBoost = imageLevel / 20.0f;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Darkens pictures in EPUB/Markdown (0 = original).");
                }
            }

        } else
#endif
        {
            if (ImGui::CollapsingHeader("Dither (e-ink)", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* ditherNames[] = { "None", "Bayer (fast)", "Floyd-Steinberg" };
                ImGui::Combo("Algorithm", &params.ditherAlgo, ditherNames, 3);
                ImGui::SliderInt("Tone (- lighter)", &params.comicTone, -10, 10);
                ImGui::Checkbox("Skip blank pages", &params.comicSkipBlank);
                if (params.ditherAlgo > 0 || params.comicTone != 0 ||
                    params.comicMargin > 0 || params.comicSpread != 0) {
                    ImGui::SliderInt("JPEG Q", &params.jpegQuality, 50, 100);
                }
            }

            if (ImGui::CollapsingHeader("Layout", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderInt("Top/bottom margin %", &params.comicMargin, 0, 25);
                ImGui::TextUnformatted("Two pages per screen:");
                ImGui::SameLine();
                ImGui::RadioButton("Off##spread", &params.comicSpread, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Left to right", &params.comicSpread, 1);
                ImGui::SameLine();
                ImGui::RadioButton("Right to left (manga)", &params.comicSpread, 2);
                ImGui::TextDisabled("Cover and pages already wider than tall stay alone.");
            }

            if (ImGui::CollapsingHeader("Resize", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Scale to target size", &params.comicResize);
                if (params.comicResize) {
                    static int resizePreset = 0;
                    if (ImGui::RadioButton("1404x1872", &resizePreset, 0)) {
                        params.comicResizeW = 1404; params.comicResizeH = 1872;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("1920x2560", &resizePreset, 1)) {
                        params.comicResizeW = 1920; params.comicResizeH = 2560;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Custom##rz", &resizePreset, 2)) {}
                    if (resizePreset == 2) {
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("W##rw", &params.comicResizeW, 0);
                        ImGui::SameLine();
                        ImGui::Text("x");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80);
                        ImGui::InputInt("H##rh", &params.comicResizeH, 0);
                    }
                }
            }
        }

        ImGui::Separator();

        std::error_code mineruReadyError;
        const bool mineruActive = params.useMineru &&
                                  !params.comic && !params.printMode;
        const bool mineruReady = !mineruActive ||
            (isMineruSupportedInput(params.inputPath) &&
             std::filesystem::exists(std::filesystem::path(kMineruExe),
                                     mineruReadyError));
        const bool sourcePdfRaster = isPdfInput(params.inputPath)
                                  && params.pdfRaster && !params.comic;
        bool canConvert = !params.inputPath.empty()
                       && !params.outputPath.empty()
                       && mineruReady
#if HARDCOPY_HAS_TEXT_MODE
                       && (params.comic || params.printMode || sourcePdfRaster
                           || !params.fontPath.empty() || !params.fontFamily.empty())
#endif
                       && !convertState.running.load();

        if (!canConvert) ImGui::BeginDisabled();
        float btnW2 = 180.0f;
        float sendBtnW = 180.0f;
        float buttonsW = btnW2 + style.ItemSpacing.x + sendBtnW;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonsW) * 0.5f);
        if (ImGui::Button("Convert", ImVec2(btnW2, 36))) {
            convertState.reset();
            convertState.running.store(true);
            if (convertThread.joinable()) convertThread.join();
            convertThread = std::thread(runCliSubprocess, &convertState, params);
        }
        if (!canConvert) ImGui::EndDisabled();

        ImGui::SameLine();
        bool canSend = !params.outputPath.empty()
                    && std::filesystem::exists(std::filesystem::path(utf8ToWide(params.outputPath)))
                    && localSendState.hasBound
                    && !localSendState.sending.load();
        if (!canSend) ImGui::BeginDisabled();
        if (ImGui::Button("Send to device", ImVec2(sendBtnW, 36))) {
            if (sendThread.joinable()) sendThread.join();
            localSendState.sending.store(true);
            LocalSendPeer target = localSendState.bound;
            std::string output = params.outputPath;
            sendThread = std::thread([&localSendState, target, output] {
                std::string error;
                LocalSendPeer current;
                bool online = probeLocalSendPeer(target.ip, target.port, current, error);
                if (online && current.fingerprint == target.fingerprint) {
                    saveBoundLocalSendPeer(current);
                    bool sent = sendLocalSendFile(current, output, error);
                    std::lock_guard<std::mutex> lock(localSendState.mutex);
                    if (sent) {
                        localSendState.bound = current;
                        localSendState.message = "Sent to " + current.alias;
                    } else localSendState.message = "Send: " + error;
                } else {
                    std::lock_guard<std::mutex> lock(localSendState.mutex);
                    localSendState.message = "Bound device is being located; run Scan devices";
                }
                localSendState.sending.store(false);
            });
        }
        if (!canSend) ImGui::EndDisabled();

        if (ImGui::CollapsingHeader("Hardcopy Companion", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("PC: %s", companionServer.deviceName().c_str());
            ImGui::Text("Service: %s", companionServer.status().c_str());
            ImGui::Text("Device ID: %s", companionServer.deviceId().c_str());
            ImGui::Text("Active jobs: %d", companionServer.activeJobs());
            ImGui::TextWrapped("%s", companionServer.firewallStatus().c_str());
            if (!companionServer.firewallReady() &&
                ImGui::Button("Allow LAN access (one admin prompt)"))
                companionServer.ensureFirewall(true);
            if (companionServer.pairingEnabled())
                ImGui::TextColored(ImVec4(0, 0.55f, 0, 1), "Pairing available for the Supernote plugin");
            else if (ImGui::Button("Allow pairing for 2 minutes"))
                companionServer.enablePairing(120);
            if (ImGui::Checkbox("Use these settings for Supernote jobs", &g_usePcSettings)) {
                if (g_usePcSettings) companionServer.setPcSettingsCommand(pcSettingsCommand);
                else companionServer.setPcSettingsCommand(nullptr);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off: each job uses the settings sent by the phone.\n"
                                  "On: the form on the left decides fonts, typography and\n"
                                  "processing; the phone still decides the mode and page size.");
            const auto views = companionServer.jobViews();
            if (!views.empty() &&
                ImGui::TreeNodeEx("Supernote tasks", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::vector<std::string> order;
                std::map<std::string, std::vector<CompanionServer::JobView>> groups;
                for (const auto& v : views) {
                    const std::string key = v.batchId.empty() ? "job:" + v.id : v.batchId;
                    if (!groups.count(key)) order.push_back(key);
                    groups[key].push_back(v);
                }
                auto unfinished = [](const CompanionServer::JobView& v) {
                    return v.status != "completed" && v.status != "failed";
                };
                for (const auto& key : order) {
                    auto& list = groups[key];
                    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                        return a.batchIndex < b.batchIndex;
                    });
                    ImGui::PushID(key.c_str());
                    const bool batch = !list.front().batchId.empty();
                    if (batch) {
                        int done = 0;
                        bool open = false;
                        for (const auto& v : list) {
                            if (v.status == "completed") ++done;
                            if (unfinished(v)) open = true;
                        }
                        const int total = std::max<int>((int)list.size(), list.front().batchCount);
                        if (total > (int)list.size()) open = true;
                        ImGui::Text("Batch: %d/%d converted", done, total);
                        if (open) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Cancel batch"))
                                companionServer.cancelBatch(list.front().batchId);
                        }
                    }
                    for (const auto& v : list) {
                        ImGui::PushID(v.id.c_str());
                        if (batch) ImGui::Text("  [%d/%d] %s", v.batchIndex + 1, v.batchCount, v.fileName.c_str());
                        else ImGui::Text("%s", v.fileName.c_str());
                        ImGui::SameLine();
                        if (v.status == "completed") {
                            ImGui::TextDisabled("done");
                        } else if (v.cancelled) {
                            ImGui::TextDisabled("cancelled");
                        } else if (v.status == "failed") {
                            ImGui::TextColored(ImVec4(0.75f, 0, 0, 1), "failed: %s", v.message.c_str());
                        } else {
                            const std::string label = v.stage + " " + std::to_string(v.progress) + "%";
                            ImGui::ProgressBar(v.progress / 100.0f, ImVec2(160, 0), label.c_str());
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Cancel")) companionServer.cancelJob(v.id);
                        }
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            const auto recentCompanionJobs = companionServer.recentJobs();
            for (const auto& item : recentCompanionJobs)
                ImGui::BulletText("%s", item.c_str());
        }

        if (ImGui::CollapsingHeader("LocalSend device")) {
            if (localSendState.hasBound)
                ImGui::Text("Bound: %s  %s:%d", localSendState.bound.alias.c_str(),
                            localSendState.bound.ip.c_str(), localSendState.bound.port);
            else
                ImGui::TextDisabled("Choose a device after scanning");
            bool scanDisabled = localSendState.scanning.load();
            if (scanDisabled) ImGui::BeginDisabled();
            if (ImGui::Button("Scan devices")) {
                if (scanThread.joinable()) scanThread.join();
                localSendState.scanning.store(true);
                scanThread = std::thread([&localSendState] {
                    auto peers = scanLocalSendPeers();
                    std::lock_guard<std::mutex> lock(localSendState.mutex);
                    localSendState.peers = std::move(peers);
                    localSendState.message = localSendState.peers.empty()
                        ? "Scan complete" : "Select a device to bind";
                    localSendState.scanning.store(false);
                });
            }
            if (scanDisabled) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            ImGui::InputTextWithHint("##localsend-ip", "Device IP", &localSendState.manualIp);
            ImGui::SameLine();
            bool bindIpDisabled = localSendState.scanning.load() || localSendState.manualIp.empty();
            if (bindIpDisabled) ImGui::BeginDisabled();
            if (ImGui::Button("Bind IP")) {
                if (scanThread.joinable()) scanThread.join();
                localSendState.scanning.store(true);
                std::string ip = localSendState.manualIp;
                scanThread = std::thread([&localSendState, ip] {
                    LocalSendPeer peer; std::string error;
                    bool found = probeLocalSendPeer(ip, 53317, peer, error);
                    std::lock_guard<std::mutex> lock(localSendState.mutex);
                    if (found) {
                        localSendState.bound = peer;
                        localSendState.hasBound = true;
                        saveBoundLocalSendPeer(peer);
                        localSendState.message = "Bound to " + peer.alias;
                    } else localSendState.message = "Probe: " + error;
                    localSendState.scanning.store(false);
                });
            }
            if (bindIpDisabled) ImGui::EndDisabled();
            {
                std::lock_guard<std::mutex> lock(localSendState.mutex);
                for (size_t i = 0; i < localSendState.peers.size(); ++i) {
                    const auto& peer = localSendState.peers[i];
                    std::string label = "Bind " + peer.alias + " (" + peer.ip + ")##peer" + std::to_string(i);
                    if (ImGui::Button(label.c_str())) {
                        localSendState.bound = peer;
                        localSendState.hasBound = true;
                        saveBoundLocalSendPeer(peer);
                        localSendState.message = "Bound to " + peer.alias;
                    }
                }
                if (!localSendState.message.empty()) ImGui::TextWrapped("%s", localSendState.message.c_str());
            }
        }

        if (convertState.running.load() || convertState.done.load()) {
            ImGui::Separator();
            int  s = convertState.stage.load();
            int  p = convertState.percent.load();
            char label[128];
            float frac;
            if (convertState.externalParsing.load()) {
                std::snprintf(label, sizeof(label), "MinerU parsing");
                frac = -FLT_MIN;
            } else {
                std::snprintf(label, sizeof(label), "%s %d%%", stageName(s), p);
                frac = (s * 25.0f + p * 0.25f) / 100.0f;
                if (frac > 1.0f) frac = 1.0f;
            }
            ImGui::ProgressBar(frac, ImVec2(-1, 0), label);

            if (convertState.done.load()) {
                int r = convertState.result.load();
                if (r == 0) {
                    if (convertState.fontListing.load()) {
                        ImGui::TextColored(ImVec4(0, 0.6f, 0, 1), "Font listing ready");
                    } else {
                        ImGui::TextColored(ImVec4(0, 0.6f, 0, 1), "OK");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Open output")) {
                            ShellExecuteW(nullptr, L"open",
                                          utf8ToWide(params.outputPath).c_str(),
                                          nullptr, nullptr, SW_SHOW);
                        }
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.8f, 0, 0, 1),
                                       "Failed (exit %d)", r);
                }
            }
        }

        ImGui::Separator();
        ImGui::Text("Log");
        ImGui::BeginChild("##log", ImVec2(0, 140), ImGuiChildFlags_Border);
        {
            std::lock_guard<std::mutex> lk(convertState.logMtx);
            for (auto& line : convertState.logLines)
                ImGui::TextUnformatted(line.c_str());
        }
        if (convertState.running.load())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::End();

        if (frameNum < 3) trace("frame %d: ImGui::Render", frameNum);
        ImGui::Render();
        RECT rc;
        GetClientRect(g_hWnd, &rc);
        glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);
        glClearColor(0.94f, 0.94f, 0.94f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (frameNum < 3) trace("frame %d: GL draw", frameNum);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (frameNum < 3) trace("frame %d: SwapBuffers", frameNum);
        SwapBuffers(hDC);
        if (frameNum < 3) trace("frame %d: end", frameNum);
        ++frameNum;

        if (!convertState.running.load())
            Sleep(16);
    }

    trace("WinMain: main loop exited, cleaning up");
    companionServer.stop();
    removeTrayIcon();
    if (convertThread.joinable()) convertThread.join();
    if (scanThread.joinable()) scanThread.join();
    if (sendThread.joinable()) sendThread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hRC);
    ReleaseDC(g_hWnd, hDC);
    DestroyWindow(g_hWnd);

    OleUninitialize();
    trace("WinMain: exit");
    if (g_traceFile) fclose(g_traceFile);
    return 0;
}
