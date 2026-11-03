#include "epub_mupdf.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/fitz/util.h>
#include <mupdf/ucdn.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include <pugixml.hpp>

#include "../include/hardcopy/presets.h"
#include "../parser/css_font_face.h"
#include "../parser/css_subset.h"
#include "../parser/font_alias.h"
#include "../util/zip_reader.h"
#include "epub_webp.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace hardcopy {

namespace {

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s.push_back((char)cp);
    } else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (cp >> 18)));
        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0);
    out->resize((size_t)n);
    return f.read((char*)out->data(), n).good();
}

unsigned long hardcopyPid() {
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

std::string cssUrlPath(const std::filesystem::path& p) {
    std::string s = p.generic_u8string();
    s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
    return s;
}

std::string ensureFontFile(const std::string& path,
                           const std::vector<uint8_t>& mem,
                           const char* stem, Logger* log) {
    if (!path.empty()) return cssUrlPath(std::filesystem::path(path));
    if (mem.empty()) return {};
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    dir /= "hardcopy-fonts";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file =
        dir / (std::string(stem) + "-" + std::to_string(hardcopyPid()) + ".ttf");
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f || !f.write((const char*)mem.data(), (std::streamsize)mem.size())) {
        if (log) log->log(LogLevel::Warn, "epub-mupdf",
                          "cannot spill in-memory font to temp file");
        return {};
    }
    f.close();
    return cssUrlPath(file);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string parentZipPath(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string resolveZipPath(const std::string& baseDir, const std::string& hrefIn) {
    std::string href;
    href.reserve(hrefIn.size());
    for (size_t n = 0; n < hrefIn.size(); ++n) {
        if (hrefIn[n] == '%' && n + 2 < hrefIn.size()
            && std::isxdigit((unsigned char)hrefIn[n + 1])
            && std::isxdigit((unsigned char)hrefIn[n + 2])) {
            auto hv = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                return std::tolower((unsigned char)c) - 'a' + 10;
            };
            href.push_back((char)(hv(hrefIn[n + 1]) * 16 + hv(hrefIn[n + 2])));
            n += 2;
        } else {
            href.push_back(hrefIn[n]);
        }
    }
    size_t cut = href.find_first_of("#?");
    if (cut != std::string::npos) href.resize(cut);
    if (href.empty() || href.find("://") != std::string::npos
        || href.rfind("data:", 0) == 0) return {};
    std::string combined = (!href.empty() && href[0] == '/')
                         ? href.substr(1) : baseDir + href;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= combined.size()) {
        size_t j = combined.find('/', i);
        if (j == std::string::npos) j = combined.size();
        std::string p = combined.substr(i, j - i);
        if (p == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!p.empty() && p != ".") parts.push_back(p);
        i = j + 1;
        if (j == combined.size()) break;
    }
    std::string out;
    for (size_t n = 0; n < parts.size(); ++n) {
        if (n) out += '/';
        out += parts[n];
    }
    return out;
}

bool isFontEntry(const std::string& path) {
    std::string l = lowerAscii(path);
    return l.size() > 4 && (l.rfind(".ttf") == l.size() - 4
        || l.rfind(".otf") == l.size() - 4
        || l.rfind(".woff") == l.size() - 5
        || l.rfind(".woff2") == l.size() - 6);
}

struct CssSource {
    std::string text;
    std::string baseDir;
};

struct NamedFontFace {
    std::string family;
    std::string file;
    bool bold = false;
    bool embedded = false;
};

struct WarningSink {
    Logger* log = nullptr;
    std::set<std::string> seen;
};

void mupdfWarning(void* user, const char* message) {
    auto* sink = static_cast<WarningSink*>(user);
    if (!sink || !sink->log || !message || !*message) return;
    if (sink->seen.insert(message).second)
        sink->log->log(LogLevel::Warn, "epub-mupdf", message);
}

