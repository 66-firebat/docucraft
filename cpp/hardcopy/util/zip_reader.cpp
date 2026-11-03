#include "zip_reader.h"

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace hardcopy {

struct ZipReader::Impl {
    void* reader = nullptr;
    bool  isOpen = false;
    std::vector<uint8_t> fileBuf;
};

ZipReader::ZipReader() : impl_(std::make_unique<Impl>()) {
    impl_->reader = mz_zip_reader_create();
}

ZipReader::~ZipReader() {
    close();
    if (impl_->reader) mz_zip_reader_delete(&impl_->reader);
}

bool ZipReader::open(const std::filesystem::path& path) {
    if (!impl_->reader) return false;
    if (impl_->isOpen) close();

#ifdef _WIN32
    std::wstring wpath = path.wstring();
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }
    impl_->fileBuf.resize((size_t)liSize.QuadPart);
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, impl_->fileBuf.data(), (DWORD)liSize.QuadPart, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead != (DWORD)liSize.QuadPart) {
        impl_->fileBuf.clear();
        return false;
    }
#else
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    if (sz <= 0) return false;
    impl_->fileBuf.resize((size_t)sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(impl_->fileBuf.data()), sz);
    if (!f) { impl_->fileBuf.clear(); return false; }
    f.close();
#endif

    int32_t rc = mz_zip_reader_open_buffer(impl_->reader,
        impl_->fileBuf.data(), (int32_t)impl_->fileBuf.size(), 0);
    impl_->isOpen = (rc == MZ_OK);
    if (!impl_->isOpen) impl_->fileBuf.clear();
    return impl_->isOpen;
}

void ZipReader::close() {
    if (impl_->reader && impl_->isOpen) {
        mz_zip_reader_close(impl_->reader);
        impl_->isOpen = false;
    }
    impl_->fileBuf.clear();
    impl_->fileBuf.shrink_to_fit();
}

bool ZipReader::read(const std::string& entryName, std::vector<uint8_t>* out) {
    if (!impl_->reader || !impl_->isOpen || !out) return false;
    if (mz_zip_reader_locate_entry(impl_->reader, entryName.c_str(), 1) != MZ_OK) {
        return false;
    }
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(impl_->reader, &info) != MZ_OK || !info) {
        return false;
    }
    if (mz_zip_reader_entry_open(impl_->reader) != MZ_OK) return false;

    out->resize((size_t)info->uncompressed_size);
    int32_t bytes = 0;
    if (info->uncompressed_size > 0) {
        bytes = mz_zip_reader_entry_read(impl_->reader, out->data(),
                                         (int32_t)info->uncompressed_size);
    }
    mz_zip_reader_entry_close(impl_->reader);
    return bytes >= 0;
}

bool ZipReader::readText(const std::string& entryName, std::string* out) {
    if (!out) return false;
    std::vector<uint8_t> buf;
    if (!read(entryName, &buf)) return false;
    out->assign(buf.begin(), buf.end());
    return true;
}

std::vector<std::string> ZipReader::entries() {
    std::vector<std::string> result;
    if (!impl_->reader || !impl_->isOpen) return result;

    int32_t rc = mz_zip_reader_goto_first_entry(impl_->reader);
    while (rc == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(impl_->reader, &info) == MZ_OK && info) {
            if (info->filename && info->filename[0])
                result.emplace_back(info->filename);
        }
        rc = mz_zip_reader_goto_next_entry(impl_->reader);
    }
    return result;
}

}
