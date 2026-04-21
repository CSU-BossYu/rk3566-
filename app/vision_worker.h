#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "utils/thread_safe_queue.h"
#include "utils/face_db.h"

#include "rknn/scrfd_detector_rknn.h"
#include "rknn/arcface_rknn.h"
#include "opencv_process/face_types.h"
#include "opencv_process/face_align.h"
#include "ui/ui_app.h"

class VisionWorker {
public:
    struct Packet {
        uint64_t ts_ms = 0;
        std::vector<uint8_t> rgb_cam; // cam_w*cam_h*3
        std::vector<uint8_t> rgb_det; // det_w*det_h*3
    };

    struct Config {
        int cam_w = 640;
        int cam_h = 480;
        int det_w = 320;
        int det_h = 320;
        int ui_w  = 480;
        int ui_h  = 480;

        std::string det_model = "models/det_500m_320.rknn";
        std::string arc_model = "models/w600k_mbf_112.rknn";
        std::string db_path   = "faces.db";

        float det_score_th = 0.5f;
        float det_nms_th   = 0.45f;
        float recog_th     = 0.35f;

        // --- 兼容 app_runtime.cpp：保留字段，但我们在“最终版本”里不再通过它们调参 ---
        // 你现在 app_runtime.cpp 在写这些字段，所以必须存在
        int fps_log_interval_ms   = 1000; // 兼容字段：Vision fps log 间隔
        int infer_min_interval_ms = 0;    // 兼容字段：推理最小间隔（最终版忽略，det 每帧跑）
    };

public:
    VisionWorker(std::atomic<bool>* stop_flag, const Config& cfg);
    ~VisionWorker();

    VisionWorker(const VisionWorker&) = delete;
    VisionWorker& operator=(const VisionWorker&) = delete;

    bool start();
    void join();
    void stop();

    // latest-only submit
    bool submit(Packet&& p);

private:
    void run();

    // DB
    bool db_open_();
    void db_reload_cache_();
    void ui_push_manage_list_();
    bool db_add_person_(const std::string& name, const std::vector<float>& feat512);
    bool db_delete_selected_();

    // UI actions
    void handle_ui_actions_();

    // Vision pipeline
    void process_frame_(const Packet& p,
                        double& det_ms,
                        double& align_ms,
                        double& arc_ms,
                        double& db_ms,
                        int& arc_ran);

    // helpers
    static uint64_t now_ms_();
    static uint64_t now_ns_();
    static float cosine_512_(const float* a, const float* b);
    static std::string make_default_name_();

    static void map_det_to_cam_kps_(int cam_w, int cam_h, int det_w, int det_h,
                                   const float det_kps[10], float cam_kps[10]);

    static void map_det_to_ui_boxes_(int ui_w, int ui_h, int det_w, int det_h,
                                    const std::vector<FaceBox>& faces,
                                    ui_face_box_t out4[4], int& out_n,
                                    int& out_cx, int& out_cy);

private:
    std::atomic<bool>* stop_ = nullptr;
    Config cfg_{};

    std::thread th_;

    ThreadSafeQueue<Packet, 2> q_;

    ScrfdDetectorRknn det_;
    ArcFaceRknn arc_;

    FaceDB db_;
    std::vector<FaceDB::Record> db_cache_;
    std::vector<std::string>    name_cache_;

    bool enrolling_ = false;
    std::string enroll_name_;

    ui_mode_t last_mode_ = UI_MODE_RECOG;
    bool manage_dirty_ = true;

    // Arc throttle state（最终版固定策略会用到）
    uint64_t last_arc_ms_ = 0;
    int last_cx_ = -999999;
    int last_cy_ = -999999;
    std::string last_name_ = "unknown";
    float last_sim_ = -1.0f;

    // profiling / fps
    uint64_t fps_last_ms_ = 0;
    uint32_t fps_det_cnt_ = 0;
    uint32_t fps_arc_cnt_ = 0;
    uint32_t fps_drop_cnt_ = 0;

    double sum_det_ms_ = 0;
    double sum_align_ms_ = 0;
    double sum_arc_ms_ = 0;
    double sum_db_ms_  = 0;
};
