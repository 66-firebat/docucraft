#include "font_alias.h"

#include <cctype>
#include <cstring>

namespace hardcopy {

namespace {

struct AliasEntry {
    const char* key;
    FontRole    role;
};

const AliasEntry kAliases[] = {
    {"serif",            FontRole::Serif},
    {"songti",           FontRole::Serif},
    {"song",             FontRole::Serif},
    {"\xe5\xae\x8b\xe4\xbd\x93", FontRole::Serif},
    {"\xe5\x8d\x8e\xe6\x96\x87\xe4\xb8\xad\xe5\xae\x8b", FontRole::Serif},
    {"stsong",           FontRole::Serif},
    {"stzhongsong",      FontRole::Serif},
    {"songtisc",         FontRole::Serif},
    {"songtitc",         FontRole::Serif},
    {"simsun",           FontRole::Serif},
    {"nsimsun",          FontRole::Serif},
    {"dksongti",         FontRole::Serif},
    {"dkshusong",        FontRole::Serif},
    {"stbshusong",       FontRole::Serif},
    {"fzshusong",        FontRole::Serif},
    {"fzss",             FontRole::Serif},
    {"cnepub",           FontRole::Serif},
    {"timesnewroman",    FontRole::Serif},
    {"times",            FontRole::Serif},
    {"georgia",          FontRole::Serif},
    {"garamond",         FontRole::Serif},
    {"palatino",         FontRole::Serif},
    {"charissil",        FontRole::Serif},
    {"mincho",           FontRole::Serif},
    {"msmincho",         FontRole::Serif},
    {"mspmincho",        FontRole::Serif},
    {"hiraginominchopro", FontRole::Serif},
    {"hiraginominchopron", FontRole::Serif},
    {"yumincho",         FontRole::Serif},
    {"tbmincho",         FontRole::Serif},
    {"ipamincho",        FontRole::Serif},
    {"ipaexmincho",      FontRole::Serif},
    {"notoserif",        FontRole::Serif},
    {"notoserifcjksc",   FontRole::Serif},
    {"notoserifcjkjp",   FontRole::Serif},
    {"sourcehanserif",   FontRole::Serif},
    {"sourcehanserifsc", FontRole::Serif},
    {"sourcehanserifcn", FontRole::Serif},
    {"\xe6\x98\x8e\xe6\x9c\x9d", FontRole::Serif},
    {"hymyeongjo",       FontRole::Serif},

    {"sansserif",        FontRole::Sans},
    {"sans",             FontRole::Sans},
    {"heiti",            FontRole::Sans},
    {"hei",              FontRole::Sans},
    {"\xe9\xbb\x91\xe4\xbd\x93", FontRole::Sans},
    {"\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91", FontRole::Sans},
    {"microsoftyahei",   FontRole::Sans},
    {"msyh",             FontRole::Sans},
    {"simhei",           FontRole::Sans},
    {"stheiti",          FontRole::Sans},
    {"heitisc",          FontRole::Sans},
    {"heititc",          FontRole::Sans},
    {"dkheiti",          FontRole::Sans},
    {"dkxiheiti",        FontRole::Sans},
    {"fzlth",            FontRole::Sans},
    {"fzlanting",        FontRole::Sans},
    {"fzlantinghei",     FontRole::Sans},
    {"fzhei",            FontRole::Sans},
    {"myingheis",        FontRole::Sans},
    {"myingheit",        FontRole::Sans},
    {"tbgothic",         FontRole::Sans},
    {"helvetica",        FontRole::Sans},
    {"arial",            FontRole::Sans},
    {"verdana",          FontRole::Sans},
    {"tahoma",           FontRole::Sans},
    {"roboto",           FontRole::Sans},
    {"droidsans",        FontRole::Sans},
    {"droidsansfallback", FontRole::Sans},
    {"gothic",           FontRole::Sans},
    {"msgothic",         FontRole::Sans},
    {"mspgothic",        FontRole::Sans},
    {"hiraginokakugothicpro", FontRole::Sans},
    {"hiraginosans",     FontRole::Sans},
    {"yugothic",         FontRole::Sans},
    {"meiryo",           FontRole::Sans},
    {"ipagothic",        FontRole::Sans},
    {"notosans",         FontRole::Sans},
    {"notosanscjksc",    FontRole::Sans},
    {"notosanscjkjp",    FontRole::Sans},
    {"sourcehansans",    FontRole::Sans},
    {"sourcehansanssc",  FontRole::Sans},
    {"sourcehansanscn",  FontRole::Sans},
    {"pingfangsc",       FontRole::Sans},
    {"pingfang",         FontRole::Sans},
    {"\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf", FontRole::Sans},

    {"kaiti",            FontRole::Kai},
    {"kai",              FontRole::Kai},
    {"\xe6\xa5\xb7\xe4\xbd\x93", FontRole::Kai},
    {"\xe6\xa5\xb7\xe4\xbd\x93gb2312", FontRole::Kai},
    {"stkai",            FontRole::Kai},
    {"stkaiti",          FontRole::Kai},
    {"kaitisc",          FontRole::Kai},
    {"kaititc",          FontRole::Kai},
    {"simkai",           FontRole::Kai},
    {"dkkaiti",          FontRole::Kai},
    {"fzkai",            FontRole::Kai},
    {"mkaiprc",          FontRole::Kai},
    {"\xe4\xbb\xbf\xe5\xae\x8b", FontRole::Kai},
    {"\xe4\xbb\xbf\xe5\xae\x8bgb2312", FontRole::Kai},
    {"fangsong",         FontRole::Kai},
    {"stfangsong",       FontRole::Kai},
    {"simfang",          FontRole::Kai},
    {"dkfangsong",       FontRole::Kai},
    {"\xe5\xb9\xbc\xe5\x9c\x86", FontRole::Kai},
    {"yuanti",           FontRole::Kai},
    {"yuan",             FontRole::Kai},
    {"styuan",           FontRole::Kai},
    {"yuantisc",         FontRole::Kai},
    {"yuantitc",         FontRole::Kai},

    {"monospace",        FontRole::Mono},
    {"courier",          FontRole::Mono},
    {"couriernew",       FontRole::Mono},
    {"consolas",         FontRole::Mono},
    {"menlo",            FontRole::Mono},
    {"monaco",           FontRole::Mono},
    {"sourcecodepro",    FontRole::Mono},
    {"jetbrainsmono",    FontRole::Mono},
};

}

std::string normalizeFamilyKey(const std::string& family) {
    std::string key;
    key.reserve(family.size());
    for (char c : family) {
        unsigned char u = (unsigned char)c;
        if (u == '"' || u == '\'' || u == ' ' || u == '\t'
            || u == '-' || u == '_' || u == '\r' || u == '\n') continue;
        key.push_back((char)std::tolower(u));
    }
    return key;
}

FontRole roleForFamily(const std::string& family) {
    const std::string key = normalizeFamilyKey(family);
    if (key.empty()) return FontRole::None;
    for (const auto& e : kAliases) {
        if (key == e.key) return e.role;
    }
    return FontRole::None;
}

FontRole roleFromNames(const std::vector<std::string>& names) {
    for (const auto& n : names) {
        FontRole r = roleForFamily(n);
        if (r != FontRole::None) return r;
    }
    return FontRole::None;
}

std::vector<std::string> splitFamilyList(const std::string& value) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false;
    char quoteCh = 0;
    for (char c : value) {
        if (inQuote) {
            if (c == quoteCh) { inQuote = false; continue; }
            cur.push_back(c);
            continue;
        }
        if (c == '"' || c == '\'') { inQuote = true; quoteCh = c; continue; }
        if (c == ',') {
            size_t a = 0, b = cur.size();
            while (a < b && std::isspace((unsigned char)cur[a])) ++a;
            while (b > a && std::isspace((unsigned char)cur[b - 1])) --b;
            if (b > a) out.push_back(cur.substr(a, b - a));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    size_t a = 0, b = cur.size();
    while (a < b && std::isspace((unsigned char)cur[a])) ++a;
    while (b > a && std::isspace((unsigned char)cur[b - 1])) --b;
    if (b > a) out.push_back(cur.substr(a, b - a));
    return out;
}

}
