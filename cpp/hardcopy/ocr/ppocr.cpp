#include "ocr/ppocr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include "logger.h"

#if HARDCOPY_HAS_OCR
#include <net.h>

extern "C" const unsigned char hc_ppocr_det_param[], hc_ppocr_det_bin[], hc_ppocr_det_bin_end[];
extern "C" const unsigned char hc_ppocr_rec_param[], hc_ppocr_rec_bin[], hc_ppocr_rec_bin_end[];

namespace {
#include "ppocrv5_dict.h"
}
#endif

namespace hardcopy {

#if HARDCOPY_HAS_OCR
namespace {

using Clock = std::chrono::steady_clock;
long long msSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

struct Box { int x0, y0, x1, y1; };
struct Spike { int id; float t; float prob; };

constexpr float kDetScale = 0.5f;
constexpr int kDetMaxSide = 1280;
constexpr int kDetBand = 320;
constexpr int kDetOverlap = 64;
constexpr float kDetThreshold = 0.3f;
constexpr float kBoxThreshold = 0.6f;
constexpr float kUnclipRatio = 1.5f;
constexpr int kRecHeight = 48;
constexpr float kMinLineProb = 0.5f;
constexpr int kInk = 160;

}

struct PpOcr::Impl {
    ncnn::Net det, rec;
    int dictSize = 0;
    mutable std::mutex detMutex;
};

PpOcr::PpOcr() : impl_(std::make_unique<Impl>()) {}
PpOcr::~PpOcr() = default;

std::shared_ptr<PpOcr> PpOcr::create(Logger* log, int threads) {
    std::shared_ptr<PpOcr> ocr(new PpOcr());
    Impl& m = *ocr->impl_;
    for (ncnn::Net* n : {&m.det, &m.rec}) {
        n->opt.use_vulkan_compute = false;
        n->opt.lightmode = true;
    }
    m.det.opt.num_threads = std::max(1, threads);
    m.rec.opt.num_threads = 1;
    const auto start = Clock::now();
    const bool ok =
        m.det.load_param_mem(reinterpret_cast<const char*>(hc_ppocr_det_param)) == 0 &&
        m.det.load_model(hc_ppocr_det_bin) > 0 &&
        m.rec.load_param_mem(reinterpret_cast<const char*>(hc_ppocr_rec_param)) == 0 &&
        m.rec.load_model(hc_ppocr_rec_bin) > 0;
    m.dictSize = (int)(sizeof(character_dict) / sizeof(character_dict[0]));
    char msg[160];
    if (!ok) {
        if (log) log->log(LogLevel::Error, "ocr", "PP-OCR model load failed");
        return nullptr;
    }
    std::snprintf(msg, sizeof(msg), "PP-OCRv5 mobile loaded in %lldms (models %d KB)",
                  msSince(start), (int)((hc_ppocr_det_bin_end - hc_ppocr_det_bin) +
                                        (hc_ppocr_rec_bin_end - hc_ppocr_rec_bin)) / 1024);
    if (log) log->log(LogLevel::Info, "ocr", msg);
    return ocr;
}

namespace {

void detectBand(const ncnn::Net& net, const Bitmap8& page, const ImageRect& r, float scale,
                int coreY0, int coreY1, std::vector<Box>* boxes) {
    const int rw = r.x1 - r.x0, rh = r.y1 - r.y0;
    const int w = std::max(32, (int)std::lround(rw * scale));
    const int h = std::max(32, (int)std::lround(rh * scale));
    const float sx = (float)w / rw, sy = (float)h / rh;
    ncnn::Mat in = ncnn::Mat::from_pixels_roi_resize(page.pixels.data(), ncnn::Mat::PIXEL_GRAY2BGR,
        page.width, page.height, r.x0, r.y0, rw, rh, w, h);
    const int wpad = (w + 31) / 32 * 32 - w, hpad = (h + 31) / 32 * 32 - h;
    ncnn::Mat padded;
    ncnn::copy_make_border(in, padded, 0, hpad, 0, wpad, ncnn::BORDER_CONSTANT, 255.f);
    in.release();
    const float mean[3] = {0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f};
    const float norm[3] = {1 / 0.229f / 255.f, 1 / 0.224f / 255.f, 1 / 0.225f / 255.f};
    padded.substract_mean_normalize(mean, norm);
    ncnn::Mat out;
    {
        ncnn::Extractor ex = net.create_extractor();
        ex.input("in0", padded);
        ex.extract("out0", out);
    }
    const int ow = std::min(out.w, w), oh = std::min(out.h, h), stride = out.w;
    const float* prob = out.channel(0);
    std::vector<unsigned char> bin((size_t)ow * oh);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) bin[(size_t)y * ow + x] = prob[(size_t)y * stride + x] > kDetThreshold;
    std::vector<int> stack;
    for (int y0 = 0; y0 < oh; ++y0)
        for (int x0 = 0; x0 < ow; ++x0) {
            if (!bin[(size_t)y0 * ow + x0]) continue;
            int bx0 = x0, bx1 = x0, by0 = y0, by1 = y0, n = 0;
            double sum = 0;
            stack.assign(1, y0 * ow + x0);
            bin[(size_t)y0 * ow + x0] = 0;
            while (!stack.empty()) {
                const int i = stack.back(); stack.pop_back();
                const int x = i % ow, y = i / ow;
                sum += prob[(size_t)y * stride + x]; ++n;
                bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
                by0 = std::min(by0, y); by1 = std::max(by1, y);
                if (x > 0 && bin[i - 1]) { bin[i - 1] = 0; stack.push_back(i - 1); }
                if (x + 1 < ow && bin[i + 1]) { bin[i + 1] = 0; stack.push_back(i + 1); }
                if (y > 0 && bin[i - ow]) { bin[i - ow] = 0; stack.push_back(i - ow); }
                if (y + 1 < oh && bin[i + ow]) { bin[i + ow] = 0; stack.push_back(i + ow); }
            }
            const float bw = bx1 - bx0 + 1.f, bh = by1 - by0 + 1.f;
            if (std::max(bw, bh) < 3 || sum / n < kBoxThreshold) continue;
            const float d = bw * bh * kUnclipRatio / (2 * (bw + bh));
            Box b;
            b.x0 = r.x0 + std::max(0, (int)std::floor((bx0 - d) / sx));
            b.y0 = r.y0 + std::max(0, (int)std::floor((by0 - d) / sy));
            b.x1 = r.x0 + std::min(rw, (int)std::ceil((bx1 + 1 + d) / sx));
            b.y1 = r.y0 + std::min(rh, (int)std::ceil((by1 + 1 + d) / sy));
            const int cy = r.y0 + (int)((by0 + by1 + 1) / 2.f / sy);
            if (cy < coreY0 || cy >= coreY1) continue;
            if (b.x1 - b.x0 >= 4 && b.y1 - b.y0 >= 4) boxes->push_back(b);
        }
}

std::vector<Box> detectLines(const ncnn::Net& net, const Bitmap8& page, const ImageRect& r) {
    const int rw = r.x1 - r.x0, rh = r.y1 - r.y0;
    const float scale = std::min(kDetScale, (float)kDetMaxSide / rw);
    const int band = std::max(64, (int)std::lround(kDetBand / scale));
    const int overlap = (int)std::lround(kDetOverlap / scale);
    std::vector<Box> boxes;
    for (int y = r.y0; y < r.y1; y += band - overlap) {
        ImageRect b = r;
        b.y0 = y;
        b.y1 = std::min(r.y1, y + band);
        if (r.y1 - b.y1 < overlap) b.y1 = r.y1;
        const int coreY0 = y == r.y0 ? r.y0 : y + overlap / 2;
        const int coreY1 = b.y1 == r.y1 ? r.y1 : b.y1 - overlap / 2;
        detectBand(net, page, b, scale, coreY0, coreY1, &boxes);
        if (b.y1 == r.y1) break;
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
        const int ca = a.y0 + a.y1, cb = b.y0 + b.y1;
        if (std::abs(ca - cb) > std::min(a.y1 - a.y0, b.y1 - b.y0)) return ca < cb;
        return a.x0 < b.x0;
    });
    return boxes;
}

