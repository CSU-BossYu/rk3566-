#include "screen_enroll.h"
#include <cstring>
#include <algorithm>

static constexpr int UI_W = 480;
static constexpr int CAM_H = 480;
static constexpr int UI_H = 800;

static constexpr int H_TITLE = 20;
static constexpr int H_INFO  = 220;
static constexpr int H_BTN   = 80;

static void set_visible(lv_obj_t* obj, bool vis) {
    if (!obj) return;
    if (vis) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_box_style(lv_obj_t* o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_0, 0);
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_set_style_border_color(o, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

ScreenEnroll* screen_enroll_create() {
    auto* v = new ScreenEnroll();
    v->root = lv_obj_create(NULL);
    lv_obj_set_size(v->root, UI_W, UI_H);
    lv_obj_set_style_pad_all(v->root, 0, 0);
    lv_obj_set_style_border_width(v->root, 0, 0);

    v->cam_area = lv_obj_create(v->root);
    lv_obj_set_size(v->cam_area, UI_W, CAM_H);
    lv_obj_align(v->cam_area, LV_ALIGN_TOP_MID, 0, H_TITLE);
    lv_obj_set_style_pad_all(v->cam_area, 0, 0);
    lv_obj_set_style_border_width(v->cam_area, 0, 0);

    v->img = lv_img_create(v->cam_area);
    lv_obj_set_size(v->img, UI_W, CAM_H);
    lv_obj_align(v->img, LV_ALIGN_TOP_LEFT, 0, 0);

    for (int i = 0; i < 4; ++i) {
        v->box_obj[i] = lv_obj_create(v->cam_area);
        set_box_style(v->box_obj[i]);
    }

    lv_obj_t* info = lv_obj_create(v->root);
    lv_obj_set_size(info, UI_W, H_INFO);
    lv_obj_align_to(info, v->cam_area, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(info, 8, 0);

    v->status_label = lv_label_create(info);
    lv_label_set_text(v->status_label, "ENROLL");
    lv_label_set_long_mode(v->status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v->status_label, UI_W - 16);
    lv_obj_align(v->status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    v->ta_name = lv_textarea_create(info);
    lv_obj_set_size(v->ta_name, UI_W - 16 - 170, 44);
    lv_obj_align(v->ta_name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_textarea_set_one_line(v->ta_name, true);
    lv_textarea_set_placeholder_text(v->ta_name, "Name...");

    v->btn_action = lv_btn_create(info);
    lv_obj_set_size(v->btn_action, 160, 44);
    lv_obj_align(v->btn_action, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    v->btn_action_label = lv_label_create(v->btn_action);
    lv_label_set_text(v->btn_action_label, "Start");
    lv_obj_center(v->btn_action_label);

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
    lv_label_set_text(v->mode_label, "MODE: ENROLL");
    lv_obj_center(v->mode_label);

    // keyboard
    v->kb = lv_keyboard_create(v->root);
    lv_obj_set_size(v->kb, UI_W, 260);
    lv_obj_align(v->kb, LV_ALIGN_BOTTOM_MID, 0, -H_BTN);
    set_visible(v->kb, false);

    return v;
}

void screen_enroll_destroy(ScreenEnroll* v) {
    if (!v) return;
    if (v->root && lv_obj_is_valid(v->root)) lv_obj_del(v->root);
    delete v;
}

void screen_enroll_set_status(ScreenEnroll* v, const char* s) {
    if (!v || !v->status_label || !s) return;
    lv_label_set_text(v->status_label, s);
}

void screen_enroll_set_mode_text(ScreenEnroll* v, const char* s) {
    if (!v || !v->mode_label || !s) return;
    lv_label_set_text(v->mode_label, s);
}

void screen_enroll_set_image(ScreenEnroll* v, const void* bgra, int w, int h) {
    if (!v || !v->img || !bgra) return;
    if (w <= 0 || h <= 0) return;

    std::memset(&v->img_dsc, 0, sizeof(v->img_dsc));
    v->img_dsc.header.w = (uint32_t)w;
    v->img_dsc.header.h = (uint32_t)h;
    v->img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    v->img_dsc.data_size = (uint32_t)(w * h * 4);
    v->img_dsc.data = (const uint8_t*)bgra;

    lv_img_set_src(v->img, &v->img_dsc);
}

void screen_enroll_set_boxes(ScreenEnroll* v, const ui_face_box_t* boxes, int n, int ui_w, int ui_h) {
    if (!v) return;
    if (n < 0) n = 0;
    if (n > 4) n = 4;

    for (int i = 0; i < 4; ++i) {
        if (!v->box_obj[i]) continue;
        if (i >= n || !boxes) {
            lv_obj_add_flag(v->box_obj[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int x1 = std::max(0, std::min(boxes[i].x1, ui_w - 1));
        int y1 = std::max(0, std::min(boxes[i].y1, ui_h - 1));
        int x2 = std::max(0, std::min(boxes[i].x2, ui_w - 1));
        int y2 = std::max(0, std::min(boxes[i].y2, ui_h - 1));
        if (x2 <= x1) x2 = std::min(ui_w - 1, x1 + 1);
        if (y2 <= y1) y2 = std::min(ui_h - 1, y1 + 1);

        lv_obj_set_pos(v->box_obj[i], x1, y1);
        lv_obj_set_size(v->box_obj[i], x2 - x1, y2 - y1);
        lv_obj_clear_flag(v->box_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void screen_enroll_set_action_text(ScreenEnroll* v, const char* t) {
    if (!v || !v->btn_action_label || !t) return;
    lv_label_set_text(v->btn_action_label, t);
}

void screen_enroll_kb_show_for(ScreenEnroll* v, lv_obj_t* ta) {
    if (!v || !v->kb) return;
    lv_keyboard_set_textarea(v->kb, ta);
    set_visible(v->kb, true);
    lv_obj_move_foreground(v->kb);
}

void screen_enroll_kb_hide(ScreenEnroll* v) {
    if (!v || !v->kb) return;
    set_visible(v->kb, false);
    lv_keyboard_set_textarea(v->kb, nullptr);
}
