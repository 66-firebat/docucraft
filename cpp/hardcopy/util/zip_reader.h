#ifndef HARDCOPY_UTIL_ZIP_READER_H_
#define HARDCOPY_UTIL_ZIP_READER_H_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hardcopy {

class ZipReader {
public:
    ZipReader();
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool open(const std::filesystem::path& path);
    void close();

    bool read(const std::string& entryName, std::vector<uint8_t>* out);

    bool readText(const std::string& entryName, std::string* out);

    std::vector<std::string> entries();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