std::string cssQuoted(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void collectFamilyRefs(const std::string& css, std::vector<std::string>* out) {
    std::string low = lowerAscii(css);
    size_t pos = 0;
    while ((pos = low.find("font-family", pos)) != std::string::npos) {
        size_t colon = css.find(':', pos + 11);
        if (colon == std::string::npos) break;
        size_t end = css.find_first_of(";}", colon + 1);
        std::string value = css.substr(colon + 1,
            end == std::string::npos ? std::string::npos : end - colon - 1);
        size_t bang = value.find('!');
        if (bang != std::string::npos) value.resize(bang);
        auto names = splitFamilyList(value);
        out->insert(out->end(), names.begin(), names.end());
        pos = end == std::string::npos ? css.size() : end + 1;
    }
}

struct AuthorRule {
    std::string selector;
    std::string decls;
};

void collectAuthorRules(const std::string& rawCss, std::vector<AuthorRule>* out) {
    std::string css;
    css.reserve(rawCss.size());
    for (size_t i = 0; i < rawCss.size(); ++i) {
        if (rawCss[i] == '/' && i + 1 < rawCss.size() && rawCss[i + 1] == '*') {
            const size_t e = rawCss.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 1;
            continue;
        }
        css.push_back(rawCss[i]);
    }
    size_t i = 0;
    while (i < css.size()) {
        while (i < css.size() && std::isspace((unsigned char)css[i])) ++i;
        if (i >= css.size()) break;
        const size_t brace = css.find('{', i);
        if (css[i] == '@') {
            const size_t semi = css.find(';', i);
            if (semi != std::string::npos && (brace == std::string::npos || semi < brace)) {
                i = semi + 1;
                continue;
            }
            if (brace == std::string::npos) break;
            int depth = 1;
            size_t j = brace + 1;
            while (j < css.size() && depth > 0) {
                if (css[j] == '{') ++depth;
                else if (css[j] == '}') --depth;
                ++j;
            }
            const std::string at = lowerAscii(css.substr(i, brace - i));
            if (at.rfind("@media", 0) == 0 && at.find("print") == std::string::npos
                && j > brace + 1)
                collectAuthorRules(css.substr(brace + 1, j - brace - 2), out);
            i = j;
            continue;
        }
        if (brace == std::string::npos) break;
        const size_t close = css.find('}', brace);
        if (close == std::string::npos) break;
        AuthorRule r;
        r.selector = css.substr(i, brace - i);
        r.decls = css.substr(brace + 1, close - brace - 1);
        while (!r.selector.empty() && std::isspace((unsigned char)r.selector.back()))
            r.selector.pop_back();
        if (!r.selector.empty()) out->push_back(std::move(r));
        i = close + 1;
    }
}

std::string declValue(const std::string& decls, const char* prop) {
    const std::string low = lowerAscii(decls);
    const std::string want = prop;
    std::string value;
    size_t pos = 0;
    while ((pos = low.find(want, pos)) != std::string::npos) {
        const bool startOk = pos == 0 || low[pos - 1] == ';' || low[pos - 1] == '{'
                          || std::isspace((unsigned char)low[pos - 1]);
        size_t k = pos + want.size();
        while (k < low.size() && std::isspace((unsigned char)low[k])) ++k;
        if (!startOk || k >= low.size() || low[k] != ':') { pos += want.size(); continue; }
        const size_t end = decls.find(';', k + 1);
        value = decls.substr(k + 1, end == std::string::npos ? std::string::npos : end - k - 1);
        const size_t bang = value.find('!');
        if (bang != std::string::npos) value.resize(bang);
        while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
        while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(0, 1);
        pos = end == std::string::npos ? low.size() : end;
    }
    return value;
}

int calloutBackgroundGray(const std::string& decls) {
    std::string value = declValue(decls, "background-color");
    if (value.empty()) value = declValue(decls, "background");
    const int gray = cssBackgroundGray(value);
    return gray > 0 ? gray : -1;
}

bool isCalloutSelector(const std::string& sel) {
    const std::string l = lowerAscii(sel);
    if (l.find("body") != std::string::npos || l.find("html") != std::string::npos
        || l.find(':') != std::string::npos || l.find('*') != std::string::npos)
        return false;
    if (l.find('.') != std::string::npos || l.find('#') != std::string::npos) return true;
    std::string last = l.substr(l.find_last_of(" >+~") == std::string::npos
                                ? 0 : l.find_last_of(" >+~") + 1);
    return last == "pre" || last == "aside" || last == "blockquote";
}

template <typename Pred>
std::string selectorParts(const std::string& selector, Pred keep) {
    std::string kept;
    size_t a = 0;
    while (a <= selector.size()) {
        size_t b = selector.find(',', a);
        if (b == std::string::npos) b = selector.size();
        std::string part = selector.substr(a, b - a);
        while (!part.empty() && std::isspace((unsigned char)part.front())) part.erase(0, 1);
        while (!part.empty() && std::isspace((unsigned char)part.back())) part.pop_back();
        if (!part.empty() && keep(part))
            kept += (kept.empty() ? "" : ", ") + part;
        a = b + 1;
    }
    return kept;
}

bool isHeadingSelector(const std::string& sel) {
    const std::string l = lowerAscii(sel);
    if (l.find(':') != std::string::npos) return false;
    const size_t cut = l.find_last_of(" >+~");
    std::string last = l.substr(cut == std::string::npos ? 0 : cut + 1);
    last = last.substr(0, last.find_first_of(".#["));
    return last.size() == 2 && last[0] == 'h' && last[1] >= '1' && last[1] <= '6';
}

std::string labelFontSize(const std::string& authored) {
    const std::string v = lowerAscii(authored);
    char* end = nullptr;
    const double n = std::strtod(v.c_str(), &end);
    if (end != v.c_str() && n > 0) {
        const std::string unit(end);
        if (unit == "em" && n >= 0.8 && n <= 1.3) return v;
        if (unit == "%" && n >= 80 && n <= 130) return v;
    }
    return "1em";
}

std::vector<NamedFontFace> discoverNamedFonts(
        const std::filesystem::path& epubPath,
        const std::string& regularFile, const std::string& boldFile,
        const std::string& sansFile, const std::string& kaiFile,
        const std::string& monoFile, Logger* log,
        std::string* authorOverrides = nullptr) {
    ZipReader zip;
    if (!zip.open(epubPath)) return {};
    const auto entries = zip.entries();
    std::map<std::string, std::string> actualByLower;
    std::map<std::string, std::string> fontByStem;
    for (const auto& e : entries) {
        actualByLower[lowerAscii(e)] = e;
        if (isFontEntry(e)) fontByStem.emplace(fileStemKey(e), e);
    }

    std::vector<CssSource> sources;
    for (const auto& e : entries) {
        std::string l = lowerAscii(e);
        if (l.size() > 4 && l.rfind(".css") == l.size() - 4) {
            std::string text;
            if (zip.readText(e, &text)) sources.push_back({std::move(text), parentZipPath(e)});
        } else if ((l.size() > 6 && l.rfind(".xhtml") == l.size() - 6)
                   || (l.size() > 5 && l.rfind(".html") == l.size() - 5)) {
            std::string xml;
            if (!zip.readText(e, &xml)) continue;
            pugi::xml_document doc;
            if (!doc.load_string(xml.c_str())) continue;
            for (auto sn : doc.select_nodes("//*[local-name()='style']")) {
                std::string text;
                for (auto c : sn.node().children())
                    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
                        text += c.value();
                if (!text.empty()) sources.push_back({std::move(text), parentZipPath(e)});
            }
        }
    }

    std::map<std::string, bool> authorEmbedded;
    struct Pending { FontFaceDecl decl; std::string baseDir; };
    std::vector<Pending> pending;
    std::vector<std::string> refs;
    for (const auto& src : sources) {
        std::vector<FontFaceDecl> faces;
        collectFontFaces(src.text, &faces);
        collectFamilyRefs(src.text, &refs);
        for (auto& f : faces) {
            bool resolved = false;
            for (const auto& u : f.urls) {
                std::string p = resolveZipPath(src.baseDir, u);
                if (!p.empty() && actualByLower.count(lowerAscii(p))) {
                    resolved = true;
                    break;
                }
            }
            const std::string key = normalizeFamilyKey(f.family);
            if (resolved) authorEmbedded[key] = true;
            else pending.push_back({std::move(f), src.baseDir});
        }
    }

    auto fileForRole = [&](FontRole role, bool bold) -> std::string {
        switch (role) {
            case FontRole::Sans:
                return !sansFile.empty() ? sansFile
                     : (!boldFile.empty() ? boldFile : regularFile);
            case FontRole::Kai:  return !kaiFile.empty() ? kaiFile : regularFile;
            case FontRole::Mono: return monoFile;
            case FontRole::Serif:
                return bold && !boldFile.empty() ? boldFile : regularFile;
            default: return {};
        }
    };

    std::vector<NamedFontFace> result;
    std::set<std::string> emitted;
    auto emitFace = [&](const std::string& family, const std::string& file,
                        bool bold, bool embedded) {
        if (family.empty() || file.empty()) return;
        std::string key = normalizeFamilyKey(family) + (bold ? "#b" : "#n");
        if (!emitted.insert(key).second) return;
        result.push_back({family, file, bold, embedded});
        if (log) {
            std::string msg = std::string(embedded ? "embedded filename face: "
                                                   : "substitute named face: ")
                            + family + " -> " + file;
            log->log(LogLevel::Info, "epub-mupdf", msg.c_str());
        }
    };

    for (const auto& p : pending) {
        const std::string famKey = normalizeFamilyKey(p.decl.family);
        if (authorEmbedded.count(famKey)) continue;
        FontRole role = roleFromNames(p.decl.localNames);
        if (role == FontRole::None) role = roleForFamily(p.decl.family);
        emitFace(p.decl.family, fileForRole(role, p.decl.bold),
                 p.decl.bold, false);
    }

    for (const auto& family : refs) {
        const std::string famKey = normalizeFamilyKey(family);
        if (famKey.empty() || authorEmbedded.count(famKey)) continue;
        auto fit = fontByStem.find(famKey);
        if (fit != fontByStem.end()) {
            std::vector<uint8_t> data;
            if (zip.read(fit->second, &data) && !data.empty()) {
                std::string temp = ensureFontFile({}, data,
                    (std::string("embedded-") + famKey).c_str(), log);
                emitFace(family, temp, false, true);
                continue;
            }
        }
        FontRole role = roleForFamily(family);
        emitFace(family, fileForRole(role, false), false, false);
        std::string bf = fileForRole(role == FontRole::Serif ? FontRole::Serif : role, true);
        if (!bf.empty() && bf != fileForRole(role, false))
            emitFace(family, bf, true, false);
    }

    if (authorOverrides) {
        std::set<std::string> resolvable;
        for (const auto& kv : authorEmbedded) resolvable.insert(kv.first);
        for (const auto& f : result) resolvable.insert(normalizeFamilyKey(f.family));
        for (const char* a : {"kaiti", "kai", "heiti", "hei", "yuanti", "fzlth", "dkheiti"})
            resolvable.insert(normalizeFamilyKey(a));
        std::string& o = *authorOverrides;
        for (const auto& src : sources) {
            std::vector<AuthorRule> rules;
            collectAuthorRules(src.text, &rules);
            for (const auto& r : rules) {
                if (r.selector.find('{') != std::string::npos) continue;
                const std::string fam = declValue(r.decls, "font-family");
                if (!fam.empty()) {
                    const auto names = splitFamilyList(fam);
                    const std::string key0 = names.empty() ? std::string()
                                                           : normalizeFamilyKey(names[0]);
                    const bool mono = !names.empty() && roleFromNames(names) == FontRole::Mono;
                    if (mono && monoFile.empty() && !authorEmbedded.count(key0)) {
                        o += r.selector + " { font-family: monospace !important; }\n";
                    } else if (!names.empty() && resolvable.count(key0)) {
                        o += r.selector + " { font-family: " + fam + " !important; }\n";
                    }
                }
                if (lowerAscii(declValue(r.decls, "display")) == "inline-block")
                    o += r.selector + " { display: inline !important; }\n";
                const std::string align = lowerAscii(declValue(r.decls, "text-align"));
                if (align == "center" || align == "right" || align == "end") {
                    o += r.selector + " { text-indent: 0 !important; }\n";
                }
                const int gray = calloutBackgroundGray(r.decls);
                if (gray >= 0) {
                    const std::string kept = selectorParts(r.selector, isCalloutSelector);
                    if (!kept.empty()) {
                        char fill[160];
                        std::snprintf(fill, sizeof(fill),
                            " { background-color: #%02x%02x%02x !important; "
                            "padding: 0.5em 0.8em !important; "
                            "margin-bottom: 0.6em !important; }\n",
                            gray, gray, gray);
                        o += kept + fill;
                    }
                }
                const std::string flt = lowerAscii(declValue(r.decls, "float"));
                if (flt == "left" || flt == "right") {
                    const std::string heads = selectorParts(r.selector, isHeadingSelector);
                    if (!heads.empty()) {
                        o += heads + " { float: none !important; border: none !important; "
                                     "padding: 0 !important; margin: 0 0 0.3em !important; "
                                     "width: auto !important; text-indent: 0 !important; "
                                     "font-size: " + labelFontSize(declValue(r.decls, "font-size"))
                           + " !important;";
                        if (lowerAscii(declValue(r.decls, "font-style")) == "italic")
                            o += " font-style: italic !important;";
                        o += " }\n";
                    }
                }
            }
        }
    }
    return result;
}

}

