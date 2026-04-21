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

static inline uint64_t now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static constexpr int FPS_LOG_MS = 1000;

// ✅最终策略：不再节流（保证框流畅）
// - UI 每帧都出
// - Vision 每帧都喂（除 MANAGE 模式）
static constexpr int VISION_EVERY_N = 1;
static constexpr int VISION_MIN_MS = 0;

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

    last_log_ms_ = now_ms();
    cam_in_cnt_ = ui_out_cnt_ = vsn_out_cnt_ = 0;
    ui_drop_cnt_ = vsn_drop_cnt_ = 0;
    sum_ui_ms_ = sum_vsn_ms_ = 0;

    std::fprintf(stderr, "[APP] Throttle fixed: RGA->Vision N=%d min_ms=%d\n", VISION_EVERY_N, VISION_MIN_MS);

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
    uint64_t last_vsn_ms = 0;
    uint32_t frame_idx = 0;

    while (!stop_->load()) {
        Job j;
        if (!job_q_.pop(j)) break;

        if (stop_->load()) break;
        if (j.fr.index < 0) continue;
        if (j.fr.dma_fd < 0) continue;

        cam_in_cnt_++;
        frame_idx++;

        const int src_w = j.fr.width;
        const int src_h = j.fr.height;

        RgaImage in{};
        in.width  = j.fr.width;
        in.height = j.fr.height;
        in.stride = j.fr.stride;
        in.format = RK_FORMAT_YUYV_422;
        in.dma_fd = j.fr.dma_fd;

        // center square crop（UI 与 det 统一）
        const int side  = (src_w < src_h) ? src_w : src_h;
        int crop_w = side;
        int crop_h = side;
        int crop_x = (src_w - side) / 2;
        int crop_y = (src_h - side) / 2;

        crop_x = clampi(crop_x, 0, src_w - 1);
        crop_y = clampi(crop_y, 0, src_h - 1);
        if (crop_x + crop_w > src_w) crop_w = src_w - crop_x;
        if (crop_y + crop_h > src_h) crop_h = src_h - crop_y;

        // 1) UI out (BGRA) - 每帧必做
        FixedBlockPool::Block blk;
        if (!ui_pool_->acquire(blk, -1)) continue;

        uint64_t ui0 = now_ns();
        bool ok_ui = rga_.yuyv_crop_to_bgra_resize(in,
                                                   crop_x, crop_y, crop_w, crop_h,
                                                   (uint8_t*)blk.ptr,
                                                   cfg_.ui_out_w, cfg_.ui_out_h);
        uint64_t ui1 = now_ns();
        sum_ui_ms_ += (double)(ui1 - ui0) / 1e6;

        if (!ok_ui) {
            ui_pool_->release(blk.idx);
            continue;
        }

        UiFramePacket pkt_ui{};
        pkt_ui.w = cfg_.ui_out_w;
        pkt_ui.h = cfg_.ui_out_h;
        pkt_ui.blk = blk;
        pkt_ui.boxes_n = 0;

        if (!ui_q_->try_push(std::move(pkt_ui))) {
            // UI 队列满：释放 block，计 drop
            ui_pool_->release(blk.idx);
            ui_drop_cnt_++;
        } else {
            ui_out_cnt_++;
        }

        // 2) Vision out - 每帧喂（除 MANAGE）
        if (vision_ && ui_app_get_mode() != UI_MODE_MANAGE) {
            // 固定节流参数（此处写死：N=1/min_ms=0 => 实际不节流）
            bool allow = true;
            if (VISION_EVERY_N > 1) {
                allow = ((frame_idx % (uint32_t)VISION_EVERY_N) == 0);
            }
            if (allow && VISION_MIN_MS > 0) {
                uint64_t tms = now_ms();
                if (last_vsn_ms != 0 && (tms - last_vsn_ms) < (uint64_t)VISION_MIN_MS) {
                    allow = false;
                }
            }

            if (allow) {
                VisionWorker::Packet vp;
                vp.ts_ms = now_ms();

                uint64_t vs0 = now_ns();
                // cam：整帧 RGB（给 align 用）
                if (!rga_.yuyv_to_rgb_resize(in, vp.rgb_cam, src_w, src_h)) {
                    // ignore
                } else {
                    // det：center crop -> det size RGB
                    if (rga_.yuyv_crop_to_rgb_resize(in,
                                                    crop_x, crop_y, crop_w, crop_h,
                                                    vp.rgb_det,
                                                    cfg_.det_out_w, cfg_.det_out_h)) {
                        bool ok = vision_->submit(std::move(vp));
                        if (ok) {
                            vsn_out_cnt_++;
                            last_vsn_ms = vp.ts_ms;
                        } else {
                            vsn_drop_cnt_++;
                        }
                    }
                }
                uint64_t vs1 = now_ns();
                sum_vsn_ms_ += (double)(vs1 - vs0) / 1e6;
            }
        }

        // 3) FPS log (fixed 1s)
        uint64_t tnow = now_ms();
        uint64_t dt = tnow - last_log_ms_;
        if (dt >= (uint64_t)FPS_LOG_MS) {
            const double sec = (dt > 0) ? (double)dt / 1000.0 : 1.0;

            const double cam_fps = (double)cam_in_cnt_ / sec;
            const double ui_fps  = (double)ui_out_cnt_ / sec;
            const double vsn_fps = (double)vsn_out_cnt_ / sec;

            const double ui_avg_ms  = (ui_out_cnt_ ? (sum_ui_ms_ / (double)ui_out_cnt_) : 0.0);
            const double vsn_avg_ms = (cam_in_cnt_ ? (sum_vsn_ms_ / (double)cam_in_cnt_) : 0.0);

            std::fprintf(stderr,
                         "[FPS][RGA] in=%.1f ui=%.1f vsn=%.1f  ui_drop=%u vsn_drop=%u  [T] ui=%.2fms vsn=%.2fms (fixed N=%d min_ms=%d)\n",
                         cam_fps, ui_fps, vsn_fps,
                         ui_drop_cnt_, vsn_drop_cnt_,
                         ui_avg_ms, vsn_avg_ms,
                         VISION_EVERY_N, VISION_MIN_MS);

            last_log_ms_ = tnow;
            cam_in_cnt_ = ui_out_cnt_ = vsn_out_cnt_ = 0;
            ui_drop_cnt_ = vsn_drop_cnt_ = 0;
            sum_ui_ms_ = sum_vsn_ms_ = 0.0;
        }
    }
}
