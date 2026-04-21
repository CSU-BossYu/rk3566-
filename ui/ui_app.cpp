#include "ui_app.h"

#include <lvgl.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "ui_model.h"
#include "ui_runtime.h"
#include "ui_navigator.h"
#include "ui_controller.h"

// generated entry (SquareLine-style)
#include "generated/ui.h"

#include "screens/screen_recog.h"
#include "screens/screen_enroll.h"
#include "screens/screen_manage.h"

#include "utils/thread_safe_queue.h"
#include "utils/fixed_block_pool.h"

static constexpr int UI_W  = 480;
static constexpr int CAM_H = 480;

struct UiAppCtx {
    UiModel     model;
    UiRuntime   rt;
    UiNavigator nav;

    ScreenRecog*   recog  = nullptr;
    ScreenEnroll*  enroll = nullptr;
    ScreenManage*  manage = nullptr;

    UiController*  ctl    = nullptr;

    // UI thread owned timer
    lv_timer_t* timer = nullptr;

    // overlay (UI thread owned)
    ui_face_box_t boxes[4]{};
    int boxes_n = 0;

    // ---------- helpers (UI thread only) ----------
    void apply_status_to_current(const char* s) {
        const ui_mode_t m = model.mode();
        if (m == UI_MODE_RECOG && recog)        screen_recog_set_status(recog, s);
        else if (m == UI_MODE_ENROLL && enroll) screen_enroll_set_status(enroll, s);
        else if (m == UI_MODE_MANAGE && manage) screen_manage_set_status(manage, s);
    }

    void apply_boxes_to_current() {
        const ui_mode_t m = model.mode();
        if (m == UI_MODE_RECOG && recog) {
            screen_recog_set_boxes(recog, boxes, boxes_n, UI_W, CAM_H);
        } else if (m == UI_MODE_ENROLL && enroll) {
            screen_enroll_set_boxes(enroll, boxes, boxes_n, UI_W, CAM_H);
        }
        // MANAGE 不显示框
    }

    void apply_frame_to_current(const UiFramePacket* f) {
        if (!f || !f->blk.ptr) return;
        if (f->w != UI_W || f->h != CAM_H) return;

        const ui_mode_t m = model.mode();
        if (m == UI_MODE_RECOG && recog) {
            screen_recog_set_image(recog, f->blk.ptr, f->w, f->h);
        } else if (m == UI_MODE_ENROLL && enroll) {
            screen_enroll_set_image(enroll, f->blk.ptr, f->w, f->h);
        }
    }

    // manage list click callback (UI thread)
    static void on_manage_item_clicked(lv_event_t* e) {
        auto* ctx = (ScreenManageListItemCtx*)lv_event_get_user_data(e);
        if (!ctx) return;

        UiAppCtx* self = (UiAppCtx*)ctx->app_ctx;
        if (!self) return;

        const int idx = ctx->idx;
        self->model.set_selected(idx);

        if (self->manage) {
            screen_manage_list_set_selected(self->manage, idx);
            screen_manage_set_delete_enabled(self->manage, idx >= 0);
        }
    }

    // UI thread: consume model pending and apply to views
    void apply_latest() {
        if (!ctl) return;

        // 1) consume pending status / list / overlay / enroll_done
        std::string status;
        if (model.consume_status(status)) {
            apply_status_to_current(status.c_str());
        }

        std::vector<std::string> names;
        if (model.consume_manage_list(names) &&
            model.mode() == UI_MODE_MANAGE &&
            manage) {

            // rebuild list
            screen_manage_list_rebuild(manage, names, on_manage_item_clicked, this);

            int sel = model.selected();
            if (sel < 0 || sel >= (int)names.size()) {
                model.clear_selected();
                sel = -1;
            }
            screen_manage_list_set_selected(manage, sel);
            screen_manage_set_delete_enabled(manage, sel >= 0);
        }

        ui_face_box_t tmp[4];
        int tmp_n = 0;
        if (model.consume_overlay(tmp, tmp_n)) {
            boxes_n = tmp_n;
            for (int i = 0; i < boxes_n; ++i) boxes[i] = tmp[i];
            apply_boxes_to_current();
        }

        if (model.consume_enroll_done()) {
            model.set_enroll_state(UiModel::ENROLL_IDLE);
            if (enroll && enroll->ta_name) lv_textarea_set_text(enroll->ta_name, "");
            if (enroll) screen_enroll_kb_hide(enroll);
        }

        // 2) controller apply (mode labels + enroll action text + delete enable)
        ctl->apply();

        // 3) drain frames according to mode
        const bool is_manage = (model.mode() == UI_MODE_MANAGE);
        rt.drain_latest(is_manage);

        const bool need_frame = (model.mode() == UI_MODE_RECOG || model.mode() == UI_MODE_ENROLL);
        if (need_frame) {
            const UiFramePacket* f = rt.current_frame();
            apply_frame_to_current(f);
            apply_boxes_to_current();
        }
    }
};

