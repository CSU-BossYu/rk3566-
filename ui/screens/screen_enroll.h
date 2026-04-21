#pragma once
#include <lvgl.h>
#include "ui_types.h"

struct ScreenEnroll {
    lv_obj_t* root = nullptr;

    lv_obj_t* cam_area = nullptr;
    lv_obj_t* img = nullptr;
    lv_img_dsc_t img_dsc{};
    lv_obj_t* box_obj[4]{nullptr, nullptr, nullptr, nullptr};

    lv_obj_t* status_label = nullptr;

    lv_obj_t* ta_name = nullptr;
    lv_obj_t* kb = nullptr;
    lv_obj_t* btn_action = nullptr;
    lv_obj_t* btn_action_label = nullptr;

    lv_obj_t* btn_left = nullptr;
    lv_obj_t* btn_right = nullptr;
    lv_obj_t* mode_label = nullptr;
};

ScreenEnroll* screen_enroll_create();
void screen_enroll_destroy(ScreenEnroll* v);

// UI thread only
void screen_enroll_set_status(ScreenEnroll* v, const char* s);
void screen_enroll_set_mode_text(ScreenEnroll* v, const char* s);
void screen_enroll_set_image(ScreenEnroll* v, const void* bgra, int w, int h);
void screen_enroll_set_boxes(ScreenEnroll* v, const ui_face_box_t* boxes, int n, int ui_w, int ui_h);

void screen_enroll_set_action_text(ScreenEnroll* v, const char* t);
void screen_enroll_kb_show_for(ScreenEnroll* v, lv_obj_t* ta);
void screen_enroll_kb_hide(ScreenEnroll* v);