struct MupdfEpubEngine::Impl {
    std::array<std::mutex, FZ_LOCK_MAX> locks;
    fz_locks_context lockContext{};
    fz_context*  ctx = nullptr;
    fz_document* doc = nullptr;
    Logger*      log = nullptr;
    WarningSink  warnings;
    int          pages = 0;

    int pageW = 0, pageH = 0;
    int marginL = 0, marginT = 0;
    float dpr = 1.0f;

    fz_matrix pageCtm() const {
        return fz_concat(fz_scale(dpr, dpr),
                         fz_translate((float)marginL, (float)marginT));
    }
    int clipTop = 0, clipBottom = 0;
    fz_irect contentClip() const {
        return fz_make_irect(0, clipTop, pageW, clipBottom);
    }
    bool noteMode = false;

    Bitmap8 background;
    bool    hasBackground = false;

    std::vector<uint8_t> bufRegular, bufBold, bufSans, bufKai, bufFallback;
    fz_font* fRegular  = nullptr;
    fz_font* fBold     = nullptr;
    fz_font* fSans     = nullptr;
    fz_font* fKai      = nullptr;
    fz_font* fFallback = nullptr;

    std::vector<HeadingAnchor> outline;
    std::vector<int> pageMap;
    std::vector<int> rawToOut;
    int rawPage(int out0) const {
        return (out0 >= 0 && out0 < (int)pageMap.size()) ? pageMap[(size_t)out0] : out0;
    }
    int outPage(int raw) const {
        return (raw >= 0 && raw < (int)rawToOut.size()) ? rawToOut[(size_t)raw] : -1;
    }

    void logMsg(LogLevel lv, const char* msg) {
        if (log) log->log(lv, "epub-mupdf", msg);
    }
};

namespace {

void mupdfLock(void* user, int lock) {
    auto* locks = static_cast<std::array<std::mutex, FZ_LOCK_MAX>*>(user);
    if (locks && lock >= 0 && lock < FZ_LOCK_MAX) (*locks)[(size_t)lock].lock();
}

void mupdfUnlock(void* user, int lock) {
    auto* locks = static_cast<std::array<std::mutex, FZ_LOCK_MAX>*>(user);
    if (locks && lock >= 0 && lock < FZ_LOCK_MAX) (*locks)[(size_t)lock].unlock();
}

void drawNoteDivider(Bitmap8* bitmap) {
    if (!bitmap || bitmap->width <= 0 || bitmap->height <= 0) return;
    const int thick = std::max(2, std::min(bitmap->width, bitmap->height) / 720);
    if (bitmap->height >= bitmap->width) {
        const int y0 = noteDividerPosition(bitmap->height);        for (int y = y0; y < std::min(bitmap->height, y0 + thick); ++y)
            std::fill_n(&bitmap->pixels[(size_t)y * bitmap->width],
                        bitmap->width, uint8_t(72));
    } else {
        const int x0 = noteDividerPosition(bitmap->width);
        for (int y = 0; y < bitmap->height; ++y)
            for (int x = x0; x < std::min(bitmap->width, x0 + thick); ++x)
                bitmap->pixels[(size_t)y * bitmap->width + x] = 72;
    }
}

}

static MupdfEpubEngine* g_activeEngine = nullptr;

void* MupdfEpubEngine::hookSystemFont(void* ctx_, int bold) {
    fz_context* ctx = (fz_context*)ctx_;
    if (bold && impl_->fBold) return fz_keep_font(ctx, impl_->fBold);
    return impl_->fRegular ? fz_keep_font(ctx, impl_->fRegular) : nullptr;
}

void* MupdfEpubEngine::hookCjkFont(void* ctx_) {
    fz_context* ctx = (fz_context*)ctx_;
    return impl_->fRegular ? fz_keep_font(ctx, impl_->fRegular) : nullptr;
}

void* MupdfEpubEngine::hookFallbackFont(void* ctx_, int script, int bold) {
    fz_context* ctx = (fz_context*)ctx_;
    const bool cjkScript =
        script == UCDN_SCRIPT_HAN || script == UCDN_SCRIPT_HIRAGANA ||
        script == UCDN_SCRIPT_KATAKANA || script == UCDN_SCRIPT_BOPOMOFO ||
        script == UCDN_SCRIPT_HANGUL;
    if (!cjkScript) return nullptr;
    if (impl_->fFallback) return fz_keep_font(ctx, impl_->fFallback);
    if (bold && impl_->fBold) return fz_keep_font(ctx, impl_->fBold);
    return impl_->fRegular ? fz_keep_font(ctx, impl_->fRegular) : nullptr;
}

