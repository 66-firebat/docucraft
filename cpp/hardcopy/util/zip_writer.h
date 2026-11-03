#ifndef HARDCOPY_UTIL_ZIP_WRITER_H_
#define HARDCOPY_UTIL_ZIP_WRITER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace hardcopy {

class ZipWriter {
public:
    ZipWriter();
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool open(const std::filesystem::path& path);

    bool addEntry(const std::string& entryName, const uint8_t* data, size_t len);

    bool close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
