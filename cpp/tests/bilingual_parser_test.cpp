#include "parser/bilingual_parser.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hardcopy;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

std::string runText(const Block& b) {
    std::string s;
    for (const auto& r : b.runs) s += r.kind == InlineKind::LineBreak ? "\n" : r.text;
    return s;
}

const unsigned char kPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x3A, 0x7E, 0x9B,
    0x55, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x01, 0x48, 0xAF, 0xA4, 0x71, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
    0x42, 0x60, 0x82};
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bilingual_parser_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "img_3.png", std::ios::binary);
        f.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
    }
    const std::string json = R"({"version":1,"title":"T","author":"A","blocks":[
        {"kind":"h1","src":"Chapter","dst":"第一章"},
        {"kind":"p","text":"Line one\nLine two","dst":"一行\n二行"},
        {"kind":"li","src":"item","dst":"","marker":"• "},
        {"kind":"oli","src":"second","dst":"第二","level":2},
        {"kind":"code","text":"int x;\n\nreturn x;"},
        {"kind":"image","image":"img_3.png","fullPage":true},
        {"kind":"image","image":"missing.png"},
        {"kind":"table","rows":[["a","b"],["c"]],"spans":[[[1,1,1],[1,1,1]],[[2,1,0]]],
         "rowsDst":[["甲","乙"],["丙"]]},
        {"kind":"p","src":"  ","dst":""},
        {"kind":"hr"}],
      "outline":[{"title":"Chapter","level":1,"block":0},{"title":"","block":3}]})";
    Document doc;
    check(parseBilingualText(json, dir, &doc, nullptr), "parse ok");
    check(doc.title == "T" && doc.author == "A", "title/author");
    check(doc.blocks.size() == 12, "block count");
    if (doc.blocks.size() == 12) {
        const Block& h0 = doc.blocks[0];
        const Block& h1 = doc.blocks[1];
        check(h0.kind == BlockKind::Heading && h0.level == 1 && h0.pairSide == 0, "heading src");
        check(h1.kind == BlockKind::Heading && h1.pairSide == 1 && h1.pairId == h0.pairId, "heading pair");
        check(runText(h1) == "第一章", "heading dst text");
        check(runText(doc.blocks[2]) == "Line one\nLine two", "line breaks src");
        check(doc.blocks[3].runs.size() == 3 && doc.blocks[3].runs[1].kind == InlineKind::LineBreak,
              "line break run");
        check(doc.blocks[2].pairId != h0.pairId, "distinct pair ids");
        const Block& li = doc.blocks[4];
        const Block& liDst = doc.blocks[5];
        check(li.kind == BlockKind::UnorderedItem && li.runs[0].text == "\xE2\x80\xA2 ", "li marker");
        check(runText(liDst) == "\xE2\x80\xA2 item", "empty dst keeps source");
        check(doc.blocks[6].kind == BlockKind::OrderedItem && doc.blocks[6].runs[0].text == "2. ",
              "default ordered marker");
        const Block& code = doc.blocks[8];
        check(code.kind == BlockKind::CodeBlock && code.pairSide == -1, "code full width");
        check(code.runs.size() == 5 && code.runs[2].text == " ", "blank code line kept");
        const Block& img = doc.blocks[9];
        check(img.kind == BlockKind::Image && img.imageFullPage && img.imageIndex == 0, "image block");
        check(doc.images.size() == 1 && doc.images[0].width == 1, "image decoded");
        const Block& tbl = doc.blocks[10];
        check(tbl.kind == BlockKind::Table && tbl.rows.size() == 2 && tbl.rowsDst.size() == 2, "table rows");
        check(tbl.rows[1].cells[0].colspan == 2 && tbl.rowsDst[1].cells[0].colspan == 2, "spans both");
        check(tbl.rows[0].cells[0].isHeader, "header cell");
        check(doc.blocks[11].kind == BlockKind::ThematicBreak, "hr");
        check(doc.blocks[0].sourceSpineIndex == 0 && doc.blocks[6].sourceSpineIndex == 3,
              "json index kept");
    }
    check(doc.importedOutline.size() == 1 && doc.importedOutline[0].pageIndex0 == 0, "outline");

    Document bad;
    check(!parseBilingualText("{\"blocks\":3}", dir, &bad, nullptr), "rejects bad json");
    std::filesystem::remove_all(dir);
    if (failures == 0) std::printf("bilingual_parser_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
