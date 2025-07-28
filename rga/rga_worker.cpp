#include "rga_worker.h"
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "app/vision_worker.h"
#include "ui/ui_app.h"

static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline uint64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

RgaWorker::RgaWorker(ThreadSafeQueue<UiFramePacket, 8>* ui_q,
                     FixedBlockPool* ui_pool,
                     std::atomic<bool>* stop_flag)
    : RgaWorker(ui_q, ui_pool, stop_flag, Config{}, nullptr) {}

RgaWorker::RgaWorker(ThreadSafeQueue<UiFramePacket, 8>* ui_q,
                     FixedBlockPool* ui_pool,
                     std::atomic<bool>* stop_flag,
                     const Config& cfg)
    : RgaWorker(ui_q, ui_pool, stop_flag, cfg, nullptr) {}

RgaWorker::RgaWorker(ThreadSafeQueue<UiFramePacket, 8>* ui_q,
                     FixedBlockPool* ui_pool,
                     std::atomic<bool>* stop_flag,
                     const Config& cfg,
                     VisionWorker* vision)
    : ui_q_(ui_q), ui_pool_(ui_pool), stop_(stop_flag), cfg_(cfg), vision_(vision) {}

bool RgaWorker::start() {
    if (!ui_q_ || !ui_pool_ || !stop_) return false;
    if (!rga_.init()) {
        std::fprintf(stderr, "[RGA][E] init failed: %s\n", rga_.lastError().c_str());
        return false;
    }
    th_ = std::thread(&RgaWorker::thread_main, this);
    return true;
}

void RgaWorker::join() {
    if (th_.joinable()) th_.join();
}

void RgaWorker::stop() {
    job_q_.close();
}

bool RgaWorker::submit_ui(V4L2Capture::Frame&& fr) {
    Job j;
    j.fr = std::move(fr);
    return job_q_.push(std::move(j));
}

void RgaWorker::thread_main() {
    while (!stop_->load()) {
        Job j;
        if (!job_q_.pop(j)) break;

        if (stop_->load()) break;
        if (j.fr.index < 0) continue;
        if (j.fr.dma_fd < 0) continue; // 你要求走 dma_fd

        const int src_w = j.fr.width;
        const int src_h = j.fr.height;

        RgaImage in{};
        in.width  = j.fr.width;
        in.height = j.fr.height;
        in.stride = j.fr.stride;
        in.format = RK_FORMAT_YUYV_422;
        in.dma_fd = j.fr.dma_fd;

        // 1) 中心正方形 crop（UI 与 det 统一）
        const int side  = (src_w < src_h) ? src_w : src_h;
        int crop_w = side;
        int crop_h = side;
        int crop_x = (src_w - side) / 2;
        int crop_y = (src_h - side) / 2;

        crop_x = clampi(crop_x, 0, src_w - 1);
        crop_y = clampi(crop_y, 0, src_h - 1);
        if (crop_x + crop_w > src_w) crop_w = src_w - crop_x;
        if (crop_y + crop_h > src_h) crop_h = src_h - crop_y;

        // 2) UI：pool 申请 BGRA
        FixedBlockPool::Block blk;
        if (!ui_pool_->acquire(blk, -1)) continue;

        bool ok_ui = rga_.yuyv_crop_to_bgra_resize(in,
                                                   crop_x, crop_y, crop_w, crop_h,
                                                   (uint8_t*)blk.ptr,
                                                   cfg_.ui_out_w, cfg_.ui_out_h);
        if (!ok_ui) {
            ui_pool_->release(blk.idx);
            continue;
        }

        UiFramePacket pkt_ui{};
        pkt_ui.w = cfg_.ui_out_w;
        pkt_ui.h = cfg_.ui_out_h;
        pkt_ui.blk = blk;
        pkt_ui.boxes_n = 0;

        if (!ui_q_->push(std::move(pkt_ui))) {
            ui_pool_->release(blk.idx);
            break;
        }

        // 3) Vision：MANAGE 模式不喂（省 CPU）
        if (!vision_) continue;
        if (ui_app_get_mode() == UI_MODE_MANAGE) continue;

        VisionWorker::Packet vp;
        vp.ts_ms = now_ms();

        // cam：整帧 RGB
        if (!rga_.yuyv_to_rgb_resize(in, vp.rgb_cam, src_w, src_h)) continue;

        // det：中心 crop -> 320x320 RGB
        if (!rga_.yuyv_crop_to_rgb_resize(in,
                                         crop_x, crop_y, crop_w, crop_h,
                                         vp.rgb_det,
                                         cfg_.det_out_w, cfg_.det_out_h)) {
            continue;
        }

        (void)vision_->submit(std::move(vp));
    }
}
