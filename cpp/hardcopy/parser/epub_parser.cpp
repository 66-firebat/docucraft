#include "epub_parser.h"

#include <pugixml.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "../cancel.h"
#include "css_font_face.h"

#include "../util/natural_sort.h"
#include "../util/zip_reader.h"
#include "../image/image_decoder.h"
#include "../util/text_join.h"
#include "css_subset.h"
#include "txt_parser.h"

namespace hardcopy {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string parentPath(const std::string& s) {
    size_t p = s.find_last_of('/');
    if (p == std::string::npos) return "";
    return s.substr(0, p + 1);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i] == '\\' ? '/' : in[i]);
    }
    return out;
}

std::string resolvePath(const std::string& baseDir, const std::string& hrefIn) {
    std::string href = urlDecode(hrefIn);

    size_t cut = href.find_first_of("#?");
    if (cut != std::string::npos) href = href.substr(0, cut);
    if (href.empty()) return "";

    std::string combined;
    if (href[0] == '/') combined = href.substr(1);
    else                combined = baseDir + href;

    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= combined.size()) {
        size_t j = combined.find('/', i);
        if (j == std::string::npos) j = combined.size();
        std::string seg = combined.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(std::move(seg));
        }
        if (j == combined.size()) break;
        i = j + 1;
    }

    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out.push_back('/');
        out += parts[k];
    }
    return out;
}

bool isFontPath(const std::string& path) {
    std::string l = path;
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return (l.size() > 4 && (l.rfind(".ttf") == l.size() - 4
                          || l.rfind(".otf") == l.size() - 4))
        || (l.size() > 5 && l.rfind(".woff") == l.size() - 5)
        || (l.size() > 6 && l.rfind(".woff2") == l.size() - 6);
}

void addEmbeddedAlias(EmbeddedFontData* f, const std::string& family) {
    std::string key = normalizeFamilyKey(family);
    if (key.empty()) return;
    if (std::find(f->familyKeys.begin(), f->familyKeys.end(), key)
            == f->familyKeys.end()) f->familyKeys.push_back(std::move(key));
}

int registerEmbeddedFont(Document* out, ZipReader* zip,
                         const std::string& zipPath,
                         const std::string& family, bool bold, Logger* log,
                         bool italic = false) {
    if (!out || !zip || zipPath.empty()) return -1;
    for (size_t i = 0; i < out->embeddedFonts.size(); ++i) {
        if (out->embeddedFonts[i].sourcePath == zipPath) {
            addEmbeddedAlias(&out->embeddedFonts[i], family);
            out->embeddedFonts[i].bold = out->embeddedFonts[i].bold || bold;
            out->embeddedFonts[i].italic = out->embeddedFonts[i].italic && italic;
            return (int)i;
        }
    }
    if (out->embeddedFonts.size() >= (size_t)kMaxEmbeddedFonts) {
        if (log) log->log(LogLevel::Warn, "epub",
                          "embedded font limit reached; ignoring extra face");
        return -1;
    }
    EmbeddedFontData f;
    if (!zip->read(zipPath, &f.bytes) || f.bytes.empty()) return -1;
    f.sourcePath = zipPath;
    f.bold = bold;
    f.italic = italic;
    addEmbeddedAlias(&f, family);
    addEmbeddedAlias(&f, fileStemKey(zipPath));
    out->embeddedFonts.push_back(std::move(f));
    if (log) {
        std::string msg = "embedded font: " + zipPath;
        if (!family.empty()) msg += " as " + family;
        log->log(LogLevel::Info, "epub", msg.c_str());
    }
    return (int)out->embeddedFonts.size() - 1;
}

void collectFamilyRefsLegacy(const std::string& css,
                             std::vector<std::string>* out) {
    std::string low = css;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
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

void registerFontsFromCss(const std::string& css, const std::string& baseDir,
                          const std::vector<std::string>& fontEntries,
                          Document* out, ZipReader* zip, Logger* log) {
    std::vector<FontFaceDecl> faces;
    collectFontFaces(css, &faces);
    std::stable_partition(faces.begin(), faces.end(),
                          [](const FontFaceDecl& f) { return !f.italic; });
    std::vector<std::string> uprightFamilies;
    for (const auto& face : faces) {
        const std::string key = normalizeFamilyKey(face.family);
        if (!face.italic) {
            uprightFamilies.push_back(key);
        } else if (std::find(uprightFamilies.begin(), uprightFamilies.end(), key)
                       != uprightFamilies.end()) {
            continue;
        }
        for (const auto& url : face.urls) {
            std::string path = resolvePath(baseDir, url);
            auto it = std::find_if(fontEntries.begin(), fontEntries.end(),
                [&](const std::string& e) {
                    std::string a = e, b = path;
                    auto lower = [](unsigned char c) { return (char)std::tolower(c); };
                    std::transform(a.begin(), a.end(), a.begin(), lower);
                    std::transform(b.begin(), b.end(), b.begin(), lower);
                    return a == b;
                });
            if (it != fontEntries.end()) {
                registerEmbeddedFont(out, zip, *it, face.family,
                                     face.bold, log, face.italic);
                break;
            }
        }
    }

    std::vector<std::string> refs;
    collectFamilyRefsLegacy(css, &refs);
    for (const auto& family : refs) {
        const std::string key = normalizeFamilyKey(family);
        for (const auto& entry : fontEntries) {
            if (fileStemKey(entry) == key) {
                registerEmbeddedFont(out, zip, entry, family, false, log);
                break;
            }
        }
    }
}

bool readContainerOpfPath(ZipReader& zip, std::string* opfPath) {
    std::string xml;
    if (!zip.readText("META-INF/container.xml", &xml)) return false;
    pugi::xml_document doc;
    if (!doc.load_string(xml.c_str())) return false;
    auto rf = doc.select_node("//*[local-name()='rootfile']").node();
    if (!rf) return false;
    *opfPath = rf.attribute("full-path").as_string();
    return !opfPath->empty();
}

using ImageSink = std::function<void(const pugi::xml_node&)>;

void collectInlines(const pugi::xml_node& node, std::vector<InlineRun>& out,
                    InlineKind activeKind = InlineKind::Text,
                    const CssSheet* sheet = nullptr,
                    const CssStyle* inherited = nullptr,
                    const ImageSink* imageSink = nullptr,
                    const std::string& activeLink = {});

const char* localName(const pugi::xml_node& n) {
    const char* full = n.name();
    if (!full) return "";
    const char* colon = std::strchr(full, ':');
    return colon ? colon + 1 : full;
}

bool nameEq(const pugi::xml_node& n, const char* name) {
    return std::strcmp(localName(n), name) == 0;
}

std::vector<pugi::xml_node> childrenNamed(const pugi::xml_node& parent,
                                          const char* local) {
    std::vector<pugi::xml_node> out;
    for (auto c : parent.children()) {
        if (c.type() == pugi::node_element && nameEq(c, local))
            out.push_back(c);
    }
    return out;
}

bool isNonContentTag(const pugi::xml_node& n) {
    const char* nm = localName(n);
    if (!nm || !*nm) return false;
    return  std::strcmp(nm, "style") == 0
         || std::strcmp(nm, "script") == 0
         || std::strcmp(nm, "head") == 0
         || std::strcmp(nm, "title") == 0
         || std::strcmp(nm, "meta") == 0
         || std::strcmp(nm, "link") == 0
         || std::strcmp(nm, "object") == 0
         || std::strcmp(nm, "iframe") == 0
         || std::strcmp(nm, "noscript") == 0
         || std::strcmp(nm, "svg") == 0;
}

void applyRunStyle(InlineRun* run, const CssStyle* style) {
    if (!run || !style) return;
    run->cssFontRole = (int8_t)style->fontRole;
    std::memcpy(run->cssFamilyKey, style->familyKey,
                sizeof(run->cssFamilyKey));
    run->cssFontSizeEm = style->fontSizeEm;
}

class PugiCssNode : public CssNode {
 public:
    explicit PugiCssNode(pugi::xml_node n) : n_(n) {}
    std::string tag() const override { return localName(n_); }
    std::string classAttr() const override { return n_.attribute("class").as_string(); }
    std::unique_ptr<CssNode> parent() const override {
        pugi::xml_node p = n_.parent();
        if (!p || p.type() != pugi::node_element) return nullptr;
        return std::make_unique<PugiCssNode>(p);
    }
    std::unique_ptr<CssNode> prevSibling() const override {
        for (pugi::xml_node s = n_.previous_sibling(); s; s = s.previous_sibling()) {
            if (s.type() == pugi::node_element) return std::make_unique<PugiCssNode>(s);
        }
        return nullptr;
    }
 private:
    pugi::xml_node n_;
};

CssStyle ownStyleOf(const pugi::xml_node& node, const CssSheet& sheet) {
    CssStyle own = sheet.match(PugiCssNode(node));
    const char* inl = node.attribute("style").as_string();
    if (inl && *inl) own.apply(CssSheet::parseDeclarations(inl));
    return own;
}

InlineKind styledKind(InlineKind kind, const CssStyle& own) {
    if (kind == InlineKind::Code || kind == InlineKind::Ruby
        || kind == InlineKind::Superscript || kind == InlineKind::Subscript)
        return kind;
    if (own.fontBold == 1) return InlineKind::Strong;
    if (own.fontBold == 0 && kind == InlineKind::Strong)
        return own.fontItalic == 1 ? InlineKind::Emphasis : InlineKind::Text;
    if (own.fontItalic == 1 && kind == InlineKind::Text) return InlineKind::Emphasis;
    if (own.fontItalic == 0 && kind == InlineKind::Emphasis) return InlineKind::Text;
    return kind;
}

void svgImagesToSink(const pugi::xml_node& svg, const ImageSink* imageSink) {
    if (!imageSink || !*imageSink) return;
    for (auto im : svg.select_nodes(".//*[local-name()='image']"))
        (*imageSink)(im.node());
}

void collectText(const pugi::xml_node& node, std::vector<InlineRun>& out,
                 InlineKind kind, const CssSheet* sheet = nullptr,
                 const CssStyle* inherited = nullptr,
                 const ImageSink* imageSink = nullptr,
                 const std::string& activeLink = {}) {
    for (auto child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            std::string t = child.value();
            if (!t.empty()) {
                InlineRun run{kind, std::move(t), 0};
                applyRunStyle(&run, inherited);
                run.linkTarget = activeLink;
                out.push_back(std::move(run));
            }
        } else if (child.type() == pugi::node_element) {
            if (nameEq(child, "svg")) { svgImagesToSink(child, imageSink); continue; }
            if (isNonContentTag(child)) continue;
            collectInlines(child, out, kind, sheet, inherited, imageSink,
                           activeLink);
        }
    }
}

