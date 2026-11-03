#include "mobi_parser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hardcopy {

namespace {

uint16_t readBE16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t readBE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | p[3];
}

bool isJpeg(const uint8_t* p, size_t len) {
    return len >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}

bool isPng(const uint8_t* p, size_t len) {
    return len >= 4 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G';
}

bool isGif(const uint8_t* p, size_t len) {
    return len >= 3 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F';
}

bool isBmp(const uint8_t* p, size_t len) {
    return len >= 2 && p[0] == 'B' && p[1] == 'M';
}

bool isWebp(const uint8_t* p, size_t len) {
    return len >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F'
        && p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P';
}

bool isImage(const uint8_t* p, size_t len) {
    return isJpeg(p, len) || isPng(p, len) || isGif(p, len)
        || isBmp(p, len)  || isWebp(p, len);
}

const char* imageExt(const uint8_t* p, size_t len) {
    if (isJpeg(p, len)) return ".jpg";
    if (isPng(p, len))  return ".png";
    if (isGif(p, len))  return ".gif";
    if (isBmp(p, len))  return ".bmp";
    if (isWebp(p, len)) return ".webp";
    return ".bin";
}

}

bool parseMobi(const std::filesystem::path& path, ComicDocument* doc, Logger* log) {
    if (!doc) return false;

    FILE* f = nullptr;
#ifdef _WIN32
    std::wstring wpath = path.wstring();
    f = _wfopen(wpath.c_str(), L"rb");
#else
    f = fopen(path.string().c_str(), "rb");
#endif
    if (!f) {
        if (log) log->log(LogLevel::Error, "mobi", "failed to open file");
        return false;
    }

    uint8_t header[78];
    if (fread(header, 1, 78, f) != 78) {
        fclose(f);
        if (log) log->log(LogLevel::Error, "mobi", "file too small for PDB header");
        return false;
    }

    uint16_t numRecords = readBE16(header + 76);
    if (numRecords == 0) {
        fclose(f);
        if (log) log->log(LogLevel::Error, "mobi", "PDB has no records");
        return false;
    }

    std::vector<uint32_t> offsets(numRecords);
    for (uint16_t i = 0; i < numRecords; ++i) {
        uint8_t rec[8];
        if (fread(rec, 1, 8, f) != 8) {
            fclose(f);
            if (log) log->log(LogLevel::Error, "mobi", "truncated record table");
            return false;
        }
        offsets[i] = readBE32(rec);
    }

    fseek(f, 0, SEEK_END);
    uint32_t fileSize = (uint32_t)ftell(f);

    doc->title = path.stem().string();
    char pdbName[33] = {};
    std::memcpy(pdbName, header, 32);
    if (pdbName[0]) doc->title = pdbName;

    int imageCount = 0;
    for (uint16_t i = 0; i < numRecords; ++i) {
        uint32_t start = offsets[i];
        uint32_t end   = (i + 1 < numRecords) ? offsets[i + 1] : fileSize;
        if (start >= end || start >= fileSize) continue;

        uint32_t recLen = end - start;
        if (recLen < 4 || recLen > 50 * 1024 * 1024) continue;

        uint8_t magic[12];
        fseek(f, start, SEEK_SET);
        size_t magicLen = fread(magic, 1, sizeof(magic), f);
        if (magicLen < 2) continue;

        if (!isImage(magic, magicLen)) continue;

        std::vector<uint8_t> data(recLen);
        fseek(f, start, SEEK_SET);
        if (fread(data.data(), 1, recLen, f) != recLen) continue;

        ComicPage page;
        page.data = std::move(data);
        char nameBuf[32];
        std::snprintf(nameBuf, sizeof(nameBuf), "image_%04d%s",
                      ++imageCount, imageExt(page.data.data(), page.data.size()));
        page.filename = nameBuf;
        doc->pages.push_back(std::move(page));
    }

    fclose(f);

    if (doc->pages.empty()) {
        if (log) log->log(LogLevel::Error, "mobi", "no images found in MOBI file");
        return false;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "extracted %zu images from %u records",
                  doc->pages.size(), (unsigned)numRecords);
    if (log) log->log(LogLevel::Info, "mobi", buf);
    return true;
}

}
