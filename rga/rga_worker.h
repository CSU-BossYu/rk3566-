#pragma once
#include <atomic>
#include <thread>
#include <cstdint>

#include "capture/v4l2_capture.h"
#include "rga/rga_preprocess.h"
#include "utils/fixed_block_pool.h"
#include "utils/thread_safe_queue.h"
#include "ui/ui_frame_packet.h"

class VisionWorker;

class RgaWorker {
public:
    struct Config {
        int ui_out_w  = 480;
        int ui_out_h  = 480;
        int det_out_w = 320;
        int det_out_h = 320;

        // --- 兼容 app_runtime.cpp：保留字段，但“最终版本”里不再通过它们调参 ---
        // 你现在 app_runtime.cpp 在写这些字段，所以必须存在
        int vision_every_n        = 1;    // 兼容字段：送入 vision 的 N 分之一（最终版忽略：每帧喂）
        int vision_min_interval_ms= 0;    // 兼容字段：送入 vision 的最小间隔（最终版忽略）
        int fps_log_interval_ms   = 1000; // 兼容字段：RGA fps log 间隔（最终版可继续固定 1000）
    };

    RgaWorker(ThreadSafeQueue<UiFramePacket, 8>* ui_q,
              FixedBlockPool* ui_pool,
              std::atomic<bool>* stop_flag);

    RgaWorker(ThreadSafeQueue<UiFramePacket, 8>* ui_q,
              FixedBlockPool* ui_pool,
              std::atomic<bool>* stop_flag,
              const Config& cfg);

    RgaWorker(ThreadSafeQueue<UiFramePacket, 8>* ui_q,
              FixedBlockPool* ui_pool,
              std::atomic<bool>* stop_flag,
              const Config& cfg,
              VisionWorker* vision);

    bool start();
    void join();
    void stop();

    bool submit_ui(V4L2Capture::Frame&& fr);

private:
    struct Job {
        V4L2Capture::Frame fr;
    };

    void thread_main();

private:
    ThreadSafeQueue<UiFramePacket, 8>* ui_q_ = nullptr;
    FixedBlockPool* ui_pool_ = nullptr;
    std::atomic<bool>* stop_ = nullptr;

    Config cfg_{};
    VisionWorker* vision_ = nullptr;

    ThreadSafeQueue<Job, 4> job_q_;
    std::thread th_;
    RgaPreprocess rga_;

    // fps/prof
    uint64_t last_log_ms_ = 0;
    uint32_t cam_in_cnt_ = 0;
    uint32_t ui_out_cnt_ = 0;
    uint32_t vsn_out_cnt_ = 0;
    uint32_t ui_drop_cnt_ = 0;
    uint32_t vsn_drop_cnt_ = 0;

    double sum_ui_ms_ = 0;
    double sum_vsn_ms_ = 0;
};
