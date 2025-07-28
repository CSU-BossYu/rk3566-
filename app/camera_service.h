#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <linux/videodev2.h>

#include "capture/v4l2_capture.h"
#include "rga/rga_worker.h"

class CameraService {
public:
    struct Config {
        std::string dev = "/dev/video9";
        int w = 640;
        int h = 480;
        uint32_t pixfmt = V4L2_PIX_FMT_YUYV;
        int req_bufs = 4;
        int timeout_ms = 1000;
    };

    CameraService(RgaWorker* rga_worker,
                  std::atomic<bool>* stop_flag);

    CameraService(RgaWorker* rga_worker,
                  std::atomic<bool>* stop_flag,
                  const Config& cfg);

    bool start();
    void join();

private:
    void thread_main();

private:
    RgaWorker* rga_ = nullptr;
    std::atomic<bool>* stop_ = nullptr;
    Config cfg_{};
    std::thread th_;
};
