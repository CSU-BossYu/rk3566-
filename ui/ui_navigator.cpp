#include "ui_navigator.h"
#include "screens/screen_recog.h"
#include "screens/screen_enroll.h"
#include "screens/screen_manage.h"

#include <lvgl.h>

void UiNavigator::bind(ScreenRecog* r, ScreenEnroll* e, ScreenManage* m) {
    recog_ = r;
    enroll_ = e;
    manage_ = m;
}

void UiNavigator::set_mode(ui_mode_t mode) {
    cur_ = mode;

    lv_obj_t* scr = nullptr;
    if (mode == UI_MODE_RECOG && recog_) scr = recog_->root;
    else if (mode == UI_MODE_ENROLL && enroll_) scr = enroll_->root;
    else if (mode == UI_MODE_MANAGE && manage_) scr = manage_->root;

    if (scr) {
        lv_scr_load(scr);
    }
}