static UiAppCtx* g_app = nullptr;

static void timer_cb(lv_timer_t* t) {
    // LVGL version compatibility: some versions don't expose lv_timer_get_user_data().
    void* ud = nullptr;
#if defined(lv_timer_get_user_data)
    ud = lv_timer_get_user_data(t);
#else
    ud = t ? t->user_data : nullptr;
#endif
    auto* app = (UiAppCtx*)ud;
    if (!app) return;
    app->apply_latest();
}

void ui_app_init(void) {
    if (g_app) return;

    g_app = new UiAppCtx();

    // 1) generated UI init (SquareLine-style)
    ui_init();

    // 2) grab screen pointers (generated)
    g_app->recog  = ui_get_screen_recog();
    g_app->enroll = ui_get_screen_enroll();
    g_app->manage = ui_get_screen_manage();

    // 3) bind navigator (3 screens)
    g_app->nav.bind(g_app->recog, g_app->enroll, g_app->manage);

    // 4) initial mode + load
    g_app->model.set_mode(UI_MODE_RECOG);
    g_app->model.set_enroll_state(UiModel::ENROLL_IDLE);
    g_app->nav.set_mode(UI_MODE_RECOG);

    // 5) controller
    g_app->ctl = new UiController(&g_app->model, &g_app->nav,
                                  g_app->recog, g_app->enroll, g_app->manage);
    g_app->ctl->wire_events();
    g_app->ctl->apply();

    // 6) UI timer
    g_app->timer = lv_timer_create(timer_cb, 33, g_app);
}

void ui_app_tick(void) {
    // no-op (timer drives UI)
}

void ui_app_deinit(void) {
    if (!g_app) return;

    // stop timer first
    if (g_app->timer) {
        lv_timer_del(g_app->timer);
        g_app->timer = nullptr;
    }

    g_app->rt.deinit();

    if (g_app->ctl) {
        delete g_app->ctl;
        g_app->ctl = nullptr;
    }

    // generated UI deinit
    ui_deinit();

    delete g_app;
    g_app = nullptr;
}

void ui_app_bind_frame_pipe(ThreadSafeQueue<UiFramePacket, 8>* q, FixedBlockPool* pool) {
    if (!g_app) return;
    g_app->rt.bind_frame_pipe(q, pool);
}

void ui_app_set_status(const char* s) {
    if (!g_app) return;
    g_app->model.set_status_pending(s);
}

void ui_app_set_manage_list(const char** names, int n) {
    if (!g_app) return;

    std::vector<std::string> tmp;
    if (n < 0) n = 0;
    tmp.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        if (names && names[i]) tmp.emplace_back(names[i]);
    }
    g_app->model.set_manage_list_pending(tmp);
}

void ui_app_set_overlay_boxes(const ui_face_box_t* boxes, int n) {
    if (!g_app) return;

    if (!boxes || n <= 0) {
        ui_face_box_t dummy[1]{};
        g_app->model.set_overlay_pending(dummy, 0);
        return;
    }
    g_app->model.set_overlay_pending(boxes, n);
}

ui_mode_t ui_app_get_mode(void) {
    if (!g_app) return UI_MODE_RECOG;
    return g_app->model.mode();
}

void ui_app_set_mode(ui_mode_t m) {
    if (!g_app) return;

    g_app->model.set_mode(m);
    g_app->nav.set_mode(m);

    if (m != UI_MODE_ENROLL) {
        g_app->model.set_enroll_state(UiModel::ENROLL_IDLE);
    }
}

uint32_t ui_app_poll_actions(void) {
    if (!g_app) return 0;
    return g_app->model.poll_actions();
}

int ui_app_get_selected(void) {
    if (!g_app) return -1;
    return g_app->model.selected();
}

int ui_app_get_enroll_name(char* out, int cap) {
    if (!g_app) return 0;
    if (!out || cap <= 0) return 0;
    out[0] = '\0';

    std::string name;
    if (!g_app->model.get_enroll_name(name)) return 0;

    const int n = (int)std::min<size_t>((size_t)(cap - 1), name.size());
    std::memcpy(out, name.data(), (size_t)n);
    out[n] = '\0';
    return n;
}

void ui_app_notify_enroll_done(void) {
    if (!g_app) return;
    g_app->model.notify_enroll_done_pending();
}

void ui_app_clear_selected(void) {
    if (!g_app) return;
    g_app->model.clear_selected();
}