void collectInlines(const pugi::xml_node& node, std::vector<InlineRun>& out,
                    InlineKind activeKind, const CssSheet* sheet,
                    const CssStyle* inherited, const ImageSink* imageSink,
                    const std::string& activeLink) {
    if (nameEq(node, "img") || nameEq(node, "image")) {
        if (imageSink && *imageSink) (*imageSink)(node);
        return;
    }
    CssStyle effective;
    if (inherited) effective = *inherited;
    InlineKind kind = activeKind;
    if (sheet) {
        CssStyle own = ownStyleOf(node, *sheet);
        if (own.displayNone == 1) return;
        if (own.fontRole != FontRole::None || own.hasFamilyKey()) {
            effective.fontRole = own.fontRole;
            std::memcpy(effective.familyKey, own.familyKey,
                        sizeof(effective.familyKey));
        }
        if (own.fontSizeEm >= 0) {
            CssStyle sized = own;
            sized.resolveFontSize(effective);
            effective.fontSizeEm = sized.fontSizeEm;
        }
        kind = styledKind(kind, own);
    }
    if (nameEq(node, "br")) {
        InlineRun run{InlineKind::LineBreak, "", 0};
        applyRunStyle(&run, &effective);
        out.push_back(std::move(run));
        return;
    }
    if (nameEq(node, "rp")) return;
    if (nameEq(node, "rt")) {
        std::vector<InlineRun> inner;
        collectText(node, inner, InlineKind::Ruby, sheet, &effective, nullptr,
                    activeLink);
        std::string text;
        for (const auto& r : inner) text += r.text;
        text = trim(text);
        if (!text.empty()) {
            InlineRun run{InlineKind::Ruby,
                          std::string("（") + text + "）", 0};
            applyRunStyle(&run, &effective);
            out.push_back(std::move(run));
        }
        return;
    }
    if (nameEq(node, "em") || nameEq(node, "i") || nameEq(node, "cite")
        || nameEq(node, "var") || nameEq(node, "dfn")) {
        if (kind == InlineKind::Text) kind = InlineKind::Emphasis;
    } else if (nameEq(node, "strong") || nameEq(node, "b")) {
        if (kind != InlineKind::Superscript && kind != InlineKind::Subscript)
            kind = InlineKind::Strong;
    } else if (nameEq(node, "code") || nameEq(node, "kbd") || nameEq(node, "samp")
               || nameEq(node, "tt")) {
        kind = InlineKind::Code;
    } else if (nameEq(node, "sup")) {
        kind = InlineKind::Superscript;
    } else if (nameEq(node, "sub")) {
        kind = InlineKind::Subscript;
    }
    std::string link = activeLink;
    if (nameEq(node, "a")) {
        const char* href = node.attribute("href").as_string();
        if (href && *href) link = href;
        const std::string etype = node.attribute("epub:type").as_string();
        if (etype.find("noteref") != std::string::npos && kind != InlineKind::Code)
            kind = InlineKind::Superscript;
    }
    collectText(node, out, kind, sheet, &effective, imageSink, link);
}

bool isBlockTag(const pugi::xml_node& n) {
    static const char* names[] = {
        "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "li", "pre", "hr", "div", "section", "article",
        "blockquote", "header", "footer", "nav", "aside", "main",
        "figure", "figcaption", "table", "dl", "dt", "dd", "center",
        "address", "details", "summary", "hgroup", "caption", "body",
    };
    for (auto* nm : names) if (nameEq(n, nm)) return true;
    return false;
}

bool isInlineTag(const pugi::xml_node& n) {
    static const char* names[] = {
        "a", "span", "em", "i", "b", "strong", "code", "sup", "sub", "small",
        "big", "u", "s", "strike", "del", "ins", "mark", "abbr", "cite", "q",
        "dfn", "kbd", "samp", "var", "time", "font", "ruby", "rb", "rt", "rp",
        "rtc", "label", "bdi", "bdo", "wbr", "br", "tt", "nobr", "acronym",
    };
    for (auto* nm : names) if (nameEq(n, nm)) return true;
    return false;
}

bool hasBlockChild(const pugi::xml_node& n) {
    for (auto c : n.children()) {
        if (c.type() != pugi::node_element) continue;
        if (isInlineTag(c) || isNonContentTag(c)) continue;
        if (nameEq(c, "img") || nameEq(c, "image")) continue;
        return true;
    }
    return false;
}

bool isEffectivelyEmpty(const std::vector<InlineRun>& runs) {
    for (const auto& r : runs) {
        if (r.kind == InlineKind::LineBreak) continue;
        for (char c : r.text) {
            unsigned char u = (unsigned char)c;
            if (u == ' ' || u == '\t' || u == '\n' || u == '\r') continue;
            if (u == 0xC2 || u == 0xA0) continue;
            return false;
        }
    }
    return true;
}

bool isWideCp(uint32_t cp) {
    return textjoin::isWideScript(cp) || (cp >= 0x2010 && cp <= 0x2027);
}

void collapseWhitespace(std::vector<InlineRun>& runs) {
    bool pendingSpace = false;
    bool pendingNewline = false;
    uint32_t prevCp = 0;
    bool atLineStart = true;
    InlineRun* spaceOwner = nullptr;
    for (auto& run : runs) {
        if (run.kind == InlineKind::LineBreak) {
            pendingSpace = pendingNewline = false;
            atLineStart = true;
            prevCp = 0;
            continue;
        }
        std::string out;
        out.reserve(run.text.size());
        const std::string& t = run.text;
        for (size_t i = 0; i < t.size();) {
            const uint32_t cp = textjoin::decodeAt(t, i);
            size_t len = 1;
            const uint8_t c0 = (uint8_t)t[i];
            if (c0 >= 0xF0) len = 4; else if (c0 >= 0xE0) len = 3; else if (c0 >= 0xC0) len = 2;
            if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') {
                if (!atLineStart) {
                    if (!pendingSpace) spaceOwner = &run;
                    pendingSpace = true;
                }
                if (cp == '\n' || cp == '\r') pendingNewline = true;
                i += 1;
                continue;
            }
            if (pendingSpace) {
                const bool drop = pendingNewline
                    && ((textjoin::isWideScript(prevCp) && isWideCp(cp))
                        || (isWideCp(prevCp) && textjoin::isWideScript(cp)));
                if (!drop) {
                    if (spaceOwner == &run) out.push_back(' ');
                    else if (spaceOwner) spaceOwner->text.push_back(' ');
                }
            }
            pendingSpace = pendingNewline = false;
            spaceOwner = nullptr;
            out.append(t, i, len);
            prevCp = cp;
            atLineStart = false;
            i += len;
        }
        run.text = std::move(out);
    }
}

