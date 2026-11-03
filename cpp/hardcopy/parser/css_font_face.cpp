#include "css_font_face.h"

#include <cctype>
#include <cstdlib>

namespace hardcopy {

namespace {

std::string trimWs(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string unquote(const std::string& s) {
    std::string t = trimWs(s);
    if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')
        && t.back() == t.front()) {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

std::string stripCssComments(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            size_t end = css.find("*/", i + 2);
            if (end == std::string::npos) break;
            i = end + 2;
            continue;
        }
        out.push_back(css[i++]);
    }
    return out;
}

std::string toLowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

void parseSrc(const std::string& value, FontFaceDecl* decl) {
    size_t i = 0;
    while (i < value.size()) {
        size_t open = value.find('(', i);
        if (open == std::string::npos) break;
        size_t idStart = open;
        while (idStart > i && (std::isalnum((unsigned char)value[idStart - 1])
                               || value[idStart - 1] == '-')) --idStart;
        std::string fn = toLowerAscii(trimWs(value.substr(idStart, open - idStart)));
        size_t close = value.find(')', open + 1);
        if (close == std::string::npos) break;
        std::string arg = unquote(value.substr(open + 1, close - open - 1));
        if (fn == "url" && !arg.empty())        decl->urls.push_back(arg);
        else if (fn == "local" && !arg.empty()) decl->localNames.push_back(arg);
        i = close + 1;
    }
}

void parseFaceBlock(const std::string& body, std::vector<FontFaceDecl>* out) {
    FontFaceDecl decl;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t semi = body.find(';', pos);
        std::string d = body.substr(pos, semi == std::string::npos
                                          ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? body.size() : semi + 1;

        size_t colon = d.find(':');
        if (colon == std::string::npos) continue;
        std::string prop  = toLowerAscii(trimWs(d.substr(0, colon)));
        std::string value = trimWs(d.substr(colon + 1));
        if (prop == "font-family") {
            decl.family = unquote(value);
        } else if (prop == "src") {
            parseSrc(value, &decl);
        } else if (prop == "font-style") {
            std::string v = toLowerAscii(value);
            decl.italic = v.rfind("italic", 0) == 0 || v.rfind("oblique", 0) == 0;
        } else if (prop == "font-weight") {
            std::string v = toLowerAscii(value);
            if (v == "bold" || v == "bolder") decl.bold = true;
            else {
                int n = std::atoi(v.c_str());
                if (n >= 600) decl.bold = true;
            }
        }
    }
    if (!decl.family.empty()) out->push_back(std::move(decl));
}

}

void collectFontFaces(const std::string& rawCss, std::vector<FontFaceDecl>* out) {
    if (!out) return;
    const std::string css = stripCssComments(rawCss);
    size_t i = 0;
    while (i < css.size()) {
        size_t at = css.find('@', i);
        if (at == std::string::npos) break;
        static const char kw[] = "font-face";
        size_t k = at + 1;
        size_t m = 0;
        while (m < sizeof(kw) - 1 && k < css.size()
               && std::tolower((unsigned char)css[k]) == kw[m]) { ++k; ++m; }
        if (m != sizeof(kw) - 1) { i = at + 1; continue; }

        size_t open = css.find('{', k);
        if (open == std::string::npos) break;
        int depth = 1;
        size_t j = open + 1;
        while (j < css.size() && depth > 0) {
            if (css[j] == '{') ++depth;
            else if (css[j] == '}') --depth;
            ++j;
        }
        parseFaceBlock(css.substr(open + 1, j - open - 2), out);
        i = j;
    }
}

std::string fileStemKey(const std::string& zipEntryName) {
    size_t slash = zipEntryName.find_last_of("/\\");
    std::string name = (slash == std::string::npos)
                       ? zipEntryName : zipEntryName.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    std::string dec;
    dec.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size()
            && std::isxdigit((unsigned char)name[i + 1])
            && std::isxdigit((unsigned char)name[i + 2])) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                return std::tolower((unsigned char)c) - 'a' + 10;
            };
            dec.push_back((char)(hex(name[i + 1]) * 16 + hex(name[i + 2])));
            i += 2;
        } else {
            dec.push_back(name[i]);
        }
    }
    return normalizeFamilyKey(dec);
}

}
