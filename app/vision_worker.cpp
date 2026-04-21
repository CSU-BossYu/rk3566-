#include "app/vision_worker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

// ✅ 补齐：va_start/va_end
#include <cstdarg>

// ✅ 补齐：syscall/SYS_gettid
#include <unistd.h>
#include <sys/syscall.h>

#include <sys/time.h>
#include <time.h>

static inline pid_t get_tid() {
#if defined(SYS_gettid)
    return (pid_t)::syscall(SYS_gettid);
#else
    // 兜底：没有 SYS_gettid 就返回进程 id
    return (pid_t)::getpid();
#endif
}

static inline void vwlog(const char* fmt, ...) {
    std::fprintf(stderr, "[VW tid=%d] ", (int)get_tid());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

static constexpr int FPS_LOG_MS = 1000;

// ✅最终策略：框流畅优先 => det 每帧跑；arc 固定低频触发
static constexpr int ARC_MIN_MS  = 300;
static constexpr int ARC_MOVE_PX = 12;
static constexpr int MAX_UI_BOXES = 4;

uint64_t VisionWorker::now_ms_() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

uint64_t VisionWorker::now_ns_() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
        ui_app_set_status("DET init fail");
        return false;
    }
    if (!arc_.init(cfg_.arc_model)) {
        ui_app_set_status("ARC init fail");
        return false;
    }

    if (!db_open_()) {
        ui_app_set_status("DB open fail");
        return false;
    }
    db_reload_cache_();

    fps_last_ms_ = now_ms_();
    fps_det_cnt_ = fps_arc_cnt_ = fps_drop_cnt_ = 0;
    sum_det_ms_ = sum_align_ms_ = sum_arc_ms_ = sum_db_ms_ = 0;

    last_arc_ms_ = 0;
    last_cx_ = last_cy_ = -999999;
    last_name_ = "unknown";
    last_sim_ = -1.0f;

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
    // latest-only：队列满则丢旧帧，尽量保证最新帧被处理
    if (q_.try_push(std::move(p))) return true;

    Packet junk;
    (void)q_.try_pop(junk);
    fps_drop_cnt_++;

    return q_.try_push(std::move(p));
}

// ---------------- DB ----------------

bool VisionWorker::db_open_() {
    if (cfg_.db_path.empty()) return false;
    bool ok = db_.open(cfg_.db_path);
    vwlog("db_open path=%s ok=%d", cfg_.db_path.c_str(), ok ? 1 : 0);
    return ok;
}

void VisionWorker::db_reload_cache_() {
    db_cache_.clear();
    name_cache_.clear();

    std::vector<FaceDB::Record> recs;
    if (!db_.load_all(recs)) {
        vwlog("db_load_all fail");
        return;
    }
    db_cache_.swap(recs);

    name_cache_.reserve(db_cache_.size());
    for (auto& r : db_cache_) name_cache_.push_back(r.name);

    vwlog("db_cache size=%zu", db_cache_.size());
}

void VisionWorker::ui_push_manage_list_() {
    static std::vector<const char*> ptrs;
    ptrs.clear();
    ptrs.reserve(name_cache_.size());
    for (auto& s : name_cache_) ptrs.push_back(s.c_str());
    ui_app_set_manage_list(ptrs.data(), (int)ptrs.size());
}

std::string VisionWorker::make_default_name_() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "Person_%04d%02d%02d_%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

bool VisionWorker::db_add_person_(const std::string& name, const std::vector<float>& feat512) {
    if (!db_.ok() || name.empty() || (int)feat512.size() != 512) return false;
    uint64_t t0 = now_ns_();
    bool ok = db_.upsert_feat(name, feat512.data(), 512);
    uint64_t t1 = now_ns_();
    sum_db_ms_ += (double)(t1 - t0) / 1e6;

    if (!ok) return false;
    db_reload_cache_();
    manage_dirty_ = true;
    return true;
}

bool VisionWorker::db_delete_selected_() {
    if (!db_.ok()) return false;

    const int sel = ui_app_get_selected();
    if (sel < 0 || sel >= (int)name_cache_.size()) {
        ui_app_set_status("DELETE: no selection");
        return false;
    }

    const std::string name = name_cache_[(size_t)sel];

    uint64_t t0 = now_ns_();
    bool ok = db_.remove_by_name(name);
    uint64_t t1 = now_ns_();
    sum_db_ms_ += (double)(t1 - t0) / 1e6;

    if (!ok) {
        ui_app_set_status("DELETE: db fail");
        return false;
    }

    db_reload_cache_();
    manage_dirty_ = true;
    ui_app_set_status("DELETE: ok");
    return true;
}

// ---------------- UI actions ----------------

void VisionWorker::handle_ui_actions_() {
    const ui_mode_t m = ui_app_get_mode();
    const uint32_t act = ui_app_poll_actions();

    if (m != last_mode_) {
        last_mode_ = m;
        if (m == UI_MODE_MANAGE) manage_dirty_ = true;
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
        return;
    }

    if ((act & UI_ACT_DELETE) && (m == UI_MODE_MANAGE)) {
        (void)db_delete_selected_();
    }

    if (m == UI_MODE_MANAGE && manage_dirty_) {
        ui_push_manage_list_();
        manage_dirty_ = false;
    }
}

