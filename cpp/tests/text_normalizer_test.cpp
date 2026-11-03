#include <cstdlib>
#include <iostream>
#include <string>

#include "parser/text_normalizer.h"
#include "parser/txt_parser.h"

namespace {

using hardcopy::Block;
using hardcopy::BlockKind;
using hardcopy::Document;
using hardcopy::InlineKind;
using hardcopy::InlineRun;
using hardcopy::TxtMode;

std::string blockText(const Block& block) {
    std::string out;
    for (const auto& run : block.runs) out += run.text;
    return out;
}

void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void testPlainTextIndentBoundaries() {
    Document doc;
    const std::string source =
        "\xE3\x80\x80\xE3\x80\x80\xE7\xAC\xAC\xE4\xB8\x80\xE6\xAE\xB5\n"
        "\xE7\xBB\xAD\xE8\xA1\x8C\n"
        " \xE7\xAC\xAC\xE4\xBA\x8C\xE6\xAE\xB5\n"
        "\xE5\x90\x8E\xE7\xBB\xAD\n";
    expect(hardcopy::parseTxtFromString(source, &doc, TxtMode::Normal),
           "plain text parser returned false");
    expect(doc.blocks.size() == 2,
           "indented physical lines should start paragraphs");
    expect(blockText(doc.blocks[0]) ==
               "\xE7\xAC\xAC\xE4\xB8\x80\xE6\xAE\xB5\xE7\xBB\xAD\xE8\xA1\x8C",
           "first source indent should be stripped before layout");
    expect(blockText(doc.blocks[1]) ==
               "\xE7\xAC\xAC\xE4\xBA\x8C\xE6\xAE\xB5\xE5\x90\x8E\xE7\xBB\xAD",
           "single ASCII-space indent should be recognized");
}

void testPhysicalParagraphIndentCleanup() {
    Document doc;
    expect(hardcopy::parseTxtFromString(
               "  alpha.\n\tbeta.\n\xE3\x80\x80gamma.\n", &doc,
               TxtMode::Normal),
           "semantic-line parser returned false");
    expect(doc.blocks.size() == 3,
           "semantic physical lines should remain separate paragraphs");
    expect(blockText(doc.blocks[0]) == "alpha.", "ASCII indent remained");
    expect(blockText(doc.blocks[1]) == "beta.", "tab indent remained");
    expect(blockText(doc.blocks[2]) == "gamma.", "fullwidth indent remained");
}

void testDocumentIndentNormalization() {
    Document doc;
    Block block;
    block.kind = BlockKind::Paragraph;
    block.runs.push_back(InlineRun{InlineKind::Text,
        "\xE3\x80\x80  body", 0});
    doc.blocks.push_back(std::move(block));
    hardcopy::normalizeSourceParagraphIndents(&doc);
    expect(blockText(doc.blocks[0]) == "body",
           "non-Markdown source indent normalization failed");
}

void testCompactMacroRules() {
    Document doc;
    Block block;
    block.kind = BlockKind::Paragraph;
    block.runs.push_back(InlineRun{InlineKind::Text,
        "\xE4\xBB\x96\xE8\xAF\xB4, \t\"\xE4\xBD\xA0\xE5\xA5\xBD!!\"  ", 0});
    doc.blocks.push_back(std::move(block));
    hardcopy::normalizeCompactText(&doc);
    expect(blockText(doc.blocks[0]) ==
               "\xE4\xBB\x96\xE8\xAF\xB4\xEF\xBC\x8C"
               "\xE3\x80\x80\xE3\x80\x80"
               "\xE2\x80\x9C\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x81\xE2\x80\x9D",
           "compact punctuation normalization differed from macro rules");

    Document ellipsis;
    Block e;
    e.kind = BlockKind::Paragraph;
    e.runs.push_back(InlineRun{InlineKind::Text,
        "\xE7\xAD\x89\xE7\xAD\x89\xE2\x80\xA6\xE7\xBB\xA7\xE7\xBB\xAD", 0});
    ellipsis.blocks.push_back(std::move(e));
    hardcopy::normalizeCompactText(&ellipsis);
    expect(blockText(ellipsis.blocks[0]) ==
               "\xE7\xAD\x89\xE7\xAD\x89\xE2\x80\xA6\xE2\x80\xA6"
               "\xE7\xBB\xA7\xE7\xBB\xAD",
           "single Chinese ellipsis should expand to a pair");
}

void testPdfTrailingWhitespaceAcrossRuns() {
    Document doc;
    Block block;
    block.kind = BlockKind::Paragraph;
    block.runs.push_back(InlineRun{InlineKind::Text, "PDF", 0});
    block.runs.push_back(InlineRun{InlineKind::Strong, "  \t", 0});
    doc.blocks.push_back(std::move(block));
    hardcopy::normalizeCompactText(&doc);
    expect(blockText(doc.blocks[0]) == "PDF",
           "PDF line-end whitespace should be trimmed across runs");

    Document markdown;
    Block prose;
    prose.kind = BlockKind::Paragraph;
    prose.runs.push_back(InlineRun{InlineKind::Text, "\xE4\xB8\xAD\xE6\x96\x87? ", 0});
    prose.runs.push_back(InlineRun{InlineKind::Code, "code?!", 0});
    markdown.blocks.push_back(std::move(prose));
    hardcopy::normalizeCompactText(&markdown);
    expect(blockText(markdown.blocks[0]) ==
               "\xE4\xB8\xAD\xE6\x96\x87\xEF\xBC\x9F code?!",
           "Markdown inline code should retain authored punctuation");
}

}

int main() {
    testPlainTextIndentBoundaries();
    testPhysicalParagraphIndentCleanup();
    testDocumentIndentNormalization();
    testCompactMacroRules();
    testPdfTrailingWhitespaceAcrossRuns();
    std::cout << "text normalization regression passed\n";
    return 0;
}
