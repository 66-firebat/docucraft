#include <cstdio>
#include <string>

#include "parser/zh_convert.h"

using namespace hardcopy;

static int failures = 0;

static void expect(const std::string& in, ZhConvert mode, const std::string& want) {
    const std::string got = convertZhUtf8(in, mode);
    if (got != want) {
        std::printf("FAIL %s: %s -> %s, want %s\n", mode == ZhConvert::S2T ? "s2t" : "t2s",
                    in.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}

int main() {
    expect("头发很长", ZhConvert::S2T, "頭髮很長");
    expect("皇后来了", ZhConvert::S2T, "皇后來了");
    expect("干净的衣服", ZhConvert::S2T, "乾淨的衣服");
    expect("只有一只猫", ZhConvert::S2T, "只有一隻貓");
    expect("然后我们出发了", ZhConvert::S2T, "然後我們出發了");
    expect("abc 头发 \xff end", ZhConvert::S2T, "abc 頭髮 \xff end");
    expect("頭髮很長", ZhConvert::T2S, "头发很长");
    expect("乾隆皇帝", ZhConvert::T2S, "乾隆皇帝");
    expect("頭髮", ZhConvert::None, "頭髮");
    expect("", ZhConvert::S2T, "");

    Document doc;
    Block b;
    InlineRun a, c;
    a.text = "长头";
    c.text = "发";
    b.runs = {a, c};
    doc.blocks.push_back(b);
    doc.title = "后来";
    convertDocumentZh(&doc, ZhConvert::S2T);
    if (doc.blocks[0].runs[0].text != "長頭" || doc.blocks[0].runs[1].text != "髮") {
        std::printf("FAIL runs: %s|%s\n", doc.blocks[0].runs[0].text.c_str(), doc.blocks[0].runs[1].text.c_str());
        ++failures;
    }
    if (doc.title != "後來") {
        std::printf("FAIL title: %s\n", doc.title.c_str());
        ++failures;
    }
    std::printf(failures ? "%d failures\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
