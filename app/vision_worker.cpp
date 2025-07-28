#include "app/vision_worker.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <algorithm>
#include <chrono>
#include <unistd.h>
#include <sys/syscall.h>

static inline pid_t get_tid() {
    return (pid_t)syscall(SYS_gettid);
}

static void vlog(const char* tag, const char* fmt, ...) {
    std::fprintf(stderr, "[%s tid=%d] ", tag, (int)get_tid());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

VisionWorker::VisionWorker(std::atomic<bool>* stop_flag, const Config& cfg)
    : stop_(stop_flag), cfg_(cfg) {}

VisionWorker::~VisionWorker() {
    stop();
    join();
}

bool VisionWorker::start() {
    if (!stop_) return false;

    if (!det_.init(cfg_.det_model)) {
        ui_app_set_status("DET model init fail");
        return false;
    }
    if (!arc_.init(cfg_.arc_model)) {
        ui_app_set_status("ARC model init fail");
        return false;
    }

    if (!db_open_()) {
        ui_app_set_status("DB open fail");
        return false;
    }

    db_reload_cache_();
    manage_dirty_ = true;

    th_ = std::thread(&VisionWorker::run, this);
    return true;
}

void VisionWorker::join() {
    if (th_.joinable()) th_.join();
}

void VisionWorker::stop() {
    q_.close();
}

bool VisionWorker::submit(Packet&& p) {
    return q_.try_push(std::move(p));
}

bool VisionWorker::db_open_() {
    if (cfg_.db_path.empty()) return false;
    bool ok = db_.open(cfg_.db_path);
    vlog("VW", "db_open path=%s ok=%d", cfg_.db_path.c_str(), ok ? 1 : 0);
    return ok;
}

void VisionWorker::db_reload_cache_() {
    db_cache_.clear();
    name_cache_.clear();

    std::vector<FaceDB::Record> recs;
    if (!db_.load_all(recs)) {
        vlog("VW", "db_load_all fail");
        return;
    }

    db_cache_.swap(recs);
    name_cache_.reserve(db_cache_.size());
    for (auto& r : db_cache_) name_cache_.push_back(r.name);

    vlog("VW", "db_cache size=%zu", db_cache_.size());
}

void VisionWorker::ui_push_manage_list_() {
    static std::vector<const char*> ptrs;
    ptrs.clear();
    ptrs.reserve(name_cache_.size());
    for (auto& s : name_cache_) ptrs.push_back(s.c_str());
    ui_app_set_manage_list(ptrs.data(), (int)ptrs.size());
    vlog("VW", "push_manage_list n=%zu", name_cache_.size());
}

std::string VisionWorker::make_default_name_() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "Person_%04d%02d%02d_%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

float VisionWorker::cosine_512_(const float* a, const float* b) {
    double s = 0.0;
    for (int i = 0; i < 512; ++i) s += (double)a[i] * (double)b[i];
    return (float)s;
}

void VisionWorker::map_det_to_cam_kps_(int cam_w, int cam_h, int det_w, int det_h,
                                      const float det_kps[10], float cam_kps[10]) {
    const int side = (cam_w < cam_h) ? cam_w : cam_h;
    const int crop_x = (cam_w - side) / 2;
    const int crop_y = (cam_h - side) / 2;

    const float sx = (float)side / (float)det_w;
    const float sy = (float)side / (float)det_h;

    for (int i = 0; i < 5; ++i) {
        cam_kps[2*i + 0] = (float)crop_x + det_kps[2*i + 0] * sx;
        cam_kps[2*i + 1] = (float)crop_y + det_kps[2*i + 1] * sy;
    }
}

void VisionWorker::map_det_to_ui_boxes_(int ui_w, int ui_h, int det_w, int det_h,
                                       const std::vector<FaceBox>& faces,
                                       ui_face_box_t out4[4], int& out_n) {
    out_n = 0;
    const int n = (int)std::min<size_t>(4, faces.size());
    if (n <= 0) return;

    const float sx = (float)ui_w / (float)det_w;
    const float sy = (float)ui_h / (float)det_h;

    for (int i = 0; i < n; ++i) {
        const auto& f = faces[i];
        ui_face_box_t b{};
        b.x1 = (int)(f.x1 * sx);
        b.y1 = (int)(f.y1 * sy);
        b.x2 = (int)(f.x2 * sx);
        b.y2 = (int)(f.y2 * sy);
        out4[i] = b;
    }
    out_n = n;
}

bool VisionWorker::db_add_person_(const std::string& name, const std::vector<float>& feat512) {
    if (!db_.ok() || name.empty() || (int)feat512.size() != 512) return false;
    if (!db_.upsert_feat(name, feat512.data(), 512)) return false;

    db_reload_cache_();
    manage_dirty_ = true;
    return true;
}

bool VisionWorker::db_delete_selected_() {
    if (!db_.ok()) return false;

    const int sel = ui_app_get_selected();
    vlog("VW", "delete_selected sel=%d cache_n=%zu", sel, name_cache_.size());

    if (sel < 0 || sel >= (int)name_cache_.size()) {
        ui_app_set_status("DELETE: no selection");
        return false;
    }

    const std::string name = name_cache_[(size_t)sel];
    vlog("VW", "delete name=%s", name.c_str());

    if (!db_.remove_by_name(name)) {
        ui_app_set_status("DELETE: db fail");
        vlog("VW", "delete db remove fail");
        return false;
    }

    db_reload_cache_();
    manage_dirty_ = true;
    ui_app_set_status("DELETE: ok");
    vlog("VW", "delete ok");
    return true;
}

void VisionWorker::handle_ui_actions_() {
    const ui_mode_t m = ui_app_get_mode();
    const uint32_t act = ui_app_poll_actions();

    if (m != last_mode_) {
        vlog("VW", "mode change %d -> %d", (int)last_mode_, (int)m);
        last_mode_ = m;
        if (m == UI_MODE_MANAGE) manage_dirty_ = true;
    }

    if (act) {
        vlog("VW", "ui actions=0x%08x mode=%d", act, (int)m);
    }

    if ((act & UI_ACT_CANCEL_ENROLL) && (m == UI_MODE_ENROLL)) {
        enrolling_ = false;
        enroll_name_.clear();
        ui_app_set_status("ENROLL: cancelled");
        return;
    }

    if ((act & UI_ACT_START_ENROLL) && (m == UI_MODE_ENROLL)) {
        enrolling_ = true;

        char namebuf[128];
        int n = ui_app_get_enroll_name(namebuf, (int)sizeof(namebuf));
        if (n > 0) enroll_name_ = namebuf;
        else       enroll_name_ = make_default_name_();

        ui_app_set_status("ENROLL: waiting face");
        vlog("VW", "start enroll name=%s", enroll_name_.c_str());
        return;
    }

    if ((act & UI_ACT_DELETE) && (m == UI_MODE_MANAGE)) {
        (void)db_delete_selected_();
    }

    // ✅只在需要时推送（避免每轮循环都重建 LVGL list）
    if (m == UI_MODE_MANAGE && manage_dirty_) {
        ui_push_manage_list_();
        manage_dirty_ = false;
    }
}

void VisionWorker::process_frame_(const Packet& p) {
    std::vector<FaceBox> faces;
    if (!det_.detect(p.rgb_det.data(), cfg_.det_w, cfg_.det_h,
                     faces, cfg_.det_score_th, cfg_.det_nms_th)) {
        ui_app_set_status(enrolling_ ? "ENROLL: det fail" : "DET: fail");
        ui_app_set_overlay_boxes(nullptr, 0);
        return;
    }

    ui_face_box_t boxes[4];
    int boxes_n = 0;
    map_det_to_ui_boxes_(cfg_.ui_w, cfg_.ui_h, cfg_.det_w, cfg_.det_h, faces, boxes, boxes_n);
    ui_app_set_overlay_boxes(boxes, boxes_n);

    if (faces.empty()) {
        ui_app_set_status(enrolling_ ? "ENROLL: waiting face" : "DET: none\nFEAT: none");
        return;
    }

    const FaceBox& best = faces[0];

    float cam_kps[10];
    map_det_to_cam_kps_(cfg_.cam_w, cfg_.cam_h, cfg_.det_w, cfg_.det_h, best.kps, cam_kps);

    uint8_t face112[112 * 112 * 3];
    bool ok_align = face_align::align_5pts_rgb112(
        p.rgb_cam.data(), cfg_.cam_w, cfg_.cam_h, cam_kps, face112);

    if (!ok_align) {
        ui_app_set_status(enrolling_ ? "ENROLL: align fail" : "ALIGN: fail");
        return;
    }

    std::vector<float> emb;
    if (!arc_.inferEmbeddingHWC(face112, 112, 112, emb)) {
        ui_app_set_status(enrolling_ ? "ENROLL: arc fail" : "ARC: fail");
        return;
    }

    if (enrolling_) {
    if (db_add_person_(enroll_name_, emb)) {
            enrolling_ = false;
            ui_app_set_status("ENROLL: saved");

            // ✅通知 UI：录入完成，让 UI 自己把按钮文案恢复为 Start、清空输入框等
            ui_app_notify_enroll_done();
        } else {
            ui_app_set_status("ENROLL: db fail");
        }
        return;
    }


    std::string best_name = "unknown";
    float best_sim = -1.0f;

    for (const auto& r : db_cache_) {
        if ((int)r.feat.size() != 512) continue;
        float s = cosine_512_(emb.data(), r.feat.data());
        if (s > best_sim) {
            best_sim = s;
            best_name = r.name;
        }
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "DET: %.3f  faces=%d\nTOP: %s  sim=%.3f",
                  best.score, (int)faces.size(),
                  best_name.c_str(), best_sim);

    if (best_sim < cfg_.recog_th) {
        ui_app_set_status("DET: ok\nTOP: unknown");
    } else {
        ui_app_set_status(buf);
    }
}

void VisionWorker::run() {
    vlog("VW", "thread start");

    while (!stop_->load()) {
        handle_ui_actions_();

        const ui_mode_t m = ui_app_get_mode();
        if (m == UI_MODE_MANAGE) {
            Packet tmp;
            while (q_.try_pop(tmp)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        Packet p;
        if (!q_.pop(p)) break;

        if (stop_->load()) break;
        if (p.rgb_cam.empty() || p.rgb_det.empty()) continue;

        process_frame_(p);
    }

    vlog("VW", "thread exit");
}