std::vector<Spike> recognizeLine(const ncnn::Net& net, const Bitmap8& page, const Box& b, float* step) {
    const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    const int tw = std::max(16, (int)std::lround((float)bw * kRecHeight / bh));
    ncnn::Mat in = ncnn::Mat::from_pixels_roi_resize(page.pixels.data(), ncnn::Mat::PIXEL_GRAY2BGR,
        page.width, page.height, b.x0, b.y0, bw, bh, tw, kRecHeight);
    const float mean[3] = {127.5f, 127.5f, 127.5f};
    const float norm[3] = {1 / 127.5f, 1 / 127.5f, 1 / 127.5f};
    in.substract_mean_normalize(mean, norm);
    ncnn::Extractor ex = net.create_extractor();
    ex.input("in0", in);
    ncnn::Mat out;
    ex.extract("out0", out);
    std::vector<Spike> spikes;
    if (out.h <= 0) return spikes;
    *step = (float)bw / out.h;
    int last = 0;
    for (int t = 0; t < out.h; ++t) {
        const float* p = out.row(t);
        int idx = 0;
        float best = p[0];
        for (int j = 1; j < out.w; ++j) if (p[j] > best) { best = p[j]; idx = j; }
        if (idx != last && idx > 0) spikes.push_back({idx - 1, t + 0.5f, best});
        last = idx;
    }
    return spikes;
}

}

