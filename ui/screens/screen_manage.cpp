#include "screen_manage.h"
#include <algorithm>

static constexpr int UI_W = 480;
static constexpr int UI_H = 800;

static constexpr int H_INFO = 220;
static constexpr int H_BTN  = 80;

ScreenManage* screen_manage_create() {
    auto* v = new ScreenManage();
    v->root = lv_obj_create(NULL);
    lv_obj_set_size(v->root, UI_W, UI_H);
    lv_obj_set_style_pad_all(v->root, 0, 0);
    lv_obj_set_style_border_width(v->root, 0, 0);

    // list area
    v->list = lv_list_create(v->root);
    lv_obj_set_size(v->list, UI_W, UI_H - H_INFO - H_BTN);
    lv_obj_align(v->list, LV_ALIGN_TOP_MID, 0, 0);

    // info/status + delete btn
    lv_obj_t* info = lv_obj_create(v->root);
    lv_obj_set_size(info, UI_W, H_INFO);
    lv_obj_align_to(info, v->list, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(info, 8, 0);

    v->status_label = lv_label_create(info);
    lv_label_set_text(v->status_label, "MANAGE");
    lv_label_set_long_mode(v->status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v->status_label, UI_W - 16);
    lv_obj_align(v->status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    v->btn_action = lv_btn_create(info);
    lv_obj_set_size(v->btn_action, 160, 44);
    lv_obj_align(v->btn_action, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    v->btn_action_label = lv_label_create(v->btn_action);
    lv_label_set_text(v->btn_action_label, "Delete");
    lv_obj_center(v->btn_action_label);
    lv_obj_add_state(v->btn_action, LV_STATE_DISABLED);

    // bottom nav
    lv_obj_t* bar = lv_obj_create(v->root);
    lv_obj_set_size(bar, UI_W, H_BTN);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 10, 0);

    v->btn_left = lv_btn_create(bar);
    lv_obj_set_size(v->btn_left, 80, 60);
    lv_obj_align(v->btn_left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* labL = lv_label_create(v->btn_left);
    lv_label_set_text(labL, "<");
    lv_obj_center(labL);

    v->btn_right = lv_btn_create(bar);
    lv_obj_set_size(v->btn_right, 80, 60);
    lv_obj_align(v->btn_right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t* labR = lv_label_create(v->btn_right);
    lv_label_set_text(labR, ">");
    lv_obj_center(labR);

    v->mode_label = lv_label_create(bar);
    lv_label_set_text(v->mode_label, "MODE: MANAGE");
    lv_obj_center(v->mode_label);

    return v;
}

void screen_manage_destroy(ScreenManage* v) {
    if (!v) return;
    if (v->root && lv_obj_is_valid(v->root)) lv_obj_del(v->root);
    delete v;
}

void screen_manage_set_status(ScreenManage* v, const char* s) {
    if (!v || !v->status_label || !s) return;
    lv_label_set_text(v->status_label, s);
}

void screen_manage_set_mode_text(ScreenManage* v, const char* s) {
    if (!v || !v->mode_label || !s) return;
    lv_label_set_text(v->mode_label, s);
}

void screen_manage_list_rebuild(ScreenManage* v,
                                const std::vector<std::string>& names,
                                lv_event_cb_t on_item_clicked,
                                void* app_ctx) {
    if (!v || !v->list) return;

    lv_obj_clean(v->list);
    v->list_btns.clear();
    v->list_ctx.clear();

    v->list_btns.reserve(names.size());
    v->list_ctx.resize(names.size());

    for (size_t i = 0; i < names.size(); ++i) {
        v->list_ctx[i].idx = (int)i;
        v->list_ctx[i].app_ctx = app_ctx;

        lv_obj_t* btn = lv_list_add_btn(v->list, NULL, names[i].c_str());
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);

        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);

        lv_obj_add_event_cb(btn, on_item_clicked, LV_EVENT_CLICKED, &v->list_ctx[i]);
        v->list_btns.push_back(btn);
    }
}

void screen_manage_list_set_selected(ScreenManage* v, int idx) {
    if (!v) return;
    for (size_t i = 0; i < v->list_btns.size(); ++i) {
        lv_obj_t* b = v->list_btns[i];
        if (!b) continue;
        if ((int)i == idx) lv_obj_add_state(b, LV_STATE_CHECKED);
        else               lv_obj_clear_state(b, LV_STATE_CHECKED);
    }
}

void screen_manage_set_delete_enabled(ScreenManage* v, bool en) {
    if (!v || !v->btn_action) return;
    if (en) lv_obj_clear_state(v->btn_action, LV_STATE_DISABLED);
    else    lv_obj_add_state(v->btn_action, LV_STATE_DISABLED);
}