std::string collapseWhitespaceText(const std::string& s) {
    std::vector<InlineRun> runs{InlineRun{InlineKind::Text, s, 0}};
    collapseWhitespace(runs);
    return runs[0].text;
}

CssStyle blockStyleFrom(const CssStyle& own, const CssStyle& inh) {
    CssStyle s = own;
    if (s.lineHeightMul < 0) s.lineHeightMul = inh.lineHeightMul;
    if (s.textIndentEm  < 0) s.textIndentEm  = inh.textIndentEm;
    if (s.textAlign     < 0) s.textAlign     = inh.textAlign;
    if (s.fontBold      < 0) s.fontBold      = inh.fontBold;
    if (s.fontItalic    < 0) s.fontItalic    = inh.fontItalic;
    if (s.listStyleNone < 0) s.listStyleNone = inh.listStyleNone;
    if (s.fontRole == FontRole::None && !s.hasFamilyKey()) {
        s.fontRole = inh.fontRole;
        std::memcpy(s.familyKey, inh.familyKey, sizeof(s.familyKey));
    }
    s.resolveFontSize(inh);
    return s;
}

void emitImage(const pugi::xml_node& node, Document* out, ZipReader* zip,
               const std::string& docDir, Logger* log, int quoteDepth,
               bool fullPage, bool inlineImage);
bool imageIsInline(const pugi::xml_node& node, const CssStyle& style);

void emitPageBreak(Document* out) {
    if (!out || (!out->blocks.empty() && out->blocks.back().kind == BlockKind::PageBreak))
        return;
    Block b;
    b.kind = BlockKind::PageBreak;
    out->blocks.push_back(std::move(b));
}

bool nodeHasText(const pugi::xml_node& node);

bool looksLikePageBreakMarker(const pugi::xml_node& node) {
    const std::string etype = node.attribute("epub:type").as_string();
    if (etype.find("pagebreak") != std::string::npos) return false;
    if (node.attribute("title") && !nodeHasText(node)) return false;
    auto match = [](const char* raw) {
        if (!raw || !*raw) return false;
        std::string v;
        v.reserve(std::strlen(raw));
        for (const char* p = raw; *p; ++p)
            v.push_back((char)std::tolower((unsigned char)*p));
        return v.find("pagebreak") != std::string::npos
            || v.find("page-break") != std::string::npos
            || v.find("page_break") != std::string::npos
            || v.find("calibre_pb") != std::string::npos;
    };
    return match(node.attribute("id").as_string())
        || match(node.attribute("class").as_string());
}

thread_local std::vector<std::string> g_pendingAnchors;

void collectAnchorIds(const pugi::xml_node& node, bool deep) {
    const char* id = node.attribute("id").as_string();
    if (id && *id) g_pendingAnchors.emplace_back(id);
    const char* nm = node.attribute("name").as_string();
    if (nameEq(node, "a") && nm && *nm) g_pendingAnchors.emplace_back(nm);
    if (!deep) return;
    for (auto c : node.children())
        if (c.type() == pugi::node_element) collectAnchorIds(c, true);
}

void attachPendingAnchors(Document* out, size_t from) {
    if (g_pendingAnchors.empty()) return;
    for (size_t i = from; i < out->blocks.size(); ++i) {
        Block& b = out->blocks[i];
        if (b.kind == BlockKind::PageBreak) continue;
        b.anchorIds.insert(b.anchorIds.end(), g_pendingAnchors.begin(),
                           g_pendingAnchors.end());
        g_pendingAnchors.clear();
        return;
    }
}

Block makeStyledBlock(BlockKind kind, int level, int listDepth, int quoteDepth,
                      const CssStyle& style) {
    Block b;
    b.kind = kind;
    b.level = level;
    b.listDepth = listDepth;
    b.quoteDepth = quoteDepth;
    b.cssLineHeightMul  = style.lineHeightMul;
    b.cssMarginTopEm    = style.marginTopEm;
    b.cssMarginBottomEm = style.marginBottomEm;
    b.cssTextIndentEm   = style.textIndentEm;
    b.cssTextAlign      = style.textAlign;
    b.cssFontRole       = (int8_t)style.fontRole;
    std::memcpy(b.cssFamilyKey, style.familyKey, sizeof(b.cssFamilyKey));
    b.cssFontSizeEm     = style.fontSizeEm;
    return b;
}

struct WalkEnv {
    Document* out;
    ZipReader* zip;
    std::string docDir;
    Logger* log;
    const CssSheet* sheet;
};

bool emitInlineGroup(const std::vector<pugi::xml_node>& nodes, BlockKind kind,
                     int level, int listDepth, int quoteDepth,
                     const CssStyle& style, const WalkEnv& env,
                     const std::string& marker = {}, bool splitOnBreak = false) {
    Document* out = env.out;
    const size_t blocksBefore = out->blocks.size();
    Block b = makeStyledBlock(kind, level, listDepth, quoteDepth, style);
    bool markerPending = !marker.empty();
    auto flushText = [&]() {
        collapseWhitespace(b.runs);
        while (!b.runs.empty() && b.runs.back().kind == InlineKind::LineBreak)
            b.runs.pop_back();
        if (!b.runs.empty() && !isEffectivelyEmpty(b.runs)) {
            if (markerPending) {
                b.runs.insert(b.runs.begin(), InlineRun{InlineKind::Text, marker, 0});
                markerPending = false;
            }
            out->blocks.push_back(std::move(b));
        }
        b = makeStyledBlock(kind, level, listDepth, quoteDepth, style);
    };
    ImageSink sink = [&](const pugi::xml_node& img) {
        if (!env.zip) return;
        flushText();
        const bool isInline = imageIsInline(img, ownStyleOf(img, *env.sheet));
        emitImage(img, out, env.zip, env.docDir, env.log, quoteDepth, false, isInline);
    };
    const InlineKind baseKind = styledKind(InlineKind::Text, style);
    for (const auto& n : nodes) {
        if (n.type() == pugi::node_pcdata || n.type() == pugi::node_cdata) {
            InlineRun run{baseKind, n.value(), 0};
            applyRunStyle(&run, &style);
            b.runs.push_back(std::move(run));
            continue;
        }
        if (n.type() != pugi::node_element) continue;
        if (splitOnBreak && nameEq(n, "br")) {
            flushText();
            continue;
        }
        if (nameEq(n, "svg")) { svgImagesToSink(n, &sink); continue; }
        if (isNonContentTag(n)) continue;
        collectAnchorIds(n, true);
        collectInlines(n, b.runs, baseKind, env.sheet, &style, &sink);
    }
    flushText();
    attachPendingAnchors(out, blocksBefore);
    return out->blocks.size() > blocksBefore;
}

std::vector<pugi::xml_node> childNodes(const pugi::xml_node& node) {
    std::vector<pugi::xml_node> v;
    for (auto c : node.children()) v.push_back(c);
    return v;
}

void appendBlockKind(BlockKind kind, int level, int listDepth, int quoteDepth,
                     const pugi::xml_node& node, const CssStyle& style,
                     const WalkEnv& env) {
    Document* out = env.out;
    const size_t blocksBefore = out->blocks.size();
    collectAnchorIds(node, false);
    emitInlineGroup(childNodes(node), kind, level, listDepth, quoteDepth, style, env);

    if (kind == BlockKind::Paragraph && out->blocks.size() == blocksBefore
            && !out->blocks.empty()) {
        const Block& prev = out->blocks.back();
        const bool prevBlank =
            prev.kind == BlockKind::Paragraph && isEffectivelyEmpty(prev.runs);
        const bool prevBreak = prev.kind == BlockKind::PageBreak;
        if (!prevBlank && !prevBreak) {
            Block blank = makeStyledBlock(kind, level, listDepth, quoteDepth, style);
            blank.runs.push_back(InlineRun{InlineKind::LineBreak, "", 0});
            out->blocks.push_back(std::move(blank));
        }
    }
}

bool isImageExt(const std::string& href) {
    auto dot = href.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext;
    for (size_t i = dot; i < href.size(); ++i)
        ext.push_back((char)std::tolower((unsigned char)href[i]));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".gif" || ext == ".bmp" || ext == ".webp";
}

