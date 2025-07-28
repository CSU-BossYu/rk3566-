#include "ui_app.h"
#include <lvgl.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cstdint>

#include "utils/thread_safe_queue.h"
#include "utils/fixed_block_pool.h"
#include "ui/ui_frame_packet.h"
#include "ui/ui_types.h"

static const int UI_W = 480;
static const int UI_H = 800;

static const int H_TITLE = 50;
static const int H_CAM   = 480;
static const int H_INFO  = 170;
static const int H_BTN   = 100;

static lv_obj_t* g_title = nullptr;
static lv_obj_t* g_cam_area = nullptr;
static lv_obj_t* g_info = nullptr;
static lv_obj_t* g_btnbar = nullptr;

static lv_obj_t* g_title_label = nullptr;

static lv_obj_t* g_img = nullptr;
static lv_img_dsc_t g_img_dsc;

static lv_obj_t* g_box_obj[4] = {nullptr,nullptr,nullptr,nullptr};

static lv_obj_t* g_status_label = nullptr;

static lv_obj_t* g_btn_left  = nullptr;
static lv_obj_t* g_btn_right = nullptr;
static lv_obj_t* g_mode_label = nullptr;

static lv_obj_t* g_btn_action = nullptr;
static lv_obj_t* g_btn_action_label = nullptr;

static lv_obj_t* g_list = nullptr;

/* Enroll 输入控件 */
static lv_obj_t* g_ta_name = nullptr;
static lv_obj_t* g_kb = nullptr;

/* Manage 删除确认框 */
static lv_obj_t* g_mbox = nullptr;
static std::atomic<bool> g_mbox_closing{false};
static std::atomic<int>  g_mbox_ok_pending{0}; // 1=OK, 0=Cancel/none

static std::mutex g_mtx;
static std::string g_status_pending;
static bool g_has_status_pending = false;

static std::vector<std::string> g_manage_names_pending;
static bool g_has_manage_pending = false;

/* 保存 list button 指针用于高亮 */
static std::vector<lv_obj_t*> g_list_btns;

/* overlay boxes */
static ui_face_box_t g_overlay_boxes[4];
static int g_overlay_n = 0;
static bool g_has_overlay_pending = false;
static ui_face_box_t g_overlay_pending[4];
static int g_overlay_pending_n = 0;

/* Enroll name（跨线程读取） */
static std::string g_enroll_name;
static bool g_has_enroll_name = false;

static ThreadSafeQueue<UiFramePacket, 8>* g_frame_q = nullptr;
static FixedBlockPool* g_frame_pool = nullptr;

static UiFramePacket g_cur_frame{};
static bool g_has_cur_frame = false;

static std::atomic<ui_mode_t> g_mode{UI_MODE_RECOG};
static std::atomic<uint32_t>  g_actions{0};
static std::atomic<int>       g_selected{-1};

/* Enroll 状态机 */
enum { ENROLL_IDLE = 0, ENROLL_WAIT_FACE = 1 };
static std::atomic<int> g_enroll_state{ENROLL_IDLE};

static bool g_has_enroll_done_pending = false;

static const uint32_t ACT_START_ENROLL  = UI_ACT_START_ENROLL;
static const uint32_t ACT_DELETE        = UI_ACT_DELETE;
static const uint32_t ACT_CANCEL_ENROLL = UI_ACT_CANCEL_ENROLL;

static const char* mode_text(ui_mode_t m) {
    switch(m) {
    case UI_MODE_RECOG:  return "MODE: RECOG";
    case UI_MODE_ENROLL: return "MODE: ENROLL";
    case UI_MODE_MANAGE: return "MODE: MANAGE";
    default: return "MODE: ?";
    }
}

