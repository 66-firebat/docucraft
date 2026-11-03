#include "css_subset.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hardcopy {

namespace {

std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string stripComments(const std::string& css) {
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

bool parseLengthEm(const std::string& raw, float* outEm, bool percentAsMul = false) {
    std::string v = toLower(trim(raw));
    if (v.empty() || v == "auto" || v == "normal" || v == "inherit"
        || v == "initial" || v == "unset") return false;
    char* end = nullptr;
    double num = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) return false;
    std::string unit = trim(std::string(end));
    double em;
    if (unit.empty()) {
        em = num;
    } else if (unit == "em" || unit == "rem" || unit == "pc") {
        em = num;
    } else if (unit == "ex") {
        em = num * 0.5;
    } else if (unit == "px") {
        em = num / 16.0;
    } else if (unit == "pt") {
        em = num / 12.0;
    } else if (unit == "%") {
        if (!percentAsMul) return false;
        em = num / 100.0;
    } else {
        return false;
    }
    *outEm = (float)em;
    return true;
}

float clampSpacing(float em)   { return std::min(std::max(em, 0.0f), 10.0f); }
float clampIndent(float em)    { return std::min(std::max(em, 0.0f), 8.0f); }
float clampLineHeight(float m) { return std::min(std::max(m, 0.5f), 3.0f); }
float clampFontSize(float em)  { return std::min(std::max(em, 0.40f), 4.0f); }

int8_t parseBreakForce(const std::string& raw) {
    const std::string v = toLower(trim(raw));
    if (v == "always" || v == "page" || v == "left" || v == "right"
        || v == "recto" || v == "verso") return 1;
    if (v == "auto" || v == "avoid" || v == "avoid-page"
        || v == "avoid-column" || v == "avoid-region") return 0;
    return -1;
}

void parseBoxShorthand(const std::string& value,
                       float* top, float* right, float* bottom, float* left) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : value) {
        if (std::isspace((unsigned char)c)) {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty() || parts.size() > 4) return;

    float v[4];
    bool  ok[4];
    for (int i = 0; i < 4; ++i) {
        size_t src;
        switch (parts.size()) {
            case 1: src = 0; break;
            case 2: src = (size_t)(i % 2); break;
            case 3: src = (i == 3) ? 1 : (size_t)i; break;
            default: src = (size_t)i; break;
        }
        ok[i] = parseLengthEm(parts[src], &v[i]);
    }
    if (ok[0]) *top    = clampSpacing(v[0]);
    if (ok[1]) *right  = clampSpacing(v[1]);
    if (ok[2]) *bottom = clampSpacing(v[2]);
    if (ok[3]) *left   = clampSpacing(v[3]);
}

}

