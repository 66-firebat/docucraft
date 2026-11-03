#include "md_parser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>

#include "../image/image_decoder.h"
#include "../util/text_join.h"
#include "font_alias.h"
#include "text_encoding.h"

namespace hardcopy {

namespace {

void stripBom(std::string& s) {
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) {
        s.erase(0, 3);
    }
}

bool startsWith(const std::string& s, const char* p) {
    size_t n = 0;
    while (p[n]) ++n;
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

int countLeadingSpaces(const std::string& s) {
    int n = 0;
    while (n < (int)s.size() && (s[n] == ' ' || s[n] == '\t')) ++n;
    return n;
}

std::string trimCopy(const std::string& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    return s.substr(begin, end - begin);
}

bool isThematicBreak(const std::string& line) {
    int hits = 0;
    char want = 0;
    for (char c : line) {
        if (c == ' ' || c == '\t') continue;
        if (want == 0 && (c == '*' || c == '-' || c == '_')) {
            want = c;
            ++hits;
        } else if (c == want) {
            ++hits;
        } else {
            return false;
        }
    }
    return hits >= 3;
}

int atxLevel(const std::string& line) {
    int n = 0;
    while (n < (int)line.size() && line[n] == '#') ++n;
    if (n == 0 || n > 6) return 0;
    if (n < (int)line.size() && line[n] != ' ' && line[n] != '\t') return 0;
    return n;
}

std::string atxTitle(const std::string& line, int level) {
    std::string title = (size_t)level < line.size() ? line.substr(level) : std::string();
    title = trimCopy(title);
    size_t e = title.size();
    while (e > 0 && title[e - 1] == '#') --e;
    if (e == 0) return {};
    if (e < title.size() && (title[e - 1] == ' ' || title[e - 1] == '\t'))
        title = trimCopy(title.substr(0, e));
    return title;
}

int leadingColumns(const std::string& s) {
    int col = 0;
    for (char c : s) {
        if (c == ' ') ++col;
        else if (c == '\t') col = (col / 4 + 1) * 4;
        else break;
    }
    return col;
}

bool isSetextUnderline(const std::string& trimmed, char ch) {
    if (trimmed.empty()) return false;
    for (char c : trimmed) if (c != ch) return false;
    return ch == '=' || trimmed.size() >= 2;
}

std::string fenceRun(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i >= line.size() || (line[i] != '`' && line[i] != '~')) return {};
    const char ch = line[i];
    size_t e = i;
    while (e < line.size() && line[e] == ch) ++e;
    if (e - i < 3) return {};
    if (ch == '`' && line.find('`', e) != std::string::npos) return {};
    return std::string(e - i, ch);
}

bool closesFence(const std::string& line, const std::string& open) {
    const std::string t = trimCopy(line);
    if (t.size() < open.size()) return false;
    for (char c : t) if (c != open[0]) return false;
    return true;
}

bool isMdSpace(char c) {
    return c == ' ' || c == '\t';
}

bool canOpenRun(const std::string& s, size_t runStart, size_t runEnd) {
    if (runEnd >= s.size()) return false;
    if (isMdSpace(s[runEnd])) return false;
    (void)runStart;
    return true;
}

bool canCloseRun(const std::string& s, size_t runStart) {
    if (runStart == 0) return false;
    return !isMdSpace(s[runStart - 1]);
}

size_t findCloser(const std::string& s, size_t from, size_t len,
                  char delimiter) {
    size_t i = from;
    while (i < s.size()) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] != delimiter) { ++i; continue; }
        size_t runStart = i;
        size_t runEnd = i;
        while (runEnd < s.size() && s[runEnd] == delimiter) ++runEnd;
        if (runEnd - runStart >= len && canCloseRun(s, runStart)) return runStart;
        i = runEnd;
    }
    return std::string::npos;
}

std::pair<std::string, size_t> readTexGroup(const std::string& s, size_t start) {
    if (start >= s.size()) return {"", start};
    if (s[start] == '{') {
        int depth = 0;
        for (size_t i = start; i < s.size(); ++i) {
            if (s[i] == '{') ++depth;
            else if (s[i] == '}' && --depth == 0)
                return {s.substr(start + 1, i - start - 1), i + 1};
        }
        return {s.substr(start + 1), s.size()};
    }
    if (s[start] == '\\') {
        size_t i = start + 1;
        while (i < s.size() && std::isalpha((unsigned char)s[i])) ++i;
        if (i == start + 1 && i < s.size()) ++i;
        return {s.substr(start, i - start), i};
    }
    size_t len = 1;
    uint8_t c = (uint8_t)s[start];
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    len = std::min(len, s.size() - start);
    return {s.substr(start, len), start + len};
}

