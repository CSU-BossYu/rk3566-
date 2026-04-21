#pragma once

#include <lvgl.h>

// 一个“纯 C++ 手写”的 OTA 页面（不依赖 SquareLine generated）
// 目标：
// - 展示 mode/status/info
// - 输入 OTA 服务器地址（写入 /data/access/cfg/ota.conf）
// - 点击“立即升级”触发 ui_app_request_ota()

struct ScreenOta {
    lv_obj_t* root      = nullptr;

    // nav
    lv_obj_t* btn_left  = nullptr;
    lv_obj_t* btn_right = nullptr;

    // header
    lv_obj_t* lbl_mode  = nullptr;
    lv_obj_t* lbl_info  = nullptr;

    // server
    lv_obj_t* ta_server = nullptr;
    lv_obj_t* btn_update = nullptr;

    // status
    lv_obj_t* lbl_status = nullptr;
};

ScreenOta* screen_ota_create(void);
void       screen_ota_destroy(ScreenOta* s);

void screen_ota_set_mode_text(ScreenOta* s, const char* txt);
void screen_ota_set_info(ScreenOta* s, const char* txt);
void screen_ota_set_status(ScreenOta* s, const char* txt);