double cssColorLuminance(const std::string& raw, double* chroma) {
    const std::string v = toLower(trim(raw));
    if (chroma) *chroma = 0;
    auto lum = [chroma](double r, double g, double b) {
        if (chroma) *chroma = std::max({r, g, b}) - std::min({r, g, b});
        return (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
    };
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    if (!v.empty() && v[0] == '#') {
        std::string h = v.substr(1);
        while (!h.empty() && hex(h.back()) < 0) h.pop_back();
        for (char c : h) if (hex(c) < 0) return -1;
        if (h.size() == 3 || h.size() == 4)
            return lum(hex(h[0]) * 17, hex(h[1]) * 17, hex(h[2]) * 17);
        if (h.size() == 6 || h.size() == 8)
            return lum(hex(h[0]) * 16 + hex(h[1]), hex(h[2]) * 16 + hex(h[3]),
                       hex(h[4]) * 16 + hex(h[5]));
        return -1;
    }
    if (v.rfind("rgb", 0) == 0) {
        const size_t open = v.find('(');
        if (open == std::string::npos) return -1;
        double c[3];
        const char* q = v.c_str() + open + 1;
        for (int k = 0; k < 3; ++k) {
            char* end = nullptr;
            c[k] = std::strtod(q, &end);
            if (end == q) return -1;
            if (*end == '%') { c[k] *= 2.55; ++end; }
            while (*end == ' ' || *end == ',') ++end;
            q = end;
        }
        if (v.find(", 0)") != std::string::npos || v.find(",0)") != std::string::npos)
            return -1;
        return lum(c[0], c[1], c[2]);
    }
    static const std::pair<const char*, double> named[] = {
        {"white", 1.0}, {"whitesmoke", 0.96}, {"ivory", 0.99}, {"snow", 0.98},
        {"beige", 0.95}, {"linen", 0.95}, {"lightyellow", 0.99},
        {"lightgray", 0.83}, {"lightgrey", 0.83}, {"gainsboro", 0.86},
        {"silver", 0.75}, {"lavender", 0.92}, {"aliceblue", 0.97},
        {"honeydew", 0.97}, {"mintcream", 0.98}, {"azure", 0.99},
        {"lightcyan", 0.94}, {"lemonchiffon", 0.97}, {"cornsilk", 0.97},
        {"oldlace", 0.97}, {"seashell", 0.97}, {"mistyrose", 0.92},
        {"lightblue", 0.81}, {"wheat", 0.87}, {"papayawhip", 0.95},
        {"black", 0.0}, {"gray", 0.5}, {"grey", 0.5},
    };
    for (const auto& n : named) {
        if (v != n.first) continue;
        if (chroma && v == "lightyellow") *chroma = 31;
        return n.second;
    }
    return -1;
}

int cssBackgroundGray(const std::string& value) {
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace((unsigned char)value[i])) ++i;
        size_t j = i;
        int depth = 0;
        while (j < value.size() && (depth > 0 || !std::isspace((unsigned char)value[j]))) {
            if (value[j] == '(') ++depth;
            else if (value[j] == ')') --depth;
            ++j;
        }
        if (j > i) {
            const std::string tok = value.substr(i, j - i);
            const std::string low = toLower(tok);
            if (low == "none" || low == "transparent") return 0;
            double chroma = 0;
            const double l = cssColorLuminance(tok, &chroma);
            if (l >= 0) {
                const bool tinted = chroma >= 20 && l <= 0.995;
                if (l < 0.55 || (l > 0.975 && !tinted)) return 0;
                return 240;
            }
        }
        i = j;
    }
    return -1;
}

void CssStyle::apply(const CssStyle& over) {
    if (over.lineHeightMul  >= 0) lineHeightMul  = over.lineHeightMul;
    if (over.marginTopEm    >= 0) marginTopEm    = over.marginTopEm;
    if (over.marginBottomEm >= 0) marginBottomEm = over.marginBottomEm;
    if (over.marginLeftEm   >= 0) marginLeftEm   = over.marginLeftEm;
    if (over.marginRightEm  >= 0) marginRightEm  = over.marginRightEm;
    if (over.paddingTopEm    >= 0) paddingTopEm    = over.paddingTopEm;
    if (over.paddingBottomEm >= 0) paddingBottomEm = over.paddingBottomEm;
    if (over.paddingLeftEm   >= 0) paddingLeftEm   = over.paddingLeftEm;
    if (over.paddingRightEm  >= 0) paddingRightEm  = over.paddingRightEm;
    if (over.textIndentEm   >= 0) textIndentEm   = over.textIndentEm;
    if (over.fontSizeEm     >= 0) fontSizeEm     = over.fontSizeEm;
    if (over.cssFloat       >= 0) cssFloat       = over.cssFloat;
    if (over.textAlign      >= 0) textAlign      = over.textAlign;
    if (over.fontBold       >= 0) fontBold       = over.fontBold;
    if (over.fontItalic     >= 0) fontItalic     = over.fontItalic;
    if (over.displayNone    >= 0) displayNone    = over.displayNone;
    if (over.listStyleNone  >= 0) listStyleNone  = over.listStyleNone;
    if (over.pageBreakBefore >= 0) pageBreakBefore = over.pageBreakBefore;
    if (over.pageBreakAfter  >= 0) pageBreakAfter  = over.pageBreakAfter;
    if (over.backgroundGray  >= 0) backgroundGray  = over.backgroundGray;
    if (over.fontRole != FontRole::None || over.hasFamilyKey()) {
        fontRole = over.fontRole;
        std::memcpy(familyKey, over.familyKey, sizeof(familyKey));
    }
}