void PpOcr::recognize(const Bitmap8& page, const ImageRect& region, int threads,
                      std::vector<OcrTextLine>* out, OcrStats* stats) const {
    ImageRect r = region;
    r.x0 = std::clamp(r.x0, 0, page.width); r.x1 = std::clamp(r.x1, 0, page.width);
    r.y0 = std::clamp(r.y0, 0, page.height); r.y1 = std::clamp(r.y1, 0, page.height);
    if (r.x1 - r.x0 < 16 || r.y1 - r.y0 < 16) return;
    threads = std::max(1, threads);
    auto t0 = Clock::now();
    std::vector<Box> boxes;
    {
        std::lock_guard<std::mutex> lock(impl_->detMutex);
        boxes = detectLines(impl_->det, page, r);
    }
    if (stats) stats->detectMs += msSince(t0);
    t0 = Clock::now();
    struct Result { std::vector<Spike> spikes; float step = 0; };
    std::vector<Result> results(boxes.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next++) < boxes.size();)
            results[i].spikes = recognizeLine(impl_->rec, page, boxes[i], &results[i].step);
    };
    const int workers = std::min<int>(threads, (int)boxes.size());
    std::vector<std::thread> pool;
    for (int i = 1; i < workers; ++i) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    if (stats) stats->recognizeMs += msSince(t0);

    for (size_t li = 0; li < boxes.size(); ++li) {
        const Box& b = boxes[li];
        const auto& spikes = results[li].spikes;
        if (spikes.empty()) continue;
        float probSum = 0;
        for (const auto& s : spikes) probSum += s.prob;
        if (probSum / spikes.size() < kMinLineProb) continue;
        int ix0 = b.x1, ix1 = b.x0, iy0 = b.y1, iy1 = b.y0;
        for (int y = b.y0; y < b.y1; ++y) {
            const uint8_t* row = page.pixels.data() + (size_t)y * page.width;
            for (int x = b.x0; x < b.x1; ++x)
                if (row[x] < kInk) {
                    ix0 = std::min(ix0, x); ix1 = std::max(ix1, x + 1);
                    iy0 = std::min(iy0, y); iy1 = std::max(iy1, y + 1);
                }
        }
        if (ix1 <= ix0) { ix0 = b.x0; ix1 = b.x1; iy0 = b.y0; iy1 = b.y1; }
        OcrTextLine line;
        line.fontPx = std::max(4.f, (iy1 - iy0) / 0.9f);
        line.baseline = iy1 - 0.1f * line.fontPx;
        const float step = results[li].step;
        std::vector<float> centre(spikes.size());
        for (size_t i = 0; i < spikes.size(); ++i) centre[i] = b.x0 + spikes[i].t * step;
        const float pitch = spikes.size() > 1
            ? (centre.back() - centre.front()) / (spikes.size() - 1)
            : (float)(ix1 - ix0);
        for (size_t i = 0; i < spikes.size(); ++i) {
            const float x0 = i == 0 ? std::max((float)ix0, centre[i] - pitch / 2)
                                    : (centre[i - 1] + centre[i]) / 2;
            const float x1 = i + 1 == spikes.size() ? std::min((float)ix1, centre[i] + pitch / 2)
                                                    : (centre[i] + centre[i + 1]) / 2;
            OcrGlyph g;
            g.x0 = x0;
            g.x1 = std::max(x0, x1);
            const int id = spikes[i].id;
            g.text = id < impl_->dictSize ? character_dict[id] : " ";
            line.glyphs.push_back(std::move(g));
            if (stats) ++stats->chars;
        }
        out->push_back(std::move(line));
        if (stats) ++stats->lines;
    }
}

bool ocrAvailable() { return true; }

#else

struct PpOcr::Impl {};
PpOcr::PpOcr() = default;
PpOcr::~PpOcr() = default;
std::shared_ptr<PpOcr> PpOcr::create(Logger* log, int) {
    if (log) log->log(LogLevel::Warn, "ocr", "OCR not built in (no ncnn library for this target)");
    return nullptr;
}
void PpOcr::recognize(const Bitmap8&, const ImageRect&, int, std::vector<OcrTextLine>*, OcrStats*) const {}
bool ocrAvailable() { return false; }

#endif

}