bool nodeHasText(const pugi::xml_node& node) {
    for (auto child : node.children()) {
        if (child.type() == pugi::node_pcdata
                || child.type() == pugi::node_cdata) {
            const char* text = child.value();
            while (*text && std::isspace((unsigned char)*text)) ++text;
            if (*text) return true;
        } else if (child.type() == pugi::node_element && nodeHasText(child)) {
            return true;
        }
    }
    return false;
}

bool imageIsInline(const pugi::xml_node& node, const CssStyle& style) {
    if (style.cssFloat > 0) return true;
    for (auto sibling : node.parent().children()) {
        if (sibling == node) continue;
        if (sibling.type() == pugi::node_pcdata
                || sibling.type() == pugi::node_cdata) {
            const char* text = sibling.value();
            while (*text && std::isspace((unsigned char)*text)) ++text;
            if (*text) return true;
        } else if (sibling.type() == pugi::node_element
                && !isBlockTag(sibling) && nodeHasText(sibling)) {
            return true;
        }
    }
    return false;
}

void emitImage(const pugi::xml_node& node, Document* out, ZipReader* zip,
               const std::string& docDir, Logger* log, int quoteDepth,
               bool fullPage, bool inlineImage) {
    if (!zip) return;
    const char* src = node.attribute("src").as_string();
    if (!src || !*src) src = node.attribute("xlink:href").as_string();
    if (!src || !*src) src = node.attribute("href").as_string();
    if (!src || !*src) return;

    std::string imgPath = resolvePath(docDir, src);
    std::vector<uint8_t> imgData;
    if (imgPath.empty() || !zip->read(imgPath, &imgData) || imgData.empty()) {
        if (log) {
            log->log(LogLevel::Warn, "epub",
                     ("cannot load image src=" + std::string(src)
                      + " (resolved: " + imgPath + ")").c_str());
        }
        const std::string alt = trim(node.attribute("alt").as_string());
        if (!alt.empty() && !fullPage) {
            Block b;
            b.kind = BlockKind::Paragraph;
            b.quoteDepth = quoteDepth;
            b.runs.push_back(InlineRun{InlineKind::Emphasis, alt, 0});
            out->blocks.push_back(std::move(b));
        }
        return;
    }

    ImageData img;
    img.rawBytes = std::move(imgData);
    img.altText = node.attribute("alt").as_string();
    int w = 0, h = 0;
    if (imageInfo(img.rawBytes.data(), img.rawBytes.size(), &w, &h)) {
        img.width = w;
        img.height = h;
    }
    int idx = (int)out->images.size();
    out->images.push_back(std::move(img));

    Block b;
    b.kind = BlockKind::Image;
    b.imageIndex = idx;
    b.quoteDepth = quoteDepth;
    b.imageFullPage = fullPage;
    b.imageInline = inlineImage && !fullPage;
    out->blocks.push_back(std::move(b));
}

void walkBlocks(const pugi::xml_node& node, Document* out, int listDepth, int quoteDepth,
                ZipReader* zip, const std::string& docDir, Logger* log,
                const CssSheet& sheet, const CssStyle& inh);