bool CssStyle::anySet() const {
    return lineHeightMul >= 0 || marginTopEm >= 0 || marginBottomEm >= 0
        || marginLeftEm >= 0 || marginRightEm >= 0
        || paddingTopEm >= 0 || paddingBottomEm >= 0
        || paddingLeftEm >= 0 || paddingRightEm >= 0
        || textIndentEm >= 0 || fontSizeEm >= 0 || cssFloat >= 0
        || textAlign >= 0 || fontBold >= 0 || fontItalic >= 0
        || displayNone >= 0 || listStyleNone >= 0
        || pageBreakBefore >= 0 || pageBreakAfter >= 0 || backgroundGray >= 0
        || fontRole != FontRole::None || hasFamilyKey();
}

void CssStyle::resolveFontSize(const CssStyle& parent) {
    const float parentMul = parent.fontSizeEm >= 0 ? parent.fontSizeEm : 1.0f;
    if (fontSizeEm >= 0)
        fontSizeEm = std::min(std::max(parentMul * fontSizeEm, 0.40f), 4.0f);
    else
        fontSizeEm = parent.fontSizeEm;
}

CssStyle CssSheet::parseDeclarations(const std::string& decls) {
    CssStyle st;
    size_t pos = 0;
    while (pos < decls.size()) {
        size_t semi = decls.find(';', pos);
        std::string decl = decls.substr(pos, semi == std::string::npos
                                             ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? decls.size() : semi + 1;

        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string prop  = toLower(trim(decl.substr(0, colon)));
        std::string value = trim(decl.substr(colon + 1));
        size_t bang = value.find('!');
        if (bang != std::string::npos) value = trim(value.substr(0, bang));
        if (prop.empty() || value.empty()) continue;

        float em = 0;
        if (prop == "line-height") {
            if (parseLengthEm(value, &em, true))
                st.lineHeightMul = clampLineHeight(em);
        } else if (prop == "text-indent") {
            if (parseLengthEm(value, &em))
                st.textIndentEm = clampIndent(em);
        } else if (prop == "margin") {
            parseBoxShorthand(value, &st.marginTopEm, &st.marginRightEm,
                              &st.marginBottomEm, &st.marginLeftEm);
        } else if (prop == "margin-top") {
            if (parseLengthEm(value, &em)) st.marginTopEm = clampSpacing(em);
        } else if (prop == "margin-bottom") {
            if (parseLengthEm(value, &em)) st.marginBottomEm = clampSpacing(em);
        } else if (prop == "margin-left") {
            if (parseLengthEm(value, &em)) st.marginLeftEm = clampSpacing(em);
        } else if (prop == "margin-right") {
            if (parseLengthEm(value, &em)) st.marginRightEm = clampSpacing(em);
        } else if (prop == "padding") {
            parseBoxShorthand(value, &st.paddingTopEm, &st.paddingRightEm,
                              &st.paddingBottomEm, &st.paddingLeftEm);
        } else if (prop == "padding-top") {
            if (parseLengthEm(value, &em)) st.paddingTopEm = clampSpacing(em);
        } else if (prop == "padding-bottom") {
            if (parseLengthEm(value, &em)) st.paddingBottomEm = clampSpacing(em);
        } else if (prop == "padding-left") {
            if (parseLengthEm(value, &em)) st.paddingLeftEm = clampSpacing(em);
        } else if (prop == "padding-right") {
            if (parseLengthEm(value, &em)) st.paddingRightEm = clampSpacing(em);
        } else if (prop == "font-size") {
            if (parseLengthEm(value, &em, true))
                st.fontSizeEm = clampFontSize(em);
        } else if (prop == "font-family") {
            std::vector<std::string> names = splitFamilyList(value);
            if (!names.empty()) {
                st.fontRole = roleFromNames(names);
                std::string key = normalizeFamilyKey(names[0]);
                if (key != "inherit" && key != "initial" && key != "unset"
                    && key != "normal" && !key.empty()) {
                    std::snprintf(st.familyKey, sizeof(st.familyKey),
                                  "%s", key.c_str());
                }
            }
        } else if (prop == "float") {
            std::string side = toLower(value);
            if (side == "left") st.cssFloat = 1;
            else if (side == "right") st.cssFloat = 2;
            else st.cssFloat = 0;
        } else if (prop == "text-align") {
            const std::string v = toLower(value);
            if (v == "center") st.textAlign = 1;
            else if (v == "right" || v == "end") st.textAlign = 2;
            else if (v == "left" || v == "start" || v == "justify") st.textAlign = 0;
        } else if (prop == "font-weight") {
            const std::string v = toLower(value);
            if (v == "bold" || v == "bolder") st.fontBold = 1;
            else if (v == "normal" || v == "lighter") st.fontBold = 0;
            else if (!v.empty() && std::isdigit((unsigned char)v[0]))
                st.fontBold = std::atoi(v.c_str()) >= 600 ? 1 : 0;
        } else if (prop == "font-style") {
            const std::string v = toLower(value);
            if (v == "italic" || v.compare(0, 7, "oblique") == 0) st.fontItalic = 1;
            else if (v == "normal") st.fontItalic = 0;
        } else if (prop == "display") {
            st.displayNone = toLower(value) == "none" ? 1 : 0;
        } else if (prop == "list-style-type" || prop == "list-style") {
            const std::string v = toLower(value);
            if (v == "none" || v.compare(0, 5, "none ") == 0
                    || v.find(" none") != std::string::npos)
                st.listStyleNone = 1;
            else if (prop == "list-style-type")
                st.listStyleNone = 0;
        } else if (prop == "background-color" || prop == "background") {
            const int gray = cssBackgroundGray(value);
            if (gray >= 0) st.backgroundGray = (int16_t)gray;
        } else if (prop == "page-break-before" || prop == "break-before") {
            int8_t v = parseBreakForce(value);
            if (v >= 0) st.pageBreakBefore = v;
        } else if (prop == "page-break-after" || prop == "break-after") {
            int8_t v = parseBreakForce(value);
            if (v >= 0) st.pageBreakAfter = v;
        }
    }
    return st;
}

void CssSheet::addSheet(const std::string& rawCss) {
    const std::string css = stripComments(rawCss);
    size_t i = 0;
    while (i < css.size()) {
        while (i < css.size() && std::isspace((unsigned char)css[i])) ++i;
        if (i >= css.size()) break;

        if (css[i] == '@') {
            size_t brace = css.find('{', i);
            size_t semi  = css.find(';', i);
            if (semi != std::string::npos
                && (brace == std::string::npos || semi < brace)) {
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
            const std::string prelude = toLower(trim(css.substr(i, brace - i)));
            if (prelude.compare(0, 6, "@media") == 0 && j > brace + 1) {
                const std::string q = prelude.substr(6);
                const bool applies = q.find("print") == std::string::npos
                    && q.find("amzn") == std::string::npos
                    && q.find("speech") == std::string::npos
                    && q.find("aural") == std::string::npos
                    && q.find("not ") == std::string::npos;
                if (applies) addSheet(css.substr(brace + 1, j - brace - 2));
            }
            i = j;
            continue;
        }

        size_t brace = css.find('{', i);
        if (brace == std::string::npos) break;
        size_t close = css.find('}', brace);
        if (close == std::string::npos) break;

        std::string selectorList = css.substr(i, brace - i);
        std::string decls = css.substr(brace + 1, close - brace - 1);
        i = close + 1;

        CssStyle style = parseDeclarations(decls);
        if (!style.anySet()) continue;

        size_t p = 0;
        while (p <= selectorList.size()) {
            size_t comma = selectorList.find(',', p);
            std::string sel = trim(selectorList.substr(
                p, comma == std::string::npos ? std::string::npos : comma - p));
            p = (comma == std::string::npos) ? selectorList.size() + 1 : comma + 1;
            if (sel.empty()) continue;

            if (sel.find('[') != std::string::npos
                || sel.find(':') != std::string::npos
                || sel.find('#') != std::string::npos) continue;

            Rule r;
            r.style = style;
            bool bad = false;
            char pending = 0;
            size_t k = 0;
            while (k < sel.size()) {
                const char c = sel[k];
                if (std::isspace((unsigned char)c)) {
                    if (pending == 0 && !r.parts.empty()) pending = ' ';
                    ++k;
                    continue;
                }
                if (c == '>' || c == '+' || c == '~') {
                    if (r.parts.empty()) { bad = true; break; }
                    pending = c;
                    ++k;
                    continue;
                }
                size_t e = k;
                while (e < sel.size() && !std::isspace((unsigned char)sel[e])
                       && sel[e] != '>' && sel[e] != '+' && sel[e] != '~') ++e;
                const std::string comp = sel.substr(k, e - k);
                k = e;
                Compound cpd;
                if (comp != "*") {
                    size_t dot = comp.find('.');
                    cpd.tag = toLower(comp.substr(0, dot));
                    if (cpd.tag == "*") cpd.tag.clear();
                    while (dot != std::string::npos) {
                        size_t next = comp.find('.', dot + 1);
                        std::string cls = comp.substr(
                            dot + 1, next == std::string::npos
                                         ? std::string::npos : next - dot - 1);
                        if (!cls.empty()) cpd.classes.push_back(cls);
                        dot = next;
                    }
                }
                if (!r.parts.empty()) r.combinators.push_back(pending ? pending : ' ');
                pending = 0;
                r.parts.push_back(std::move(cpd));
            }
            if (bad || r.parts.empty() || pending != 0) continue;
            if (r.style.backgroundGray > 0) {
                const Compound& subj = r.parts.back();
                if (subj.classes.empty() && subj.tag != "pre" && subj.tag != "aside"
                    && subj.tag != "blockquote")
                    r.style.backgroundGray = -1;
            }
            for (const auto& cpd : r.parts)
                r.specificity += (int)cpd.classes.size() * 10 + (cpd.tag.empty() ? 0 : 1);
            rules_.push_back(std::move(r));
        }
    }
}

namespace {

std::vector<std::string> splitClasses(const std::string& classAttr) {
    std::vector<std::string> classes;
    std::string cur;
    for (char c : classAttr) {
        if (std::isspace((unsigned char)c)) {
            if (!cur.empty()) { classes.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) classes.push_back(cur);
    return classes;
}

}

bool CssSheet::compoundMatches(const Compound& c, const std::string& tagLower,
                               const std::vector<std::string>& classes) {
    if (!c.tag.empty() && c.tag != tagLower) return false;
    for (const auto& need : c.classes) {
        if (std::find(classes.begin(), classes.end(), need) == classes.end())
            return false;
    }
    return true;
}

bool CssSheet::chainMatches(const Rule& r, size_t part, const CssNode& node) {
    if (part == 0) return true;
    const Compound& want = r.parts[part - 1];
    const char comb = r.combinators[part - 1];
    auto test = [&](const CssNode& n) {
        return compoundMatches(want, toLower(n.tag()), splitClasses(n.classAttr()))
            && chainMatches(r, part - 1, n);
    };
    if (comb == '>' || comb == ' ') {
        std::unique_ptr<CssNode> p = node.parent();
        while (p) {
            if (test(*p)) return true;
            if (comb == '>') return false;
            p = p->parent();
        }
        return false;
    }
    std::unique_ptr<CssNode> sib = node.prevSibling();
    while (sib) {
        if (test(*sib)) return true;
        if (comb == '+') return false;
        sib = sib->prevSibling();
    }
    return false;
}

CssStyle CssSheet::cascade(const std::vector<size_t>& hitsIn) const {
    std::vector<size_t> hits = hitsIn;
    std::stable_sort(hits.begin(), hits.end(), [this](size_t a, size_t b) {
        return rules_[a].specificity < rules_[b].specificity;
    });
    CssStyle out;
    for (size_t idx : hits) out.apply(rules_[idx].style);
    return out;
}

CssStyle CssSheet::match(const std::string& tag,
                         const std::string& classAttr) const {
    const std::vector<std::string> classes = splitClasses(classAttr);
    const std::string tagLower = toLower(tag);
    std::vector<size_t> hits;
    for (size_t idx = 0; idx < rules_.size(); ++idx) {
        const Rule& r = rules_[idx];
        if (r.parts.size() != 1) continue;
        if (compoundMatches(r.parts.back(), tagLower, classes)) hits.push_back(idx);
    }
    return cascade(hits);
}

CssStyle CssSheet::match(const CssNode& node) const {
    const std::vector<std::string> classes = splitClasses(node.classAttr());
    const std::string tagLower = toLower(node.tag());
    std::vector<size_t> hits;
    for (size_t idx = 0; idx < rules_.size(); ++idx) {
        const Rule& r = rules_[idx];
        if (!compoundMatches(r.parts.back(), tagLower, classes)) continue;
        if (chainMatches(r, r.parts.size() - 1, node)) hits.push_back(idx);
    }
    return cascade(hits);
}

}
