#include <cstdio>
#include <string>
#include <cctype>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite.h>

#include "font_listing.h"

namespace {

std::string wcharToUtf8(const wchar_t* wstr) {
    if (!wstr) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string getStr(IDWriteLocalizedStrings* s, UINT32 i = 0) {
    if (!s) return {};
    UINT32 len = 0;
    s->GetStringLength(i, &len);
    std::wstring w(len + 1, L'\0');
    s->GetString(i, &w[0], len + 1);
    w.resize(len);
    return wcharToUtf8(w.c_str());
}

}

int printDWriteFonts(const std::string& filter, FILE* output) {
    if (!output) output = stdout;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IDWriteFactory* factory = nullptr;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory));
    if (!factory) { std::fprintf(output, "DWriteCreateFactory failed\n"); return 1; }

    IDWriteFontCollection* col = nullptr;
    factory->GetSystemFontCollection(&col, TRUE);
    if (!col) { std::fprintf(output, "GetSystemFontCollection failed\n"); return 1; }

    UINT32 fc = col->GetFontFamilyCount();
    std::fprintf(output, "Total font families: %u\n", fc);

    for (UINT32 fi = 0; fi < fc; ++fi) {
        IDWriteFontFamily* ff = nullptr;
        col->GetFontFamily(fi, &ff);
        if (!ff) continue;

        IDWriteLocalizedStrings* famNames = nullptr;
        ff->GetFamilyNames(&famNames);
        std::string famName = getStr(famNames);
        if (famNames) famNames->Release();

        bool show = filter.empty();
        if (!show) {
            std::string lower = famName;
            for (char& c : lower) c = tolower(c);
            std::string lf = filter;
            for (char& c : lf) c = tolower(c);
            show = lower.find(lf) != std::string::npos;
        }

        if (!show) {
            UINT32 fontCount = ff->GetFontCount();
            for (UINT32 fonti = 0; fonti < fontCount && !show; ++fonti) {
                IDWriteFont* font = nullptr;
                ff->GetFont(fonti, &font);
                if (!font) continue;
                BOOL has = FALSE;
                IDWriteLocalizedStrings* ps = nullptr;
                font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &ps, &has);
                if (has && ps) {
                    for (UINT32 ni = 0; ni < ps->GetCount(); ++ni) {
                        std::string s = getStr(ps, ni);
                        std::string lower = s;
                        for (char& c : lower) c = tolower(c);
                        std::string lf = filter;
                        for (char& c : lf) c = tolower(c);
                        if (lower.find(lf) != std::string::npos) { show = true; break; }
                    }
                    ps->Release();
                }
                font->Release();
            }
        }

        if (!show) { ff->Release(); continue; }

        std::fprintf(output, "Family: %s\n", famName.c_str());
        UINT32 fontCount = ff->GetFontCount();
        for (UINT32 fonti = 0; fonti < fontCount; ++fonti) {
            IDWriteFont* font = nullptr;
            ff->GetFont(fonti, &font);
            if (!font) continue;

            BOOL has = FALSE;
            IDWriteLocalizedStrings* fullN = nullptr;
            font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_FULL_NAME, &fullN, &has);
            std::string full = (has && fullN) ? getStr(fullN) : "";
            if (fullN) fullN->Release();

            IDWriteLocalizedStrings* psN = nullptr;
            font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &psN, &has);
            std::string ps = (has && psN) ? getStr(psN) : "";
            if (psN) psN->Release();

            std::fprintf(output, "  [%u] full='%s' ps='%s' weight=%d\n",
                         fonti, full.c_str(), ps.c_str(), font->GetWeight());
            font->Release();
        }
        ff->Release();
    }

    col->Release();
    factory->Release();
    CoUninitialize();
    return 0;
}
