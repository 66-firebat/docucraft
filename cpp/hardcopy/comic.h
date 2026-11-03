#ifndef HARDCOPY_COMIC_H_
#define HARDCOPY_COMIC_H_

#include <cstdint>
#include <string>
#include <vector>

namespace hardcopy {

struct ComicPage {
    std::vector<uint8_t> data;
    std::string filename;
};

struct ComicDocument {
    std::string title;
    std::string author;
    std::vector<ComicPage> pages;
};

}

#endif