extern "C" {

static fz_font* hardcopy_load_system_font(fz_context* ctx, const char*,
                                         int bold, int,
                                         int) {
    return g_activeEngine ? (fz_font*)g_activeEngine->hookSystemFont(ctx, bold)
                          : nullptr;
}

static fz_font* hardcopy_load_cjk_font(fz_context* ctx, const char*,
                                      int, int) {
    return g_activeEngine ? (fz_font*)g_activeEngine->hookCjkFont(ctx)
                          : nullptr;
}

static fz_font* hardcopy_load_fallback_font(fz_context* ctx, int script,
                                           int, int,
                                           int bold, int) {
    return g_activeEngine
             ? (fz_font*)g_activeEngine->hookFallbackFont(ctx, script, bold)
             : nullptr;
}

}

MupdfEpubEngine::MupdfEpubEngine() : impl_(std::make_unique<Impl>()) {}
MupdfEpubEngine::~MupdfEpubEngine() { close(); }

namespace {

std::string buildUserCss(const Options& opts,
                         const std::string& regularFile,
                         const std::string& boldFile,
                         const std::string& italicFile,
                         const std::string& monoFile,
                         const std::string& sansFile,
                         const std::string& kaiFile,
                         const std::vector<NamedFontFace>& namedFaces,
                         const std::string& authorOverrides,
                         float layoutScale) {
    std::string css;
    char buf[256];
    const char* textColor = "#000";
    auto addFace = [&css](const char* family, const char* weight,
                          const std::string& file) {
        if (file.empty()) return;
        css += "@font-face { font-family: ";
        css += family;
        css += "; font-weight: ";
        css += weight;
        css += "; src: url(\"";
        css += file;
        css += "\"); }\n";
    };
    auto addItalicFace = [&css](const char* family, const char* weight,
                                const std::string& file) {
        if (file.empty()) return;
        css += "@font-face { font-family: ";
        css += family;
        css += "; font-style: italic; font-weight: ";
        css += weight;
        css += "; src: url(\"";
        css += file;
        css += "\"); }\n";
    };
    addFace("serif", "normal", regularFile);
    addFace("serif", "bold", boldFile.empty() ? regularFile : boldFile);
    const std::string sansNormal = sansFile.empty()
                                 ? (boldFile.empty() ? regularFile : boldFile)
                                 : sansFile;
    addFace("sans-serif", "normal", sansNormal);
    addFace("sans-serif", "bold", sansNormal);
    addFace("monospace", "normal", monoFile);
    addFace("monospace", "bold", monoFile);

    const std::string serifItalic = italicFile.empty() ? regularFile : italicFile;
    addItalicFace("serif", "normal", serifItalic);
    addItalicFace("serif", "bold",
                  italicFile.empty() ? (boldFile.empty() ? regularFile : boldFile)
                                     : italicFile);
    const std::string sansItalic = italicFile.empty() ? sansNormal : italicFile;
    addItalicFace("sans-serif", "normal", sansItalic);
    addItalicFace("sans-serif", "bold", sansItalic);

    for (const auto& face : namedFaces) {
        if (face.family.empty() || face.file.empty()) continue;
        css += "@font-face { font-family: \"";
        css += cssQuoted(face.family);
        css += "\"; font-weight: ";
        css += face.bold ? "bold" : "normal";
        css += "; src: url(\"";
        css += face.file;
        css += "\"); }\n";
    }

    if (!kaiFile.empty()) {
        for (const char* fam : {"kaiti", "kai", "楷体", "仿宋", "yuanti"})
            addFace(fam, "normal", kaiFile);
    }
    if (!sansNormal.empty()) {
        for (const char* fam : {"heiti", "hei", "黑体", "fzlth", "DK-HEITI"}) {
            addFace(fam, "normal", sansNormal);
            addFace(fam, "bold", sansNormal);
        }
    }

    std::snprintf(buf, sizeof(buf),
                  "html, body { font-family: serif !important; font-size: %.2fpx !important; "
                  "font-weight: normal !important; font-style: normal !important; "
                  "text-decoration: none !important; "
                  "color: %s !important; background-color: transparent !important; }\n",
                  opts.fontSize / layoutScale, textColor);
    css += buf;
    const bool densePreset = (opts.preset == "web");
    css += "body * { font-family: inherit !important; "
           "letter-spacing: normal !important; "
           "word-spacing: normal !important; color: ";
    css += textColor;
    css += " !important; background-color: transparent !important; ";
    css += densePreset
             ? "margin: 0 !important; "
             : "margin-left: 0 !important; margin-right: 0 !important; ";
    css += "padding: 0 !important; }\n";
    css += "td, th { padding: 0.15em 0.35em !important; }\n";
    css += "pre, code, tt, kbd, samp { font-family: monospace !important; }\n";
    css += "p, div, section, article, header, footer, blockquote { display: block !important; }\n";
    css += "span, a, em, i, b, strong, sup, sub, small, code, ruby, rb { "
           "display: inline !important; }\n";
    css += "h1, h2, h3, h4, h5, h6 { display: block !important; font-weight: bold !important; "
           "font-style: normal !important; text-indent: 0 !important; "
           "text-decoration: none !important; }\n";
    css += "h1 { font-size: 2.00em !important; margin: 0.85em 0 0.50em !important; }\n";
    css += "h2 { font-size: 1.70em !important; margin: 0.80em 0 0.45em !important; }\n";
    css += "h3 { font-size: 1.45em !important; margin: 0.75em 0 0.40em !important; }\n";
    css += "h4 { font-size: 1.25em !important; margin: 0.70em 0 0.35em !important; }\n";
    css += "h5, h6 { font-size: 1.10em !important; margin: 0.65em 0 0.30em !important; }\n";
    css += "strong, b { font-weight: 900 !important; font-style: inherit !important; }\n";
    css += "em, i, cite, var { font-style: italic !important; font-weight: inherit !important; }\n";
    css += "u, ins { text-decoration: underline !important; }\n";
    css += "a { text-decoration: underline !important; color: ";
    css += textColor;
    css += " !important; }\n";
    css += "s, strike, del { text-decoration: line-through !important; }\n";
    css += "small { font-size: 0.80em !important; }\n";
    css += "sup { font-size: 0.65em !important; vertical-align: super !important; }\n";
    css += "sub { font-size: 0.65em !important; vertical-align: sub !important; }\n";
    css += "blockquote { margin: 0 2em 0.60em !important; }\n";
    css += "ul, ol { display: block !important; margin: 0 0 0.60em !important; "
           "padding-left: 2em !important; }\n";
    css += "li { display: list-item !important; margin: 0 !important; }\n";

    css += ".kaiti { font-family: kaiti !important; }\n";
    css += ".heiti { font-family: heiti !important; }\n";
    css += ".songti { font-family: songti !important; }\n";
    css += ".yuanti { font-family: yuanti !important; }\n";
    css += "div.fright.round.shadow { float: right !important; "
           "display: block !important; box-sizing: border-box !important; "
           "width: 44% !important; margin: 0.25em 0 0.85em 6% !important; "
           "padding: 0.72em !important; background-color: #fff !important; "
           "border: 1px solid #bdbdbd !important; border-radius: 0.70em "
           "!important; box-shadow: 0.08em 0.08em 0.12em #c8c8c8 !important; }\n";
    css += "div.fright.round.shadow p { text-align: justify !important; }\n";
    css += "div.fright.round.shadow p.right { text-align: right !important; "
           "text-indent: 0 !important; }\n";

    css += "rt { font-size: 0.55em !important; vertical-align: super !important; }\n";
    css += "rp { display: none !important; }\n";

    css += "@page { margin: 0 !important; }\n";
    css += "body { margin: 0 !important; padding: 0 !important; }\n";
    css += "svg { width: 100% !important; height: auto !important; }\n";
    css += "img { max-width: 100% !important; height: auto !important; }\n";
    std::snprintf(buf, sizeof(buf),
                  "* { line-height: %.3f !important; }\n",
                  opts.lineHeightMul);
    css += buf;
    if (opts.drawRuling) {
        css += "p { margin-top: 0 !important; margin-bottom: 0 !important; }\n";
    } else {
        const double paraEm = opts.paragraphSpacing / opts.fontSize;
        std::snprintf(buf, sizeof(buf),
                      "p { margin-top: 0 !important; margin-bottom: %.3fem !important; }\n",
                      paraEm);
        css += buf;
    }
    if (opts.firstLineIndentChars != 0.0) {
        std::snprintf(buf, sizeof(buf),
                      "p { text-indent: %.2fem !important; }\n",
                      opts.firstLineIndentChars);
        css += buf;
    }
    css += authorOverrides;
    return css;
}

fz_font* makeFont(fz_context* ctx, const char* name,
                  const std::vector<uint8_t>& memData,
                  const std::string& path,
                  std::vector<uint8_t>* fileBuf) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!memData.empty()) {
        data = memData.data();
        size = memData.size();
    } else if (!path.empty() && readFileBytes(path, fileBuf)) {
        data = fileBuf->data();
        size = fileBuf->size();
    }
    if (!data) return nullptr;
    return fz_new_font_from_memory(ctx, name, data, (int)size, 0, 0);
}

