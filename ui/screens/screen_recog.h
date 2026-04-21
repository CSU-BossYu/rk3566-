#pragma once
#include <lvgl.h>
#include "ui_types.h"

struct ScreenRecog {
    lv_obj_t* root = nullptr;

    lv_obj_t* cam_area = nullptr;
    lv_obj_t* img = nullptr;
    lv_img_dsc_t img_dsc{};
    lv_obj_t* box_obj[4]{nullptr, nullptr, nullptr, nullptr};

    lv_obj_t* status_label = nullptr;

    lv_obj_t* btn_left = nullptr;
    lv_obj_t* btn_right = nullptr;
    lv_obj_t* mode_label = nullptr;
};

ScreenRecog* screen_recog_create();
void screen_recog_destroy(ScreenRecog* v);

// UI thread only
void screen_recog_set_status(ScreenRecog* v, const char* s);
void screen_recog_set_mode_text(ScreenRecog* v, const char* s);
void screen_recog_set_image(ScreenRecog* v, const void* bgra, int w, int h);
void screen_recog_set_boxes(ScreenRecog* v, const ui_face_box_t* boxes, int n, int ui_w, int ui_h);