void emitPre(const pugi::xml_node& node, int quoteDepth, const CssStyle& style,
             const WalkEnv& env) {
    std::vector<InlineRun> runs;
    collectText(node, runs, InlineKind::Code);
    std::string flat;
    for (auto& r : runs) {
        if (r.kind == InlineKind::LineBreak) flat.push_back('\n');
        else flat += r.text;
    }
    if (!flat.empty() && flat[0] == '\r') flat.erase(0, 1);
    if (!flat.empty() && flat[0] == '\n') flat.erase(0, 1);
    while (!flat.empty() && (flat.back() == '\n' || flat.back() == '\r' || flat.back() == ' '))
        flat.pop_back();
    if (flat.empty()) return;
    Block b = makeStyledBlock(BlockKind::CodeBlock, 0, 0, quoteDepth, style);
    size_t start = 0;
    while (start <= flat.size()) {
        size_t end = flat.find('\n', start);
        std::string line = flat.substr(start, end == std::string::npos
                                                  ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
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
        if (expanded.empty()) expanded = " ";
        b.runs.push_back(InlineRun{InlineKind::Code, std::move(expanded), 0});
        if (end == std::string::npos) break;
        b.runs.push_back(InlineRun{InlineKind::LineBreak, {}, 0});
        start = end + 1;
    }
    env.out->blocks.push_back(std::move(b));
}

std::string listMarker(bool ordered, int number, int depth) {
    if (!ordered) {
        static const char* kBullets[] = {"\xE2\x80\xA2 ",
                                         "\xE2\x97\xA6 ",
                                         "\xE2\x96\xAA "};
        return kBullets[std::min(depth, 2)];
    }
    return std::to_string(number) + ". ";
}

void emitList(const pugi::xml_node& list, bool ordered, int listDepth,
              int quoteDepth, const CssStyle& listEff, const WalkEnv& env) {
    int number = 1;
    if (ordered) {
        const char* start = list.attribute("start").as_string();
        if (start && *start) number = std::atoi(start);
    }
    for (auto li : list.children()) {
        if (li.type() != pugi::node_element) continue;
        if (!nameEq(li, "li")) {
            walkBlocks(li, env.out, listDepth, quoteDepth, env.zip, env.docDir,
                       env.log, *env.sheet, listEff);
            continue;
        }
        const CssStyle liOwn = ownStyleOf(li, *env.sheet);
        if (liOwn.displayNone == 1) continue;
        const CssStyle liEff = blockStyleFrom(liOwn, listEff);
        const char* value = li.attribute("value").as_string();
        if (ordered && value && *value) number = std::atoi(value);
        const std::string marker = liEff.listStyleNone == 1
            ? std::string() : listMarker(ordered, number, listDepth);
        const BlockKind kind = ordered ? BlockKind::OrderedItem : BlockKind::UnorderedItem;
        collectAnchorIds(li, false);

        bool itemEmitted = false;
        std::vector<pugi::xml_node> group;
        auto flushGroup = [&]() {
            if (group.empty()) return;
            if (emitInlineGroup(group, itemEmitted ? BlockKind::Paragraph : kind,
                                ordered ? number : 0, listDepth, quoteDepth, liEff,
                                env, itemEmitted ? std::string() : marker))
                itemEmitted = true;
            group.clear();
        };
        for (auto c : li.children()) {
            const bool inlineNode = c.type() != pugi::node_element
                || isInlineTag(c) || nameEq(c, "img") || nameEq(c, "image")
                || nameEq(c, "svg");
            if (inlineNode) { group.push_back(c); continue; }
            if (isNonContentTag(c)) continue;
            flushGroup();
            const CssStyle cOwn = ownStyleOf(c, *env.sheet);
            if (cOwn.displayNone == 1) continue;
            const CssStyle cEff = blockStyleFrom(cOwn, liEff);
            if (nameEq(c, "ul") || nameEq(c, "ol")) {
                emitList(c, nameEq(c, "ol"), listDepth + 1, quoteDepth, cEff, env);
            } else if (!itemEmitted && !hasBlockChild(c)) {
                collectAnchorIds(c, false);
                if (emitInlineGroup(childNodes(c), kind, ordered ? number : 0,
                                    listDepth, quoteDepth, cEff, env, marker))
                    itemEmitted = true;
            } else {
                walkBlocks(c, env.out, listDepth + 1, quoteDepth, env.zip,
                           env.docDir, env.log, *env.sheet, cEff);
            }
        }
        flushGroup();
        ++number;
    }
}

void emitTable(const pugi::xml_node& table, int listDepth, int quoteDepth,
               const CssStyle& eff, const WalkEnv& env) {
    std::vector<pugi::xml_node> rows;
    for (auto r : table.select_nodes(".//*[local-name()='tr']")) {
        pugi::xml_node p = r.node().parent();
        while (p && !nameEq(p, "table")) p = p.parent();
        if (p == table) rows.push_back(r.node());
    }
    size_t maxCols = 0;
    for (const auto& r : rows) {
        size_t cols = 0;
        for (auto c : r.children()) {
            if (c.type() != pugi::node_element) continue;
            if ((nameEq(c, "td") || nameEq(c, "th")) && nodeHasText(c))
                cols += (size_t)std::max(1, c.attribute("colspan").as_int(1));
        }
        maxCols = std::max(maxCols, cols);
    }
    if (rows.empty() || maxCols < 2) {
        for (const auto& r : rows) {
            for (auto c : r.children()) {
                if (c.type() != pugi::node_element) continue;
                if (!nameEq(c, "td") && !nameEq(c, "th")) continue;
                walkBlocks(c, env.out, listDepth, quoteDepth, env.zip, env.docDir,
                           env.log, *env.sheet, blockStyleFrom(ownStyleOf(c, *env.sheet), eff));
            }
        }
        if (rows.empty())
            walkBlocks(table, env.out, listDepth, quoteDepth, env.zip, env.docDir,
                       env.log, *env.sheet, eff);
        return;
    }
    if (auto cap = table.select_node("./*[local-name()='caption']").node())
        appendBlockKind(BlockKind::Paragraph, 0, listDepth, quoteDepth, cap,
                        blockStyleFrom(ownStyleOf(cap, *env.sheet), eff), env);
    Block b = makeStyledBlock(BlockKind::Table, 0, listDepth, quoteDepth, eff);
    for (const auto& r : rows) {
        TableRow row;
        for (auto c : r.children()) {
            if (c.type() != pugi::node_element) continue;
            const bool th = nameEq(c, "th");
            if (!th && !nameEq(c, "td")) continue;
            TableCell cell;
            cell.isHeader = th;
            cell.colspan = std::max(1, c.attribute("colspan").as_int(1));
            cell.rowspan = std::max(1, c.attribute("rowspan").as_int(1));
            const CssStyle cEff = blockStyleFrom(ownStyleOf(c, *env.sheet), eff);
            std::function<void(const pugi::xml_node&)> gather = [&](const pugi::xml_node& n) {
                for (auto k : n.children()) {
                    if (k.type() == pugi::node_element && !isInlineTag(k)
                        && !isNonContentTag(k) && !nameEq(k, "img")) {
                        if (!cell.runs.empty()
                            && cell.runs.back().kind != InlineKind::LineBreak)
                            cell.runs.push_back(InlineRun{InlineKind::LineBreak, {}, 0});
                        gather(k);
                        continue;
                    }
                    if (k.type() == pugi::node_pcdata || k.type() == pugi::node_cdata) {
                        cell.runs.push_back(InlineRun{th ? InlineKind::Strong
                                                         : styledKind(InlineKind::Text, cEff),
                                                      k.value(), 0});
                    } else if (k.type() == pugi::node_element && !isNonContentTag(k)) {
                        collectInlines(k, cell.runs, th ? InlineKind::Strong
                                                        : styledKind(InlineKind::Text, cEff),
                                       env.sheet, &cEff, nullptr);
                    }
                }
            };
            gather(c);
            collapseWhitespace(cell.runs);
            while (!cell.runs.empty() && cell.runs.back().kind == InlineKind::LineBreak)
                cell.runs.pop_back();
            row.cells.push_back(std::move(cell));
        }
        if (!row.cells.empty()) b.rows.push_back(std::move(row));
    }
    if (!b.rows.empty()) env.out->blocks.push_back(std::move(b));
}

void walkBlocks(const pugi::xml_node& node, Document* out, int listDepth, int quoteDepth,
                ZipReader* zip, const std::string& docDir, Logger* log,
                const CssSheet& sheet, const CssStyle& inh) {
    const WalkEnv env{out, zip, docDir, log, &sheet};
    std::vector<pugi::xml_node> group;
    auto flushGroup = [&]() {
        if (group.empty()) return;
        emitInlineGroup(group, BlockKind::Paragraph, 0, 0, quoteDepth, inh, env,
                        {}, true);
        group.clear();
    };
    for (auto child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            group.push_back(child);
            continue;
        }
        if (child.type() != pugi::node_element) continue;
        const char* n = localName(child);
        if (!n || !*n) continue;
        if (isNonContentTag(child) && std::strcmp(n, "svg") != 0) continue;

        const CssStyle own = ownStyleOf(child, sheet);
        if (own.displayNone == 1) continue;
        const bool pageBreakMarker = looksLikePageBreakMarker(child);

        const bool isImg = std::strcmp(n, "img") == 0 || std::strcmp(n, "image") == 0;
        if (!pageBreakMarker && own.pageBreakBefore != 1 && own.pageBreakAfter != 1
            && (isInlineTag(child) || (isImg && imageIsInline(child, own)))) {
            group.push_back(child);
            continue;
        }
        flushGroup();

        const CssStyle eff = blockStyleFrom(own, inh);
        if (own.pageBreakBefore == 1 || pageBreakMarker)
            emitPageBreak(out);

        if (std::strcmp(n, "svg") == 0) {
            auto imgs = child.select_nodes(".//*[local-name()='image']");
            const bool loneImage = (imgs.size() == 1);
            for (auto im : imgs) {
                emitImage(im.node(), out, zip, docDir, log, quoteDepth,
                          loneImage, false);
            }
            if (own.pageBreakAfter == 1) emitPageBreak(out);
            continue;
        }

        const size_t blocksBefore = out->blocks.size();
        collectAnchorIds(child, false);

        if (isImg) {
            emitImage(child, out, zip, docDir, log, quoteDepth,
                      false, imageIsInline(child, own));
        } else if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && n[2] == 0) {
            appendBlockKind(BlockKind::Heading, n[1] - '0', 0, quoteDepth, child, eff, env);
        } else if (std::strcmp(n, "hr") == 0) {
            Block b;
            b.kind = BlockKind::ThematicBreak;
            b.quoteDepth = quoteDepth;
            out->blocks.push_back(std::move(b));
        } else if (std::strcmp(n, "pre") == 0) {
            emitPre(child, quoteDepth, eff, env);
        } else if (std::strcmp(n, "ul") == 0 || std::strcmp(n, "ol") == 0) {
            emitList(child, std::strcmp(n, "ol") == 0, listDepth, quoteDepth, eff, env);
        } else if (std::strcmp(n, "li") == 0) {
            appendBlockKind(BlockKind::UnorderedItem, 0, listDepth, quoteDepth, child, eff, env);
        } else if (std::strcmp(n, "table") == 0) {
            emitTable(child, listDepth, quoteDepth, eff, env);
        } else if (std::strcmp(n, "blockquote") == 0) {
            walkBlocks(child, out, listDepth, quoteDepth + 1, zip, docDir, log, sheet, eff);
        } else if (hasBlockChild(child)) {
            walkBlocks(child, out, listDepth, quoteDepth, zip, docDir, log, sheet, eff);
        } else if (std::strcmp(n, "p") == 0 || std::strcmp(n, "dt") == 0
                   || std::strcmp(n, "dd") == 0 || std::strcmp(n, "figcaption") == 0
                   || std::strcmp(n, "caption") == 0 || std::strcmp(n, "summary") == 0) {
            appendBlockKind(BlockKind::Paragraph, 0, 0, quoteDepth, child, eff, env);
        } else {
            emitInlineGroup(childNodes(child), BlockKind::Paragraph, 0, 0, quoteDepth,
                            eff, env, {}, true);
        }
        attachPendingAnchors(out, blocksBefore);
        if (own.backgroundGray > 0) {
            static std::atomic<uint32_t> nextBoxId{0};
            uint32_t id = ++nextBoxId;
            if (id == 0) id = ++nextBoxId;
            for (size_t k = blocksBefore; k < out->blocks.size(); ++k) {
                Block& bk = out->blocks[k];
                if (bk.boxId != 0 || bk.kind == BlockKind::PageBreak) continue;
                bk.boxId = id;
                bk.boxGray = (uint8_t)own.backgroundGray;
            }
        }
        if (own.pageBreakAfter == 1)
            emitPageBreak(out);
    }
    flushGroup();
}

