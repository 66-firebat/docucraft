#include "page_image_dump.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include <nlohmann/json.hpp>

#include "../encode/jpeg.h"
#include "../logger.h"
#include "../options.h"
#include "../render/page_raster.h"
#include "streaming_pdf_writer.h"

namespace hardcopy {

namespace fs = std::filesystem;

namespace {

std::string randomHexId() {
    std::random_device rd;
    const uint64_t clock = (uint64_t)std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
    std::mt19937_64 gen(((uint64_t)rd() << 32) ^ rd() ^ clock);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long)gen(), (unsigned long long)gen());
    return buf;
}

std::string localTimeNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string leafName(int leaf1) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "p%04d.jpg", leaf1);
    return buf;
}

bool writeFileAtomic(const fs::path& target, const void* data, size_t size) {
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(static_cast<const char*>(data), (std::streamsize)size);
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmp, target, ec);
    }
    return !ec;
}

}

std::unique_ptr<PageImageDump> PageImageDump::create(const Options& opts,
                                                     const fs::path& output,
                                                     const fs::path& input,
                                                     Logger* log) {
    if (opts.pageImageDir.empty()) return nullptr;
    std::unique_ptr<PageImageDump> d(new PageImageDump());
    d->id_ = randomHexId();
    fs::path name = output.stem();
    if (name.empty()) name = "output";
    name += "-" + d->id_;
    const fs::path dir = fs::u8path(opts.pageImageDir) / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        if (log) {
            const std::string msg = "page images: cannot create " + dir.u8string();
            log->log(LogLevel::Warn, "page-images", msg.c_str());
        }
        return nullptr;
    }
    d->dir_ = dir;
    d->output_ = output.u8string();
    d->input_ = input.u8string();
    d->created_ = localTimeNow();
    d->quality_ = std::clamp(opts.jpegQuality, 1, 100);
    d->leafW_ = opts.pageWidth;
    d->leafH_ = opts.pageHeight;
    d->leavesPerSheet_ = opts.splitLandscape ? 2 : 1;
    d->log_ = log;
    {
        std::lock_guard<std::mutex> lk(d->mu_);
        d->writeManifest("running");
    }
    if (log) {
        const std::string msg = "page images -> " + d->dir_.u8string();
        log->log(LogLevel::Info, "page-images", msg.c_str());
    }
    return d;
}

PageImageDump::~PageImageDump() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!finished_) writeManifest("incomplete");
}

void PageImageDump::save(int leaf1, const Bitmap8& page) {
    if (leaf1 <= 0 || page.width <= 0 || page.height <= 0) return;
    std::vector<uint8_t> jpeg;
    bool ok = encodeJpegGray(page, quality_, &jpeg) && !jpeg.empty();
    if (ok) ok = writeFileAtomic(dir_ / leafName(leaf1), jpeg.data(), jpeg.size());
    std::lock_guard<std::mutex> lk(mu_);
    if (ok) {
        saved_.insert(leaf1);
        leafW_ = page.width;
        leafH_ = page.height;
        return;
    }
    if (failures_++ == 0 && log_) {
        const std::string msg = "page images: write failed for " + leafName(leaf1);
        log_->log(LogLevel::Warn, "page-images", msg.c_str());
    }
}

void PageImageDump::finish(const StreamingPdfWriter& pdf, int leavesPerSheet) {
    std::lock_guard<std::mutex> lk(mu_);
    leavesPerSheet_ = std::max(1, leavesPerSheet);
    sheets_ = pdf.pageCount();
    outline_.clear();
    for (const auto& e : pdf.outline()) outline_.push_back({e.title, e.level, e.pageIndex0});
    writeManifest("complete");
    finished_ = true;
    if (log_) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "page images: %zu files, %d failed, %zu chapters, id=%s",
                      saved_.size(), failures_, outline_.size(), id_.c_str());
        log_->log(LogLevel::Info, "page-images", msg);
    }
}

void PageImageDump::writeManifest(const char* status) {
    const int per = std::max(1, leavesPerSheet_);
    int sheets = sheets_;
    if (sheets <= 0 && !saved_.empty()) sheets = (*saved_.rbegin() + per - 1) / per;
    nlohmann::json pages = nlohmann::json::array();
    for (int s = 0; s < sheets; ++s) {
        nlohmann::json leaves = nlohmann::json::array();
        for (int k = 1; k <= per; ++k) {
            const int leaf = s * per + k;
            if (saved_.count(leaf)) leaves.push_back(leafName(leaf));
        }
        pages.push_back(std::move(leaves));
    }
    nlohmann::json j = {
        {"version", 1},
        {"id", id_},
        {"status", status},
        {"created", created_},
        {"pdf", output_},
        {"source", input_},
        {"quality", quality_},
        {"leafWidth", leafW_},
        {"leafHeight", leafH_},
        {"leavesPerSheet", per},
        {"sheetCount", sheets},
        {"leafCount", (int)saved_.size()},
        {"failed", failures_},
        {"pages", std::move(pages)},
    };
    if (!outline_.empty()) {
        nlohmann::json chapters = nlohmann::json::array();
        for (const auto& c : outline_)
            chapters.push_back({{"title", c.title}, {"level", c.level}, {"sheet", c.sheet}});
        j["outline"] = std::move(chapters);
    }
    const std::string text = j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    writeFileAtomic(dir_ / "manifest.json", text.data(), text.size());
}

}
