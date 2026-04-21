#pragma once
#include <string>

class AppRuntime {
public:
    struct Config {
        std::string cam_dev = "/dev/video0";
        int cam_w = 640;
        int cam_h = 480;
        int cam_timeout_ms = 2000;

        int cam_fps = 30;

        int ui_out_w = 480;
        int ui_out_h = 480;

        int pool_blocks = 4;
        int pool_align  = 64;

        std::string db_path = "faces.db";

        int vision_every_n = 3;
        int vision_min_interval_ms = 0;

        // ✅新增：VisionWorker 二次节流（推理最小间隔）
        int vw_infer_min_interval_ms = 0;

        int fps_log_interval_ms = 1000;
    };

    AppRuntime();
    ~AppRuntime();

    AppRuntime(const AppRuntime&) = delete;
    AppRuntime& operator=(const AppRuntime&) = delete;

    bool init(const Config& cfg);
    int  run();
    void requestStop();

private:
    struct Impl;
    Impl* p_ = nullptr;
};