std::string expandHtmlEntities(const std::string& in) {
    static const std::map<std::string, uint32_t> kEnts = {
        {"nbsp",0xA0},{"iexcl",0xA1},{"cent",0xA2},{"pound",0xA3},{"curren",0xA4},
        {"yen",0xA5},{"brvbar",0xA6},{"sect",0xA7},{"uml",0xA8},{"copy",0xA9},
        {"ordf",0xAA},{"laquo",0xAB},{"not",0xAC},{"shy",0xAD},{"reg",0xAE},
        {"macr",0xAF},{"deg",0xB0},{"plusmn",0xB1},{"sup2",0xB2},{"sup3",0xB3},
        {"acute",0xB4},{"micro",0xB5},{"para",0xB6},{"middot",0xB7},{"cedil",0xB8},
        {"sup1",0xB9},{"ordm",0xBA},{"raquo",0xBB},{"frac14",0xBC},{"frac12",0xBD},
        {"frac34",0xBE},{"iquest",0xBF},{"Agrave",0xC0},{"Aacute",0xC1},{"Acirc",0xC2},
        {"Atilde",0xC3},{"Auml",0xC4},{"Aring",0xC5},{"AElig",0xC6},{"Ccedil",0xC7},
        {"Egrave",0xC8},{"Eacute",0xC9},{"Ecirc",0xCA},{"Euml",0xCB},{"Igrave",0xCC},
        {"Iacute",0xCD},{"Icirc",0xCE},{"Iuml",0xCF},{"ETH",0xD0},{"Ntilde",0xD1},
        {"Ograve",0xD2},{"Oacute",0xD3},{"Ocirc",0xD4},{"Otilde",0xD5},{"Ouml",0xD6},
        {"times",0xD7},{"Oslash",0xD8},{"Ugrave",0xD9},{"Uacute",0xDA},{"Ucirc",0xDB},
        {"Uuml",0xDC},{"Yacute",0xDD},{"THORN",0xDE},{"szlig",0xDF},{"agrave",0xE0},
        {"aacute",0xE1},{"acirc",0xE2},{"atilde",0xE3},{"auml",0xE4},{"aring",0xE5},
        {"aelig",0xE6},{"ccedil",0xE7},{"egrave",0xE8},{"eacute",0xE9},{"ecirc",0xEA},
        {"euml",0xEB},{"igrave",0xEC},{"iacute",0xED},{"icirc",0xEE},{"iuml",0xEF},
        {"eth",0xF0},{"ntilde",0xF1},{"ograve",0xF2},{"oacute",0xF3},{"ocirc",0xF4},
        {"otilde",0xF5},{"ouml",0xF6},{"divide",0xF7},{"oslash",0xF8},{"ugrave",0xF9},
        {"uacute",0xFA},{"ucirc",0xFB},{"uuml",0xFC},{"yacute",0xFD},{"thorn",0xFE},
        {"yuml",0xFF},{"OElig",0x152},{"oelig",0x153},{"Scaron",0x160},{"scaron",0x161},
        {"Yuml",0x178},{"fnof",0x192},{"circ",0x2C6},{"tilde",0x2DC},
        {"Alpha",0x391},{"Beta",0x392},{"Gamma",0x393},{"Delta",0x394},{"Epsilon",0x395},
        {"Zeta",0x396},{"Eta",0x397},{"Theta",0x398},{"Iota",0x399},{"Kappa",0x39A},
        {"Lambda",0x39B},{"Mu",0x39C},{"Nu",0x39D},{"Xi",0x39E},{"Omicron",0x39F},
        {"Pi",0x3A0},{"Rho",0x3A1},{"Sigma",0x3A3},{"Tau",0x3A4},{"Upsilon",0x3A5},
        {"Phi",0x3A6},{"Chi",0x3A7},{"Psi",0x3A8},{"Omega",0x3A9},
        {"alpha",0x3B1},{"beta",0x3B2},{"gamma",0x3B3},{"delta",0x3B4},{"epsilon",0x3B5},
        {"zeta",0x3B6},{"eta",0x3B7},{"theta",0x3B8},{"iota",0x3B9},{"kappa",0x3BA},
        {"lambda",0x3BB},{"mu",0x3BC},{"nu",0x3BD},{"xi",0x3BE},{"omicron",0x3BF},
        {"pi",0x3C0},{"rho",0x3C1},{"sigmaf",0x3C2},{"sigma",0x3C3},{"tau",0x3C4},
        {"upsilon",0x3C5},{"phi",0x3C6},{"chi",0x3C7},{"psi",0x3C8},{"omega",0x3C9},
        {"ensp",0x2002},{"emsp",0x2003},{"thinsp",0x2009},{"zwnj",0x200C},{"zwj",0x200D},
        {"lrm",0x200E},{"rlm",0x200F},{"ndash",0x2013},{"mdash",0x2014},{"horbar",0x2015},
        {"lsquo",0x2018},{"rsquo",0x2019},{"sbquo",0x201A},{"ldquo",0x201C},{"rdquo",0x201D},
        {"bdquo",0x201E},{"dagger",0x2020},{"Dagger",0x2021},{"bull",0x2022},
        {"hellip",0x2026},{"permil",0x2030},{"prime",0x2032},{"Prime",0x2033},
        {"lsaquo",0x2039},{"rsaquo",0x203A},{"oline",0x203E},{"frasl",0x2044},
        {"euro",0x20AC},{"trade",0x2122},{"larr",0x2190},{"uarr",0x2191},{"rarr",0x2192},
        {"darr",0x2193},{"harr",0x2194},{"lArr",0x21D0},{"rArr",0x21D2},{"hArr",0x21D4},
        {"forall",0x2200},{"part",0x2202},{"exist",0x2203},{"empty",0x2205},{"nabla",0x2207},
        {"isin",0x2208},{"notin",0x2209},{"ni",0x220B},{"prod",0x220F},{"sum",0x2211},
        {"minus",0x2212},{"lowast",0x2217},{"radic",0x221A},{"prop",0x221D},{"infin",0x221E},
        {"ang",0x2220},{"and",0x2227},{"or",0x2228},{"cap",0x2229},{"cup",0x222A},
        {"int",0x222B},{"there4",0x2234},{"sim",0x223C},{"cong",0x2245},{"asymp",0x2248},
        {"ne",0x2260},{"equiv",0x2261},{"le",0x2264},{"ge",0x2265},{"sub",0x2282},
        {"sup",0x2283},{"sube",0x2286},{"supe",0x2287},{"oplus",0x2295},{"otimes",0x2297},
        {"perp",0x22A5},{"sdot",0x22C5},{"loz",0x25CA},{"spades",0x2660},{"clubs",0x2663},
        {"hearts",0x2665},{"diams",0x2666},
    };
    if (in.find('&') == std::string::npos) return in;
    std::string s;
    s.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { s.push_back(in[i]); continue; }
        const size_t semi = in.find(';', i + 1);
        if (semi != std::string::npos && semi - i <= 10 && semi > i + 1) {
            const std::string name = in.substr(i + 1, semi - i - 1);
            auto it = kEnts.find(name);
            if (it != kEnts.end()) {
                const uint32_t cp = it->second;
                if (cp < 0x800) {
                    s.push_back((char)(0xC0 | (cp >> 6)));
                    s.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    s.push_back((char)(0xE0 | (cp >> 12)));
                    s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    s.push_back((char)(0x80 | (cp & 0x3F)));
                }
                i = semi;
                continue;
            }
        }
        s.push_back('&');
    }
    return s;
}

