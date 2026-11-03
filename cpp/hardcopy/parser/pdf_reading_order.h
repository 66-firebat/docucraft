#ifndef HARDCOPY_PARSER_PDF_READING_ORDER_H_
#define HARDCOPY_PARSER_PDF_READING_ORDER_H_

#include <vector>

namespace hardcopy {

struct ReadingBox { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

struct ReadingOrder {
    std::vector<int> order;
    std::vector<int> region;
    std::vector<ReadingBox> regions;
};

ReadingOrder readingOrder(const std::vector<ReadingBox>& boxes, float minGutter);

struct ReadingLine {
    float x0 = 0, x1 = 0;
    float baseline = 0;
    float size = 0;
    int region = 0;
    bool bold = false;
    bool sentenceEnd = false;
    float itemTextX = -1;
    bool itemOrdered = false;
    float firstWordW = 0;
};

struct ReadingParagraph {
    bool item = false;
};

std::vector<int> segmentLines(const std::vector<ReadingLine>& lines,
                              std::vector<ReadingParagraph>* paragraphs);

}

#endif
