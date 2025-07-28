#pragma once
#include <atomic>
#include <string>

class AppRuntime {
public:
    struct Config {
        std::string cam_dev = "/dev/video9";
        int cam_w = 640;
        int cam_h = 480;

        int ui_out_w = 480;
        int ui_out_h = 480;

        // UI pool 参数：每块大小 = ui_out_w * ui_out_h * 4 (BGRA)
        int pool_blocks = 4;
        int pool_align  = 64;

        // camera grab timeout
        int cam_timeout_ms = 1000;

        // ✅Face DB 路径：用于 VisionWorker / FaceDB
        std::string db_path = "/tmp/face.db";
    };

    AppRuntime();
    ~AppRuntime();

    AppRuntime(const AppRuntime&) = delete;
    AppRuntime& operator=(const AppRuntime&) = delete;

    bool init(const Config& cfg);
    int  run();             // 阻塞运行直到退出
    void requestStop();     // SIGINT / 外部调用触发退出

private:
    struct Impl;
    Impl* p_ = nullptr;
};