std::string closeVoidTags(const std::string& in) {
    static const char* kVoid[] = {"br", "hr", "img", "meta", "link", "input",
                                  "col", "area", "base", "wbr", "source", "embed"};
    std::string out;
    out.reserve(in.size() + 64);
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '<' && i + 1 < in.size() && std::isalpha((unsigned char)in[i + 1])) {
            size_t e = i + 1;
            while (e < in.size() && std::isalnum((unsigned char)in[e])) ++e;
            std::string name;
            for (size_t k = i + 1; k < e; ++k)
                name.push_back((char)std::tolower((unsigned char)in[k]));
            bool isVoid = false;
            for (const char* v : kVoid) if (name == v) { isVoid = true; break; }
            const size_t gt = in.find('>', e);
            if (isVoid && gt != std::string::npos && in[gt - 1] != '/') {
                out.append(in, i, gt - i);
                out += "/>";
                i = gt;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

void parseXhtmlIntoBlocks(const std::string& rawXml, Document* out, Logger* log,
                          ZipReader* zip, const std::string& docPath,
                          const CssSheet& globalSheet,
                          const std::vector<std::string>& fontEntries) {
    const std::string xml = expandHtmlEntities(rawXml);
    pugi::xml_document doc;
    const unsigned flags = pugi::parse_default | pugi::parse_ws_pcdata;
    auto res = doc.load_string(xml.c_str(), flags);
    if (!res) {
        pugi::xml_document retry;
        auto res2 = retry.load_string(closeVoidTags(xml).c_str(), flags);
        if (res2) {
            doc.reset(retry);
        } else {
            if (log) {
                log->log(LogLevel::Warn, "epub",
                         ("xhtml parse failed (" + std::string(res.description())
                          + "); using the part parsed before the error: "
                          + docPath).c_str());
            }
            if (!doc.first_child()) doc.reset(retry);
        }
    }
    g_pendingAnchors.clear();

    const std::string docDir = parentPath(docPath);

    CssSheet sheet = globalSheet;
    for (auto ln : doc.select_nodes("//*[local-name()='link']")) {
        auto node = ln.node();
        std::string rel = node.attribute("rel").as_string();
        std::string href = node.attribute("href").as_string();
        if (href.empty()) continue;
        if (rel.find("stylesheet") == std::string::npos) continue;
        std::string cssPath = resolvePath(docDir, href);
        std::string cssText;
        if (!cssPath.empty() && zip && zip->readText(cssPath, &cssText)) {
            registerFontsFromCss(cssText, parentPath(cssPath), fontEntries,
                                 out, zip, log);
            sheet.addSheet(cssText);
        }
    }
    for (auto sn : doc.select_nodes("//*[local-name()='style']")) {
        std::string cssText;
        for (auto c : sn.node().children()) {
            if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
                cssText += c.value();
        }
        if (!cssText.empty()) {
            registerFontsFromCss(cssText, docDir, fontEntries, out, zip, log);
            sheet.addSheet(cssText);
        }
    }

    auto body = doc.select_node("//*[local-name()='body']").node();
    if (!body) body = doc.first_child();

    CssStyle bodyEff = ownStyleOf(body, sheet);
    if (!out->hasCssPageMargin) {
        auto side = [](float m, float p) {
            return (m >= 0 ? m : 0.0f) + (p >= 0 ? p : 0.0f);
        };
        float t = side(bodyEff.marginTopEm,    bodyEff.paddingTopEm);
        float r = side(bodyEff.marginRightEm,  bodyEff.paddingRightEm);
        float b = side(bodyEff.marginBottomEm, bodyEff.paddingBottomEm);
        float l = side(bodyEff.marginLeftEm,   bodyEff.paddingLeftEm);
        if (t > 0 || r > 0 || b > 0 || l > 0) {
            out->hasCssPageMargin = true;
            out->cssPageMarginEm[0] = t;
            out->cssPageMarginEm[1] = r;
            out->cssPageMarginEm[2] = b;
            out->cssPageMarginEm[3] = l;
        }
    }

    CssStyle inh;
    inh.lineHeightMul = bodyEff.lineHeightMul;
    inh.textIndentEm  = bodyEff.textIndentEm;
    inh.fontRole      = bodyEff.fontRole;
    std::memcpy(inh.familyKey, bodyEff.familyKey, sizeof(inh.familyKey));
    inh.fontSizeEm    = bodyEff.fontSizeEm;
    inh.textAlign     = bodyEff.textAlign;
    inh.fontBold      = bodyEff.fontBold;
    inh.fontItalic    = bodyEff.fontItalic;

    const size_t blocksBefore = out->blocks.size();
    walkBlocks(body, out, 0, 0, zip, docDir, log, sheet, inh);
    g_pendingAnchors.clear();

    while (out->blocks.size() > blocksBefore
           && out->blocks.back().kind == BlockKind::ThematicBreak)
        out->blocks.pop_back();
    while (out->blocks.size() > blocksBefore
           && out->blocks[blocksBefore].kind == BlockKind::ThematicBreak)
        out->blocks.erase(out->blocks.begin() + (long)blocksBefore);

    if (out->blocks.size() == blocksBefore + 1) {
        Block& only = out->blocks.back();
        if (only.kind == BlockKind::Image) only.imageFullPage = true;
    }
}

void splitTargetHref(const std::string& href, std::string* path,
                     std::string* fragment) {
    if (path) *path = href;
    if (fragment) fragment->clear();
    const size_t hash = href.find('#');
    if (hash == std::string::npos) return;
    if (path) *path = href.substr(0, hash);
    if (fragment) *fragment = href.substr(hash + 1);
}

std::string nodeTextRecursive(const pugi::xml_node& node) {
    std::string out;
    for (auto c : node.children()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
            out += c.value();
        else if (c.type() == pugi::node_element)
            out += nodeTextRecursive(c);
    }
    return trim(collapseWhitespaceText(out));
}

void appendEpubToc(Document* out, const std::string& title,
                   const std::string& href, int level,
                   const std::string& navPath,
                   const std::map<std::string, int>& spineByPath) {
    if (!out || title.empty() || href.empty()) return;
    std::string relPath, fragment;
    splitTargetHref(urlDecode(href), &relPath, &fragment);
    const std::string target = resolvePath(parentPath(navPath), relPath);
    if (target.empty()) return;
    Document::EpubTocEntry entry;
    entry.title = title;
    entry.level = std::max(1, level);
    entry.targetPath = target;
    entry.fragment = fragment;
    auto it = spineByPath.find(target);
    if (it != spineByPath.end()) entry.sourceSpineIndex = it->second;
    out->epubToc.push_back(std::move(entry));
}

void collectNavList(const pugi::xml_node& list, int level,
                    const std::string& navPath, Document* out,
                    const std::map<std::string, int>& spineByPath) {
    for (auto li : list.children()) {
        if (li.type() != pugi::node_element || !nameEq(li, "li")) continue;
        auto a = li.select_node("./*[local-name()='a']").node();
        if (a) {
            appendEpubToc(out, nodeTextRecursive(a),
                          a.attribute("href").as_string(), level,
                          navPath, spineByPath);
        }
        auto nested = li.select_node("./*[local-name()='ol']").node();
        if (nested) collectNavList(nested, level + 1, navPath, out,
                                   spineByPath);
    }
}

void parseEpub3Nav(const std::string& rawXml, const std::string& navPath,
                  Document* out, const std::map<std::string, int>& spineByPath,
                  Logger* log) {
    pugi::xml_document nav;
    const std::string xml = expandHtmlEntities(rawXml);
    if (!nav.load_string(xml.c_str(), pugi::parse_default)) {
        if (log) log->log(LogLevel::Warn, "epub", "EPUB nav parse failed");
        return;
    }
    pugi::xml_node selected;
    for (auto n : nav.select_nodes("//*[local-name()='nav']")) {
        const auto node = n.node();
        const std::string type = node.attribute("epub:type").as_string();
        if (type.find("toc") != std::string::npos) {
            selected = node;
            break;
        }
        if (!selected) selected = node;
    }
    if (!selected) return;
    auto list = selected.select_node("./*[local-name()='ol']").node();
    if (!list) list = selected.select_node(".//*[local-name()='ol']").node();
    if (list) collectNavList(list, 1, navPath, out, spineByPath);
}

void collectNcxPoints(const pugi::xml_node& map, int level,
                      const std::string& ncxPath, Document* out,
                      const std::map<std::string, int>& spineByPath) {
    for (auto point : map.children()) {
        if (point.type() != pugi::node_element || !nameEq(point, "navPoint"))
            continue;
        auto label = point.select_node("./*[local-name()='navLabel']").node();
        auto text = label ? label.select_node(".//*[local-name()='text']").node()
                          : pugi::xml_node{};
        auto content = point.select_node("./*[local-name()='content']").node();
        if (text && content)
            appendEpubToc(out, nodeTextRecursive(text),
                          content.attribute("src").as_string(), level,
                          ncxPath, spineByPath);
        auto nested = point.select_node("./*[local-name()='navPoint']").node();
        if (nested) collectNcxPoints(point, level + 1, ncxPath, out,
                                     spineByPath);
    }
}

void parseEpubNcx(const std::string& rawXml, const std::string& ncxPath,
                  Document* out, const std::map<std::string, int>& spineByPath,
                  Logger* log) {
    pugi::xml_document ncx;
    if (!ncx.load_string(rawXml.c_str(), pugi::parse_default)) {
        if (log) log->log(LogLevel::Warn, "epub", "EPUB NCX parse failed");
        return;
    }
    auto map = ncx.select_node("//*[local-name()='navMap']").node();
    if (map) collectNcxPoints(map, 1, ncxPath, out, spineByPath);
}

}

bool parseEpub(const std::filesystem::path& path, Document* out, Logger* log) {
    if (!out) return false;
    ZipReader zip;
    if (!zip.open(path)) {
        if (log) log->log(LogLevel::Error, "epub", "cannot open epub");
        return false;
    }

    std::string opfPath;
    if (!readContainerOpfPath(zip, &opfPath)) {
        if (log) log->log(LogLevel::Error, "epub", "container.xml missing or unreadable");
        return false;
    }
    std::string opfXml;
    if (!zip.readText(opfPath, &opfXml)) {
        if (log) log->log(LogLevel::Error, "epub", "OPF unreadable");
        return false;
    }

    pugi::xml_document opfDoc;
    if (!opfDoc.load_string(opfXml.c_str())) {
        if (log) log->log(LogLevel::Error, "epub", "OPF parse failed");
        return false;
    }

    auto md = opfDoc.select_node("//*[local-name()='metadata']").node();
    if (md) {
        for (auto t : md.children()) {
            std::string nm = t.name();
            if (nm.find("title") != std::string::npos && out->title.empty()) {
                out->title = trim(t.text().as_string());
            } else if (nm.find("creator") != std::string::npos && out->author.empty()) {
                out->author = trim(t.text().as_string());
            }
        }
    }

    std::vector<std::string> fontEntries;
    for (const auto& e : zip.entries())
        if (isFontPath(e)) fontEntries.push_back(e);

    std::string opfDir = parentPath(opfPath);
    std::map<std::string, std::string> idToHref;
    CssSheet globalSheet;
    if (auto man = opfDoc.select_node("//*[local-name()='manifest']").node()) {
        for (auto item : childrenNamed(man, "item")) {
            std::string id    = item.attribute("id").as_string();
            std::string href  = item.attribute("href").as_string();
            std::string media = item.attribute("media-type").as_string();
            if (id.empty() || href.empty()) continue;
            bool isCss = media == "text/css"
                || (href.size() > 4
                    && href.compare(href.size() - 4, 4, ".css") == 0);
            if (isCss) {
                std::string cssText;
                std::string cssPath = resolvePath(opfDir, href);
                if (!cssPath.empty() && zip.readText(cssPath, &cssText)) {
                    registerFontsFromCss(cssText, parentPath(cssPath), fontEntries,
                                         out, &zip, log);
                    globalSheet.addSheet(cssText);
                }
            }
            idToHref.emplace(std::move(id), std::move(href));
        }
    }
    if (log && !globalSheet.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "css subset: %zu applicable rules",
                      globalSheet.ruleCount());
        log->log(LogLevel::Info, "epub", buf);
    }

    std::string coverHref, coverId;
    if (auto guide = opfDoc.select_node("//*[local-name()='guide']").node()) {
        for (auto ref : childrenNamed(guide, "reference")) {
            std::string type = ref.attribute("type").as_string();
            if (type.find("cover") != std::string::npos) {
                coverHref = resolvePath(opfDir, ref.attribute("href").as_string());
                break;
            }
        }
    }
    if (md) {
        for (auto t : childrenNamed(md, "meta")) {
            if (std::strcmp(t.attribute("name").as_string(), "cover") == 0) {
                coverId = t.attribute("content").as_string();
                break;
            }
        }
    }

    auto spine = opfDoc.select_node("//*[local-name()='spine']").node();
    if (!spine) {
        if (log) log->log(LogLevel::Warn, "epub", "OPF has no spine");
        return true;
    }
    bool firstItemref = true;
    int sourceSpineIndex = 0;
    std::vector<std::string> spinePaths;
    for (auto ir : childrenNamed(spine, "itemref")) {
        if (currentJobCancelled()) break;
        std::string idref = ir.attribute("idref").as_string();

        auto href_it = idToHref.find(idref);
        bool isCover = false;
        if (href_it != idToHref.end()) {
            std::string resolved = resolvePath(opfDir, href_it->second);
            isCover = (!coverHref.empty() && resolved == coverHref)
                   || (!coverId.empty() && idref == coverId);
        }

        const char* linear = ir.attribute("linear").as_string();
        bool nonLinear = linear && (std::strcmp(linear, "no") == 0
                                    || std::strcmp(linear, "No") == 0
                                    || std::strcmp(linear, "NO") == 0);
        if (nonLinear && !(isCover && firstItemref)) {
            firstItemref = false;
            continue;
        }
        firstItemref = false;

        if (href_it == idToHref.end()) continue;

        std::string docPath = resolvePath(opfDir, href_it->second);
        if (docPath.empty()) continue;

        std::string xhtml;
        if (!zip.readText(docPath, &xhtml)) {
            if (log) log->log(LogLevel::Warn, "epub", ("missing spine entry: " + docPath).c_str());
            continue;
        }
        if (!out->blocks.empty()) emitPageBreak(out);
        const size_t blocksBefore = out->blocks.size();
        parseXhtmlIntoBlocks(xhtml, out, log, &zip, docPath, globalSheet,
                             fontEntries);
        for (size_t bi = blocksBefore; bi < out->blocks.size(); ++bi) {
            out->blocks[bi].sourceSpineIndex = sourceSpineIndex;
            out->blocks[bi].sourcePath = docPath;
            for (auto& run : out->blocks[bi].runs) {
                if (run.linkTarget.empty()) continue;
                const std::string raw = urlDecode(run.linkTarget);
                const bool external = raw.find("://") != std::string::npos
                                   || raw.rfind("mailto:", 0) == 0
                                   || raw.rfind("tel:", 0) == 0;
                if (external) continue;
                std::string relPath, fragment;
                splitTargetHref(raw, &relPath, &fragment);
                const std::string target = relPath.empty()
                    ? docPath : resolvePath(parentPath(docPath), relPath);
                if (!target.empty()) {
                    run.linkTarget = target;
                    if (!fragment.empty()) run.linkTarget += "#" + fragment;
                }
            }
        }
        spinePaths.push_back(docPath);
        ++sourceSpineIndex;
    }

    {
        size_t headings = 0;
        std::vector<std::pair<size_t, int>> candidates;
        for (size_t bi = 0; bi < out->blocks.size(); ++bi) {
            const Block& b = out->blocks[bi];
            if (b.kind == BlockKind::Heading) { ++headings; continue; }
            if (b.kind != BlockKind::Paragraph || b.quoteDepth > 0) continue;
            std::string flat;
            for (const auto& r : b.runs) {
                if (r.kind == InlineKind::LineBreak) break;
                flat += r.text;
            }
            if (flat.size() > 160) continue;
            if (const int lvl = plainTextChapterLevel(flat)) candidates.emplace_back(bi, lvl);
        }
        if (candidates.size() >= 3 && candidates.size() > headings * 3) {
            for (const auto& c : candidates) {
                Block& b = out->blocks[c.first];
                b.kind = BlockKind::Heading;
                b.level = c.second;
            }
            if (log) {
                char msg[96];
                std::snprintf(msg, sizeof(msg),
                              "promoted %zu chapter-title paragraphs to headings",
                              candidates.size());
                log->log(LogLevel::Info, "epub", msg);
            }
        }
    }

    std::map<std::string, int> spineByPath;
    for (size_t i = 0; i < spinePaths.size(); ++i)
        spineByPath.emplace(spinePaths[i], (int)i);
    std::string navPath;
    for (const auto& kv : idToHref) {
        const std::string id = kv.first;
        auto item = opfDoc.select_node(("//*[local-name()='item' and @id='" + id + "']").c_str()).node();
        if (!item) continue;
        const std::string props = item.attribute("properties").as_string();
        if (props.find("nav") == std::string::npos) continue;
        navPath = resolvePath(opfDir, kv.second);
        break;
    }
    if (!navPath.empty()) {
        std::string navXml;
        if (zip.readText(navPath, &navXml))
            parseEpub3Nav(navXml, navPath, out, spineByPath, log);
    }
    const std::string tocId = spine.attribute("toc").as_string();
    auto tocIt = idToHref.find(tocId);
    if (tocIt != idToHref.end() && out->epubToc.empty()) {
        const std::string ncxPath = resolvePath(opfDir, tocIt->second);
        std::string ncxXml;
        if (zip.readText(ncxPath, &ncxXml))
            parseEpubNcx(ncxXml, ncxPath, out, spineByPath, log);
    }
    if (log && !out->epubToc.empty()) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "EPUB navigation entries: %zu",
                      out->epubToc.size());
        log->log(LogLevel::Info, "epub", msg);
    }
    return true;
}