const std::unordered_map<std::string, std::string>& texSymbols() {
    static const std::unordered_map<std::string, std::string> k = {
        {"alpha","α"},{"beta","β"},{"gamma","γ"},{"delta","δ"},{"epsilon","ε"},
        {"zeta","ζ"},{"eta","η"},{"theta","θ"},{"iota","ι"},{"kappa","κ"},
        {"lambda","λ"},{"mu","μ"},{"nu","ν"},{"xi","ξ"},{"pi","π"},
        {"rho","ρ"},{"sigma","σ"},{"tau","τ"},{"phi","φ"},{"chi","χ"},
        {"psi","ψ"},{"omega","ω"},{"Gamma","Γ"},{"Delta","Δ"},{"Theta","Θ"},
        {"Lambda","Λ"},{"Xi","Ξ"},{"Pi","Π"},{"Sigma","Σ"},{"Phi","Φ"},
        {"Psi","Ψ"},{"Omega","Ω"},{"times","×"},{"cdot","·"},{"div","÷"},
        {"pm","±"},{"mp","∓"},{"leq","≤"},{"le","≤"},{"geq","≥"},{"ge","≥"},
        {"neq","≠"},{"ne","≠"},{"approx","≈"},{"equiv","≡"},{"sim","∼"},
        {"propto","∝"},{"infty","∞"},{"partial","∂"},{"nabla","∇"},{"sum","∑"},
        {"prod","∏"},{"int","∫"},{"oint","∮"},{"rightarrow","→"},{"to","→"},
        {"leftarrow","←"},{"Rightarrow","⇒"},{"Leftarrow","⇐"},{"leftrightarrow","↔"},
        {"Leftrightarrow","⇔"},{"in","∈"},{"notin","∉"},{"subset","⊂"},
        {"subseteq","⊆"},{"cup","∪"},{"cap","∩"},{"emptyset","∅"},{"forall","∀"},
        {"exists","∃"},{"land","∧"},{"lor","∨"},{"neg","¬"},{"oplus","⊕"},
        {"otimes","⊗"},{"angle","∠"},{"perp","⊥"},{"parallel","∥"},{"degree","°"},
        {"ldots","…"},{"cdots","⋯"},{"dots","…"},{"prime","′"},{"hbar","ℏ"},
        {"ell","ℓ"},{"Re","ℜ"},{"Im","ℑ"},{"quad","  "},{"qquad","    "},
        {";"," "},{","," "},{" "," "},{"left",""},{"right",""},
        {"displaystyle",""},{"limits",""},{"{","{"},{"}","}"},{"_","_"},
        {"%","%"},{"#","#"},{"$","$"},{"&","&"},
    };
    return k;
}

void appendInlineRun(std::vector<InlineRun>& runs, InlineKind kind,
                     const std::string& text) {
    if (text.empty()) return;
    if (!runs.empty() && runs.back().kind == kind
            && runs.back().cssFontRole == -1 && runs.back().cssFamilyKey[0] == 0) {
        runs.back().text += text;
    } else {
        runs.push_back(InlineRun{kind, text, 0});
    }
}

void appendTexRuns(const std::string& tex, std::vector<InlineRun>& runs,
                   InlineKind inherited = InlineKind::Text) {
    auto styledKind = [&](InlineKind requested) {
        return inherited == InlineKind::Superscript || inherited == InlineKind::Subscript
            ? inherited : requested;
    };
    size_t i = 0;
    while (i < tex.size()) {
        char c = tex[i];
        if (c == '\\') {
            size_t j = i + 1;
            while (j < tex.size() && std::isalpha((unsigned char)tex[j])) ++j;
            if (j == i + 1 && j < tex.size()) ++j;
            std::string cmd = tex.substr(i + 1, j - i - 1);
            if (cmd == "frac") {
                auto num = readTexGroup(tex, j);
                auto den = readTexGroup(tex, num.second);
                bool simple = num.first.size() <= 4 && den.first.size() <= 4
                           && num.first.find('\\') == std::string::npos
                           && den.first.find('\\') == std::string::npos;
                if (simple) {
                    appendTexRuns(num.first, runs, InlineKind::Superscript);
                    appendInlineRun(runs, inherited, "⁄");
                    appendTexRuns(den.first, runs, InlineKind::Subscript);
                } else {
                    appendInlineRun(runs, inherited, "(");
                    appendTexRuns(num.first, runs, inherited);
                    appendInlineRun(runs, inherited, ")⁄(");
                    appendTexRuns(den.first, runs, inherited);
                    appendInlineRun(runs, inherited, ")");
                }
                i = den.second;
                continue;
            }
            if (cmd == "sqrt") {
                auto arg = readTexGroup(tex, j);
                appendInlineRun(runs, inherited, "√(");
                appendTexRuns(arg.first, runs, inherited);
                appendInlineRun(runs, inherited, ")");
                i = arg.second;
                continue;
            }
            if (cmd == "text" || cmd == "mathrm" || cmd == "mathbf"
                    || cmd == "mathit" || cmd == "operatorname") {
                auto arg = readTexGroup(tex, j);
                InlineKind kind = inherited;
                if (cmd == "mathbf") kind = styledKind(InlineKind::Strong);
                else if (cmd == "mathit") kind = styledKind(InlineKind::Emphasis);
                appendTexRuns(arg.first, runs, kind);
                i = arg.second;
                continue;
            }
            if (cmd == "vec" || cmd == "hat" || cmd == "widehat" || cmd == "bar"
                    || cmd == "overline" || cmd == "dot" || cmd == "ddot"
                    || cmd == "tilde" || cmd == "widetilde" || cmd == "check"
                    || cmd == "breve") {
                auto arg = readTexGroup(tex, j);
                const char* mark = cmd == "vec" ? "⃗" :
                    (cmd == "hat" || cmd == "widehat") ? "̂" :
                    cmd == "bar" ? "̄" : cmd == "overline" ? "̅" :
                    cmd == "dot" ? "̇" : cmd == "ddot" ? "̈" :
                    cmd == "check" ? "̌" : cmd == "breve" ? "̆" : "̃";
                size_t p = 0;
                while (p < arg.first.size()) {
                    auto ch = readTexGroup(arg.first, p);
                    appendInlineRun(runs, inherited, ch.first + mark);
                    p = ch.second;
                }
                i = arg.second;
                continue;
            }
            auto symbol = texSymbols().find(cmd);
            appendInlineRun(runs, inherited,
                            symbol != texSymbols().end() ? symbol->second : cmd);
            i = j;
            continue;
        }
        if (c == '^' || c == '_') {
            auto arg = readTexGroup(tex, i + 1);
            appendTexRuns(arg.first, runs,
                          c == '^' ? InlineKind::Superscript : InlineKind::Subscript);
            i = arg.second;
            continue;
        }
        if (c == '{' || c == '}') { ++i; continue; }
        auto ch = readTexGroup(tex, i);
        appendInlineRun(runs, inherited, ch.first);
        i = ch.second;
    }
}

