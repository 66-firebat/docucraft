#ifndef HARDCOPY_PDF_PAGE_IMAGE_DUMP_H_
#define HARDCOPY_PDF_PAGE_IMAGE_DUMP_H_

#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hardcopy {

struct Bitmap8;
struct Logger;
struct Options;
class StreamingPdfWriter;

class PageImageDump {
public:
    static std::unique_ptr<PageImageDump> create(const Options& opts,
                                                 const std::filesystem::path& output,
                                                 const std::filesystem::path& input,
                                                 Logger* log);
    ~PageImageDump();

    PageImageDump(const PageImageDump&) = delete;
    PageImageDump& operator=(const PageImageDump&) = delete;

    const std::string& id() const { return id_; }

    void save(int leaf1, const Bitmap8& page);

    void finish(const StreamingPdfWriter& pdf, int leavesPerSheet);

private:
    PageImageDump() = default;
    void writeManifest(const char* status);

    std::string id_;
    std::filesystem::path dir_;
    std::string output_;
    std::string input_;
    std::string created_;
    int quality_ = 90;
    int leafW_ = 0;
    int leafH_ = 0;
    int sheets_ = 0;
    int leavesPerSheet_ = 1;
    struct Chapter {
        std::string title;
        int level = 1;
        int sheet = 0;
    };
    std::vector<Chapter> outline_;
    bool finished_ = false;
    Logger* log_ = nullptr;

    std::mutex mu_;
    std::set<int> saved_;
    int failures_ = 0;
};

}

#endif