// ---------------- mapping helpers ----------------

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
                                       ui_face_box_t out4[4], int& out_n,
                                       int& out_cx, int& out_cy) {
    out_n = 0;
    out_cx = -1;
    out_cy = -1;

    const int n = (int)std::min<size_t>(MAX_UI_BOXES, faces.size());
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

    const auto& f0 = faces[0];
    out_cx = (int)(((f0.x1 + f0.x2) * 0.5f) * sx);
    out_cy = (int)(((f0.y1 + f0.y2) * 0.5f) * sy);
}

// ---------------- pipeline ----------------

void VisionWorker::process_frame_(const Packet& p,
                                 double& det_ms,
                                 double& align_ms,
                                 double& arc_ms,
                                 double& db_ms,
                                 int& arc_ran) {
    det_ms = align_ms = arc_ms = db_ms = 0.0;
    arc_ran = 0;

    // 1) DET：每帧都跑
    uint64_t t0 = now_ns_();
    std::vector<FaceBox> faces;
    bool ok_det = det_.detect(p.rgb_det.data(), cfg_.det_w, cfg_.det_h,
                             faces, cfg_.det_score_th, cfg_.det_nms_th);
    uint64_t t1 = now_ns_();
    det_ms = (double)(t1 - t0) / 1e6;

    if (!ok_det) {
        ui_app_set_overlay_boxes(nullptr, 0);
        ui_app_set_status(enrolling_ ? "ENROLL: det fail" : "DET: fail");
        return;
    }

    ui_face_box_t boxes[MAX_UI_BOXES];
    int boxes_n = 0;
    int cx = -1, cy = -1;
    map_det_to_ui_boxes_(cfg_.ui_w, cfg_.ui_h, cfg_.det_w, cfg_.det_h, faces, boxes, boxes_n, cx, cy);
    ui_app_set_overlay_boxes(boxes, boxes_n);

    if (faces.empty()) {
        last_name_ = "unknown";
        last_sim_ = -1.0f;
        ui_app_set_status(enrolling_ ? "ENROLL: waiting face" : "DET: none\nFEAT: none");
        return;
    }

    const FaceBox& best = faces[0];

    // 2) ENROLL：要写库 => 每次都跑 arc
    if (enrolling_) {
        float cam_kps[10];
        map_det_to_cam_kps_(cfg_.cam_w, cfg_.cam_h, cfg_.det_w, cfg_.det_h, best.kps, cam_kps);

        uint8_t face112[112 * 112 * 3];
        uint64_t a0 = now_ns_();
        bool ok_align = face_align::align_5pts_rgb112(
            p.rgb_cam.data(), cfg_.cam_w, cfg_.cam_h, cam_kps, face112);
        uint64_t a1 = now_ns_();
        align_ms = (double)(a1 - a0) / 1e6;

        if (!ok_align) {
            ui_app_set_status("ENROLL: align fail");
            return;
        }

        std::vector<float> emb;
        uint64_t r0 = now_ns_();
        bool ok_arc = arc_.inferEmbeddingHWC(face112, 112, 112, emb);
        uint64_t r1 = now_ns_();
        arc_ms = (double)(r1 - r0) / 1e6;
        arc_ran = 1;

        if (!ok_arc) {
            ui_app_set_status("ENROLL: arc fail");
            return;
        }

        uint64_t d0 = now_ns_();
        bool ok = db_add_person_(enroll_name_, emb);
        uint64_t d1 = now_ns_();
        db_ms = (double)(d1 - d0) / 1e6;

        if (ok) {
            enrolling_ = false;
            ui_app_set_status("ENROLL: saved");
            ui_app_notify_enroll_done();
        } else {
            ui_app_set_status("ENROLL: db fail");
        }
        return;
    }

    // 3) RECOG：Arc 固定节流（300ms + 12px）
    const uint64_t now_ms = p.ts_ms;
    const bool hit_time = (last_arc_ms_ == 0) ? true : ((now_ms - last_arc_ms_) >= (uint64_t)ARC_MIN_MS);
    const int dx = (last_cx_ == -999999) ? 999999 : (cx - last_cx_);
    const int dy = (last_cy_ == -999999) ? 999999 : (cy - last_cy_);
    const int dist2 = dx*dx + dy*dy;
    const bool hit_move = (last_cx_ == -999999) ? true : (dist2 >= ARC_MOVE_PX * ARC_MOVE_PX);
    const bool do_arc = hit_time && hit_move;

    if (!do_arc) {
        char buf[256];
        if (last_sim_ >= cfg_.recog_th) {
            std::snprintf(buf, sizeof(buf), "DET: ok\nTOP: %s sim=%.3f", last_name_.c_str(), last_sim_);
        } else {
            std::snprintf(buf, sizeof(buf), "DET: ok\nTOP: unknown");
        }
        ui_app_set_status(buf);
        return;
    }

    last_arc_ms_ = now_ms;
    last_cx_ = cx;
    last_cy_ = cy;

    float cam_kps[10];
    map_det_to_cam_kps_(cfg_.cam_w, cfg_.cam_h, cfg_.det_w, cfg_.det_h, best.kps, cam_kps);

    uint8_t face112[112 * 112 * 3];
    uint64_t a0 = now_ns_();
    bool ok_align = face_align::align_5pts_rgb112(
        p.rgb_cam.data(), cfg_.cam_w, cfg_.cam_h, cam_kps, face112);
    uint64_t a1 = now_ns_();
    align_ms = (double)(a1 - a0) / 1e6;

    if (!ok_align) {
        ui_app_set_status("ALIGN: fail");
        return;
    }

    std::vector<float> emb;
    uint64_t r0 = now_ns_();
    bool ok_arc = arc_.inferEmbeddingHWC(face112, 112, 112, emb);
    uint64_t r1 = now_ns_();
    arc_ms = (double)(r1 - r0) / 1e6;
    arc_ran = 1;

    if (!ok_arc) {
        ui_app_set_status("ARC: fail");
        return;
    }

    uint64_t s0 = now_ns_();
    std::string best_name = "unknown";
    float best_sim = -1.0f;
    for (const auto& r : db_cache_) {
        if ((int)r.feat.size() != 512) continue;
        float s = cosine_512_(emb.data(), r.feat.data());
        if (s > best_sim) { best_sim = s; best_name = r.name; }
    }
    uint64_t s1 = now_ns_();
    db_ms = (double)(s1 - s0) / 1e6;

    last_name_ = best_name;
    last_sim_  = best_sim;

    char buf[256];
    if (best_sim < cfg_.recog_th) {
        std::snprintf(buf, sizeof(buf), "DET: ok\nTOP: unknown");
    } else {
        std::snprintf(buf, sizeof(buf), "DET: ok\nTOP: %s sim=%.3f", best_name.c_str(), best_sim);
    }
    ui_app_set_status(buf);
}

