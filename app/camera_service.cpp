#include "camera_service.h"
#include <cstdio>

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
    V4L2Capture cap(cfg_.dev, cfg_.w, cfg_.h, cfg_.pixfmt, cfg_.req_bufs);
    if (!cap.ok()) {
        std::fprintf(stderr, "[CAM][E] open failed: %s\n", cap.lastError().c_str());
        return;
    }

    while (!stop_->load()) {
        auto fr = cap.grab(cfg_.timeout_ms);
        if (fr.index < 0) continue;

        // 把 Frame move 给 RgaWorker：直到 RgaWorker 用完，Frame 才析构 qbuf
        if (!rga_->submit_ui(std::move(fr))) {
            // 如果 RgaWorker 已 close，退出
            break;
        }
    }
}
