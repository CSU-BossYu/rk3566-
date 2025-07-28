#pragma once
#include <atomic>
#include <thread>

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
    void stop(); // close internal queue

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
};
