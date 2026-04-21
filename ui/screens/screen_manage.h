#pragma once
#include <lvgl.h>
#include <vector>
#include <string>
#include "ui_types.h"

struct ScreenManageListItemCtx {
    int idx;
    void* app_ctx; // opaque pointer passed back to click callback
};

struct ScreenManage {
    lv_obj_t* root = nullptr;

    lv_obj_t* list = nullptr;
    std::vector<lv_obj_t*> list_btns;
    std::vector<ScreenManageListItemCtx> list_ctx;

    lv_obj_t* status_label = nullptr;

    lv_obj_t* btn_action = nullptr;        // Delete
    lv_obj_t* btn_action_label = nullptr;

    lv_obj_t* btn_left = nullptr;
    lv_obj_t* btn_right = nullptr;
    lv_obj_t* mode_label = nullptr;

    lv_obj_t* mbox = nullptr; // confirm box
};

ScreenManage* screen_manage_create();
void screen_manage_destroy(ScreenManage* v);

// UI thread only
void screen_manage_set_status(ScreenManage* v, const char* s);
void screen_manage_set_mode_text(ScreenManage* v, const char* s);

void screen_manage_list_rebuild(ScreenManage* v,
                                const std::vector<std::string>& names,
                                lv_event_cb_t on_item_clicked,
                                void* app_ctx);
void screen_manage_list_set_selected(ScreenManage* v, int idx);
void screen_manage_set_delete_enabled(ScreenManage* v, bool en);
