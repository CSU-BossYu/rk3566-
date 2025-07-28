#include "app_runtime.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/time.h>
#include <time.h>

#include <lvgl.h>
#include "lv_drivers/display/drm.h"
#include "lv_drivers/indev/evdev.h"

#include "ui/ui_app.h"
#include "ui/ui_frame_packet.h"

#include "utils/fixed_block_pool.h"
#include "utils/thread_safe_queue.h"

#include "rga/rga_worker.h"
#include "app/camera_service.h"
#include "app/vision_worker.h"

static inline uint64_t now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static inline void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

static inline bool env_is_1(const char* key)
{
    const char* v = std::getenv(key);
    if(!v) return false;
    return (std::strcmp(v, "1") == 0) || (std::strcmp(v, "true") == 0) || (std::strcmp(v, "TRUE") == 0);
}

static inline const char* env_str(const char* key)
{
    const char* v = std::getenv(key);
    return (v && v[0]) ? v : nullptr;
}

static void print_banner(const char* db_path)
{
    std::printf("============================================================\n");
    std::printf("demo_rga_v4l2 (LVGL + DRM)  UI + CAM + VISION\n");
    std::printf("Route:\n");
    std::printf("  Camera(dma_fd,YUYV) -> RgaWorker -> UI(BGRA) -> LVGL\n");
    std::printf("                           |-> Vision(rgb_cam + rgb_det)\n");
    std::printf("DB:\n");
    std::printf("  %s\n", (db_path && db_path[0]) ? db_path : "(null)");
    std::printf("============================================================\n");
    std::fflush(stdout);
}

struct AppRuntime::Impl {
    Config cfg{};

    std::atomic<bool> stop{false};

    bool drm_inited = false;
    bool lv_inited  = false;

    FixedBlockPool* ui_pool = nullptr;
    ThreadSafeQueue<UiFramePacket, 8>* ui_q = nullptr;

    VisionWorker* vision = nullptr;
    RgaWorker* rga = nullptr;
    CameraService* cam = nullptr;

    uint64_t last_tick = 0;

    void cleanup()
    {
        stop.store(true);

        // 先让 worker 关闭队列/停止工作
        if (vision) vision->stop();
        if (rga)    rga->stop();

        // join：生产者先停
        if (cam)    cam->join();
        if (rga)    rga->join();
        if (vision) vision->join();

        // UI 释放当前持有的帧 block（否则 pool 少一块）
        ui_app_deinit();

        // 解绑 pipe，避免悬挂引用
        ui_app_bind_frame_pipe(nullptr, nullptr);

        delete cam; cam = nullptr;
        delete rga; rga = nullptr;
        delete vision; vision = nullptr;

        delete ui_q; ui_q = nullptr;
        delete ui_pool; ui_pool = nullptr;

        if (drm_inited) {
            drm_exit();
            drm_inited = false;
        }
    }
};

AppRuntime::AppRuntime() { p_ = new Impl(); }

AppRuntime::~AppRuntime()
{
    if (!p_) return;
    p_->cleanup();
    delete p_;
    p_ = nullptr;
}

void AppRuntime::requestStop()
{
    if (p_) p_->stop.store(true);
}

bool AppRuntime::init(const Config& cfg)
{
    if (!p_) return false;
    p_->cfg = cfg;

    // ✅环境变量兜底（仅当 cfg.db_path 为空时生效）
    if (p_->cfg.db_path.empty()) {
        const char* e = env_str("FACE_DB");
        if (e) p_->cfg.db_path = e;
    }

    print_banner(p_->cfg.db_path.c_str());

    std::printf("[APP] lv_init...\n");
    lv_init();
    p_->lv_inited = true;

    // --- DRM display ---
    std::printf("[APP] drm_disp_drv_init...\n");
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    if (drm_disp_drv_init(&disp_drv) != 0) {
        std::printf("[APP][E] drm_disp_drv_init failed\n");
        return false;
    }

    // 你之前用它规避局刷背景问题：保留
    disp_drv.full_refresh = 1;

    lv_disp_drv_register(&disp_drv);
    p_->drm_inited = true;

    // --- EVDEV touch ---
    std::printf("[APP] evdev_init...\n");
    evdev_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);

    // --- UI app ---
    std::printf("[APP] ui_app_init...\n");
    ui_app_init();

    // --- UI pipe: pool + queue ---
    const uint32_t blk_bytes = (uint32_t)(p_->cfg.ui_out_w * p_->cfg.ui_out_h * 4);
    p_->ui_pool = new FixedBlockPool(blk_bytes,
                                    (uint32_t)p_->cfg.pool_blocks,
                                    (uint32_t)p_->cfg.pool_align);
    p_->ui_q = new ThreadSafeQueue<UiFramePacket, 8>();
    ui_app_bind_frame_pipe(p_->ui_q, p_->ui_pool);

    // --- Vision worker ---
    VisionWorker::Config vcfg;
    vcfg.cam_w = p_->cfg.cam_w;
    vcfg.cam_h = p_->cfg.cam_h;
    vcfg.det_w = 320;
    vcfg.det_h = 320;
    vcfg.ui_w  = p_->cfg.ui_out_w;
    vcfg.ui_h  = p_->cfg.ui_out_h;

    // ✅DB path 注入
    vcfg.db_path = p_->cfg.db_path;

    p_->vision = new VisionWorker(&p_->stop, vcfg);
    if (!p_->vision->start()) {
        ui_app_set_status("VISION start fail");
        return false;
    }

    // --- RGA worker ---
    RgaWorker::Config rga_cfg;
    rga_cfg.ui_out_w  = p_->cfg.ui_out_w;
    rga_cfg.ui_out_h  = p_->cfg.ui_out_h;
    rga_cfg.det_out_w = 320;
    rga_cfg.det_out_h = 320;

    p_->rga = new RgaWorker(p_->ui_q, p_->ui_pool, &p_->stop, rga_cfg, p_->vision);
    if (!p_->rga->start()) {
        ui_app_set_status("RGA init fail");
        return false;
    }

    // --- Camera service ---
    CameraService::Config cam_cfg;
    cam_cfg.dev = p_->cfg.cam_dev;
    cam_cfg.w = p_->cfg.cam_w;
    cam_cfg.h = p_->cfg.cam_h;
    cam_cfg.timeout_ms = p_->cfg.cam_timeout_ms;

    p_->cam = new CameraService(p_->rga, &p_->stop, cam_cfg);
    if (!p_->cam->start()) {
        ui_app_set_status("CAM init fail");
        p_->stop.store(true);
        return false;
    }

    p_->last_tick = now_ms();
    return true;
}

int AppRuntime::run()
{
    if (!p_) return -1;

    const bool no_handler = env_is_1("LV_NO_HANDLER");

    while (!p_->stop.load()) {
        uint64_t now = now_ms();
        uint32_t elapse = (uint32_t)(now - p_->last_tick);
        if (elapse > 0) {
            lv_tick_inc(elapse);
            p_->last_tick = now;
        }

        // UI 合并渲染固定驱动
        ui_app_tick();

        if (!no_handler) {
            lv_timer_handler();
        }

        sleep_ms(8);
    }

    return 0;
}