void VisionWorker::run() {
    vwlog("thread start (fixed det_each_frame, arc_min=%dms move=%dpx)", ARC_MIN_MS, ARC_MOVE_PX);

    while (!stop_->load()) {
        handle_ui_actions_();

        // manage 模式：清队列，避免 list 交互被打断
        if (ui_app_get_mode() == UI_MODE_MANAGE) {
            Packet tmp;
            while (q_.try_pop(tmp)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        Packet p;
        if (!q_.pop(p)) break;
        if (stop_->load()) break;

        // drain：只保留最新
        Packet newer;
        while (q_.try_pop(newer)) p = std::move(newer);

        if (p.rgb_cam.empty() || p.rgb_det.empty()) continue;

        double det_ms, align_ms, arc_ms, db_ms;
        int arc_ran = 0;
        process_frame_(p, det_ms, align_ms, arc_ms, db_ms, arc_ran);

        fps_det_cnt_++;
        if (arc_ran) fps_arc_cnt_++;
        sum_det_ms_   += det_ms;
        sum_align_ms_ += align_ms;
        sum_arc_ms_   += arc_ms;
        sum_db_ms_    += db_ms;

        uint64_t now = now_ms_();
        uint64_t dt  = now - fps_last_ms_;
        if (dt >= (uint64_t)FPS_LOG_MS) {
            const double sec = (dt > 0) ? (double)dt / 1000.0 : 1.0;
            const double det_fps = (double)fps_det_cnt_ / sec;
            const double arc_fps = (double)fps_arc_cnt_ / sec;

            const double det_avg = (fps_det_cnt_ ? (sum_det_ms_ / (double)fps_det_cnt_) : 0.0);
            const double ali_avg = (fps_det_cnt_ ? (sum_align_ms_ / (double)fps_det_cnt_) : 0.0);
            const double arc_avg = (fps_det_cnt_ ? (sum_arc_ms_ / (double)fps_det_cnt_) : 0.0);
            const double db_avg  = (fps_det_cnt_ ? (sum_db_ms_  / (double)fps_det_cnt_) : 0.0);

            std::fprintf(stderr,
                         "[FPS][VW ] det=%.1f arc=%.1f drop=%u  [ms] det=%.2f align=%.2f arc=%.2f db=%.2f  (fixed arc_min=%d move=%d)\n",
                         det_fps, arc_fps, fps_drop_cnt_,
                         det_avg, ali_avg, arc_avg, db_avg,
                         ARC_MIN_MS, ARC_MOVE_PX);

            fps_last_ms_ = now;
            fps_det_cnt_ = fps_arc_cnt_ = fps_drop_cnt_ = 0;
            sum_det_ms_ = sum_align_ms_ = sum_arc_ms_ = sum_db_ms_ = 0.0;
        }
    }

    vwlog("thread exit");
}