bool isAsciiPunct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
        || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

size_t findBracketClose(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '`') {
            size_t e = s.find('`', i + 1);
            if (e == std::string::npos) return std::string::npos;
            i = e;
            continue;
        }
        if (s[i] == '[') ++depth;
        else if (s[i] == ']' && --depth == 0) return i;
    }
    return std::string::npos;
}

size_t findParenClose(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) return i;
        else if (s[i] == '\n') return std::string::npos;
    }
    return std::string::npos;
}

bool startsWithCi(const std::string& s, size_t at, const char* p) {
    for (size_t k = 0; p[k]; ++k) {
        if (at + k >= s.size()) return false;
        if (std::tolower((unsigned char)s[at + k]) != p[k]) return false;
    }
    return true;
}

std::string htmlTagName(const std::string& s, size_t lt, bool* closing,
                        size_t* tagEnd) {
    size_t i = lt + 1;
    *closing = false;
    if (i < s.size() && s[i] == '/') { *closing = true; ++i; }
    size_t nameStart = i;
    while (i < s.size() && std::isalpha((unsigned char)s[i])) ++i;
    if (i == nameStart) return {};
    std::string name;
    for (size_t k = nameStart; k < i; ++k)
        name.push_back((char)std::tolower((unsigned char)s[k]));
    const size_t gt = s.find('>', i);
    if (gt == std::string::npos) return {};
    for (size_t k = i; k < gt; ++k)
        if (s[k] == '<' || s[k] == '\n') return {};
    if (i < gt && s[i] != ' ' && s[i] != '/' && s[i] != '\t') return {};
    *tagEnd = gt + 1;
    return name;
}

InlineKind nestKind(InlineKind outer, InlineKind inner) {
    if (outer == InlineKind::Strong || inner == InlineKind::Strong)
        return InlineKind::Strong;
    if (outer == InlineKind::Superscript || outer == InlineKind::Subscript)
        return outer;
    return inner;
}

void parseInlinesInto(const std::string& text, InlineKind base,
                      std::vector<InlineRun>& runs);

void appendStyled(std::vector<InlineRun>& runs, InlineKind kind,
                  const std::string& text) {
    if (text.empty()) return;
    if (!runs.empty() && runs.back().kind == kind && kind != InlineKind::Code
            && kind != InlineKind::LineBreak) {
        runs.back().text += text;
    } else {
        runs.push_back(InlineRun{kind, text, 0});
    }
}