bool parseEpubComic(const std::filesystem::path& path, ComicDocument* doc, Logger* log) {
    if (!doc) return false;

    ZipReader zip;
    if (!zip.open(path)) {
        if (log) log->log(LogLevel::Error, "epub-comic", "failed to open EPUB");
        return false;
    }

    auto allEntries = zip.entries();
    std::vector<std::string> imageEntries;
    for (const auto& e : allEntries) {
        if (e.find("__MACOSX") != std::string::npos) continue;
        auto dot = e.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext;
        for (size_t i = dot; i < e.size(); ++i)
            ext.push_back((char)std::tolower((unsigned char)e[i]));
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png"
            || ext == ".gif" || ext == ".bmp" || ext == ".webp")
            imageEntries.push_back(e);
    }

    std::sort(imageEntries.begin(), imageEntries.end(), natsort::less);

    doc->title = path.stem().string();
    doc->pages.reserve(imageEntries.size());

    for (const auto& entry : imageEntries) {
        ComicPage page;
        if (!zip.read(entry, &page.data)) continue;
        page.filename = entry;
        doc->pages.push_back(std::move(page));
    }

    if (doc->pages.empty()) {
        if (log) log->log(LogLevel::Error, "epub-comic", "no images found");
        return false;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "extracted %zu images from EPUB", doc->pages.size());
    if (log) log->log(LogLevel::Info, "epub-comic", buf);
    return true;
}

}