void walkOutline(fz_context* ctx, fz_document* doc, fz_outline* ol, int depth,
                 std::vector<HeadingAnchor>* out) {
    for (; ol; ol = ol->next) {
        if (ol->title && *ol->title) {
            fz_location loc = {-1, -1};
            if (ol->uri) {
                fz_var(loc);
                fz_try(ctx) {
                    loc = fz_resolve_link(ctx, doc, ol->uri,
                                          nullptr, nullptr);
                } fz_catch(ctx) {
                    loc = ol->page;
                }
            } else {
                loc = ol->page;
            }
            int page0 = fz_page_number_from_location(ctx, doc, loc);
            if (page0 >= 0) {
                HeadingAnchor a;
                a.title = ol->title;
                a.level = depth;
                a.pageIndex0 = page0;
                a.yOnPage = 0.0f;
                out->push_back(std::move(a));
            }
        }
        if (ol->down) walkOutline(ctx, doc, ol->down, depth + 1, out);
    }
}

bool mupdfPageHasContent(fz_context* ctx, fz_document* doc, int pageIndex0) {
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* device = nullptr;
    bool content = false;
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, pageIndex0);
        const fz_rect pb = fz_bound_page(ctx, page);
        const float pw = std::max(1.0f, pb.x1 - pb.x0);
        const float ph = std::max(1.0f, pb.y1 - pb.y0);
        pix = fz_new_pixmap(ctx, fz_device_gray(ctx), 64, 64, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        device = fz_new_draw_device(ctx, fz_scale(64.0f / pw, 64.0f / ph), pix);
        fz_run_page(ctx, page, device, fz_identity, nullptr);
        fz_close_device(ctx, device);
        for (int y = 0; y < pix->h && !content; ++y) {
            for (int x = 0; x < pix->w; ++x) {
                if (pix->samples[(size_t)y * pix->stride + x] < 250) {
                    content = true;
                    break;
                }
            }
        }
    } fz_catch(ctx) {
        content = true;
    }
    if (device) fz_drop_device(ctx, device);
    if (pix) fz_drop_pixmap(ctx, pix);
    if (page) fz_drop_page(ctx, page);
    return content;
}

}