void parseInlinesInto(const std::string& text, InlineKind base,
                      std::vector<InlineRun>& runs) {
    std::string buf;
    auto flushText = [&]() {
        if (!buf.empty()) {
            appendStyled(runs, base, buf);
            buf.clear();
        }
    };

    size_t i = 0, n = text.size();
    while (i < n) {
        char c = text[i];

        if (c == '\n') {
            flushText();
            runs.push_back(InlineRun{InlineKind::LineBreak, {}, (int32_t)i});
            ++i;
            continue;
        }

        if (c == '\\' && i + 1 < n && isAsciiPunct(text[i + 1])) {
            buf.push_back(text[i + 1]);
            i += 2;
            continue;
        }

        if (c == '`') {
            size_t openEnd = i;
            while (openEnd < n && text[openEnd] == '`') ++openEnd;
            size_t tickLen = openEnd - i;
            size_t search = openEnd;
            size_t closeStart = std::string::npos;
            while (search < n) {
                if (text[search] != '`') { ++search; continue; }
                size_t runEnd = search;
                while (runEnd < n && text[runEnd] == '`') ++runEnd;
                if (runEnd - search == tickLen) { closeStart = search; break; }
                search = runEnd;
            }
            if (closeStart != std::string::npos) {
                flushText();
                std::string code = text.substr(openEnd, closeStart - openEnd);
                for (auto& ch : code) if (ch == '\n') ch = ' ';
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
                    code = code.substr(1, code.size() - 2);
                runs.push_back(InlineRun{InlineKind::Code, std::move(code), (int32_t)i});
                i = closeStart + tickLen;
                continue;
            }
            buf.append(text, i, tickLen);
            i = openEnd;
            continue;
        }

        if (c == '$') {
            const size_t dollarLen = (i + 1 < n && text[i + 1] == '$') ? 2 : 1;
            const std::string delimiter(dollarLen, '$');
            size_t closeStart = text.find(delimiter, i + dollarLen);
            bool ok = closeStart != std::string::npos && closeStart > i + dollarLen;
            if (ok && dollarLen == 1) {
                const char first = text[i + 1];
                const char last = text[closeStart - 1];
                const char after = closeStart + 1 < n ? text[closeStart + 1] : 0;
                ok = first != ' ' && first != '\t' && last != ' ' && last != '\t'
                  && !(after >= '0' && after <= '9')
                  && text.find('\n', i) > closeStart;
            }
            if (ok) {
                flushText();
                appendTexRuns(text.substr(i + dollarLen,
                                          closeStart - i - dollarLen), runs);
                i = closeStart + dollarLen;
                continue;
            }
            buf.append(delimiter);
            i += dollarLen;
            continue;
        }

        if (c == '[' || (c == '!' && i + 1 < n && text[i + 1] == '[')) {
            const bool image = c == '!';
            const size_t open = image ? i + 1 : i;
            if (!image && open + 1 < n && text[open + 1] == '^') {
                const size_t close = text.find(']', open + 2);
                if (close != std::string::npos && close > open + 2
                        && text.find_first_of(" \n", open) > close) {
                    flushText();
                    appendStyled(runs, InlineKind::Superscript,
                                 text.substr(open + 2, close - open - 2));
                    i = close + 1;
                    continue;
                }
            }
            const size_t close = findBracketClose(text, open);
            if (close != std::string::npos) {
                size_t next = close + 1;
                size_t destEnd = std::string::npos;
                if (next < n && text[next] == '(') {
                    destEnd = findParenClose(text, next);
                } else if (next < n && text[next] == '[') {
                    const size_t refEnd = text.find(']', next);
                    if (refEnd != std::string::npos) destEnd = refEnd;
                }
                if (destEnd != std::string::npos) {
                    flushText();
                    const std::string label = text.substr(open + 1, close - open - 1);
                    if (image) appendStyled(runs, base, label);
                    else parseInlinesInto(label, base, runs);
                    i = destEnd + 1;
                    continue;
                }
            }
            buf.push_back(c);
            ++i;
            continue;
        }

        if (c == '<') {
            if (text.compare(i, 4, "<!--") == 0) {
                const size_t e = text.find("-->", i + 4);
                if (e != std::string::npos) { i = e + 3; continue; }
            }
            const size_t gt = text.find('>', i + 1);
            if (gt != std::string::npos) {
                const std::string inner = text.substr(i + 1, gt - i - 1);
                const bool spaceless = inner.find_first_of(" \t\n<") == std::string::npos;
                if (spaceless && (inner.find("://") != std::string::npos
                                  || startsWithCi(inner, 0, "mailto:")
                                  || (inner.find('@') != std::string::npos
                                      && inner.find('.') != std::string::npos))) {
                    flushText();
                    appendStyled(runs, base, inner);
                    i = gt + 1;
                    continue;
                }
            }
            bool closing = false;
            size_t tagEnd = 0;
            const std::string tag = htmlTagName(text, i, &closing, &tagEnd);
            if (tag == "br") {
                flushText();
                runs.push_back(InlineRun{InlineKind::LineBreak, {}, (int32_t)i});
                i = tagEnd;
                continue;
            }
            if (!closing && (tag == "sup" || tag == "sub" || tag == "b"
                             || tag == "strong" || tag == "i" || tag == "em")) {
                const std::string endTag = "</" + tag + ">";
                size_t e = tagEnd;
                while (e < n && !startsWithCi(text, e, endTag.c_str())) ++e;
                if (e < n) {
                    flushText();
                    InlineKind kind = tag == "sup" ? InlineKind::Superscript
                                    : tag == "sub" ? InlineKind::Subscript
                                    : (tag == "b" || tag == "strong") ? InlineKind::Strong
                                    : InlineKind::Emphasis;
                    parseInlinesInto(text.substr(tagEnd, e - tagEnd),
                                     nestKind(base, kind), runs);
                    i = e + endTag.size();
                    continue;
                }
            }
            static const char* kDropTags[] = {
                "span", "font", "u", "ins", "mark", "kbd", "small", "big",
                "a", "abbr", "cite", "del", "s", "strike", "center", "div",
                "p", "sup", "sub", "b", "strong", "i", "em", "img",
            };
            bool dropped = false;
            for (const char* t : kDropTags) {
                if (tag == t) { dropped = true; break; }
            }
            if (dropped) {
                if (tag == "img") {
                    const std::string attrs = text.substr(i, tagEnd - i);
                    const size_t a = attrs.find("alt=\"");
                    if (a != std::string::npos) {
                        const size_t q = attrs.find('"', a + 5);
                        if (q != std::string::npos) buf += attrs.substr(a + 5, q - a - 5);
                    }
                }
                i = tagEnd;
                continue;
            }
            buf.push_back(c);
            ++i;
            continue;
        }

        if (c == '~' && i + 1 < n && text[i + 1] == '~') {
            const size_t e = text.find("~~", i + 2);
            if (e != std::string::npos && e > i + 2) {
                flushText();
                parseInlinesInto(text.substr(i + 2, e - i - 2), base, runs);
                i = e + 2;
                continue;
            }
        }

        if (c == '*' || c == '_') {
            const char delimiter = c;
            size_t runEnd = i;
            while (runEnd < n && text[runEnd] == delimiter) ++runEnd;
            size_t runLen = runEnd - i;
            const bool intrawordUnderscore = delimiter == '_'
                && i > 0 && runEnd < n
                && std::isalnum((unsigned char)text[i - 1])
                && std::isalnum((unsigned char)text[runEnd]);
            if (!intrawordUnderscore && canOpenRun(text, i, runEnd)) {
                for (size_t want = std::min<size_t>(runLen, 3); want >= 1; --want) {
                    size_t closeStart = findCloser(text, i + want, want, delimiter);
                    if (closeStart == std::string::npos) continue;
                    flushText();
                    InlineKind kind = want >= 2 ? InlineKind::Strong
                                                : InlineKind::Emphasis;
                    parseInlinesInto(text.substr(i + want, closeStart - (i + want)),
                                     nestKind(base, kind), runs);
                    i = closeStart + want;
                    goto next_char;
                }
            }
            buf.append(text, i, runLen);
            i = runEnd;
            continue;
        }

        buf.push_back(c);
        ++i;
    next_char:;
    }
    flushText();
}