static void set_visible(lv_obj_t* obj, bool vis) {
    if(!obj) return;
    if(vis) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_manage_delete_enable()
{
    if(!g_btn_action) return;
    ui_mode_t m = g_mode.load();
    if(m != UI_MODE_MANAGE) return;

    const int sel = g_selected.load();
    if(sel >= 0) lv_obj_clear_state(g_btn_action, LV_STATE_DISABLED);
    else         lv_obj_add_state(g_btn_action, LV_STATE_DISABLED);
}

static void update_manage_list_highlight()
{
    const int sel = g_selected.load();
    for(size_t i = 0; i < g_list_btns.size(); ++i) {
        lv_obj_t* b = g_list_btns[i];
        if(!b) continue;
        if((int)i == sel) lv_obj_add_state(b, LV_STATE_CHECKED);
        else              lv_obj_clear_state(b, LV_STATE_CHECKED);
    }
}

/* ---------- Msgbox: 稳定关闭与防连点 ---------- */

static void on_mbox_deleted(lv_event_t* e)
{
    if(lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_obj_t* obj = lv_event_get_target(e);
    if(obj == g_mbox) g_mbox = nullptr;
    g_mbox_closing.store(false);
    g_mbox_ok_pending.store(0);
}

static void mbox_async_close(void* p)
{
    lv_obj_t* mbox = (lv_obj_t*)p;
    if(!mbox) return;

    // 发 action（事件栈之外）
    if (g_mbox_ok_pending.exchange(0) == 1) {
        g_actions.fetch_or(ACT_DELETE);
    }

    // 只做 async delete，避免 lv_obj_del 在 timer/async 上下文触发 invalidate 链崩溃
    if (lv_obj_is_valid(mbox)) {
        lv_obj_del_async(mbox);
    }
}

static void on_mbox_btnmatrix(lv_event_t* e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t* btnm = lv_event_get_target(e);
    lv_obj_t* mbox = (lv_obj_t*)lv_event_get_user_data(e);
    if(!btnm || !mbox) return;

    // ✅可靠取 id：直接从 btnmatrix 读选中项
    uint16_t id = lv_btnmatrix_get_selected_btn(btnm);
    // btns={"Cancel","OK",""} => OK 的 index=1
    g_mbox_ok_pending.store(id == 1 ? 1 : 0);

    // ✅防连点：只允许调度一次关闭
    bool expected = false;
    if (!g_mbox_closing.compare_exchange_strong(expected, true)) {
        return;
    }

    lv_async_call(mbox_async_close, mbox);
}

/* ---------- Buttons / List ---------- */

static void on_btn_left(lv_event_t* e)
{
    (void)e;
    ui_mode_t m = g_mode.load();
    ui_mode_t nm = (m == UI_MODE_RECOG) ? UI_MODE_MANAGE : (ui_mode_t)(m - 1);
    g_mode.store(nm);
}

static void on_btn_right(lv_event_t* e)
{
    (void)e;
    ui_mode_t m = g_mode.load();
    ui_mode_t nm = (m == UI_MODE_MANAGE) ? UI_MODE_RECOG : (ui_mode_t)(m + 1);
    g_mode.store(nm);
}

static void show_keyboard_for(lv_obj_t* ta)
{
    if(!g_kb) return;
    lv_keyboard_set_textarea(g_kb, ta);
    set_visible(g_kb, true);

    if(g_btnbar) lv_obj_align_to(g_kb, g_btnbar, LV_ALIGN_OUT_TOP_MID, 0, 0);
    else         lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_move_foreground(g_kb);
}

static void hide_keyboard()
{
    if(!g_kb) return;
    set_visible(g_kb, false);
    lv_keyboard_set_textarea(g_kb, nullptr);
}

static void on_ta_name(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(!g_kb || !g_ta_name) return;

    if(code == LV_EVENT_FOCUSED) {
        show_keyboard_for(g_ta_name);
    } else if(code == LV_EVENT_DEFOCUSED) {
        hide_keyboard();
    } else if(code == LV_EVENT_READY) {
        hide_keyboard();
    }
}

static void on_list_item(lv_event_t* e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    g_selected.store((int)idx);

    update_manage_list_highlight();
    update_manage_delete_enable();
}

static void on_btn_action(lv_event_t* e)
{
    (void)e;
    ui_mode_t m = g_mode.load();

    if(m == UI_MODE_ENROLL) {
        int st = g_enroll_state.load();
        if(st == ENROLL_IDLE) {
            const char* txt = (g_ta_name) ? lv_textarea_get_text(g_ta_name) : nullptr;
            if(!txt) txt = "";

            std::string name = txt;
            auto ltrim = [](std::string& s){
                size_t i=0; while(i<s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
                if(i>0) s.erase(0,i);
            };
            auto rtrim = [](std::string& s){
                while(!s.empty()){
                    char c=s.back();
                    if(c==' '||c=='\t'||c=='\n'||c=='\r') s.pop_back();
                    else break;
                }
            };
            ltrim(name); rtrim(name);

            if(name.empty()) { ui_app_set_status("ENROLL: name empty"); return; }

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_enroll_name = name;
                g_has_enroll_name = true;
            }

            g_actions.fetch_or(ACT_START_ENROLL);
            g_enroll_state.store(ENROLL_WAIT_FACE);
            ui_app_set_status("ENROLL: waiting face");
            hide_keyboard();
            return;
        } else {
            g_actions.fetch_or(ACT_CANCEL_ENROLL);
            g_enroll_state.store(ENROLL_IDLE);
            ui_app_set_status("ENROLL: cancelled");
            hide_keyboard();
            return;
        }
    }

    if(m == UI_MODE_MANAGE) {
        const int sel = g_selected.load();
        if(sel < 0) { ui_app_set_status("DELETE: select one"); return; }

        // ✅防连点：mbox 存在或正在关闭时，不允许再创建
        if(g_mbox || g_mbox_closing.load()) return;

        static const char* btns[] = {"Cancel", "OK", ""};
        lv_obj_t* parent = lv_layer_top();
        g_mbox = lv_msgbox_create(parent, "Confirm", "Delete selected person?", btns, true);
        lv_obj_center(g_mbox);
        lv_obj_move_foreground(g_mbox);

        // 删除事件：真正删除后清理状态
        lv_obj_add_event_cb(g_mbox, on_mbox_deleted, LV_EVENT_DELETE, nullptr);

        lv_obj_t* btnm = lv_msgbox_get_btns(g_mbox);
        if(btnm) {
            lv_obj_add_event_cb(btnm, on_mbox_btnmatrix, LV_EVENT_VALUE_CHANGED, g_mbox);
        } else {
            // 理论不应该发生，兜底关闭
            g_mbox_ok_pending.store(0);
            g_mbox_closing.store(true);
            lv_async_call(mbox_async_close, g_mbox);
        }
        return;
    }
}

/* ---------- Frame drain & apply ---------- */

static void ui_drain_latest_frame(ui_mode_t m)
{
    if (!g_frame_q || !g_frame_pool) return;

    UiFramePacket pkt{};
    bool got = false;
    UiFramePacket last{};

    while (g_frame_q->try_pop(pkt)) {
        if (got && last.blk.ptr) g_frame_pool->release(last.blk.idx);
        last = pkt;
        got = true;
    }

    if (m == UI_MODE_MANAGE) {
        if (got && last.blk.ptr) g_frame_pool->release(last.blk.idx);

        if (g_has_cur_frame && g_cur_frame.blk.ptr) g_frame_pool->release(g_cur_frame.blk.idx);
        g_has_cur_frame = false;
        g_cur_frame = UiFramePacket{};
        return;
    }

    if (!got) return;

    if (g_has_cur_frame && g_cur_frame.blk.ptr) g_frame_pool->release(g_cur_frame.blk.idx);
    g_cur_frame = last;
    g_has_cur_frame = true;

    if (!g_img || !g_cur_frame.blk.ptr) return;

    const int w = UI_W, h = H_CAM;
    if (g_cur_frame.w != w || g_cur_frame.h != h) return;

    std::memset(&g_img_dsc, 0, sizeof(g_img_dsc));
    g_img_dsc.header.w = w;
    g_img_dsc.header.h = h;
    g_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_img_dsc.data_size = (uint32_t)(w * h * 4);
    g_img_dsc.data = (const uint8_t*)g_cur_frame.blk.ptr;

    lv_img_set_src(g_img, &g_img_dsc);

    int n = g_overlay_n;
    for(int i=0;i<4;i++){
        if(!g_box_obj[i]) continue;
        if(i < n) {
            int x1 = std::max(0, std::min(g_overlay_boxes[i].x1, w-1));
            int y1 = std::max(0, std::min(g_overlay_boxes[i].y1, h-1));
            int x2 = std::max(0, std::min(g_overlay_boxes[i].x2, w-1));
            int y2 = std::max(0, std::min(g_overlay_boxes[i].y2, h-1));
            if(x2 <= x1) x2 = std::min(w-1, x1+1);
            if(y2 <= y1) y2 = std::min(h-1, y1+1);

            lv_obj_set_pos(g_box_obj[i], x1, y1);
            lv_obj_set_size(g_box_obj[i], x2-x1, y2-y1);
            lv_obj_clear_flag(g_box_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_box_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_apply_latest()
{
    static ui_mode_t prev_mode = UI_MODE_RECOG;

    ui_mode_t m = g_mode.load();
    if(g_mode_label) lv_label_set_text(g_mode_label, mode_text(m));

    if(m != prev_mode) {
        if(g_status_label) lv_label_set_text(g_status_label, "DET: none\nFEAT: none");
        prev_mode = m;

        for(int i=0;i<4;i++){
            if(g_box_obj[i]) lv_obj_add_flag(g_box_obj[i], LV_OBJ_FLAG_HIDDEN);
        }

        hide_keyboard();
        if(m != UI_MODE_ENROLL) g_enroll_state.store(ENROLL_IDLE);

        // 切换模式时如果 msgbox 存在：不发 delete，只安全关掉
        if(g_mbox && !g_mbox_closing.load()) {
            g_mbox_ok_pending.store(0);
            g_mbox_closing.store(true);
            lv_async_call(mbox_async_close, g_mbox);
        }

        if (m == UI_MODE_MANAGE) {
            if (g_has_cur_frame && g_frame_pool && g_cur_frame.blk.ptr) {
                g_frame_pool->release(g_cur_frame.blk.idx);
            }
            g_has_cur_frame = false;
            g_cur_frame = UiFramePacket{};
        }
    }

    set_visible(g_img, (m != UI_MODE_MANAGE));
    set_visible(g_list, (m == UI_MODE_MANAGE));

    set_visible(g_ta_name, (m == UI_MODE_ENROLL));
    if(m != UI_MODE_ENROLL) hide_keyboard();

    if(g_btn_action_label) {
        if(m == UI_MODE_ENROLL) {
            int st = g_enroll_state.load();
            lv_label_set_text(g_btn_action_label, (st == ENROLL_WAIT_FACE) ? "Cancel" : "Start");
            set_visible(g_btn_action, true);
            lv_obj_clear_state(g_btn_action, LV_STATE_DISABLED);
        } else if(m == UI_MODE_MANAGE) {
            lv_label_set_text(g_btn_action_label, "Delete");
            set_visible(g_btn_action, true);
            update_manage_delete_enable();
        } else {
            set_visible(g_btn_action, false);
        }
    }

    std::string status_local;
    bool do_status=false;

    std::vector<std::string> names_local;
    bool do_manage=false;

    ui_face_box_t boxes_local[4];
    int boxes_n_local = 0;
    bool do_boxes=false;

    bool do_enroll_done = false;

    {
        std::lock_guard<std::mutex> lk(g_mtx);

        if(g_has_status_pending) {
            status_local = g_status_pending;
            g_has_status_pending = false;
            do_status = true;
        }

        if(g_has_manage_pending) {
            names_local = g_manage_names_pending;
            g_has_manage_pending = false;
            do_manage = true;
        }

        if(g_has_overlay_pending) {
            boxes_n_local = g_overlay_pending_n;
            for(int i=0;i<boxes_n_local;i++) boxes_local[i] = g_overlay_pending[i];
            g_has_overlay_pending = false;
            do_boxes = true;
        }

        if(g_has_enroll_done_pending) {
            g_has_enroll_done_pending = false;
            do_enroll_done = true;
        }
    }

    if(do_status && g_status_label) {
        lv_label_set_text(g_status_label, status_local.c_str());
    }

    if(do_manage && g_list && m == UI_MODE_MANAGE) {
        lv_obj_clean(g_list);
        g_list_btns.clear();
        g_list_btns.reserve(names_local.size());

        for(size_t i=0;i<names_local.size();i++) {
            lv_obj_t* btn = lv_list_add_btn(g_list, NULL, names_local[i].c_str());

            lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);

            // ✅给 CHECKED 态配高亮样式
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
            lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);

            lv_obj_add_event_cb(btn, on_list_item, LV_EVENT_CLICKED, (void*)(intptr_t)i);

            g_list_btns.push_back(btn);
        }

        int sel = g_selected.load();
        if(sel < 0 || sel >= (int)names_local.size()) g_selected.store(-1);

        update_manage_list_highlight();
        update_manage_delete_enable();
    }

    if(do_boxes) {
        g_overlay_n = boxes_n_local;
        for(int i=0;i<boxes_n_local;i++) g_overlay_boxes[i] = boxes_local[i];
    }

    if(do_enroll_done) {
        g_enroll_state.store(ENROLL_IDLE);
        if(g_ta_name) lv_textarea_set_text(g_ta_name, "");
        hide_keyboard();
    }

    ui_drain_latest_frame(m);
}

static void timer_cb(lv_timer_t*) { ui_apply_latest(); }

/* ---------- public API ---------- */

void ui_app_init(void)
{
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    g_title = lv_obj_create(scr);
    lv_obj_set_size(g_title, UI_W, H_TITLE);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(g_title, 0, 0);
    lv_obj_set_style_border_width(g_title, 0, 0);

    g_title_label = lv_label_create(g_title);
    lv_label_set_text(g_title_label, "DEMO");
    lv_obj_center(g_title_label);

    g_cam_area = lv_obj_create(scr);
    lv_obj_set_size(g_cam_area, UI_W, H_CAM);
    lv_obj_align_to(g_cam_area, g_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(g_cam_area, 0, 0);
    lv_obj_set_style_border_width(g_cam_area, 0, 0);

    g_img = lv_img_create(g_cam_area);
    lv_obj_set_size(g_img, UI_W, H_CAM);
    lv_obj_align(g_img, LV_ALIGN_TOP_LEFT, 0, 0);

    for(int i=0;i<4;i++){
        g_box_obj[i] = lv_obj_create(g_cam_area);
        lv_obj_set_style_bg_opa(g_box_obj[i], LV_OPA_0, 0);
        lv_obj_set_style_border_width(g_box_obj[i], 3, 0);
        lv_obj_set_style_border_color(g_box_obj[i], lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_add_flag(g_box_obj[i], LV_OBJ_FLAG_HIDDEN);
    }

    g_list = lv_list_create(g_cam_area);
    lv_obj_set_size(g_list, UI_W, H_CAM);
    lv_obj_align(g_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);

    g_info = lv_obj_create(scr);
    lv_obj_set_size(g_info, UI_W, H_INFO);
    lv_obj_align_to(g_info, g_cam_area, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(g_info, 8, 0);

    g_status_label = lv_label_create(g_info);
    lv_label_set_text(g_status_label, "DET: none\nFEAT: none");
    lv_obj_align(g_status_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status_label, UI_W - 16);

    g_ta_name = lv_textarea_create(g_info);
    lv_obj_set_size(g_ta_name, UI_W - 16 - 170, 44);
    lv_obj_align(g_ta_name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_textarea_set_one_line(g_ta_name, true);
    lv_textarea_set_placeholder_text(g_ta_name, "Name...");
    lv_obj_add_event_cb(g_ta_name, on_ta_name, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(g_ta_name, LV_OBJ_FLAG_HIDDEN);

    g_btn_action = lv_btn_create(g_info);
    lv_obj_set_size(g_btn_action, 160, 44);
    lv_obj_align(g_btn_action, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(g_btn_action, on_btn_action, LV_EVENT_CLICKED, NULL);

    g_btn_action_label = lv_label_create(g_btn_action);
    lv_label_set_text(g_btn_action_label, "ACTION");
    lv_obj_center(g_btn_action_label);
    lv_obj_add_flag(g_btn_action, LV_OBJ_FLAG_HIDDEN);

    g_btnbar = lv_obj_create(scr);
    lv_obj_set_size(g_btnbar, UI_W, H_BTN);
    lv_obj_align_to(g_btnbar, g_info, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(g_btnbar, 10, 0);

    g_btn_left = lv_btn_create(g_btnbar);
    lv_obj_set_size(g_btn_left, 80, 60);
    lv_obj_align(g_btn_left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(g_btn_left, on_btn_left, LV_EVENT_CLICKED, NULL);
    lv_obj_t* labL = lv_label_create(g_btn_left);
    lv_label_set_text(labL, "<");
    lv_obj_center(labL);

    g_btn_right = lv_btn_create(g_btnbar);
    lv_obj_set_size(g_btn_right, 80, 60);
    lv_obj_align(g_btn_right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(g_btn_right, on_btn_right, LV_EVENT_CLICKED, NULL);
    lv_obj_t* labR = lv_label_create(g_btn_right);
    lv_label_set_text(labR, ">");
    lv_obj_center(labR);

    g_mode_label = lv_label_create(g_btnbar);
    lv_label_set_text(g_mode_label, mode_text(g_mode.load()));
    lv_obj_center(g_mode_label);

    g_kb = lv_keyboard_create(scr);
    lv_obj_set_size(g_kb, UI_W, 260);
    lv_obj_align_to(g_kb, g_btnbar, LV_ALIGN_OUT_TOP_MID, 0, 0);
    set_visible(g_kb, false);

    lv_timer_create(timer_cb, 33, nullptr);
}

void ui_app_tick(void) {}

void ui_app_bind_frame_pipe(ThreadSafeQueue<UiFramePacket, 8>* q, FixedBlockPool* pool)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_frame_q = q;
    g_frame_pool = pool;
}

void ui_app_set_status(const char* s)
{
    if(!s) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_status_pending = s;
    g_has_status_pending = true;
}

ui_mode_t ui_app_get_mode(void) { return g_mode.load(); }

void ui_app_set_mode(ui_mode_t m)
{
    g_mode.store(m);
    if(m != UI_MODE_ENROLL) {
        g_enroll_state.store(ENROLL_IDLE);
        if(g_ta_name) lv_textarea_set_text(g_ta_name, "");
        hide_keyboard();
    }
}

uint32_t ui_app_poll_actions(void) { return g_actions.exchange(0); }
int ui_app_get_selected(void) { return g_selected.load(); }

void ui_app_set_manage_list(const char** names, int n)
{
    std::vector<std::string> tmp;
    tmp.reserve((size_t)std::max(0, n));
    for(int i=0;i<n;i++){
        if(names && names[i]) tmp.emplace_back(names[i]);
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_manage_names_pending.swap(tmp);
    g_has_manage_pending = true;
}

void ui_app_set_overlay_boxes(const ui_face_box_t* boxes, int n)
{
    if(n < 0) n = 0;
    if(n > 4) n = 4;

    std::lock_guard<std::mutex> lk(g_mtx);
    g_overlay_pending_n = n;
    for(int i=0;i<n;i++) g_overlay_pending[i] = boxes[i];
    g_has_overlay_pending = true;
}

int ui_app_get_enroll_name(char* out, int cap)
{
    if(!out || cap <= 0) return 0;
    out[0] = '\0';

    std::string local;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if(!g_has_enroll_name) return 0;
        local = g_enroll_name;
    }

    auto ltrim = [](std::string& s){
        size_t i=0; while(i<s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
        if(i>0) s.erase(0,i);
    };
    auto rtrim = [](std::string& s){
        while(!s.empty()){
            char c=s.back();
            if(c==' '||c=='\t'||c=='\n'||c=='\r') s.pop_back();
            else break;
        }
    };
    ltrim(local); rtrim(local);
    if(local.empty()) return 0;

    int n = (int)std::min<size_t>((size_t)(cap - 1), local.size());
    std::memcpy(out, local.data(), (size_t)n);
    out[n] = '\0';
    return n;
}

void ui_app_notify_enroll_done(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_has_enroll_done_pending = true;
}

void ui_app_clear_selected(void) { g_selected.store(-1); }

void ui_app_deinit(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);

    if (g_has_cur_frame && g_frame_pool && g_cur_frame.blk.ptr) {
        g_frame_pool->release(g_cur_frame.blk.idx);
    }
    g_has_cur_frame = false;
    g_cur_frame = UiFramePacket{};

    g_frame_q = nullptr;
    g_frame_pool = nullptr;

    g_has_status_pending = false;
    g_status_pending.clear();
    g_has_manage_pending = false;
    g_manage_names_pending.clear();

    g_has_overlay_pending = false;
    g_overlay_pending_n = 0;
    g_overlay_n = 0;

    g_has_enroll_name = false;
    g_enroll_name.clear();

    g_has_enroll_done_pending = false;
    g_enroll_state.store(ENROLL_IDLE);

    if(g_mbox && !g_mbox_closing.load()) {
        g_mbox_ok_pending.store(0);
        g_mbox_closing.store(true);
        lv_async_call(mbox_async_close, g_mbox);
    }

    hide_keyboard();
}
