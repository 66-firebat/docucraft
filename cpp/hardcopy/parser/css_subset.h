#ifndef HARDCOPY_PARSER_CSS_SUBSET_H_
#define HARDCOPY_PARSER_CSS_SUBSET_H_

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "font_alias.h"

namespace hardcopy {

struct CssStyle {
    float lineHeightMul  = -1.0f;
    float marginTopEm    = -1.0f;
    float marginBottomEm = -1.0f;
    float marginLeftEm   = -1.0f;
    float marginRightEm  = -1.0f;
    float paddingTopEm    = -1.0f;
    float paddingBottomEm = -1.0f;
    float paddingLeftEm   = -1.0f;
    float paddingRightEm  = -1.0f;
    float textIndentEm   = -1.0f;
    float fontSizeEm     = -1.0f;
    int8_t cssFloat      = -1;
    int8_t textAlign     = -1;
    int8_t fontBold      = -1;
    int8_t fontItalic    = -1;
    int8_t displayNone   = -1;
    int8_t listStyleNone = -1;
    int8_t pageBreakBefore = -1;
    int8_t pageBreakAfter  = -1;
    int16_t backgroundGray = -1;

    FontRole fontRole = FontRole::None;

    char familyKey[24] = {};

    bool hasFamilyKey() const { return familyKey[0] != '\0'; }

    bool anySet() const;

    void apply(const CssStyle& over);

    void resolveFontSize(const CssStyle& parent);
};

double cssColorLuminance(const std::string& value, double* chroma = nullptr);

int cssBackgroundGray(const std::string& value);

class CssNode {
 public:
    virtual ~CssNode() = default;
    virtual std::string tag() const = 0;
    virtual std::string classAttr() const = 0;
    virtual std::unique_ptr<CssNode> parent() const = 0;
    virtual std::unique_ptr<CssNode> prevSibling() const = 0;
};

class CssSheet {
 public:
    void addSheet(const std::string& css);

    CssStyle match(const std::string& tag, const std::string& classAttr) const;
    CssStyle match(const CssNode& node) const;

    static CssStyle parseDeclarations(const std::string& decls);

    bool empty() const { return rules_.empty(); }
    size_t ruleCount() const { return rules_.size(); }

 private:
    struct Compound {
        std::string tag;
        std::vector<std::string> classes;
    };
    struct Rule {
        std::vector<Compound> parts;
        std::vector<char> combinators;
        int specificity = 0;
        CssStyle style;
    };
    static bool compoundMatches(const Compound& c, const std::string& tagLower,
                                const std::vector<std::string>& classes);
    static bool chainMatches(const Rule& r, size_t part, const CssNode& node);
    CssStyle cascade(const std::vector<size_t>& hits) const;
    std::vector<Rule> rules_;
};

}

#endif
