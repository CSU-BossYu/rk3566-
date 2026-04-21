#include "camera_service.h"
#include <cstdio>
#include <sys/time.h>

static inline uint64_t now_ms_cam() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

CameraService::CameraService(RgaWorker* rga_worker,
                             std::atomic<bool>* stop_flag)
    : CameraService(rga_worker, stop_flag, Config{}) {}

CameraService::CameraService(RgaWorker* rga_worker,
                             std::atomic<bool>* stop_flag,
                             const Config& cfg)
    : rga_(rga_worker), stop_(stop_flag), cfg_(cfg) {}

bool CameraService::start() {
    if (!rga_ || !stop_) return false;
    th_ = std::thread(&CameraService::thread_main, this);
    return true;
}

void CameraService::join() {
    if (th_.joinable()) th_.join();
}

void CameraService::thread_main() {
    V4L2Capture cap(cfg_.dev, cfg_.w, cfg_.h, cfg_.pixfmt, cfg_.req_bufs, cfg_.fps);
    if (!cap.ok()) {
        std::fprintf(stderr, "[CAM][E] open failed: %s\n", cap.lastError().c_str());
        return;
    }

    uint64_t last_ms = now_ms_cam();
    uint32_t cnt = 0;
    uint32_t drop = 0;

    while (!stop_->load()) {
        auto fr = cap.grab(cfg_.timeout_ms);
        if (fr.index < 0) continue;

        cnt++;

        // ✅不会阻塞：满了就丢（Frame 析构会自动 requeue）
        if (!rga_->submit_ui(std::move(fr))) {
            drop++;
        }

        if (cfg_.fps_log_interval_ms > 0) {
            uint64_t now = now_ms_cam();
            uint64_t dt = now - last_ms;
            if (dt >= (uint64_t)cfg_.fps_log_interval_ms) {
                double sec = (dt > 0) ? (double)dt / 1000.0 : 1.0;
                double fps = (double)cnt / sec;
                std::fprintf(stderr, "[FPS][CAM] dq=%.1f drop=%u\n", fps, drop);
                last_ms = now;
                cnt = 0;
                drop = 0;
            }
        }
    }
}
