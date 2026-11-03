#ifndef HARDCOPY_PRINT_PRINT_LAYOUT_H_
#define HARDCOPY_PRINT_PRINT_LAYOUT_H_

#include <filesystem>
#include <string>
#include <vector>

namespace hardcopy {
namespace print {

enum class Paper  { A4, A5, A6 };
enum class Layout { Single, TwoUp, FourUp };

struct PrintOptions {
    Paper  paper       = Paper::A4;
    Layout layout      = Layout::Single;
    int    dpi         = 300;
    double marginMm    = 5.0;
    int    jpegQuality = 90;
    bool   cutMarks    = true;
};

bool buildPrintPdf(const std::vector<std::filesystem::path>& imagePaths,
                   const PrintOptions& opts,
                   const std::filesystem::path& outPdf,
                   std::string* err);

std::vector<std::filesystem::path> collectImages(
    const std::filesystem::path& dir);

bool parsePaper(const std::string& s, Paper* out);
bool parseLayout(const std::string& s, Layout* out);

}
}

#endif
