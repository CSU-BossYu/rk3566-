#pragma once
#include <lvgl.h>
#include <string>

#include "ui_types.h"

class UiModel;
class UiNavigator;

struct ScreenRecog;
struct ScreenEnroll;
struct ScreenManage;

class UiController {
public:
    UiController(UiModel* model, UiNavigator* nav,
                 ScreenRecog* r, ScreenEnroll* e, ScreenManage* m);

    // UI thread only
    void wire_events();
    void apply(); // update mode labels, enroll action text, delete enable, etc.

private:
    UiModel* model_{nullptr};
    UiNavigator* nav_{nullptr};
    ScreenRecog*  recog_{nullptr};
    ScreenEnroll* enroll_{nullptr};
    ScreenManage* manage_{nullptr};

    bool mbox_closing_{false};
    int  mbox_ok_pending_{0};

    // thunks
    static void on_btn_left(lv_event_t* e);
    static void on_btn_right(lv_event_t* e);

    static void on_enroll_action(lv_event_t* e);
    static void on_enroll_ta(lv_event_t* e);

    static void on_manage_delete(lv_event_t* e);
    static void on_manage_list_item(lv_event_t* e);

    static void on_mbox_btnmatrix(lv_event_t* e);
    static void on_mbox_deleted(lv_event_t* e);
    static void mbox_async_close(void* p);

    // helpers
    void switch_left_();
    void switch_right_();

    void enroll_action_();
    void enroll_ta_event_(lv_event_code_t code);

    void manage_delete_();
    void manage_list_item_(int idx);

    void open_delete_confirm_();

    const char* mode_text_(ui_mode_t m) const;
};