bool MupdfEpubEngine::open(const std::filesystem::path& epubPath,
                           const Options& opts, Logger* log) {
    close();
    Impl* impl = impl_.get();
    impl->log = log;

    impl->pageW = opts.pageWidth;
    impl->pageH = opts.pageHeight;
    impl->marginL = opts.marginLeft;
    impl->marginT = opts.marginTop;
    {
        float d = (float)opts.pageWidth / 720.0f;
        if (d < 1.0f) d = 1.0f;
        if (d > 3.0f) d = 3.0f;
        impl->dpr = d;
    }
    impl->noteMode = opts.preset == "note";
    impl->hasBackground = !opts.backgroundPath.empty()
                       && loadPageBackground(opts, log, &impl->background);
    impl->lockContext.user = &impl->locks;
    impl->lockContext.lock = mupdfLock;
    impl->lockContext.unlock = mupdfUnlock;

    impl->ctx = fz_new_context(nullptr, &impl->lockContext, FZ_STORE_DEFAULT);
    if (!impl->ctx) {
        impl->logMsg(LogLevel::Error, "fz_new_context failed");
        return false;
    }
    fz_context* ctx = impl->ctx;
    impl->warnings.log = log;
    impl->warnings.seen.clear();
    fz_set_warning_callback(ctx, mupdfWarning, &impl->warnings);
    fz_register_document_handlers(ctx);

    const std::string regularFile =
        ensureFontFile(opts.fontPath, opts.fontData, "regular", log);
    const std::string boldFile =
        ensureFontFile(opts.fontPathBold, opts.fontDataBold, "bold", log);
    const std::string italicFile =
        ensureFontFile(opts.fontPathItalic, opts.fontDataItalic, "italic", log);
    const std::string monoFile =
        ensureFontFile(opts.fontPathMono, opts.fontDataMono, "mono", log);
    const std::string sansFile =
        ensureFontFile(opts.fontPathSans, opts.fontDataSans, "sans", log);
    const std::string kaiFile =
        ensureFontFile(opts.fontPathKai, opts.fontDataKai, "kai", log);
    if (regularFile.empty())
        impl->logMsg(LogLevel::Warn,
                     "no usable regular font file; falling back to builtin fonts");
    std::string authorOverrides;
    const auto namedFaces = discoverNamedFonts(epubPath, regularFile, boldFile,
                                               sansFile, kaiFile, monoFile, log,
                                               &authorOverrides);
    const std::string userCss = buildUserCss(opts, regularFile, boldFile,
                                             italicFile, monoFile, sansFile,
                                             kaiFile, namedFaces, authorOverrides,
                                             impl->dpr);
    impl->logMsg(LogLevel::Info, "epub-mupdf build " __DATE__ " " __TIME__
                                 " (mono=builtin-courier)");
    impl->logMsg(LogLevel::Info,
                 (std::string("user CSS:\n") + userCss).c_str());
    fz_set_user_css(ctx, userCss.c_str());
    g_activeEngine = this;
    fz_install_load_system_font_funcs(ctx,
                                      hardcopy_load_system_font,
                                      hardcopy_load_cjk_font,
                                      hardcopy_load_fallback_font);

    fz_outline* ol = nullptr;
    bool ok = false;

    fz_var(ol);
    fz_try(ctx) {
        impl->fRegular = makeFont(ctx, "hardcopy-regular",
                                  opts.fontData, opts.fontPath,
                                  &impl->bufRegular);
        if (!impl->fRegular)
            fz_throw(ctx, FZ_ERROR_GENERIC, "cannot load regular font");
        impl->fBold = makeFont(ctx, "hardcopy-bold",
                               opts.fontDataBold, opts.fontPathBold,
                               &impl->bufBold);
        impl->fSans = makeFont(ctx, "hardcopy-sans",
                               opts.fontDataSans, opts.fontPathSans,
                               &impl->bufSans);
        impl->fKai = makeFont(ctx, "hardcopy-kai",
                              opts.fontDataKai, opts.fontPathKai,
                              &impl->bufKai);
        impl->fFallback = makeFont(ctx, "hardcopy-fallback",
                                   opts.fontDataFallback, opts.fontPathFallback,
                                   &impl->bufFallback);

        impl->doc = openMupdfDocument(ctx, epubPath, log);
        if (!impl->doc)
            fz_throw(ctx, FZ_ERROR_GENERIC, "fz_open_document returned NULL");
        if (!fz_is_document_reflowable(ctx, impl->doc))
            fz_throw(ctx, FZ_ERROR_GENERIC, "document is not reflowable");

        char mbuf[1024];
        int n = fz_lookup_metadata(ctx, impl->doc, FZ_META_INFO_TITLE,
                                   mbuf, sizeof(mbuf));
        if (n > 0) title.assign(mbuf, (size_t)(n - 1));
        n = fz_lookup_metadata(ctx, impl->doc, FZ_META_INFO_AUTHOR,
                               mbuf, sizeof(mbuf));
        if (n > 0) author.assign(mbuf, (size_t)(n - 1));

        const float contentW = (float)(opts.pageWidth
                                       - opts.marginLeft - opts.marginRight);
        const float contentH = (float)(opts.pageHeight - opts.marginTop
                                       - footerContentBottomInsetPx(opts));
        fz_layout_document(ctx, impl->doc, contentW / impl->dpr,
                           contentH / impl->dpr,
                           (float)opts.fontSize / impl->dpr);
        impl->clipTop = std::max(0, opts.marginTop);
        impl->clipBottom = std::min(opts.pageHeight,
                                    (int)std::lround(opts.marginTop + contentH));
        impl->pages = fz_count_pages(ctx, impl->doc);
        if (impl->pages <= 0)
            fz_throw(ctx, FZ_ERROR_GENERIC, "layout produced no pages");

        impl->pageMap.clear();
        impl->rawToOut.assign((size_t)impl->pages, -1);
        for (int raw = 0; raw < impl->pages; ++raw) {
            if (mupdfPageHasContent(ctx, impl->doc, raw)) {
                impl->pageMap.push_back(raw);
            }
        }
        if (impl->pageMap.empty()) impl->pageMap.push_back(0);
        {
            int next = (int)impl->pageMap.size() - 1;
            size_t k = impl->pageMap.size();
            for (int raw = impl->pages - 1; raw >= 0; --raw) {
                while (k > 0 && impl->pageMap[k - 1] >= raw) next = (int)--k;
                impl->rawToOut[(size_t)raw] = next;
            }
        }
        const int skipped = impl->pages - (int)impl->pageMap.size();
        if (skipped > 0) {
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "mupdf epub skipped %d empty page(s)", skipped);
            impl->logMsg(LogLevel::Info, msg);
        }

        ol = fz_load_outline(ctx, impl->doc);
        if (ol) {
            walkOutline(ctx, impl->doc, ol, 1, &impl->outline);
            fz_drop_outline(ctx, ol);
            ol = nullptr;
        }
        {
            std::vector<HeadingAnchor> adjusted;
            for (auto a : impl->outline) {
                a.pageIndex0 = impl->outPage(a.pageIndex0);
                if (a.pageIndex0 >= 0) adjusted.push_back(std::move(a));
            }
            impl->outline = std::move(adjusted);
        }

        ok = true;
    } fz_catch(ctx) {
        impl->logMsg(LogLevel::Warn, fz_caught_message(ctx));
        ok = false;
    }
    if (ol) fz_drop_outline(ctx, ol);

    if (!ok) {
        close();
        return false;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "mupdf epub layout: %d pages (%dx%d)",
                  (int)impl->pageMap.size(), impl->pageW, impl->pageH);
    impl->logMsg(LogLevel::Info, buf);
    if (!impl->outline.empty()) {
        std::snprintf(buf, sizeof(buf), "mupdf epub navigation entries: %zu",
                      impl->outline.size());
        impl->logMsg(LogLevel::Info, buf);
    }
    return true;
}

void MupdfEpubEngine::close() {
    Impl* impl = impl_.get();
    if (!impl->ctx) return;
    fz_context* ctx = impl->ctx;

    fz_install_load_system_font_funcs(ctx, nullptr, nullptr, nullptr);
    if (g_activeEngine == this) g_activeEngine = nullptr;

    if (impl->fRegular)  { fz_drop_font(ctx, impl->fRegular);  impl->fRegular = nullptr; }
    if (impl->fBold)     { fz_drop_font(ctx, impl->fBold);     impl->fBold = nullptr; }
    if (impl->fSans)     { fz_drop_font(ctx, impl->fSans);     impl->fSans = nullptr; }
    if (impl->fKai)      { fz_drop_font(ctx, impl->fKai);      impl->fKai = nullptr; }
    if (impl->fFallback) { fz_drop_font(ctx, impl->fFallback); impl->fFallback = nullptr; }
    if (impl->doc)       { fz_drop_document(ctx, impl->doc);   impl->doc = nullptr; }

    fz_drop_context(ctx);
    impl->ctx = nullptr;
    impl->pages = 0;
    impl->pageMap.clear();
    impl->rawToOut.clear();
    impl->outline.clear();
}

int MupdfEpubEngine::pageCount() const {
    return impl_->doc ? (int)impl_->pageMap.size() : 0;
}

const std::vector<HeadingAnchor>& MupdfEpubEngine::outline() const {
    return impl_->outline;
}

namespace {

void clearPagePixmap(fz_context* ctx, fz_pixmap* pix, const Bitmap8* bg) {
    if (!bg) {
        fz_clear_pixmap_with_value(ctx, pix, 255);
        return;
    }
    for (int y = 0; y < bg->height; ++y) {
        std::memcpy(pix->samples + (ptrdiff_t)y * pix->stride,
                    bg->pixels.data() + (size_t)y * bg->width,
                    (size_t)bg->width);
    }
}

}