std::vector<InlineRun> parseInlines(const std::string& text) {
    std::vector<InlineRun> runs;
    parseInlinesInto(text, InlineKind::Text, runs);
    while (!runs.empty() && runs.back().kind == InlineKind::LineBreak) runs.pop_back();
    return runs;
}

void emitParagraph(Document* doc, std::string& buf) {
    while (!buf.empty() && (buf.back() == ' ' || buf.back() == '\r' || buf.back() == '\n')) buf.pop_back();
    if (buf.empty()) return;
    Block b;
    b.kind = BlockKind::Paragraph;
    b.runs = parseInlines(buf);
    doc->blocks.push_back(std::move(b));
    buf.clear();
}

void emitCodeBlock(Document* doc, std::string code) {
    while (!code.empty() && (code.back() == '\r' || code.back() == '\n')) code.pop_back();
    Block b;
    b.kind = BlockKind::CodeBlock;
    size_t start = 0;
    while (start <= code.size()) {
        size_t end = code.find('\n', start);
        std::string line = code.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find('\t') != std::string::npos) {
            std::string expanded;
            int col = 0;
            for (char c : line) {
                if (c == '\t') {
                    do { expanded.push_back(' '); ++col; } while (col % 4);
                } else {
                    expanded.push_back(c);
                    if (((unsigned char)c & 0xC0) != 0x80) ++col;
                }
            }
            line = std::move(expanded);
        }
        if (line.empty()) line = " ";
        b.runs.push_back(InlineRun{InlineKind::Code, std::move(line), (int32_t)start});
        if (end == std::string::npos) break;
        b.runs.push_back(InlineRun{InlineKind::LineBreak, {}, (int32_t)end});
        start = end + 1;
    }
    doc->blocks.push_back(std::move(b));
}

void emitMathBlock(Document* doc, const std::string& tex) {
    Block b;
    b.kind = BlockKind::MathBlock;
    appendTexRuns(tex, b.runs);
    if (!b.runs.empty()) doc->blocks.push_back(std::move(b));
}

