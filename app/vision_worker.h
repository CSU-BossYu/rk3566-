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
#include "opencv_process/face_types.h"   // FaceBox
#include "opencv_process/face_align.h"   // align_5pts_rgb112
#include "ui/ui_app.h"                   // UI actions / overlay / status

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

        int ui_w = 480;
        int ui_h = 480;

        std::string det_model = "models/det_500m_320.rknn";
        std::string arc_model = "models/w600k_mbf_112.rknn";

        std::string db_path = "faces.db";

        float det_score_th = 0.5f;
        float det_nms_th   = 0.45f;

        float recog_th = 0.35f; // cosine threshold
    };

public:
    VisionWorker(std::atomic<bool>* stop_flag, const Config& cfg);
    ~VisionWorker();

    VisionWorker(const VisionWorker&) = delete;
    VisionWorker& operator=(const VisionWorker&) = delete;

    bool start();
    void join();

    void stop();
    bool submit(Packet&& p);

private:
    void run();

    // DB
    bool db_open_();
    void db_reload_cache_();
    void ui_push_manage_list_();
    bool db_add_person_(const std::string& name, const std::vector<float>& feat512);
    bool db_delete_selected_();

    // Vision pipeline
    void handle_ui_actions_();
    void process_frame_(const Packet& p);

    // helpers
    static float cosine_512_(const float* a, const float* b);
    static std::string make_default_name_();
    static void map_det_to_cam_kps_(int cam_w, int cam_h, int det_w, int det_h,
                                   const float det_kps[10], float cam_kps[10]);

    static void map_det_to_ui_boxes_(int ui_w, int ui_h, int det_w, int det_h,
                                    const std::vector<FaceBox>& faces,
                                    ui_face_box_t out4[4], int& out_n);

private:
    std::atomic<bool>* stop_ = nullptr;
    Config cfg_{};

    std::thread th_;
    ThreadSafeQueue<Packet, 4> q_;

    ScrfdDetectorRknn det_;
    ArcFaceRknn arc_;

    FaceDB db_;
    std::vector<FaceDB::Record> db_cache_;
    std::vector<std::string>    name_cache_;

    // enroll
    bool enrolling_ = false;
    std::string enroll_name_;

    // manage list 推送节流（避免每一轮都重建 list 导致 LVGL 堆抖动甚至崩溃）
    ui_mode_t last_mode_ = UI_MODE_RECOG;
    bool manage_dirty_ = true;
};