bool MupdfEpubEngine::renderPageInContext(void* ctxOpaque, int pageIndex0,
                                           Bitmap8* out,
                                           std::vector<ImageRect>* imageRects) {
    Impl* impl = impl_.get();
    fz_context* ctx = static_cast<fz_context*>(ctxOpaque);
    if (!ctx || !impl->doc || !out) return false;

    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    bool ok = false;

    fz_var(page);
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx) {
        page = fz_load_page(ctx, impl->doc, impl->rawPage(pageIndex0));
        pix = fz_new_pixmap(ctx, fz_device_gray(ctx),
                            impl->pageW, impl->pageH, nullptr, 0);
        clearPagePixmap(ctx, pix,
                        impl->hasBackground ? &impl->background : nullptr);
        {
            const fz_irect clip = impl->contentClip();
            dev = fz_new_draw_device_with_bbox(ctx, impl->pageCtm(), pix, &clip);
        }
        fz_run_page(ctx, page, dev, fz_identity, nullptr);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);
        dev = nullptr;
        if (imageRects) {
            fz_stext_page* sp = nullptr;
            fz_var(sp);
            fz_try(ctx) {
                fz_stext_options sopts{};
                sopts.flags = FZ_STEXT_PRESERVE_IMAGES;
                sp = fz_new_stext_page_from_page_number(
                    ctx, impl->doc, impl->rawPage(pageIndex0), &sopts);
                imageRectsFromStextPage(ctx, sp, imageRects);
            } fz_catch(ctx) {}
            if (sp) fz_drop_stext_page(ctx, sp);
        }
        fz_drop_page(ctx, page);
        page = nullptr;
        ok = true;
    } fz_catch(ctx) {
        impl->logMsg(LogLevel::Error, fz_caught_message(ctx));
    }
    if (dev)  fz_drop_device(ctx, dev);
    if (page) fz_drop_page(ctx, page);

    if (!ok) {
        if (pix) fz_drop_pixmap(ctx, pix);
        return false;
    }

    out->width = impl->pageW;
    out->height = impl->pageH;
    out->pixels.resize((size_t)impl->pageW * impl->pageH);
    for (int y = 0; y < impl->pageH; ++y) {
        const uint8_t* src = pix->samples + (ptrdiff_t)y * pix->stride;
        std::memcpy(out->pixels.data() + (size_t)y * impl->pageW,
                    src, (size_t)impl->pageW);
    }
    fz_drop_pixmap(ctx, pix);
    if (impl->noteMode) drawNoteDivider(out);
    return true;
}

void MupdfEpubEngine::textLayerInContext(void* ctxOpaque, int pageIndex0,
                                          std::vector<PlacedGlyph>* out) {
    Impl* impl = impl_.get();
    fz_context* ctx = static_cast<fz_context*>(ctxOpaque);
    if (!ctx || !impl->doc || !out) return;

    fz_stext_page* sp = nullptr;
    fz_var(sp);
    fz_try(ctx) {
        fz_stext_options sopts{};
        sp = fz_new_stext_page_from_page_number(
            ctx, impl->doc, impl->rawPage(pageIndex0),
                                                &sopts);
    } fz_catch(ctx) {
        impl->logMsg(LogLevel::Warn, fz_caught_message(ctx));
    }
    if (!sp) return;

    const float ox = (float)impl->marginL;
    const float oy = (float)impl->marginT;
    const float k = impl->dpr;
    uint32_t blockId = 0;
    for (fz_stext_block* b = sp->first_block; b; b = b->next, ++blockId) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
            for (fz_stext_char* ch = l->first_char; ch; ch = ch->next) {
                if (ch->c <= 0 || ch->c == 0xFFFD) continue;
                PlacedGlyph g{};
                g.glyphIndex = 0;
                g.x = ch->origin.x * k + ox;
                g.y = ch->origin.y * k + oy;
                g.advance = k * std::hypot(ch->quad.ur.x - ch->quad.ul.x,
                                           ch->quad.ur.y - ch->quad.ul.y);
                if (ch->font == impl->fBold)      g.fontSlot = FontSlot::Bold;
                else if (ch->font == impl->fSans) g.fontSlot = FontSlot::Sans;
                else if (ch->font == impl->fKai)  g.fontSlot = FontSlot::Kai;
                else if (ch->font == impl->fFallback)
                                                   g.fontSlot = FontSlot::Fallback;
                else                               g.fontSlot = FontSlot::Regular;
                g.fontPx = ch->size * k;
                g.textBlockId = blockId;
                g.blockX0 = b->bbox.x0 * k + ox;
                g.blockY0 = b->bbox.y0 * k + oy;
                g.blockX1 = b->bbox.x1 * k + ox;
                g.blockY1 = b->bbox.y1 * k + oy;
                appendUtf8(g.sourceUtf8, (uint32_t)ch->c);
                out->push_back(std::move(g));
            }
        }
    }
    fz_drop_stext_page(ctx, sp);
}

void MupdfEpubEngine::textLayerFromDisplayListInContext(
        void* ctxOpaque, void* listOpaque, std::vector<PlacedGlyph>* out) {
    Impl* impl = impl_.get();
    fz_context* ctx = static_cast<fz_context*>(ctxOpaque);
    fz_display_list* list = static_cast<fz_display_list*>(listOpaque);
    if (!ctx || !list || !out) return;

    fz_stext_page* sp = nullptr;
    fz_var(sp);
    fz_try(ctx) {
        fz_stext_options sopts{};
        sp = fz_new_stext_page_from_display_list(ctx, list, &sopts);
    } fz_catch(ctx) {
        impl->logMsg(LogLevel::Warn, fz_caught_message(ctx));
    }
    if (!sp) return;

    const float ox = (float)impl->marginL;
    const float oy = (float)impl->marginT;
    const float k = impl->dpr;
    uint32_t blockId = 0;
    for (fz_stext_block* b = sp->first_block; b; b = b->next, ++blockId) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
            for (fz_stext_char* ch = l->first_char; ch; ch = ch->next) {
                if (ch->c <= 0 || ch->c == 0xFFFD) continue;
                PlacedGlyph g{};
                g.glyphIndex = 0;
                g.x = ch->origin.x * k + ox;
                g.y = ch->origin.y * k + oy;
                g.advance = k * std::hypot(ch->quad.ur.x - ch->quad.ul.x,
                                           ch->quad.ur.y - ch->quad.ul.y);
                if (ch->font == impl->fBold)      g.fontSlot = FontSlot::Bold;
                else if (ch->font == impl->fSans) g.fontSlot = FontSlot::Sans;
                else if (ch->font == impl->fKai)  g.fontSlot = FontSlot::Kai;
                else if (ch->font == impl->fFallback)
                                                   g.fontSlot = FontSlot::Fallback;
                else                               g.fontSlot = FontSlot::Regular;
                g.fontPx = ch->size * k;
                g.textBlockId = blockId;
                g.blockX0 = b->bbox.x0 * k + ox;
                g.blockY0 = b->bbox.y0 * k + oy;
                g.blockX1 = b->bbox.x1 * k + ox;
                g.blockY1 = b->bbox.y1 * k + oy;
                appendUtf8(g.sourceUtf8, (uint32_t)ch->c);
                out->push_back(std::move(g));
            }
        }
    }
    fz_drop_stext_page(ctx, sp);
}

void MupdfEpubEngine::imageRectsFromStextPage(
        void* ctxOpaque, void* stextOpaque, std::vector<ImageRect>* out) {
    if (!out) return;
    Impl* impl = impl_.get();
    fz_context* ctx = static_cast<fz_context*>(ctxOpaque);
    fz_stext_page* sp = static_cast<fz_stext_page*>(stextOpaque);
    if (!ctx || !sp) return;
    const float ox = (float)impl->marginL;
    const float oy = (float)impl->marginT;
    for (fz_stext_block* b = sp->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_IMAGE) continue;
        ImageRect r;
        const float k = impl->dpr;
        r.x0 = (int)std::floor(b->bbox.x0 * k + ox);
        r.y0 = (int)std::floor(b->bbox.y0 * k + oy);
        r.x1 = (int)std::ceil(b->bbox.x1 * k + ox);
        r.y1 = (int)std::ceil(b->bbox.y1 * k + oy);
        if (r.x1 > r.x0 && r.y1 > r.y0) out->push_back(r);
    }
}