std::vector<std::string> splitTableCells(const std::string& line) {
    std::vector<std::string> cells;
    size_t start = 0, end = line.size();
    while (start < end && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (start < end && line[start] == '|') ++start;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    if (end > start && line[end - 1] == '|') --end;

    std::string cell;
    for (size_t i = start; i < end; ++i) {
        if (line[i] == '\\' && i + 1 < end) {
            cell.push_back(line[i + 1]);
            ++i;
        } else if (line[i] == '|') {
            size_t a = 0, b = cell.size();
            while (a < b && cell[a] == ' ') ++a;
            while (b > a && cell[b - 1] == ' ') --b;
            cells.push_back(cell.substr(a, b - a));
            cell.clear();
        } else {
            cell.push_back(line[i]);
        }
    }
    {
        size_t a = 0, b = cell.size();
        while (a < b && cell[a] == ' ') ++a;
        while (b > a && cell[b - 1] == ' ') --b;
        cells.push_back(cell.substr(a, b - a));
    }
    return cells;
}

bool isTableSeparator(const std::string& line) {
    if (line.find('|') == std::string::npos) return false;
    auto cells = splitTableCells(line);
    if (cells.empty()) return false;
    for (auto& c : cells) {
        size_t i = 0, n = c.size();
        if (n == 0) return false;
        if (c[i] == ':') ++i;
        int dashes = 0;
        while (i < n && c[i] == '-') { ++i; ++dashes; }
        if (i < n && c[i] == ':') ++i;
        if (dashes < 1 || i != n) return false;
    }
    return true;
}

bool tryEmitImageBlock(const std::string& line,
                       const std::filesystem::path& baseDir,
                       Document* doc) {
    if (!doc || baseDir.empty() || !startsWith(line, "![")) return false;
    const size_t altEnd = line.find("](", 2);
    if (altEnd == std::string::npos || line.back() != ')') return false;

    std::string alt = line.substr(2, altEnd - 2);
    std::string target = trimCopy(
        line.substr(altEnd + 2, line.size() - altEnd - 3));
    if (target.size() >= 2 && target.front() == '<' && target.back() == '>')
        target = target.substr(1, target.size() - 2);
    if (target.empty() || target.find("://") != std::string::npos) return false;

    std::string decoded;
    decoded.reserve(target.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] == '%' && i + 2 < target.size()) {
            int hi = hex(target[i + 1]), lo = hex(target[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(target[i]);
    }

    std::filesystem::path imagePath = std::filesystem::u8path(decoded);
    if (imagePath.is_relative()) imagePath = baseDir / imagePath;
    std::ifstream imageFile(imagePath, std::ios::binary);
    if (!imageFile) return false;

    ImageData image;
    image.altText = std::move(alt);
    image.rawBytes.assign(std::istreambuf_iterator<char>(imageFile),
                          std::istreambuf_iterator<char>());
    if (image.rawBytes.empty()) return false;
    imageInfo(image.rawBytes.data(), image.rawBytes.size(),
              &image.width, &image.height);

    Block block;
    block.kind = BlockKind::Image;
    block.imageIndex = (int)doc->images.size();
    doc->images.push_back(std::move(image));
    doc->blocks.push_back(std::move(block));
    return true;
}

bool tryListMarker(const std::string& line, int* outOrdered, int* outContentStart) {
    int i = countLeadingSpaces(line);
    if (i >= (int)line.size()) return false;

    if (line[i] == '-' || line[i] == '*' || line[i] == '+') {
        if (i + 1 < (int)line.size() && line[i + 1] == ' ') {
            *outOrdered = -1;
            *outContentStart = i + 2;
            return true;
        }
    }
    int j = i;
    while (j < (int)line.size() && std::isdigit((unsigned char)line[j])) ++j;
    if (j > i && j - i <= 9 && j < (int)line.size()
            && (line[j] == '.' || line[j] == ')')
            && j + 1 < (int)line.size() && line[j + 1] == ' ') {
        *outOrdered = std::atoi(line.c_str() + i);
        *outContentStart = j + 2;
        return true;
    }
    return false;
}

}

static bool parseMarkdownImpl(const std::string& srcIn, Document* out,
                              const std::filesystem::path& baseDir) {
    if (!out) return false;
    std::string src = srcIn;
    stripBom(src);

    std::stringstream ss(src);
    std::vector<std::string> lines;
    {
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }

    std::string paraBuf;
    bool inFence = false;
    std::string fenceMark;
    std::string fenceBuf;
    size_t fenceIndent = 0;

    std::string quoteBuf;
    int quoteDepth = 0;
    auto flushQuote = [&]() {
        while (!quoteBuf.empty() && (quoteBuf.back() == ' ' || quoteBuf.back() == '\n'))
            quoteBuf.pop_back();
        if (!quoteBuf.empty()) {
            Block b;
            b.kind = BlockKind::Paragraph;
            b.quoteDepth = quoteDepth;
            b.runs = parseInlines(quoteBuf);
            if (!b.runs.empty()) out->blocks.push_back(std::move(b));
        }
        quoteBuf.clear();
    };

    std::vector<int> listIndents;
    std::vector<int> listNumbers;
    int itemContentCol = -1;
    bool lastWasBlank = false;
    std::string itemBuf;
    Block itemBlock;
    bool itemOpen = false;
    auto flushItem = [&]() {
        if (!itemOpen) return;
        while (!itemBuf.empty() && (itemBuf.back() == ' ' || itemBuf.back() == '\n'))
            itemBuf.pop_back();
        auto contentRuns = parseInlines(itemBuf);
        itemBlock.runs.insert(itemBlock.runs.end(),
                              std::make_move_iterator(contentRuns.begin()),
                              std::make_move_iterator(contentRuns.end()));
        out->blocks.push_back(std::move(itemBlock));
        itemBlock = Block{};
        itemBuf.clear();
        itemOpen = false;
    };
    auto endList = [&]() {
        flushItem();
        listIndents.clear();
        listNumbers.clear();
        itemContentCol = -1;
    };
    auto flushAll = [&]() {
        emitParagraph(out, paraBuf);
        flushQuote();
        flushItem();
    };
    auto appendLine = [](std::string& buf, const std::string& rawLine) {
        const bool hard = (rawLine.size() >= 2
                           && rawLine.compare(rawLine.size() - 2, 2, "  ") == 0);
        std::string t = trimCopy(rawLine);
        bool backslash = false;
        if (!t.empty() && t.back() == '\\') { backslash = true; t.pop_back(); }
        if (t.empty() && !backslash) return;
        if (buf.empty() || buf.back() == '\n') buf += t;
        else textjoin::appendSoftWrap(buf, t);
        if (hard || backslash) buf.push_back('\n');
    };

    size_t li = 0;
    if (!lines.empty() && trimCopy(lines[0]) == "---") {
        for (size_t k = 1; k < lines.size() && k < 200; ++k) {
            const std::string t = trimCopy(lines[k]);
            if (t == "---" || t == "...") { li = k + 1; break; }
        }
    }

    for (; li < lines.size(); ++li) {
        const std::string& line = lines[li];

        if (inFence) {
            if (closesFence(line, fenceMark)) {
                emitCodeBlock(out, std::move(fenceBuf));
                fenceBuf.clear();
                inFence = false;
                fenceMark.clear();
            } else {
                size_t strip = 0;
                while (strip < fenceIndent && strip < line.size() && line[strip] == ' ')
                    ++strip;
                fenceBuf.append(line, strip, std::string::npos);
                fenceBuf.push_back('\n');
            }
            continue;
        }

        const std::string trimmed = trimCopy(line);
        const bool blank = trimmed.empty();

        if (startsWith(trimmed, "<!--") && trimmed.find("-->") == std::string::npos) {
            flushAll();
            while (li + 1 < lines.size() && lines[li].find("-->") == std::string::npos) ++li;
            continue;
        }

        const std::string fence = fenceRun(line);
        if (!fence.empty()) {
            flushAll();
            endList();
            inFence = true;
            fenceMark = fence;
            fenceIndent = (size_t)countLeadingSpaces(line);
            continue;
        }

        if (startsWith(trimmed, "$$")) {
            flushAll();
            std::string tex = trimmed.substr(2);
            if (tex.size() >= 2 && tex.compare(tex.size() - 2, 2, "$$") == 0) {
                tex.resize(tex.size() - 2);
            } else {
                while (li + 1 < lines.size()) {
                    std::string next = trimCopy(lines[++li]);
                    bool closes = next.size() >= 2
                        && next.compare(next.size() - 2, 2, "$$") == 0;
                    if (closes) next.resize(next.size() - 2);
                    if (!tex.empty() && !next.empty()) tex.push_back(' ');
                    tex += next;
                    if (closes) break;
                }
            }
            emitMathBlock(out, trimCopy(tex));
            continue;
        }

        if (blank) {
            emitParagraph(out, paraBuf);
            flushQuote();
            if (itemOpen) itemBuf.push_back('\n');
            lastWasBlank = true;
            continue;
        }
        const bool afterBlank = lastWasBlank;
        lastWasBlank = false;

        if (!paraBuf.empty() && (isSetextUnderline(trimmed, '=')
                                 || isSetextUnderline(trimmed, '-'))) {
            Block b;
            b.kind = BlockKind::Heading;
            b.level = trimmed[0] == '=' ? 1 : 2;
            b.cssFontRole = (int8_t)FontRole::Sans;
            b.runs = parseInlines(paraBuf);
            paraBuf.clear();
            if (out->title.empty() && b.level == 1) {
                std::string flat;
                for (const auto& r : b.runs) flat += r.text;
                out->title = std::move(flat);
            }
            out->blocks.push_back(std::move(b));
            continue;
        }

        if (startsWith(trimmed, "![")) {
            if (tryEmitImageBlock(trimmed, baseDir, out)) {
                Block img = std::move(out->blocks.back());
                out->blocks.pop_back();
                flushAll();
                endList();
                out->blocks.push_back(std::move(img));
                continue;
            }
        }

        if (isThematicBreak(line) && leadingColumns(line) < 4) {
            flushAll();
            endList();
            Block b;
            b.kind = BlockKind::ThematicBreak;
            out->blocks.push_back(std::move(b));
            continue;
        }

        if (line.find('|') != std::string::npos
                && li + 1 < lines.size()
                && isTableSeparator(lines[li + 1])) {
            flushAll();
            endList();
            Block b;
            b.kind = BlockKind::Table;
            auto hdrCells = splitTableCells(line);
            TableRow hdr;
            for (auto& ct : hdrCells) {
                TableCell tc;
                tc.isHeader = true;
                tc.runs = parseInlines(ct);
                hdr.cells.push_back(std::move(tc));
            }
            b.rows.push_back(std::move(hdr));
            ++li;
            while (li + 1 < lines.size()) {
                const std::string& next = lines[li + 1];
                if (trimCopy(next).empty() || next.find('|') == std::string::npos) break;
                ++li;
                auto dataCells = splitTableCells(lines[li]);
                TableRow dr;
                for (auto& ct : dataCells) {
                    TableCell tc;
                    tc.runs = parseInlines(ct);
                    dr.cells.push_back(std::move(tc));
                }
                b.rows.push_back(std::move(dr));
            }
            out->blocks.push_back(std::move(b));
            continue;
        }

        {
            int qd = 0;
            std::string stripped = line;
            while (!stripped.empty()) {
                int sp = countLeadingSpaces(stripped);
                if (sp < 4 && sp < (int)stripped.size() && stripped[sp] == '>') {
                    ++qd;
                    stripped = stripped.substr(sp + 1);
                    if (!stripped.empty() && stripped[0] == ' ') stripped = stripped.substr(1);
                } else {
                    break;
                }
            }
            if (qd > 0) {
                emitParagraph(out, paraBuf);
                flushItem();
                endList();
                if (qd != quoteDepth) flushQuote();
                quoteDepth = qd;
                const std::string inner = trimCopy(stripped);
                if (inner.empty()) {
                    flushQuote();
                    continue;
                }
                const int qh = atxLevel(inner);
                if (qh > 0) {
                    flushQuote();
                    Block b;
                    b.kind = BlockKind::Heading;
                    b.level = qh;
                    b.quoteDepth = qd;
                    b.cssFontRole = (int8_t)FontRole::Sans;
                    b.runs = parseInlines(atxTitle(inner, qh));
                    if (!b.runs.empty()) out->blocks.push_back(std::move(b));
                    continue;
                }
                int qord = 0, qcs = 0;
                if (tryListMarker(inner, &qord, &qcs)) {
                    flushQuote();
                    Block b;
                    b.kind = (qord == -1) ? BlockKind::UnorderedItem : BlockKind::OrderedItem;
                    b.level = (qord == -1) ? 0 : qord;
                    b.quoteDepth = qd;
                    b.runs.push_back(InlineRun{InlineKind::Strong,
                        qord == -1 ? std::string("\xE2\x80\xA2 ")
                                   : std::to_string(qord) + ". ", 0});
                    auto r = parseInlines(trimCopy(inner.substr(qcs)));
                    b.runs.insert(b.runs.end(), std::make_move_iterator(r.begin()),
                                  std::make_move_iterator(r.end()));
                    out->blocks.push_back(std::move(b));
                    continue;
                }
                appendLine(quoteBuf, stripped);
                continue;
            }
            if (!quoteBuf.empty() && !afterBlank && atxLevel(trimmed) == 0) {
                int o = 0, c = 0;
                if (!tryListMarker(line, &o, &c)) {
                    appendLine(quoteBuf, line);
                    continue;
                }
            }
            flushQuote();
        }

        int hLevel = atxLevel(trimmed);
        if (hLevel > 0 && leadingColumns(line) < 4) {
            flushAll();
            endList();
            Block b;
            b.kind = BlockKind::Heading;
            b.level = hLevel;
            b.cssFontRole = (int8_t)FontRole::Sans;
            b.runs = parseInlines(atxTitle(trimmed, hLevel));
            if (out->title.empty() && hLevel == 1) {
                std::string flat;
                for (const auto& r : b.runs) flat += r.text;
                out->title = std::move(flat);
            }
            if (!b.runs.empty()) out->blocks.push_back(std::move(b));
            continue;
        }

        if (startsWith(trimmed, "[^")) {
            const size_t close = trimmed.find("]:");
            if (close != std::string::npos && close > 2) {
                flushAll();
                endList();
                Block b;
                b.kind = BlockKind::Paragraph;
                b.runs.push_back(InlineRun{InlineKind::Superscript,
                                           trimmed.substr(2, close - 2), 0});
                b.runs.push_back(InlineRun{InlineKind::Text, " ", 0});
                auto r = parseInlines(trimCopy(trimmed.substr(close + 2)));
                b.runs.insert(b.runs.end(), std::make_move_iterator(r.begin()),
                              std::make_move_iterator(r.end()));
                out->blocks.push_back(std::move(b));
                continue;
            }
        }

        const int indent = leadingColumns(line);
        int ord = 0, cs = 0;
        if (tryListMarker(line, &ord, &cs)) {
            emitParagraph(out, paraBuf);
            flushItem();
            while (!listIndents.empty() && indent < listIndents.back()) {
                listIndents.pop_back();
                listNumbers.pop_back();
            }
            if (listIndents.empty() || indent >= listIndents.back() + 2) {
                listIndents.push_back(indent);
                listNumbers.push_back(0);
            }
            const int depth = std::max(0, (int)listIndents.size() - 1);
            int number = ord;
            if (ord != -1) {
                number = listNumbers.back() == 0 ? ord : listNumbers.back() + 1;
                listNumbers.back() = number;
            }
            std::string content = line.substr(cs);
            std::string marker = (ord == -1)
                ? std::string("\xE2\x80\xA2 ")
                : std::to_string(number) + ". ";
            const std::string ct = trimCopy(content);
            if (ord == -1 && ct.size() >= 3 && ct[0] == '['
                    && (ct[1] == ' ' || ct[1] == 'x' || ct[1] == 'X') && ct[2] == ']') {
                marker = ct[1] == ' ' ? "\xE2\x98\x90 " : "\xE2\x98\x91 ";
                content = ct.substr(3);
            }
            itemBlock = Block{};
            itemBlock.kind = (ord == -1) ? BlockKind::UnorderedItem : BlockKind::OrderedItem;
            itemBlock.level = (ord == -1) ? 0 : number;
            itemBlock.listDepth = depth;
            itemBlock.runs.push_back(InlineRun{InlineKind::Strong, marker, 0});
            itemBuf.clear();
            appendLine(itemBuf, content);
            itemOpen = true;
            itemContentCol = cs;
            continue;
        }

        if (itemOpen && (indent >= itemContentCol - 1 || !afterBlank)) {
            appendLine(itemBuf, line);
            continue;
        }
        if (itemOpen || !listIndents.empty()) endList();

        appendLine(paraBuf, line);
    }

    flushAll();

    if (inFence) {
        emitCodeBlock(out, std::move(fenceBuf));
    }
    return true;
}

bool parseMarkdownFromString(const std::string& src, Document* out) {
    return parseMarkdownImpl(src, out, {});
}

bool parseMarkdown(const std::filesystem::path& path, Document* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    return parseMarkdownImpl(decodeTextToUtf8(buf.str()), out, path.parent_path());
}

}
