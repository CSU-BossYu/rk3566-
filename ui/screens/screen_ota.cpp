#include "screen_ota.h"

#include <cstdio>

static constexpr int UI_W = 480;
static constexpr int UI_H = 800;

static lv_obj_t* mk_btn(lv_obj_t* parent, const char* txt, int w, int h)
{
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

ScreenOta* screen_ota_create(void)
{
    auto* s = new ScreenOta();
    s->root = lv_obj_create(nullptr);
    lv_obj_clear_flag(s->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s->root, UI_W, UI_H);

    // top bar
    s->btn_left  = mk_btn(s->root, "<", 60, 44);
    s->btn_right = mk_btn(s->root, ">", 60, 44);
    lv_obj_align(s->btn_left,  LV_ALIGN_TOP_LEFT,  8, 8);
    lv_obj_align(s->btn_right, LV_ALIGN_TOP_RIGHT, -8, 8);

    s->lbl_mode = lv_label_create(s->root);
    lv_label_set_text(s->lbl_mode, "MODE: OTA");
    lv_obj_align(s->lbl_mode, LV_ALIGN_TOP_MID, 0, 18);

    // info
    s->lbl_info = lv_label_create(s->root);
    lv_label_set_text(s->lbl_info, "current: ?\nlast_good: ?\npending: ?");
    lv_obj_set_width(s->lbl_info, UI_W - 16);
    lv_label_set_long_mode(s->lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(s->lbl_info, LV_ALIGN_TOP_LEFT, 8, 70);

    // server input
    lv_obj_t* lbl_srv = lv_label_create(s->root);
    lv_label_set_text(lbl_srv, "OTA服务器(例: http://192.168.110.77:5000)");
    lv_obj_align(lbl_srv, LV_ALIGN_TOP_LEFT, 8, 170);

    s->ta_server = lv_textarea_create(s->root);
    lv_obj_set_size(s->ta_server, UI_W - 16, 44);
    lv_obj_align(s->ta_server, LV_ALIGN_TOP_LEFT, 8, 200);
    lv_textarea_set_one_line(s->ta_server, true);
    lv_textarea_set_placeholder_text(s->ta_server, "http://<ip>:<port>");

    s->btn_update = mk_btn(s->root, "立即升级", UI_W - 16, 52);
    lv_obj_align(s->btn_update, LV_ALIGN_TOP_LEFT, 8, 260);

    // status box
    lv_obj_t* box = lv_obj_create(s->root);
    lv_obj_set_size(box, UI_W - 16, 420);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 8, 330);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s->lbl_status = lv_label_create(box);
    lv_label_set_text(s->lbl_status, "OTA: idle");
    lv_obj_set_width(s->lbl_status, UI_W - 32);
    lv_label_set_long_mode(s->lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s->lbl_status, LV_ALIGN_TOP_LEFT, 0, 0);

    return s;
}

void screen_ota_destroy(ScreenOta* s)
{
    if (!s) return;
    if (s->root && lv_obj_is_valid(s->root)) {
        lv_obj_del(s->root);
    }
    delete s;
}

void screen_ota_set_mode_text(ScreenOta* s, const char* txt)
{
    if (!s || !s->lbl_mode) return;
    lv_label_set_text(s->lbl_mode, txt ? txt : "");
}

void screen_ota_set_info(ScreenOta* s, const char* txt)
{
    if (!s || !s->lbl_info) return;
    lv_label_set_text(s->lbl_info, txt ? txt : "");
}

void screen_ota_set_status(ScreenOta* s, const char* txt)
{
    if (!s || !s->lbl_status) return;
    lv_label_set_text(s->lbl_status, txt ? txt : "");
}