bool MupdfEpubEngine::renderPage(int pageIndex0, Bitmap8* out,
                                 std::vector<ImageRect>* imageRects) {
    return renderPageInContext(impl_->ctx, pageIndex0, out, imageRects);
}

void MupdfEpubEngine::textLayer(int pageIndex0, std::vector<PlacedGlyph>* out) {
    textLayerInContext(impl_->ctx, pageIndex0, out);
}

void MupdfEpubEngine::links(int pageIndex0, std::vector<PdfLink>* out) {
    if (!out) return;
    out->clear();
    Impl* impl = impl_.get();
    if (!impl->ctx || !impl->doc || pageIndex0 < 0
        || pageIndex0 >= pageCount()) return;
    fz_page* page = nullptr;
    fz_link* list = nullptr;
    fz_var(page);
    fz_var(list);
    fz_try(impl->ctx) {
        page = fz_load_page(impl->ctx, impl->doc,
                            impl->rawPage(pageIndex0));
        list = fz_load_links(impl->ctx, page);
        for (fz_link* link = list; link; link = link->next) {
            if (!link->uri) continue;
            PdfLink item;
            item.x0 = link->rect.x0 * impl->dpr + impl->marginL;
            item.y0 = link->rect.y0 * impl->dpr + impl->marginT;
            item.x1 = link->rect.x1 * impl->dpr + impl->marginL;
            item.y1 = link->rect.y1 * impl->dpr + impl->marginT;
            item.uri = link->uri;
            int target = -1;
            fz_try(impl->ctx) {
                const fz_link_dest dest = fz_resolve_link_dest(
                    impl->ctx, impl->doc, link->uri);
                target = fz_page_number_from_location(
                    impl->ctx, impl->doc, dest.loc);
            } fz_catch(impl->ctx) {}
            target = impl->outPage(target);
            if (target >= 0 && target < pageCount()) {
                item.targetPage0 = target;
                item.uri.clear();
            }
            out->push_back(std::move(item));
        }
    } fz_catch(impl->ctx) {
        impl->logMsg(LogLevel::Warn, fz_caught_message(impl->ctx));
        out->clear();
    }
    if (list) fz_drop_link(impl->ctx, list);
    if (page) fz_drop_page(impl->ctx, page);
}

MupdfEpubEngine::PageDisplayList::PageDisplayList(MupdfEpubEngine* engine)
    : engine_(engine) {}

MupdfEpubEngine::PageDisplayList::~PageDisplayList() {
    if (engine_ && engine_->impl_->ctx && list_)
        fz_drop_display_list(engine_->impl_->ctx,
                             static_cast<fz_display_list*>(list_));
}

std::shared_ptr<MupdfEpubEngine::PageDisplayList>
MupdfEpubEngine::preparePage(int pageIndex0, std::vector<PlacedGlyph>* glyphs) {
    Impl* impl = impl_.get();
    if (!impl->ctx || !impl->doc) return nullptr;
    auto page = std::shared_ptr<PageDisplayList>(new PageDisplayList(this));
    fz_try(impl->ctx) {
        page->list_ = fz_new_display_list_from_page_number(impl->ctx, impl->doc,
                                                            impl->rawPage(pageIndex0));
    } fz_catch(impl->ctx) {
        impl->logMsg(LogLevel::Error, fz_caught_message(impl->ctx));
    }
    if (!page->list_) return nullptr;
    if (glyphs) textLayerFromDisplayListInContext(impl->ctx, page->list_, glyphs);
    return page;
}

MupdfEpubEngine::PageRenderer::PageRenderer(MupdfEpubEngine* engine)
    : engine_(engine) {
    if (engine_ && engine_->impl_->ctx)
        ctx_ = fz_clone_context(engine_->impl_->ctx);
}

MupdfEpubEngine::PageRenderer::~PageRenderer() {
    if (ctx_) fz_drop_context(static_cast<fz_context*>(ctx_));
}

bool MupdfEpubEngine::PageRenderer::renderPage(
        const PageDisplayList& pageList, Bitmap8* out,
        std::vector<ImageRect>* imageRects) {
    if (!engine_ || !ctx_ || !pageList.list_ || !out) return false;
    Impl* impl = engine_->impl_.get();
    fz_context* ctx = static_cast<fz_context*>(ctx_);
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    bool ok = false;

    fz_var(pix);
    fz_var(dev);
    fz_try(ctx) {
        pix = fz_new_pixmap(ctx, fz_device_gray(ctx), impl->pageW, impl->pageH,
                            nullptr, 0);
        clearPagePixmap(ctx, pix,
                        impl->hasBackground ? &impl->background : nullptr);
        {
            const fz_irect clip = impl->contentClip();
            dev = fz_new_draw_device_with_bbox(ctx, impl->pageCtm(), pix, &clip);
        }
        fz_run_display_list(ctx, static_cast<fz_display_list*>(pageList.list_), dev,
                            fz_identity, fz_infinite_rect, nullptr);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);
        dev = nullptr;
        if (imageRects) {
            fz_stext_page* sp = nullptr;
            fz_var(sp);
            fz_try(ctx) {
                fz_stext_options sopts{};
                sopts.flags = FZ_STEXT_PRESERVE_IMAGES;
                sp = fz_new_stext_page_from_display_list(
                    ctx, static_cast<fz_display_list*>(pageList.list_), &sopts);
                engine_->imageRectsFromStextPage(ctx, sp, imageRects);
            } fz_catch(ctx) {}
            if (sp) fz_drop_stext_page(ctx, sp);
        }
        ok = true;
    } fz_catch(ctx) {
        impl->logMsg(LogLevel::Error, fz_caught_message(ctx));
    }
    if (dev) fz_drop_device(ctx, dev);
    if (!ok) {
        if (pix) fz_drop_pixmap(ctx, pix);
        return false;
    }

    out->width = impl->pageW;
    out->height = impl->pageH;
    out->pixels.resize((size_t)impl->pageW * impl->pageH);
    for (int y = 0; y < impl->pageH; ++y) {
        const uint8_t* src = pix->samples + (ptrdiff_t)y * pix->stride;
        std::memcpy(out->pixels.data() + (size_t)y * impl->pageW,
                    src, (size_t)impl->pageW);
    }
    fz_drop_pixmap(ctx, pix);
    if (impl->noteMode) drawNoteDivider(out);
    return true;
}

std::unique_ptr<MupdfEpubEngine::PageRenderer>
MupdfEpubEngine::createPageRenderer() {
    auto renderer = std::unique_ptr<PageRenderer>(new PageRenderer(this));
    return renderer->isReady() ? std::move(renderer) : nullptr;
}

namespace {
fz_context* hbGlueContext() {
    static fz_context* s_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    return s_ctx;
}
int s_hbLockDepth = 0;
}

void hbMupdfLock() {
    if (s_hbLockDepth++ == 0) fz_hb_lock(hbGlueContext());
}

void hbMupdfUnlock() {
    if (--s_hbLockDepth == 0) fz_hb_unlock(hbGlueContext());
}

}
