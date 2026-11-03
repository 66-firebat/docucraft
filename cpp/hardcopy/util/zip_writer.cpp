#include "zip_writer.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace hardcopy {

struct ZipWriter::Impl {
    void* writer = nullptr;
    bool  isOpen = false;
};

ZipWriter::ZipWriter() : impl_(std::make_unique<Impl>()) {
    impl_->writer = mz_zip_writer_create();
}

ZipWriter::~ZipWriter() {
    close();
    if (impl_->writer) mz_zip_writer_delete(&impl_->writer);
}

bool ZipWriter::open(const std::filesystem::path& path) {
    if (!impl_->writer) return false;
    if (impl_->isOpen) close();
    int32_t rc = mz_zip_writer_open_file(impl_->writer, path.string().c_str(), 0, 0);
    impl_->isOpen = (rc == MZ_OK);
    return impl_->isOpen;
}

bool ZipWriter::addEntry(const std::string& entryName, const uint8_t* data, size_t len) {
    if (!impl_->writer || !impl_->isOpen) return false;

    mz_zip_file file_info = {};
    file_info.filename = entryName.c_str();
    file_info.compression_method = MZ_COMPRESS_METHOD_STORE;
    file_info.uncompressed_size = (int64_t)len;

    if (mz_zip_writer_entry_open(impl_->writer, &file_info) != MZ_OK)
        return false;

    int32_t written = 0;
    if (len > 0)
        written = mz_zip_writer_entry_write(impl_->writer, data, (int32_t)len);

    mz_zip_writer_entry_close(impl_->writer);
    return written >= 0;
}

bool ZipWriter::close() {
    if (impl_->writer && impl_->isOpen) {
        int32_t rc = mz_zip_writer_close(impl_->writer);
        impl_->isOpen = false;
        return rc == MZ_OK;
    }
    return true;
}

}
