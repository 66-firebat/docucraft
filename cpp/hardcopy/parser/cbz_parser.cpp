#include "cbz_parser.h"

#include <algorithm>
#include <cctype>

#include "../util/natural_sort.h"
#include "../util/zip_reader.h"

namespace hardcopy {

namespace {

bool isImageExt(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext;
    for (size_t i = dot; i < name.size(); ++i)
        ext.push_back((char)std::tolower((unsigned char)name[i]));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".gif" || ext == ".bmp"  || ext == ".webp";
}

bool shouldSkip(const std::string& name) {
    if (name.find("__MACOSX") != std::string::npos) return true;
    auto slash = name.rfind('/');
    size_t nameStart = (slash != std::string::npos) ? slash + 1 : 0;
    if (nameStart < name.size() && name[nameStart] == '.') return true;
    return false;
}

}

bool parseCbz(const std::filesystem::path& path, ComicDocument* doc, Logger* log) {
    if (!doc) return false;

    ZipReader zip;
    if (!zip.open(path)) {
        if (log) log->log(LogLevel::Error, "cbz", "failed to open archive");
        return false;
    }

    auto entries = zip.entries();
    std::vector<std::string> imageEntries;
    for (const auto& e : entries) {
        if (!shouldSkip(e) && isImageExt(e))
            imageEntries.push_back(e);
    }

    std::sort(imageEntries.begin(), imageEntries.end(), natsort::less);

    doc->title = path.stem().string();
    doc->pages.reserve(imageEntries.size());

    for (const auto& entry : imageEntries) {
        ComicPage page;
        if (!zip.read(entry, &page.data)) {
            if (log) {
                std::string msg = "failed to read: " + entry;
                log->log(LogLevel::Warn, "cbz", msg.c_str());
            }
            continue;
        }
        page.filename = entry;
        doc->pages.push_back(std::move(page));
    }

    if (doc->pages.empty()) {
        if (log) log->log(LogLevel::Error, "cbz", "no images found in archive");
        return false;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "extracted %zu images", doc->pages.size());
    if (log) log->log(LogLevel::Info, "cbz", buf);
    return true;
}

}
